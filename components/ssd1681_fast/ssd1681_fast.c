#include "ssd1681_fast.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* SAFETY: never send command 0x30, which programs waveform OTP. LUTs use volatile command 0x32;
 * the private command whitelist excludes every OTP programmer and booster write. */
enum {
    CMD_DRIVER_OUTPUT = 0x01, CMD_SLEEP = 0x10, CMD_ENTRY = 0x11,
    CMD_RESET = 0x12, CMD_TEMPERATURE = 0x18, CMD_ACTIVATE = 0x20,
    CMD_UPDATE = 0x22, CMD_CURRENT = 0x24, CMD_PREVIOUS = 0x26,
    CMD_LUT = 0x32, CMD_BORDER = 0x3c, CMD_X_WINDOW = 0x44,
    CMD_Y_WINDOW = 0x45, CMD_X_CURSOR = 0x4e, CMD_Y_CURSOR = 0x4f,
    CMD_VENDOR_GATE = 0x03, CMD_VENDOR_SOURCE = 0x04, CMD_VENDOR_VCOM = 0x2c,
    CMD_VENDOR_PARTIAL = 0x37, CMD_VENDOR_END = 0x3f,
};
/* Pins of the Waveshare ESP32-S3-ePaper-1.54 V2; PIN_PWR drives the active-low AO3401 panel supply switch. */
enum { PIN_PWR = 6, PIN_BUSY = 8, PIN_RST = 9, PIN_DC = 10,
       PIN_CS = 11, PIN_CLK = 12, PIN_MOSI = 13 };
#define BUSY_TIMEOUT_MS 10000
#define ROW_BYTES (SSD1681_WIDTH / 8)
/* 0.4 ms at 20 MHz; shorter transfers end before queue/ISR/wake overhead pays off. */
#define IRQ_TX_MIN_BYTES 1000
static const char *TAG = "ssd1681";
static const gpio_num_t signal_pins[] = {PIN_RST, PIN_DC, PIN_CS, PIN_CLK, PIN_MOSI};

struct ssd1681_device {
    spi_device_handle_t spi;
    bool fast_transport;
    unsigned active_spi_hz;
    SemaphoreHandle_t completion;
    portMUX_TYPE lock;
    bool armed;
    int64_t rise_us, fall_us;
    bool powered, analog_on, fault, full_done;
    bool bus_created, isr_service, isr_handler;
    bool vendor_profile, vendor_partial, vendor_keep_power, vendor_mixed;
    uint8_t vendor_g0_frames;
    uint8_t gray_frame_rate;
    uint8_t gray_strength, gray_wave_mode;
    uint8_t *gray_wave_planes;
    bool vendor_skip_hold_frame;
    unsigned updates, since_clean;
    bool continuous_play;
    /* Rows outside [ram_first, ram_last] are zero in both planes (0x24 BW, 0x26 R). Cleared by
     * everything but mixed uploads and 0x0c scans; one span survives a plane-to-plane copy. */
    bool window_ok, ram_known;
    int16_t ram_first, ram_last;
};

static void busy_isr(void *arg)
{
    struct ssd1681_device *d = arg;
    const int level = gpio_get_level(PIN_BUSY);
    const int64_t now = esp_timer_get_time();
    bool done = false;
    portENTER_CRITICAL_ISR(&d->lock);
    if (d->armed) {
        if (level && d->rise_us == 0) d->rise_us = now;
        if (!level && d->rise_us && d->fall_us == 0) {
            d->fall_us = now;
            done = true;
        }
    }
    portEXIT_CRITICAL_ISR(&d->lock);
    if (done) {
        BaseType_t wake = pdFALSE;
        xSemaphoreGiveFromISR(d->completion, &wake);
        if (wake) portYIELD_FROM_ISR();
    }
}

