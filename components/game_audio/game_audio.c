#include "game_audio.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vendor/esp_codec_dev/device/include/es8311_codec.h"

/* Pins of the Waveshare ESP32-S3-ePaper-1.54 V2 board. */
enum {
    PIN_AUDIO_MCLK = 14,
    PIN_AUDIO_BCLK = 15,
    PIN_AUDIO_LRCLK = 38,
    PIN_AUDIO_DOUT = 45,
    PIN_CODEC_SDA = 47,
    PIN_CODEC_SCL = 48,
    PIN_CODEC_RAIL = 42,
    PIN_AUDIO_AMP_ENABLE = 46,
    /* Audio supply switch: LOW powers audio, HIGH removes its supply. */
    AUDIO_RAIL_ON = 0,
    AUDIO_RAIL_OFF = 1,
};

/* One game frame at 32 kHz contains 535 or 536 samples. */
#define AUDIO_BLOCK_SAMPLES 536
#define AUDIO_QUEUE_BLOCKS 4
#define AUDIO_DMA_DESC_COUNT 6
#define AUDIO_DMA_FRAME_COUNT 256
#define AUDIO_DMA_BYTES_PER_FRAME 4
#define AUDIO_DMA_FLUSH_BYTES (AUDIO_DMA_DESC_COUNT * AUDIO_DMA_FRAME_COUNT * AUDIO_DMA_BYTES_PER_FRAME)
#define AUDIO_TASK_STACK 4096
#define AUDIO_TASK_PRIORITY (tskIDLE_PRIORITY + 3)

static const char *TAG = "game_audio";

typedef struct {
    size_t count;
    size_t nonzero_count;
    uint32_t generation;
    int16_t samples[AUDIO_BLOCK_SAMPLES];
} audio_block_t;

typedef struct {
    audio_codec_ctrl_if_t iface;
    i2c_master_dev_handle_t device;
    bool failed;
} codec_control_t;

static codec_control_t s_control;
static const audio_codec_if_t *s_codec;
static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_tx;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_worker_done;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_enabled;
static bool s_playing;
static bool s_stop;
static bool s_accepting;
static unsigned s_submitters;
static uint32_t s_state_generation;
static unsigned s_sample_rate;
static bool s_ready;
static bool s_amp_active;
static bool s_tx_enabled;
static float s_volume_db = -12.0f;
static uint32_t s_window_submitted_blocks;
static uint32_t s_window_written_blocks;
static uint32_t s_window_dropped_blocks;
static uint32_t s_window_submitted_nonzero;
static uint32_t s_window_written_nonzero;
static uint32_t s_window_dropped_nonzero;
static int16_t s_stereo[AUDIO_BLOCK_SAMPLES * 2];
static uint8_t s_dma_silence[AUDIO_DMA_FLUSH_BYTES];

static bool codec_control_open(const audio_codec_ctrl_if_t *interface)
{
    (void)interface;
    return s_control.device != NULL;
}

static int codec_read_reg(const audio_codec_ctrl_if_t *interface, int reg,
                          int reg_len, void *data, int len)
{
    (void)interface;
    if (reg_len != 1 || !s_control.device || !data || len <= 0) return ESP_FAIL;
    const uint8_t address = (uint8_t)reg;
    esp_err_t err = i2c_master_transmit_receive(s_control.device, &address, 1,
                                                data, len, 100);
    s_control.failed |= err != ESP_OK;
    return err;
}

static int codec_write_reg(const audio_codec_ctrl_if_t *interface, int reg,
                           int reg_len, void *data, int len)
{
    (void)interface;
    if (reg_len != 1 || len != 1 || !data || !s_control.device) return ESP_FAIL;
    uint8_t bytes[2] = {(uint8_t)reg, *(uint8_t *)data};
    esp_err_t err = i2c_master_transmit(s_control.device, bytes, sizeof(bytes), 100);
    s_control.failed |= err != ESP_OK;
    return err;
}

