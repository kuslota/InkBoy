#include "game_session.h"
#include "display_config.h"
#include "fast_display.h"
#include "game_audio.h"
#include "game_storage.h"
#include "game_view.h"
#include "gb_emulator.h"
#include "input.h"
#include "latest_frame.h"
#include "menu.h"
#include "player_fps.h"
#include "screen.h"
#include "screen_draw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "player";
static latest_frame_t completed_frame;
static _Atomic bool publish_completed;
static game_entry_t current_game;
static game_entry_t games[GAME_CATALOG_MAX];
static uint8_t *current_rom;
static bool game_ready, state_dirty;
static size_t current_rom_size;
static uint32_t current_rom_crc;
static SemaphoreHandle_t core_lock;
static _Atomic bool paused = true;
static uint32_t emulated_frames;
static size_t save_size;
/* The writer owns this buffer while a save is in flight.  The emulator task
 * only touches it while holding core_lock, then SD I/O runs without that lock. */
static uint8_t *save_worker_buffer;
static size_t save_worker_size;
static bool save_worker_pending;
static SemaphoreHandle_t save_worker_lock;
static TaskHandle_t save_worker_task;
static uint32_t save_worker_issued, save_worker_completed;
static bool save_worker_requested;
static esp_err_t save_worker_result;
static TaskHandle_t emulator_task;

static unsigned convert_image(gb_video_t *v, const uint8_t *p, uint8_t *out)
{
    const display_config_t config = display_config_get();
    if (config.border == BORDER_FILL) {
        const uint8_t *scaled = game_view_scale(p);
        if (config.mode == DISPLAY_GRAYSCALE) {
            fast_display_set_gray_game_view(scaled, GAME_VIEW_FILL);
            return game_view_prepare_gray(scaled, out);
        }
        fast_display_set_gray_game(NULL);
        return game_view_convert_fast(scaled, out);
    }
    if (config.mode == DISPLAY_GRAYSCALE) {
        fast_display_set_gray_game(p);
        return gb_video_prepare_gray(v, p, out);
    }
    fast_display_set_gray_game(NULL);
    return gb_video_convert_fast(v, p, out);
}

static void emulate(void *unused)
{
    (void)unused;
    int64_t epoch = esp_timer_get_time(), cpu = 0, last_log = epoch;
    uint32_t session_frames = 0, log_frames = 0;
    bool io_reported = false;
    int16_t audio_frame[576];
    for (;;) {
        if (screen_failed()) break;
        if (atomic_load(&paused)) {
            /* Block until a session unpauses us instead of polling the flag
             * 200 times a second for the whole time the player sits in menus.
             * Notifications latch, so a wake issued just before we block is
             * consumed here and not lost; a stale one costs one extra loop. */
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            epoch = esp_timer_get_time(); session_frames = 0;
            last_log = epoch; log_frames = 0; cpu = 0;
            io_reported = false;
            continue;
        }
        int64_t start = esp_timer_get_time();
        xSemaphoreTake(core_lock, portMAX_DELAY);
        if (atomic_load(&paused)) { xSemaphoreGive(core_lock); continue; }
        gb_emulator_buttons(input_buttons());
        bool ok = gb_emulator_frame(false);
        size_t audio_count = ok ? gb_emulator_audio_samples(audio_frame, 576) : 0;
        gb_emulator_diagnostics_t diagnostic = gb_emulator_diagnostics();
        ++emulated_frames;
        if (ok && atomic_load(&publish_completed))
            latest_frame_publish(&completed_frame, gb_emulator_pixels());
        xSemaphoreGive(core_lock);
        if (!ok) {
            ESP_LOGE(TAG, "PLAYER_CORE_ERROR,code,%d,argument,0x%04x,pc,0x%04x",
                     diagnostic.error, diagnostic.error_argument, diagnostic.error_pc);
            screen_fail(); break;
        }
        if (audio_count) game_audio_submit(audio_frame, audio_count);
        if (!io_reported && (diagnostic.ignored_io_reads || diagnostic.ignored_io_writes)) {
            ESP_LOGI(TAG, "PLAYER_UNUSED_IO,address,0x%04x,pc,0x%04x,reads,%" PRIu32
                         ",writes,%" PRIu32,
                     diagnostic.first_ignored_io_address, diagnostic.first_ignored_io_pc,
                     diagnostic.ignored_io_reads, diagnostic.ignored_io_writes);
            io_reported = true;
        }
        cpu += esp_timer_get_time()-start;
        ++session_frames; ++log_frames;
        int64_t remaining = epoch + (int64_t)gb_frame_time_us(session_frames) - esp_timer_get_time();
        if (remaining > 0) vTaskDelay(pdMS_TO_TICKS((remaining+999)/1000));
        else vTaskDelay(1); /* A slow ROM must still let the idle/watchdog task run. */
        int64_t now = esp_timer_get_time();
        if (now-last_log >= 5000000) {
            ESP_LOGI(TAG, "PLAYER_CLOCK,frames,%" PRIu32 ",elapsed_us,%" PRId64 ",cpu_us,%" PRId64
                          ",total_frames,%" PRIu32 ",reports,%" PRIu32,
                          log_frames, now-last_log, cpu, emulated_frames, input_report_count());
            last_log = now; log_frames = 0; cpu = 0;
        }
    }
    ESP_LOGE(TAG, "Emulator stopped, core error %d", gb_emulator_error());
    vTaskDelete(NULL);
}