static esp_err_t wait_idle(struct ssd1681_device *d)
{
    const int64_t deadline = esp_timer_get_time() + BUSY_TIMEOUT_MS * 1000LL;
    while (gpio_get_level(PIN_BUSY)) {
        if (esp_timer_get_time() >= deadline) {
            d->fault = true;
            ESP_LOGE(TAG, "BUSY stuck high; aborting");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static esp_err_t transmit(struct ssd1681_device *d, bool data,
                          const uint8_t *bytes, size_t length, bool plane)
{
    (void)plane;
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_DC, data), TAG, "DC");
    spi_transaction_t tx = {.length = length * 8, .tx_buffer = bytes};
    const unsigned hz = d->fast_transport ? 20000000 : SSD1681_SPI_HZ;
    /* IDF retains the override. Pay its setup cost only when changing mode. */
    if (hz != d->active_spi_hz) tx.override_freq_hz = hz;
#ifdef CONFIG_SSD1681_EXPERIMENTAL_INTERRUPT_FRAME_TX
    esp_err_t err = length >= IRQ_TX_MIN_BYTES ? spi_device_transmit(d->spi, &tx)
                                               : spi_device_polling_transmit(d->spi, &tx);
#else
    esp_err_t err = spi_device_polling_transmit(d->spi, &tx);
#endif
    if (err == ESP_OK) d->active_spi_hz = hz;
    if (err != ESP_OK) d->fault = true;
    return err;
}

static esp_err_t transfer(struct ssd1681_device *d, bool data,
                          const uint8_t *bytes, size_t length)
{
    return transmit(d, data, bytes, length, false);
}

static esp_err_t send_command(struct ssd1681_device *d, uint8_t command)
{
    switch (command) {
    case CMD_DRIVER_OUTPUT: case CMD_SLEEP: case CMD_ENTRY: case CMD_RESET:
    case CMD_TEMPERATURE: case CMD_ACTIVATE: case CMD_UPDATE: case CMD_CURRENT:
    case CMD_PREVIOUS: case CMD_LUT: case CMD_BORDER: case CMD_X_WINDOW:
    case CMD_Y_WINDOW: case CMD_X_CURSOR: case CMD_Y_CURSOR:
    case CMD_VENDOR_GATE: case CMD_VENDOR_SOURCE: case CMD_VENDOR_VCOM:
    case CMD_VENDOR_PARTIAL: case CMD_VENDOR_END:
        break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    if (!d->powered || !d->spi || d->fault) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(wait_idle(d), TAG, "command while busy");
    return transfer(d, false, &command, 1);
}

static esp_err_t reg_write(struct ssd1681_device *d, uint8_t command,
                           const uint8_t *data, size_t length)
{
    ESP_RETURN_ON_ERROR(send_command(d, command), TAG, "command");
    return transfer(d, true, data, length);
}

static esp_err_t reg_byte(struct ssd1681_device *d, uint8_t command, uint8_t data)
{
    return reg_write(d, command, &data, 1);
}

static void ram_forget(struct ssd1681_device *d)
{
    d->window_ok = d->ram_known = false;
}

static esp_err_t activate(struct ssd1681_device *d, uint8_t control,
                          ssd1681_timing_t *timing)
{
    *timing = (ssd1681_timing_t){0};
    /* Only the mixed scan (ping-pong off) is trusted to leave RAM and window untouched. */
    if (control != 0x0c) ram_forget(d);
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_UPDATE, control), TAG, "update control");
    while (xSemaphoreTake(d->completion, 0) == pdTRUE) {}
    portENTER_CRITICAL(&d->lock);
    d->rise_us = d->fall_us = 0;
    d->armed = true;
    portEXIT_CRITICAL(&d->lock);
    const int64_t started = esp_timer_get_time();
    esp_err_t err = send_command(d, CMD_ACTIVATE);
    if (err == ESP_OK && xSemaphoreTake(d->completion,
                         pdMS_TO_TICKS(BUSY_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "No complete BUSY pulse for control 0x%02x (GPIO=%d)",
                 control, gpio_get_level(PIN_BUSY));
        err = ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&d->lock);
    d->armed = false;
    timing->busy_us = d->fall_us > d->rise_us ? d->fall_us - d->rise_us : 0;
    portEXIT_CRITICAL(&d->lock);
    timing->activation_us = esp_timer_get_time() - started;
    if (err != ESP_OK || gpio_get_level(PIN_BUSY)) {
        d->fault = true;
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    return ESP_OK;
}

static bool update_limit_reached(const struct ssd1681_device *d, unsigned reserve)
{
    if (d->continuous_play) return false;
    return d->updates >= SSD1681_MAX_UPDATES - reserve;
}

static bool clean_limit_reached(const struct ssd1681_device *d)
{
    if (d->continuous_play) return false;
    return d->since_clean >= CLEAN_REFRESH_INTERVAL;
}

static void increment_saturated(unsigned *counter)
{
    if (*counter != UINT_MAX) ++*counter;
}

esp_err_t ssd1681_enable_continuous_play(ssd1681_handle_t d)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    if (d->powered || d->fault || d->updates || d->since_clean)
        return ESP_ERR_INVALID_STATE;
    d->continuous_play = true;
    return ESP_OK;
}

/* Vendor profile: entry 0x01 (Y decrements) from Y=199, so frame row r lives at Y=199-r. */
static esp_err_t ram_cursor(struct ssd1681_device *d, unsigned row)
{
    const uint8_t cursor_y[] = {(uint8_t)(d->vendor_profile ? SSD1681_HEIGHT - 1 - row : row), 0};
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_X_CURSOR, 0), TAG, "X cursor");
    return reg_write(d, CMD_Y_CURSOR, cursor_y, sizeof(cursor_y));
}

static esp_err_t frame_window(struct ssd1681_device *d)
{
    const uint8_t x[] = {0, 24};
    const uint8_t y[] = {d->vendor_profile ? 199 : 0, 0,
                         d->vendor_profile ? 0 : 199, 0};
    ESP_RETURN_ON_ERROR(reg_write(d, CMD_X_WINDOW, x, sizeof(x)), TAG, "X window");
    ESP_RETURN_ON_ERROR(reg_write(d, CMD_Y_WINDOW, y, sizeof(y)), TAG, "Y window");
    d->window_ok = true;
    return ram_cursor(d, 0);
}