static void amp_set(bool enabled)
{
    if (s_amp_active == enabled) return;
    gpio_set_level(PIN_AUDIO_AMP_ENABLE, enabled ? 1 : 0);
    s_amp_active = enabled;
}

static void audio_release(void)
{
    amp_set(false);
    if (s_codec) {
        (void)s_codec->mute(s_codec, true);
        (void)s_codec->enable(s_codec, false);
        (void)s_codec->close(s_codec);
        free((void *)s_codec);
        s_codec = NULL;
    }
    if (s_control.device) {
        i2c_master_bus_rm_device(s_control.device);
        s_control.device = NULL;
    }
    if (s_tx && s_tx_enabled) {
        (void)i2s_channel_disable(s_tx);
        s_tx_enabled = false;
    }
    if (s_tx) {
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_i2c_bus) {
        (void)i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    gpio_set_level(PIN_CODEC_RAIL, AUDIO_RAIL_OFF);
    s_ready = false;
}

static bool codec_init(unsigned sample_rate)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_CODEC_RAIL) | (1ULL << PIN_AUDIO_AMP_ENABLE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* Preload inactive levels before enabling outputs or releasing sleep holds. */
    esp_err_t err = gpio_set_level(PIN_AUDIO_AMP_ENABLE, 0);
    if (err == ESP_OK) err = gpio_set_level(PIN_CODEC_RAIL, AUDIO_RAIL_OFF);
    if (err == ESP_OK) err = gpio_config(&outputs);
    if (err == ESP_OK) err = gpio_hold_dis(PIN_AUDIO_AMP_ENABLE);
    if (err == ESP_OK) err = gpio_hold_dis(PIN_CODEC_RAIL);
    if (err != ESP_OK) goto fail;
    gpio_set_level(PIN_AUDIO_AMP_ENABLE, 0);
    s_amp_active = false;
    gpio_set_level(PIN_CODEC_RAIL, AUDIO_RAIL_OFF);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_CODEC_SDA,
        .scl_io_num = PIN_CODEC_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) goto fail;

    gpio_set_level(PIN_CODEC_RAIL, AUDIO_RAIL_ON);
    ESP_LOGI(TAG, "Audio rail enabled: GPIO %u = %u (active low)", PIN_CODEC_RAIL, AUDIO_RAIL_ON);
    vTaskDelay(pdMS_TO_TICKS(30));

    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    channel_cfg.dma_desc_num = AUDIO_DMA_DESC_COUNT;
    channel_cfg.dma_frame_num = AUDIO_DMA_FRAME_COUNT;
    err = i2s_new_channel(&channel_cfg, &s_tx, NULL);
    if (err != ESP_OK) goto fail;

    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_AUDIO_MCLK,
            .bclk = PIN_AUDIO_BCLK,
            .ws = PIN_AUDIO_LRCLK,
            .dout = PIN_AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &i2s_cfg);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) goto fail;
    s_tx_enabled = true;

    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_CODEC_DEFAULT_ADDR >> 1,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &device_cfg, &s_control.device);
    if (err != ESP_OK) goto fail;

    s_control.iface.is_open = codec_control_open;
    s_control.iface.read_reg = codec_read_reg;
    s_control.iface.write_reg = codec_write_reg;
    s_control.failed = false;
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = &s_control.iface,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, /* Amplifier is sequenced explicitly through GPIO 46. */
        .use_mclk = true,
        .hw_gain.pa_gain = 6,
    };
    s_codec = es8311_codec_new(&codec_cfg);
    if (!s_codec || s_control.failed) {
        err = ESP_FAIL;
        goto fail;
    }

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = sample_rate,
    };
    if (s_codec->set_fs(s_codec, &format) != ESP_OK ||
        s_codec->enable(s_codec, true) != ESP_OK ||
        s_codec->set_vol(s_codec, s_volume_db) != ESP_OK ||
        s_codec->mute(s_codec, true) != ESP_OK || s_control.failed) {
        err = ESP_FAIL;
        goto fail;
    }
    return true;