static void snapshot(void)
{
    uint8_t *const packed = screen_packed_image();
    if (atomic_load(&publish_completed) && !atomic_load(&paused)) {
        latest_frame_copy(&completed_frame, packed);
        return;
    }
    xSemaphoreTake(core_lock, portMAX_DELAY);
    memcpy(packed, gb_emulator_pixels(), GB_PACKED_BYTES);
    xSemaphoreGive(core_lock);
}

static void pause_core(void)
{
    atomic_store(&paused, true);
    game_audio_set_playing(false);
    xSemaphoreTake(core_lock, portMAX_DELAY);
    atomic_store(&publish_completed, false);
    xSemaphoreGive(core_lock);
}

static void start_completed_frames(void)
{
    /* Seed even after a ROM change so no old cartridge image can leak into the first live snapshot. */
    xSemaphoreTake(core_lock, portMAX_DELAY);
    latest_frame_seed(&completed_frame, gb_emulator_pixels());
    atomic_store(&publish_completed, true);
    xSemaphoreGive(core_lock);
}

/* Snapshots battery RAM under core_lock, then writes from its own buffer so slow FAT I/O never
 * blocks the emulator. Failed snapshots stay pending; ROM changes and unmounts drain it first. */
static bool save_worker_claim(uint32_t *sequence)
{
    xSemaphoreTake(save_worker_lock, portMAX_DELAY);
    if (!save_worker_requested) {
        xSemaphoreGive(save_worker_lock);
        return false; /* A coalesced request left a stale task notification. */
    }
    *sequence = save_worker_issued;
    save_worker_requested = false;
    xSemaphoreGive(save_worker_lock);
    return true;
}

static esp_err_t save_worker_process(void)
{
    esp_err_t result = ESP_OK;
    if (save_size && (!save_worker_buffer || save_worker_size != save_size))
        return ESP_ERR_INVALID_STATE;
    if (save_size) {
        xSemaphoreTake(core_lock, portMAX_DELAY);
        if (gb_emulator_save_dirty()) {
            if (gb_emulator_save_copy(save_worker_buffer, save_size)) {
                gb_emulator_save_clean();
                save_worker_pending = true;
            } else result = ESP_FAIL;
        }
        xSemaphoreGive(core_lock);
    }
    if (result == ESP_OK && save_worker_pending) {
        int64_t started = esp_timer_get_time();
        result = game_storage_save(&current_game, save_worker_buffer, save_worker_size);
        if (result == ESP_OK) save_worker_pending = false;
        else ESP_LOGE(TAG, "SAVE_FAILED: retaining snapshot for retry");
        ESP_LOGI(TAG, "SAVE_ASYNC,result,%d,bytes,%zu,elapsed_us,%" PRId64,
                 result, save_worker_size, esp_timer_get_time()-started);
    }
    return result;
}

static void save_worker(void *unused)
{
    (void)unused;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t sequence;
        if (!save_worker_claim(&sequence)) continue;

        esp_err_t result = save_worker_process();
        xSemaphoreTake(save_worker_lock, portMAX_DELAY);
        save_worker_result = result;
        save_worker_completed = sequence;
        xSemaphoreGive(save_worker_lock);
    }
}

static bool save_worker_start(void)
{
    save_worker_lock = xSemaphoreCreateMutex();
    return save_worker_lock &&
        xTaskCreate(save_worker, "game_save", 4096, NULL, 1, &save_worker_task) == pdPASS;
}

/* Called only after ensure_saved() drained the previous game's writer. Takes
 * ownership of the preallocated per-ROM snapshot buffer. */
static void save_worker_configure(uint8_t *buffer, size_t size)
{
    uint8_t *old = save_worker_buffer;
    save_worker_buffer = buffer;
    save_worker_size = size;
    save_worker_pending = false;
    if (old) heap_caps_free(old);
}