/* The window stays full height: the counter walks rows first..first+rows-1 and the
 * transfer ends before it can reach the window end, exactly as within a full upload. */
static esp_err_t plane_upload(struct ssd1681_device *d, const uint8_t *frame,
                              unsigned first, unsigned rows, bool previous)
{
    const bool fresh = !d->window_ok;
    if (fresh) ESP_RETURN_ON_ERROR(frame_window(d), TAG, "frame window");
    if (!fresh || first) ESP_RETURN_ON_ERROR(ram_cursor(d, first), TAG, "RAM cursor");
    ESP_RETURN_ON_ERROR(send_command(d, previous ? CMD_PREVIOUS : CMD_CURRENT), TAG, "RAM write");
    return transmit(d, true, frame + first * ROW_BYTES, rows * ROW_BYTES, true);
}

static esp_err_t spi_open(struct ssd1681_device *d)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = SSD1681_FRAME_BYTES,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    d->bus_created = true;
    spi_device_interface_config_t device = {
        .clock_speed_hz = SSD1681_SPI_HZ, .mode = 0,
        .spics_io_num = PIN_CS, .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &device, &d->spi);
    if (err != ESP_OK) return err;
    d->active_spi_hz = SSD1681_SPI_HZ;
    return ESP_OK;
}

esp_err_t ssd1681_create(ssd1681_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    struct ssd1681_device *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    d->completion = xSemaphoreCreateBinary();
    esp_err_t err = d->completion ? ESP_OK : ESP_ERR_NO_MEM;
    if (err != ESP_OK) goto fail;
    /* Preload OFF before enabling the active-low power output. */
    err = gpio_set_level(PIN_PWR, 1);
    if (err != ESP_OK) goto fail;
    gpio_config_t pins = {.pin_bit_mask = 1ULL << PIN_PWR,
                          .mode = GPIO_MODE_OUTPUT};
    err = gpio_config(&pins);
    if (err != ESP_OK) goto fail;
    pins.pin_bit_mask = (1ULL << PIN_RST) | (1ULL << PIN_DC);
    err = gpio_config(&pins);
    if (err != ESP_OK) goto fail;
    pins.pin_bit_mask = 1ULL << PIN_BUSY;
    pins.mode = GPIO_MODE_INPUT;
    pins.intr_type = GPIO_INTR_ANYEDGE;
    err = gpio_config(&pins);
    if (err != ESP_OK) goto fail;
    err = gpio_install_isr_service(0);
    d->isr_service = err == ESP_OK;
    /* The board buttons may own the shared service; uninstalling it on destroy would drop their ISR. */
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err != ESP_OK) goto fail;
    err = gpio_isr_handler_add(PIN_BUSY, busy_isr, d);
    if (err != ESP_OK) goto fail;
    d->isr_handler = true;
    for (size_t i = 0; i < sizeof(signal_pins) / sizeof(signal_pins[0]); ++i) {
        err = gpio_set_direction(signal_pins[i], GPIO_MODE_INPUT);
        if (err != ESP_OK) goto fail;
    }
    *out = d;
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "create: %s", esp_err_to_name(err));
    esp_err_t cleanup = ssd1681_destroy(d);
    if (cleanup != ESP_OK) ESP_LOGE(TAG, "cleanup: %s", esp_err_to_name(cleanup));
    return err;
}

esp_err_t ssd1681_power_on(ssd1681_handle_t d)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    if (d->powered || d->fault || update_limit_reached(d, 0))
        return ESP_ERR_INVALID_STATE;
    ram_forget(d);
    d->vendor_profile = d->vendor_partial = false;
    d->vendor_keep_power = d->vendor_mixed = false;
    d->vendor_g0_frames = 15;
    d->gray_frame_rate = 2;
    d->gray_strength = d->gray_wave_mode = 0;
    heap_caps_free(d->gray_wave_planes);d->gray_wave_planes=NULL;
    d->fast_transport = false;
    d->vendor_skip_hold_frame = false;
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_PWR, 0), TAG, "power on");
    d->powered = true;
    vTaskDelay(pdMS_TO_TICKS(10));
    /* IDF 6.1 gpio_set_direction(OUTPUT) disconnects peripheral routing, so only GPIO controls are set
     * here; SPI initialization owns MOSI/SCK/CS. */
    ESP_RETURN_ON_ERROR(gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT), TAG, "RST output");
    ESP_RETURN_ON_ERROR(gpio_set_direction(PIN_DC, GPIO_MODE_OUTPUT), TAG, "DC output");
    ESP_RETURN_ON_ERROR(spi_open(d), TAG, "SPI routing");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RST, 1), TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RST, 0), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RST, 1), TAG, "reset release");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(send_command(d, CMD_RESET), TAG, "software reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(wait_idle(d), TAG, "reset busy");
    /* Increasing X/Y addressing, MSB left, as in the GxEPD2 GDEY0154D67 driver; Waveshare's sample
     * uses decreasing Y with a matching reversed gate scan. */
    const uint8_t output[] = {199, 0, 0};
    ESP_RETURN_ON_ERROR(reg_write(d, CMD_DRIVER_OUTPUT, output, sizeof(output)), TAG, "gate scan");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_ENTRY, 0x03), TAG, "RAM entry");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_BORDER, 0x05), TAG, "border");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_TEMPERATURE, 0x80), TAG, "internal temperature");
    d->full_done = d->analog_on = false;
    return frame_window(d);
}