fail:
    ESP_LOGW(TAG, "Audio hardware unavailable (%s); continuing silently", esp_err_to_name(err));
    audio_release();
    return false;
}

static bool apply_output_state(bool active)
{
    if (!s_codec) return false;
    /* Stop TX and preload silence through every DMA descriptor before resuming. */
    if (s_codec->mute(s_codec, true) != ESP_OK || s_control.failed) return false;
    if (s_tx_enabled) {
        if (i2s_channel_disable(s_tx) != ESP_OK) return false;
        s_tx_enabled = false;
    }
    if (active) {
        size_t bytes_loaded = 0;
        esp_err_t err = i2s_channel_preload_data(s_tx, s_dma_silence,
                                                  sizeof(s_dma_silence), &bytes_loaded);
        if (err != ESP_OK || bytes_loaded != sizeof(s_dma_silence)) {
            ESP_LOGW(TAG, "Could not clear I2S DMA (%u/%u bytes)",
                     (unsigned)bytes_loaded, (unsigned)sizeof(s_dma_silence));
            return false;
        }
        if (i2s_channel_enable(s_tx) != ESP_OK) return false;
        s_tx_enabled = true;
    }
    if (active) {
        portENTER_CRITICAL(&s_state_mux);
        const float volume_db = s_volume_db;
        portEXIT_CRITICAL(&s_state_mux);
        if (s_codec->set_vol(s_codec, volume_db) != ESP_OK ||
            s_codec->mute(s_codec, false) != ESP_OK || s_control.failed) return false;
    }
    return true;
}

static void count_dropped_block(const audio_block_t *block)
{
    portENTER_CRITICAL(&s_state_mux);
    ++s_window_dropped_blocks;
    s_window_dropped_nonzero += block->nonzero_count;
    portEXIT_CRITICAL(&s_state_mux);
}

static void maybe_log_audio_stats(TickType_t *last_stats)
{
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - *last_stats) < pdMS_TO_TICKS(5000)) return;

    uint32_t submitted_blocks, written_blocks, dropped_blocks;
    uint32_t submitted_nonzero, written_nonzero, dropped_nonzero;
    portENTER_CRITICAL(&s_state_mux);
    submitted_blocks = s_window_submitted_blocks;
    written_blocks = s_window_written_blocks;
    dropped_blocks = s_window_dropped_blocks;
    submitted_nonzero = s_window_submitted_nonzero;
    written_nonzero = s_window_written_nonzero;
    dropped_nonzero = s_window_dropped_nonzero;
    s_window_submitted_blocks = s_window_written_blocks = s_window_dropped_blocks = 0;
    s_window_submitted_nonzero = s_window_written_nonzero = s_window_dropped_nonzero = 0;
    portEXIT_CRITICAL(&s_state_mux);
    if (submitted_blocks || written_blocks || dropped_blocks) {
        ESP_LOGI(TAG, "5s blocks submitted/written/dropped=%" PRIu32 "/%" PRIu32 "/%" PRIu32
                 " nonzero submitted/written/dropped=%" PRIu32 "/%" PRIu32 "/%" PRIu32,
                 submitted_blocks, written_blocks, dropped_blocks,
                 submitted_nonzero, written_nonzero, dropped_nonzero);
    }
    *last_stats = now;
}