static bool save_worker_finished(uint32_t sequence, bool *success)
{
    xSemaphoreTake(save_worker_lock, portMAX_DELAY);
    bool done = save_worker_completed >= sequence;
    if (done) *success = save_worker_result == ESP_OK;
    xSemaphoreGive(save_worker_lock);
    return done;
}

static bool save_worker_request(bool wait)
{
    if (!save_worker_task || !save_worker_lock) return false;
    xSemaphoreTake(save_worker_lock, portMAX_DELAY);
    uint32_t sequence = ++save_worker_issued;
    save_worker_requested = true;
    xSemaphoreGive(save_worker_lock);
    xTaskNotifyGive(save_worker_task);
    if (!wait) return true;
    for (;;) {
        bool success;
        if (save_worker_finished(sequence, &success)) return success;
        vTaskDelay(1);
    }
}

static bool persist(void)
{
    /* Also drain harmless no-battery requests before reusing game/card state. */
    return save_worker_request(true);
}

/* Keep the panel context so returning Home does not clean twice. */
unsigned game_session_play(void)
{
    uint8_t *const panel = screen_panel(), *const packed = screen_packed_image();
    gb_video_t *const video = screen_video();
    const display_config_t config = display_config_get();
    /* IDF main=1, Bluetooth host=19/20, timer=22, controller=23. Brief CPU work
     * outranks background host work; BUSY waits yield. Restore on every exit. */
    const UBaseType_t caller_priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 20);
    start_completed_frames();
    ssd1681_handle_t d = NULL;
    esp_err_t error = ssd1681_create(&d);
    if (!error) error = ssd1681_enable_continuous_play(d);
    unsigned reason = 1, custom = 0, activations = 0, settling_left = 0;
    bool analog_on = false;
    int64_t started = esp_timer_get_time(), last_change = started, last_save = started;
    int64_t next_image = 0;
    screen_draw_hud(panel, config.border);
    if (config.show_fps) screen_draw_fps(panel,0,config.border);
    memset(video, 0, sizeof(*video));
    game_view_reset();
    snapshot(); convert_image(video, packed, panel);
    if (error == ESP_OK) error = ssd1681_power_on(d);
    if (error == ESP_OK) { error = screen_full_refresh(d); ++activations; }
    player_fps_t fps;
    player_fps_reset(&fps,esp_timer_get_time());
    state_dirty = true;
    game_audio_set_playing(error == ESP_OK);
    input_set_gameplay(true);
    atomic_store(&paused, false);
    if (emulator_task) xTaskNotifyGive(emulator_task); /* Wake the blocked core. */
    while (error == ESP_OK && !screen_failed()) {
        if (input_hardware_event()) { reason = 1; break; }
        if (!input_connected()) { reason = 3; break; }
        /* Pace electrical work before sampling, so a waiting drive never holds
         * an unnecessarily old snapshot. */
        fast_display_wait_slot(d, next_image);
        int64_t now = esp_timer_get_time();
        if (save_size && now-last_save >= 60000000) {
            /* Queue/coalesce the periodic write; never stall the display loop. */
            if (!save_worker_request(false)) ESP_LOGW(TAG, "SAVE_QUEUE_FAILED");
            last_save = now;
        }
        now = esp_timer_get_time();
        unsigned rows = 0;
        if (now >= next_image) {
            snapshot();
            rows = convert_image(video, packed, panel);
            ESP_LOGI(TAG, "PLAYER_%s_SAMPLE,due_us,%" PRId64 ",start_us,%" PRId64 ",ready_us,%" PRId64,
                     fast_display_drive_name(config.mode), next_image, now, esp_timer_get_time());
            next_image = fast_display_next_image(next_image, now);
            ESP_LOGI(TAG, "PLAYER_%s_IMAGE,mode,%u,rows,%u,time_us,%" PRId64,
                     fast_display_drive_name(config.mode), config.mode, rows, now);
        }
        bool fps_changed=config.show_fps && player_fps_poll(&fps,esp_timer_get_time());
        if (fps_changed) screen_draw_fps(panel,fps.tenths,config.border);
        if (rows || fps_changed) last_change = now;
        /* Pending pixels drive the scheduler. Always submit the latest image:
         * a reversal replaces only its own budget. */
        settling_left = fast_display_pending(d);
        if (rows || fps_changed || settling_left) {
            error = fast_display_update(d, panel);
            unsigned attempts = screen_partial_activations(); activations += attempts; custom += attempts;
            if (error) break;
            if (rows && config.show_fps) player_fps_presented(&fps);
            analog_on = true;
            settling_left = fast_display_pending(d);
            if (!settling_left)
                ESP_LOGI(TAG, "PLAYER_%s_SETTLED,custom_total,%u", fast_display_drive_name(config.mode), custom);
        }
        /* Idle shutdown does not refresh or change the image. Keep both RAM
         * planes and the loaded LUT so motion can resume with analog startup. */
        if (!rows && !settling_left && analog_on && now-last_change >= 750000) {
            ssd1681_timing_t timing;
            error = ssd1681_analog_power(d, false, &timing);
            analog_on = false;
        }
    }
    input_set_gameplay(false);
    pause_core();
    if (error == ESP_OK && !screen_failed()) {
        if (screen_take_display(d, activations, analog_on)) d = NULL;
        else error = fast_display_finish(d, panel);
    }
    esp_err_t off = ssd1681_destroy(d);
    if (error == ESP_OK) error = off;
    if (error != ESP_OK) screen_fail();
    ESP_LOGI(TAG, "PLAYER_SESSION_END,reason,%u,custom,%u,activations,%u,error,%d", reason, custom, activations, error);
    vTaskPrioritySet(NULL, caller_priority);
    return reason;
}