esp_err_t ssd1681_write_frame(ssd1681_handle_t d, const uint8_t *frame,
                            size_t length, bool previous, int64_t *upload_us)
{
    if (!d || !frame || length != SSD1681_FRAME_BYTES || !upload_us)
        return ESP_ERR_INVALID_ARG;
    const int64_t start = esp_timer_get_time();
    d->ram_known = false;
    ESP_RETURN_ON_ERROR(plane_upload(d, frame, 0, SSD1681_HEIGHT, previous), TAG, "frame upload");
    *upload_us = esp_timer_get_time() - start;
    return ESP_OK;
}

esp_err_t ssd1681_analog_power(ssd1681_handle_t d, bool on, ssd1681_timing_t *timing)
{
    if (!d || !timing) return ESP_ERR_INVALID_ARG;
    *timing = (ssd1681_timing_t){0};
    if (!d->powered || d->fault) return ESP_ERR_INVALID_STATE;
    if (d->vendor_profile && on) return ESP_ERR_INVALID_STATE;
    /* Menu idle may already have powered analog down; repeating power-off gives no BUSY edge and
     * activate() would wait out its timeout. Software state stays authoritative. */
    if (d->analog_on == on) return ESP_OK;
    ESP_RETURN_ON_ERROR(activate(d, on ? 0xc0 : 0x03, timing), TAG, "analog power");
    d->analog_on = on;
    return ESP_OK;
}


#include "ssd1681_vendor_tables.h"

static esp_err_t vendor_reset(struct ssd1681_device *d, bool software)
{
    ram_forget(d);
    ESP_RETURN_ON_ERROR(wait_idle(d), TAG, "vendor reset while busy");
    if (d->analog_on) {
        ssd1681_timing_t power;
        ESP_RETURN_ON_ERROR(ssd1681_analog_power(d, false, &power), TAG, "vendor analog off");
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RST, 1), TAG, "vendor reset high");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RST, 0), TAG, "vendor reset low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RST, 1), TAG, "vendor reset release");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(wait_idle(d), TAG, "vendor hardware reset");
    if (software) {
        ESP_RETURN_ON_ERROR(send_command(d, CMD_RESET), TAG, "vendor software reset");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(wait_idle(d), TAG, "vendor software reset busy");
    }
    return ESP_OK;
}

/* Bytes 153..158 are not part of command 0x32: EOPT, gate, 3 source, VCOM.
 * Command 0x37 is the vendor partial setup, sent as-is. */
static esp_err_t vendor_load(struct ssd1681_device *d, bool partial)
{
    ram_forget(d);
    const uint8_t *p = partial ? ws_vendor_partial : ws_vendor_full;
    const uint8_t *lut = p;
    uint8_t mixed_lut[SSD1681_LUT_BYTES];
    if (partial && d->vendor_mixed) {
        memcpy(mixed_lut, p, sizeof(mixed_lut));
        mixed_lut[1] = mixed_lut[37] = 0;
        mixed_lut[60] = d->vendor_g0_frames;
        /* Single-frame waveform: keep G1 A's active frame; skip its all-VSS B frame. */
        if (d->vendor_skip_hold_frame) mixed_lut[68] = 0;
        /* SSD1681 fig 6-6: byte 144 low nibble FR[1], high nibble FR[0].
         * Only the active G1 frame-frequency selector changes. */
        mixed_lut[144] = (mixed_lut[144] & 0xf8u) | d->gray_frame_rate;
        ssd1681_gray_wave_lut(mixed_lut,d->gray_wave_mode);
        lut = mixed_lut;
    }
    ESP_RETURN_ON_ERROR(reg_write(d, CMD_LUT, lut, SSD1681_LUT_BYTES), TAG, "vendor LUT");
    ESP_RETURN_ON_ERROR(wait_idle(d), TAG, "vendor LUT busy");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_VENDOR_END, p[153]), TAG, "vendor end");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_VENDOR_GATE, p[154]), TAG, "vendor gate");
    const uint8_t *source = p + 155;
    /* SSD1681 rev 0.13 p21: VSL -15/-13/-11/-9 V, selected by LUT1 for white drive. The weak W>L
     * waveform also selects bounded VSH2; VSH1, VCOM, gate and EOPT stay stock. */
    static const uint8_t vsl[] = {0x32, 0x2a, 0x22, 0x1a};
    uint8_t gray_source[3];
    if (partial && d->vendor_mixed &&
        d->vendor_skip_hold_frame && d->gray_frame_rate == 7 &&
        (d->gray_strength || d->gray_wave_mode)) {
        memcpy(gray_source, source, sizeof(gray_source));
        gray_source[2] = vsl[d->gray_strength];
        gray_source[1] = ssd1681_gray_wave_vsh2(d->gray_wave_mode);
        source = gray_source;
    }
    ESP_RETURN_ON_ERROR(reg_write(d, CMD_VENDOR_SOURCE, source, 3), TAG, "vendor source");
    return reg_byte(d, CMD_VENDOR_VCOM, p[158]);
}