static void audio_worker(void *arg)
{
    (void)arg;
    bool was_active = false;
    uint32_t handled_generation = 0;
    TickType_t last_stats = xTaskGetTickCount();
    audio_block_t block;
    for (;;) {
        bool active;
        bool stop;
        uint32_t generation;
        portENTER_CRITICAL(&s_state_mux);
        active = s_ready && s_enabled && s_playing;
        stop = s_stop;
        generation = s_state_generation;
        portEXIT_CRITICAL(&s_state_mux);
        if (stop) break;

        if (active != was_active || generation != handled_generation) {
            bool output_ok = apply_output_state(active);
            if (!output_ok && s_ready) {
                ESP_LOGW(TAG, "Audio state transition failed; disabling output");
                portENTER_CRITICAL(&s_state_mux);
                s_enabled = false;
                ++s_state_generation;
                active = false;
                portEXIT_CRITICAL(&s_state_mux);
            }
            was_active = active;
            handled_generation = generation;

            /* Serialize amplifier re-enable with synchronous mute requests. */
            portENTER_CRITICAL(&s_state_mux);
            const bool still_active = s_ready && s_enabled && s_playing && !s_stop &&
                                      s_state_generation == generation;
            if (still_active) {
                amp_set(true);
                was_active = true;
            } else {
                amp_set(false);
                was_active = false;
            }
            portEXIT_CRITICAL(&s_state_mux);
        }

        if (!s_queue || !s_tx || !active) {
            while (s_queue && xQueueReceive(s_queue, &block, 0) == pdTRUE) {
                count_dropped_block(&block);
            }
            maybe_log_audio_stats(&last_stats);
            /* Not playing (the whole time the player sits in menus): state
             * changes are rare and a session start tolerates this latency
             * easily, so check at 10 Hz instead of 100 Hz and let the core
             * idle in between. The playing path below is unchanged. */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (xQueueReceive(s_queue, &block, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        /* Discard blocks submitted before any pause/disable/resume transition. */
        portENTER_CRITICAL(&s_state_mux);
        active = s_ready && s_enabled && s_playing;
        const bool current_block = block.generation == s_state_generation && !s_stop;
        portEXIT_CRITICAL(&s_state_mux);
        if (!active || !current_block) {
            count_dropped_block(&block);
            continue;
        }

        for (size_t i = 0; i < block.count; ++i) {
            s_stereo[2 * i] = block.samples[i];
            s_stereo[2 * i + 1] = block.samples[i];
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_stereo,
                                           block.count * 2 * sizeof(int16_t),
                                           &written, 50);
        if (err != ESP_OK || written != block.count * 4) {
            ESP_LOGW(TAG, "I2S write failed; dropping audio block");
            count_dropped_block(&block);
            portENTER_CRITICAL(&s_state_mux);
            amp_set(false);
            portEXIT_CRITICAL(&s_state_mux);
            (void)s_codec->mute(s_codec, true);
            was_active = false;
            /* Avoid coupling game execution to persistent peripheral failures. */
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            portENTER_CRITICAL(&s_state_mux);
            ++s_window_written_blocks;
            s_window_written_nonzero += block.nonzero_count;
            portEXIT_CRITICAL(&s_state_mux);
        }

        maybe_log_audio_stats(&last_stats);
    }
    portENTER_CRITICAL(&s_state_mux);
    amp_set(false);
    portEXIT_CRITICAL(&s_state_mux);
    /* After signaling, this task only self-deletes and touches no shared state. */
    xSemaphoreGive(s_worker_done);
    vTaskDelete(NULL);
}

esp_err_t game_audio_init(unsigned sample_rate)
{
    if (s_task || s_queue) return ESP_ERR_INVALID_STATE;
    if (sample_rate < 8000 || sample_rate > 48000) return ESP_ERR_INVALID_ARG;
    s_sample_rate = sample_rate;
    s_stop = false;
    s_enabled = false;
    s_playing = false;
    s_state_generation = 1;
    s_submitters = 0;
    s_window_submitted_blocks = s_window_written_blocks = s_window_dropped_blocks = 0;
    s_window_submitted_nonzero = s_window_written_nonzero = s_window_dropped_nonzero = 0;
    s_queue = xQueueCreate(AUDIO_QUEUE_BLOCKS, sizeof(audio_block_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    s_worker_done = xSemaphoreCreateBinary();
    if (!s_worker_done) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    bool hw_ok = codec_init(sample_rate);
    s_ready = hw_ok;
    if (xTaskCreate(audio_worker, "game_audio", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIORITY, &s_task) != pdPASS) {
        audio_release();
        vQueueDelete(s_queue);
        s_queue = NULL;
        vSemaphoreDelete(s_worker_done);
        s_worker_done = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_state_mux);
    s_accepting = true;
    portEXIT_CRITICAL(&s_state_mux);
    ESP_LOGI(TAG, "Output %u Hz, hardware %s", s_sample_rate, hw_ok ? "ready" : "silent");
    /* Hardware setup failure is intentionally nonfatal for gameplay. */
    return ESP_OK;
}

void game_audio_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_state_mux);
    if (!s_accepting) {
        portEXIT_CRITICAL(&s_state_mux);
        return;
    }
    if (s_enabled != enabled) ++s_state_generation;
    s_enabled = enabled;
    if (!enabled) amp_set(false);
    portEXIT_CRITICAL(&s_state_mux);
}

void game_audio_set_playing(bool playing)
{
    portENTER_CRITICAL(&s_state_mux);
    if (!s_accepting) {
        portEXIT_CRITICAL(&s_state_mux);
        return;
    }
    if (s_playing != playing) ++s_state_generation;
    s_playing = playing;
    if (!playing) amp_set(false);
    portEXIT_CRITICAL(&s_state_mux);
}

void game_audio_set_volume(float db)
{
    portENTER_CRITICAL(&s_state_mux);
    s_volume_db = db;
    portEXIT_CRITICAL(&s_state_mux);
}

void game_audio_submit(const int16_t *mono, size_t sample_count)
{
    if (!mono || !sample_count || sample_count > AUDIO_BLOCK_SAMPLES) return;
    portENTER_CRITICAL(&s_state_mux);
    bool active = s_accepting && s_ready && s_enabled && s_playing && !s_stop;
    uint32_t generation = s_state_generation;
    QueueHandle_t queue = s_queue;
    if (active && queue) ++s_submitters;
    portEXIT_CRITICAL(&s_state_mux);
    if (!active || !queue) return;

    audio_block_t block;
    block.count = sample_count;
    block.nonzero_count = 0;
    block.generation = generation;
    memcpy(block.samples, mono, sample_count * sizeof(*mono));
    for (size_t i = 0; i < sample_count; ++i) {
        if (mono[i] != 0) ++block.nonzero_count;
    }
    /* A state change racing the send is tagged and discarded by the worker. */
    BaseType_t queued = xQueueSend(queue, &block, 0);
    portENTER_CRITICAL(&s_state_mux);
    ++s_window_submitted_blocks;
    s_window_submitted_nonzero += block.nonzero_count;
    if (queued != pdTRUE) {
        ++s_window_dropped_blocks;
        s_window_dropped_nonzero += block.nonzero_count;
    }
    --s_submitters;
    portEXIT_CRITICAL(&s_state_mux);
}

void game_audio_shutdown(void)
{
    if (!s_queue && !s_task) return;
    portENTER_CRITICAL(&s_state_mux);
    s_accepting = false;
    s_stop = true;
    s_enabled = false;
    s_playing = false;
    ++s_state_generation;
    amp_set(false);
    portEXIT_CRITICAL(&s_state_mux);
    /* Wait for bounded in-flight submit copies/sends before queue deletion. */
    for (;;) {
        portENTER_CRITICAL(&s_state_mux);
        unsigned submitters = s_submitters;
        portEXIT_CRITICAL(&s_state_mux);
        if (!submitters) break;
        vTaskDelay(1);
    }
    if (s_task) (void)xSemaphoreTake(s_worker_done, portMAX_DELAY);
    audio_block_t block;
    while (s_queue && xQueueReceive(s_queue, &block, 0) == pdTRUE) {
        count_dropped_block(&block);
    }
    audio_release();
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_task = NULL;
    if (s_worker_done) {
        vSemaphoreDelete(s_worker_done);
        s_worker_done = NULL;
    }
    s_stop = false;
}