/* Checkpoints are copied only while the emulator is paused. SD I/O never holds
 * core_lock. A failed write keeps the live state dirty for a later retry. */
static bool save_checkpoint(void)
{
    if (!game_ready || !state_dirty) return true;
    size_t size = gb_emulator_state_size();
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return false;
    xSemaphoreTake(core_lock, portMAX_DELAY);
    bool copied = gb_emulator_state_copy(data, size);
    xSemaphoreGive(core_lock);
    esp_err_t error = copied ? game_storage_save_state(&current_game, current_rom_crc,
        current_rom_size, data, size) : ESP_FAIL;
    heap_caps_free(data);
    if (error) { ESP_LOGE(TAG, "STATE_SAVE_FAILED,error,%d", error); return false; }
    state_dirty = false;
    ESP_LOGI(TAG, "STATE_SAVED,game,%s,bytes,%zu", current_game.name, size);
    return true;
}

bool game_session_load_checkpoint(void)
{
    /* A queued battery-RAM snapshot must finish before state restore changes
     * the emulator or a later game/storage operation reuses its identity. */
    if (!persist()) {
        menu_notice("SAVE ERROR", "PROGRESS IS STILL IN MEMORY", "CHECK CARD / RETRY FROM HOME");
        return false;
    }
    size_t size = gb_emulator_state_size();
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t error = data ? game_storage_load_state(&current_game, current_rom_crc,
        current_rom_size, data, size) : ESP_ERR_NO_MEM;
    if (!error) {
        xSemaphoreTake(core_lock, portMAX_DELAY);
        if (!gb_emulator_state_load(data, size)) error = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(core_lock);
    }
    heap_caps_free(data);
    if (error) {
        menu_notice("LOAD ERROR", error == ESP_ERR_NOT_FOUND ? "NO SAVED STATE FOR THIS GAME" :
               "SAVED STATE COULD NOT BE LOADED", "CURRENT GAME KEPT");
        return false;
    }
    state_dirty = false;
    ESP_LOGI(TAG, "STATE_LOADED,game,%s,bytes,%zu", current_game.name, size);
    return true;
}

bool game_session_ensure_saved(void)
{
    if (!game_ready || (persist() && save_checkpoint())) return true;
    menu_notice("SAVE ERROR", "PROGRESS IS STILL IN MEMORY", "CHECK CARD / CHOOSE AGAIN TO RETRY");
    return false;
}