static esp_err_t vendor_full_run(struct ssd1681_device *d, const uint8_t *frame,
                                 ssd1681_timing_t *timing)
{
    ESP_RETURN_ON_ERROR(vendor_reset(d, true), TAG, "vendor full reset");
    const uint8_t output[] = {199, 0, 1};
    ESP_RETURN_ON_ERROR(reg_write(d, CMD_DRIVER_OUTPUT, output, sizeof(output)), TAG, "vendor gates");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_ENTRY, 0x01), TAG, "vendor entry");
    ESP_RETURN_ON_ERROR(frame_window(d), TAG, "vendor window");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_BORDER, 0x01), TAG, "vendor border");
    ESP_RETURN_ON_ERROR(reg_byte(d, CMD_TEMPERATURE, 0x80), TAG, "vendor temperature");
    ssd1681_timing_t prepare;
    ESP_RETURN_ON_ERROR(activate(d, 0xb1, &prepare), TAG, "vendor temperature load");
    ESP_RETURN_ON_ERROR(vendor_load(d, false), TAG, "vendor full package");
    int64_t upload;
    ESP_RETURN_ON_ERROR(ssd1681_write_frame(d, frame, SSD1681_FRAME_BYTES, false, &upload), TAG, "vendor current");
    ESP_RETURN_ON_ERROR(ssd1681_write_frame(d, frame, SSD1681_FRAME_BYTES, true, &upload), TAG, "vendor previous");
    return activate(d, 0xc7, timing);
}

static esp_err_t vendor_partial_prepare(struct ssd1681_device *d)
{
    if (!d->vendor_partial) {
        ESP_RETURN_ON_ERROR(vendor_reset(d, false), TAG, "vendor partial reset");
        ESP_RETURN_ON_ERROR(vendor_load(d, true), TAG, "vendor partial package");
        /* SSD1681 R37 F[6]: 0 disables automatic RAM ping-pong. Mixed scans
         * supply both planes, so no implicit previous image. */
        const uint8_t partial_config[] = {0,0,0,0,0,0,0,0,0,0};
        ESP_RETURN_ON_ERROR(reg_write(d, CMD_VENDOR_PARTIAL, partial_config,
                                      sizeof(partial_config)), TAG, "vendor partial options");
        ESP_RETURN_ON_ERROR(reg_byte(d, CMD_BORDER, 0x80), TAG, "vendor partial border");
        ssd1681_timing_t power;
        ESP_RETURN_ON_ERROR(activate(d, 0xc0, &power), TAG, "vendor partial power");
        d->analog_on = true;
    }
    return ESP_OK;
}

static esp_err_t vendor_result(struct ssd1681_device *d, esp_err_t err, bool partial)
{
    if (err != ESP_OK) {
        d->fault = true;
        ram_forget(d);
        esp_err_t cleanup = ssd1681_power_off(d);
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "vendor fault cleanup: %s", esp_err_to_name(cleanup));
        return err;
    }
    d->analog_on = partial && d->vendor_mixed && d->vendor_keep_power;
    d->vendor_partial = partial;
    d->full_done = true;
    if (partial) increment_saturated(&d->since_clean);
    else d->since_clean = 0;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_full(ssd1681_handle_t d, const uint8_t *frame,
                            size_t length, ssd1681_timing_t *timing)
{
    if (!d || !frame || !timing || length != SSD1681_FRAME_BYTES) return ESP_ERR_INVALID_ARG;
    *timing = (ssd1681_timing_t){0};
    if (!d->powered || d->fault || update_limit_reached(d, 0))
        return ESP_ERR_INVALID_STATE;
    increment_saturated(&d->updates); /* Controller resets must never reset the lifetime attempt cap. */
    ram_forget(d);
    d->fast_transport = false;
    d->vendor_profile = true;
    d->vendor_partial = d->full_done = false;
    d->vendor_keep_power = d->vendor_mixed = false;
    d->vendor_g0_frames = 15;
    d->gray_frame_rate = 2;
    d->gray_strength = d->gray_wave_mode = 0;
    heap_caps_free(d->gray_wave_planes);d->gray_wave_planes=NULL;
    d->vendor_skip_hold_frame = false;
    return vendor_result(d, vendor_full_run(d, frame, timing), false);
}

esp_err_t ssd1681_vendor_enable_mixed(ssd1681_handle_t d)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    if (!d->powered || d->fault || !d->vendor_profile || !d->full_done ||
        d->vendor_partial || d->analog_on || d->since_clean != 0 ||
        update_limit_reached(d, 1)) return ESP_ERR_INVALID_STATE;
    d->vendor_mixed = d->vendor_keep_power = true;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_set_mixed_frames(ssd1681_handle_t d, unsigned frames)
{
    if (!d || (frames != 4 && frames != 2 && frames != 1)) return ESP_ERR_INVALID_ARG;
    if (!d->powered || d->fault || !d->vendor_profile || !d->full_done ||
        !d->vendor_mixed || d->vendor_partial || d->analog_on ||
        d->since_clean != 0 || update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    d->vendor_skip_hold_frame = frames == 1;
    d->vendor_g0_frames = frames == 1 ? 0 : frames - 2;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_set_fast_transport(ssd1681_handle_t d, bool enabled)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    if (!d->powered || d->fault || !d->vendor_profile || !d->full_done ||
        !d->vendor_mixed ||
        d->vendor_partial || d->analog_on ||
        update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    d->fast_transport = enabled;
    return ESP_OK;
}
esp_err_t ssd1681_vendor_gray_frame_rate(ssd1681_handle_t d, unsigned code)
{
    if (!d || code > 7) return ESP_ERR_INVALID_ARG;
    if ((d->gray_strength || d->gray_wave_mode) && code != 7) return ESP_ERR_INVALID_STATE;
    if (!d->powered || d->fault || !d->full_done || !d->vendor_profile ||
        !d->vendor_mixed ||
        !d->vendor_skip_hold_frame || d->analog_on ||
        clean_limit_reached(d) || update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    d->gray_frame_rate = code;
    /* An existing partial session can change its volatile LUT while analog
     * power is off. Both RAM masks must be supplied by the next mixed scan. */
    if (d->vendor_partial) {
        esp_err_t error = vendor_load(d, true);
        if (error) return vendor_result(d,error,true);
    }
    return ESP_OK;
}
esp_err_t ssd1681_vendor_gray_wave_mode(ssd1681_handle_t d, unsigned mode)
{
    if (!d || mode>=SSD1681_GRAY_WAVE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!d->powered || d->fault || !d->full_done || !d->vendor_profile ||
        !d->vendor_mixed ||
        !d->vendor_skip_hold_frame || d->gray_frame_rate!=7 || d->analog_on ||
        clean_limit_reached(d) || update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    if(mode>=SSD1681_GRAY_WAVE_WEAK11 && !d->gray_wave_planes) {
        d->gray_wave_planes=heap_caps_calloc(2,SSD1681_FRAME_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
        if(!d->gray_wave_planes) return ESP_ERR_NO_MEM;
    }
    d->gray_wave_mode=mode;
    if(mode<SSD1681_GRAY_WAVE_WEAK11) { heap_caps_free(d->gray_wave_planes);d->gray_wave_planes=NULL; }
    if (d->vendor_partial) {
        esp_err_t error=vendor_load(d,true);
        if (error) return vendor_result(d,error,true);
    }
    return ESP_OK;
}

esp_err_t ssd1681_vendor_gray_strength(ssd1681_handle_t d, unsigned preset)
{
    if (!d || preset > 3) return ESP_ERR_INVALID_ARG;
    if (!d->powered || d->fault || !d->full_done || !d->vendor_profile ||
        !d->vendor_mixed ||
        !d->vendor_skip_hold_frame || d->gray_frame_rate != 7 || d->analog_on ||
        clean_limit_reached(d) || update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    d->gray_strength = preset;
    if (d->vendor_partial) {
        esp_err_t error = vendor_load(d, true);
        if (error) return vendor_result(d,error,true);
    }
    return ESP_OK;
}
esp_err_t ssd1681_vendor_mixed_resume(ssd1681_handle_t d, ssd1681_timing_t *timing)
{
    if (!d || !timing) return ESP_ERR_INVALID_ARG;
    *timing = (ssd1681_timing_t){0};
    if (!d->powered || d->fault || !d->vendor_profile || !d->full_done ||
        !d->vendor_mixed || !d->vendor_keep_power || !d->vendor_partial ||
        clean_limit_reached(d) || update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    if (d->analog_on) return ESP_OK;
    esp_err_t error = activate(d, 0xc0, timing);
    if (error != ESP_OK) return vendor_result(d, error, true);
    d->analog_on = true;
    return ESP_OK;
}

typedef struct { int16_t first, last; } row_span_t; /* first > last: no nonzero row */
typedef struct { row_span_t rows[2]; uint32_t hash; } mask_scan_t;
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "mask words are read little-endian");

/* Words straddle 25-byte rows, so the exact row comes from the first/last nonzero byte. */
static row_span_t nonzero_rows(const uint8_t *mask, int first_word, int last_word)
{
    if (first_word < 0) return (row_span_t){SSD1681_HEIGHT, -1};
    uint32_t a, b;
    memcpy(&a, mask + 4 * first_word, 4); memcpy(&b, mask + 4 * last_word, 4);
    return (row_span_t){(int16_t)((4 * first_word + __builtin_ctz(a) / 8) / ROW_BYTES),
                        (int16_t)((4 * last_word + (31 - __builtin_clz(b)) / 8) / ROW_BYTES)};
}

/* One pass: safety checks, weak-mode planes (special only), nonzero rows and benchmark hash.
 * Writes nothing to the panel; r/bw may be partly rewritten when the masks are rejected. */
static inline __attribute__((always_inline)) esp_err_t mask_scan(struct ssd1681_device *d,
    const uint8_t *black, const uint8_t *white, const uint8_t *special,
    uint8_t *r, uint8_t *bw, mask_scan_t *out)
{
    int first[2] = {-1, -1}, last[2] = {-1, -1};
    unsigned driven = 0;
    (void)d;
    const uint32_t hash = 0;
    for (int word = 0; word < SSD1681_FRAME_BYTES / 4; ++word) {
        const size_t i = 4 * (size_t)word;
        uint32_t b, w, s = 0;
        memcpy(&b, black + i, 4); memcpy(&w, white + i, 4);
        if (special) memcpy(&s, special + i, 4);
        if ((b & w) || ((b | w) & s)) return ESP_ERR_INVALID_ARG;
        driven += __builtin_popcount(b | w | s);
        const uint32_t plane[2] = {w | s, b | s};
        if (special) { memcpy(bw + i, &plane[0], 4); memcpy(r + i, &plane[1], 4); }
        for (unsigned p = 0; p < 2; ++p)
            if (plane[p]) { if (first[p] < 0) first[p] = word; last[p] = word; }
    }
    if (driven > SSD1681_MIXED_MAX_DRIVEN_PIXELS) return ESP_ERR_INVALID_ARG;
    out->rows[0] = nonzero_rows(special ? bw : white, first[0], last[0]);
    out->rows[1] = nonzero_rows(special ? r : black, first[1], last[1]);
    out->hash = hash;
    return ESP_OK;
}

/* Both planes get one span: rows nonzero in either new mask or either plane's RAM. Every other
 * row is zero in both masks and both planes, even if the controller copied one plane over the other. */
static esp_err_t mixed_planes(struct ssd1681_device *d, const uint8_t *black, const uint8_t *white,
                              const mask_scan_t *scan, uint32_t *bytes)
{
    const row_span_t *r = scan->rows;
    int used_first = r[0].first < r[1].first ? r[0].first : r[1].first;
    int used_last = r[0].last > r[1].last ? r[0].last : r[1].last;
    int first = used_first, last = used_last;
    if (!d->ram_known) { first = 0; last = SSD1681_HEIGHT - 1; }
    else {
        if (d->ram_first < first) first = d->ram_first;
        if (d->ram_last > last) last = d->ram_last;
    }
    if (first > last) return ESP_OK;
    /* Four-row steps keep the DMA address and length word aligned, avoiding IDF's bounce copy. */
    first &= ~3; last |= 3;
    const unsigned rows = (unsigned)(last - first + 1);
    d->ram_known = false;
    ESP_RETURN_ON_ERROR(plane_upload(d, black, (unsigned)first, rows, true), TAG, "mixed R mask");
    ESP_RETURN_ON_ERROR(plane_upload(d, white, (unsigned)first, rows, false), TAG, "mixed BW mask");
    d->ram_known = true;
    d->ram_first = (int16_t)used_first; d->ram_last = (int16_t)used_last;
    *bytes += 2 * rows * ROW_BYTES;
    return ESP_OK;
}

static esp_err_t vendor_mixed_run(struct ssd1681_device *d, const uint8_t *black,
                                 const uint8_t *white, const mask_scan_t *scan,
                                 ssd1681_timing_t *timing)
{
    ESP_RETURN_ON_ERROR(vendor_partial_prepare(d), TAG, "mixed prepare");
    if (!d->analog_on) return ESP_ERR_INVALID_STATE;
    const int64_t start = esp_timer_get_time();
    uint32_t bytes = 0;
    ESP_RETURN_ON_ERROR(mixed_planes(d, black, white, scan, &bytes), TAG, "mixed masks");
    const int64_t upload_us = esp_timer_get_time() - start;
    esp_err_t error = activate(d, 0x0c, timing);
    timing->upload_us = upload_us;
    timing->upload_bytes = bytes;
    return error;
}

esp_err_t ssd1681_vendor_gray_wave_scan(ssd1681_handle_t d,
    const uint8_t *black, const uint8_t *white, const uint8_t *special,
    size_t length, ssd1681_timing_t *timing)
{
    if (!d || !black || !white || !special || !timing || length!=SSD1681_FRAME_BYTES)
        return ESP_ERR_INVALID_ARG;
    *timing=(ssd1681_timing_t){0};
    if (!d->powered || d->fault || !d->vendor_profile || !d->full_done ||
        !d->vendor_mixed || !d->vendor_keep_power ||
        d->gray_frame_rate!=7 || !d->gray_wave_planes || d->gray_wave_mode<SSD1681_GRAY_WAVE_WEAK11 ||
        d->gray_wave_mode>=SSD1681_GRAY_WAVE_COUNT ||
        (d->vendor_partial && !d->analog_on) ||
        clean_limit_reached(d) || update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    uint8_t *r=d->gray_wave_planes,*bw=r+SSD1681_FRAME_BYTES;
    mask_scan_t scan;
    esp_err_t error=mask_scan(d,black,white,special,r,bw,&scan);
    if (error) return error;
    increment_saturated(&d->updates);
    return vendor_result(d,vendor_mixed_run(d,r,bw,&scan,timing),true);
}

void ssd1681_vendor_mixed_upload_all_next(ssd1681_handle_t d)
{
    if (d) d->ram_known = false;
}

esp_err_t ssd1681_vendor_mixed_scan(ssd1681_handle_t d, const uint8_t *black,
                                  const uint8_t *white, size_t length,
                                  ssd1681_timing_t *timing)
{
    if (!d || !black || !white || !timing || length != SSD1681_FRAME_BYTES)
        return ESP_ERR_INVALID_ARG;
    *timing = (ssd1681_timing_t){0};
    if (!d->powered || d->fault || !d->vendor_profile || !d->full_done ||
        !d->vendor_mixed || !d->vendor_keep_power ||
        (d->vendor_partial && !d->analog_on) ||
        clean_limit_reached(d) || update_limit_reached(d, 1))
        return ESP_ERR_INVALID_STATE;
    mask_scan_t scan;
    esp_err_t error = mask_scan(d, black, white, NULL, NULL, NULL, &scan);
    if (error) return error;
    increment_saturated(&d->updates); /* Invalid masks above must not send commands or consume a slot. */
    return vendor_result(d, vendor_mixed_run(d, black, white, &scan, timing), true);
}

esp_err_t ssd1681_power_off(ssd1681_handle_t d)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;
    if (d->powered && !d->fault) {
        if (d->analog_on) {
            ssd1681_timing_t timing;
            result = ssd1681_analog_power(d, false, &timing);
        }
        if (result == ESP_OK) result = reg_byte(d, CMD_SLEEP, 0x01);
    }
    if (d->spi) {
        esp_err_t err = spi_bus_remove_device(d->spi);
        if (err == ESP_OK) d->spi = NULL;
        if (result == ESP_OK) result = err;
    }
    if (d->bus_created && !d->spi) {
        esp_err_t err = spi_bus_free(SPI2_HOST);
        if (err == ESP_OK) d->bus_created = false;
        if (result == ESP_OK) result = err;
    }
    /* Always isolate signals and cut panel power, even after BUSY/SPI failure. */
    for (size_t i = 0; i < sizeof(signal_pins) / sizeof(signal_pins[0]); ++i) {
        esp_err_t err = gpio_set_direction(signal_pins[i], GPIO_MODE_INPUT);
        if (result == ESP_OK) result = err;
    }
    esp_err_t err = gpio_set_level(PIN_PWR, 1);
    if (result == ESP_OK) result = err;
    ram_forget(d);
    d->powered = d->analog_on = d->full_done = false;
    d->vendor_profile = d->vendor_partial = false;
    d->vendor_keep_power = d->vendor_mixed = false;
    d->vendor_g0_frames = 15;
    d->gray_frame_rate = 2;
    d->gray_strength = d->gray_wave_mode = 0;
    heap_caps_free(d->gray_wave_planes);d->gray_wave_planes=NULL;
    d->vendor_skip_hold_frame = false;
    return result;
}

esp_err_t ssd1681_destroy(ssd1681_handle_t d)
{
    if (!d) return ESP_OK;
    esp_err_t result = ssd1681_power_off(d);
    if (d->isr_handler) {
        esp_err_t err = gpio_isr_handler_remove(PIN_BUSY);
        if (result == ESP_OK) result = err;
    }
    if (d->isr_service) gpio_uninstall_isr_service();
    if (d->spi) {
        esp_err_t err = spi_bus_remove_device(d->spi);
        if (result == ESP_OK) result = err;
    }
    if (d->bus_created) {
        esp_err_t err = spi_bus_free(SPI2_HOST);
        if (result == ESP_OK) result = err;
    }
    if (d->completion) vSemaphoreDelete(d->completion);
    free(d);
    return result;
}