static bool load_game(const game_entry_t *entry)
{
    if (!game_session_ensure_saved()) return false;
    uint8_t *rom = NULL, *candidate_save = NULL, *candidate_snapshot = NULL;
    size_t rom_size = 0;
    esp_err_t error = game_storage_read(entry, &rom, &rom_size);
    if (error) {
        menu_notice("ROM ERROR", error == ESP_ERR_NOT_SUPPORTED ? "UNSUPPORTED OR INVALID CARTRIDGE" : "CANNOT READ ROM FROM CARD", "DMG / DUAL MODE UP TO 2 MB");
        return false;
    }
    size_t candidate_size = gb_emulator_rom_save_size(rom, rom_size);
    bool have_save = false;
    size_t loaded_size = 0;
    if (candidate_size) {
        candidate_save = heap_caps_malloc(candidate_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        candidate_snapshot = heap_caps_malloc(candidate_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        error = candidate_save && candidate_snapshot ?
            game_storage_load_save(entry, rom, rom_size, candidate_save, candidate_size, &loaded_size) : ESP_ERR_NO_MEM;
        have_save = error == ESP_OK;
        if (error && error != ESP_ERR_NOT_FOUND) {
            heap_caps_free(candidate_save); heap_caps_free(candidate_snapshot); heap_caps_free(rom);
            menu_notice("SAVE ERROR", "EXISTING SAVE IS UNREADABLE", "SAVE FILE WAS NOT OVERWRITTEN");
            return false;
        }
    }
    /* ROM/save preflight finished with the previous paused game still intact. */
    xSemaphoreTake(core_lock, portMAX_DELAY);
    bool ok = gb_emulator_init(rom, rom_size);
    if (ok) ESP_LOGI(TAG, "PLAYER_GRAPHICS,ram,%s", esp_ptr_external_ram(gb_emulator_pixels()) ? "external" : "internal");
    if (ok) ok = gb_emulator_save_size() == candidate_size;
    if (ok && have_save) ok = gb_emulator_save_load(candidate_save, loaded_size);
    emulated_frames = 0;
    xSemaphoreGive(core_lock);
    heap_caps_free(current_rom); heap_caps_free(candidate_save);
    current_rom = rom; save_size = candidate_size;
    save_worker_configure(candidate_snapshot, candidate_size);
    current_game = *entry; game_ready = ok;
    current_rom_size = rom_size; current_rom_crc = game_storage_crc32(rom, rom_size);
    state_dirty = false;
    if (!ok) { menu_notice("ROM ERROR", "CARTRIDGE COULD NOT START", "CHOOSE ANOTHER GAME"); return false; }
    ESP_LOGI(TAG, "PLAYER_READY,game,%s,rom_bytes,%zu,save_bytes,%zu", entry->name, rom_size, save_size);
    return true;
}

bool game_session_select(void)
{
    size_t count = 0; bool truncated = false;
    esp_err_t error = game_storage_list(games, GAME_CATALOG_MAX, &count, &truncated);
    if (error || !count) {
        menu_notice(error ? "CARD ERROR" : "NO GAMES", "PUT GB FILES IN CARD /ROMS", "POWER OFF BEFORE REMOVING CARD");
        return false;
    }
    const char *items[GAME_CATALOG_MAX + 1];
    items[0] = MENU_BACK;
    for (size_t i = 0; i < count; ++i) items[i + 1] = games[i].name;
    unsigned selected = 1;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(current_game.name, games[i].name)) selected = (unsigned)i + 1;
    for (;;) {
        int choice = menu_choose("GAMES", truncated ? "FIRST 64 FILES /ROMS" : "SD CARD /ROMS", items, count + 1, &selected, NULL);
        if (choice <= 0) return false;
        if (!input_wait_controller()) return false;
        if (!input_mapping_valid() && !input_calibrate()) return false;
        if (load_game(&games[choice - 1])) {
            bool exists = false;
            error = game_storage_has_state(&current_game, current_rom_crc,
                current_rom_size, gb_emulator_state_size(), &exists);
            if (!error && !exists) return true;
            const char *resume_items[] = {"CONTINUE SAVED", "START NEW", MENU_BACK};
            const char *invalid_items[] = {MENU_BACK, "START NEW"};
            unsigned resume_selected = 0;
            if (error) {
                int action = menu_choose("SAVE UNAVAILABLE", "OLD FILE KEPT UNTIL YOU PLAY",
                                  invalid_items, 2, &resume_selected, NULL);
                return action == 1;
            }
            int action = menu_choose("SAVED GAME", current_game.name, resume_items, 3,
                              &resume_selected, NULL);
            if (action == 0) return game_session_load_checkpoint();
            return action == 1;
        }
        if (screen_failed()) return false;
    }
}

void game_session_save_state(void)
{
    state_dirty = true;
    if (game_session_ensure_saved()) menu_notice("GAME SAVED", "CHECKPOINT SAVED TO SD CARD", current_game.name);
}

bool game_session_ready(void) { return game_ready; }
const char *game_session_name(void) { return current_game.name; }

bool game_session_start(void)
{
    core_lock = xSemaphoreCreateMutex();
    if (core_lock && save_worker_start() &&
        xTaskCreatePinnedToCore(emulate, "gb_player", 8192, NULL, 4, &emulator_task, 1) == pdPASS) return true;
    ESP_LOGE(TAG, "PLAYER_START_FAILED,internal_free,%u,internal_largest,%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return false;
}
