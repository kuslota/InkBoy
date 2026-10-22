#include "fast_display.h"
#include "drive_counters.h"
#include "gray_plan.h"
#include "gray_plan_parallel.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <inttypes.h>

static drive_counters_t *drive_counters;
static unsigned dither_black = DITHER_PULSES_DEFAULT, dither_white = DITHER_PULSES_DEFAULT;

esp_err_t fast_display_set_dither_pulses(unsigned black_pulses, unsigned white_pulses)
{
    if (!black_pulses || black_pulses > DITHER_PULSES_MAX || !white_pulses || white_pulses > DITHER_PULSES_MAX)
        return ESP_ERR_INVALID_ARG;
    dither_black = black_pulses; dither_white = white_pulses;
    return ESP_OK;
}

static gray_plan_t *gray_plan;
static bool gray_parallel_active;
static gray_config_t gray_settings = GRAY_CONFIG_DEFAULT;
static const uint8_t *gray_game;
static game_view_t gray_view = GAME_VIEW_CLASSIC;
static int64_t gray_costs[8];
static unsigned gray_cost_next;
static unsigned gray_strength = GRAY_STRENGTH_DEFAULT, gray_applied_strength;
static bool gray_immediate = GRAY_IMMEDIATE_DEFAULT;
static unsigned gray_last_scans;
static unsigned gray_holds = GRAY_HOLDS_DEFAULT;
static gray_adaptive_t gray_adaptive = GRAY_ADAPTIVE_DEFAULT;
static unsigned gray_waveform_applied;
static gray_history_t *gray_history_store;
static uint8_t *gray_weak_buffers;
static uint8_t *gray_weak_black,*gray_weak_special;
static unsigned gray_effective_waveform(void) {
    return gray_adaptive.weak_wl ? SSD1681_GRAY_WAVE_WEAK11+gray_adaptive.weak_wl-1 : SSD1681_GRAY_WAVE_NORMAL;
}
static esp_err_t gray_resources(bool history,bool buffers) {
    if (history && !gray_history_store) {
        gray_history_store=heap_caps_calloc(GRAY_PIXELS,sizeof(*gray_history_store),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!gray_history_store) return ESP_ERR_NO_MEM;
        for(unsigned i=0;i<GRAY_PIXELS;++i) gray_history_store[i].origin=4;
    }
    if(buffers && !gray_weak_buffers) {
        gray_weak_buffers=heap_caps_calloc(2,SSD1681_FRAME_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!gray_weak_buffers) return ESP_ERR_NO_MEM;
        gray_weak_black=gray_weak_buffers;gray_weak_special=gray_weak_black+SSD1681_FRAME_BYTES;
    }
    return ESP_OK;
}
static void gray_attach_policy(void) {
    bool history=gray_adaptive.history_mode;
    if (gray_plan) {
        if(history && gray_plan->history!=gray_history_store)
            for(unsigned i=0;i<GRAY_PIXELS;++i) gray_history_store[i]=(gray_history_t){.origin=4};
        gray_plan->history=history ? gray_history_store : NULL;
        gray_plan->history_mode=gray_adaptive.history_mode;
        gray_plan->adaptive=gray_adaptive;
        gray_plan->adaptive_configured=true;
    }
    if(!history) {
        heap_caps_free(gray_history_store);gray_history_store=NULL;
    }
    if(!gray_adaptive.weak_wl) {
        heap_caps_free(gray_weak_buffers);gray_weak_buffers=NULL;
        gray_weak_black=gray_weak_special=NULL;
    }
}
esp_err_t fast_display_reserve_gray_adaptive(const gray_adaptive_t *config) {
    if(!gray_adaptive_valid(config)) return ESP_ERR_INVALID_ARG;
    return gray_resources(config->history_mode,config->weak_wl);
}
esp_err_t fast_display_set_gray_adaptive(const gray_adaptive_t *config) {
    esp_err_t error=fast_display_reserve_gray_adaptive(config);
    if(error) return error;
    gray_adaptive=*config;gray_attach_policy();
    return ESP_OK;
}
static DMA_ATTR uint8_t gray_follow_black[SSD1681_FRAME_BYTES], gray_follow_white[SSD1681_FRAME_BYTES];
esp_err_t fast_display_set_gray_holds(unsigned mask)
{
    if (!gray_hold_valid(mask)) return ESP_ERR_INVALID_ARG;
    gray_holds=mask;
    return ESP_OK;
}
unsigned fast_display_gray_last_scans(void) { return gray_last_scans; }
esp_err_t fast_display_set_gray_immediate(unsigned enabled)
{
    if (enabled > 1) return ESP_ERR_INVALID_ARG;
    gray_immediate = enabled != 0;
    return ESP_OK;
}
esp_err_t fast_display_set_gray_strength(unsigned preset)
{
    if (preset > 3) return ESP_ERR_INVALID_ARG;
    gray_strength = preset;
    return ESP_OK;
}
void fast_display_set_gray_game(const uint8_t *packed) {
    fast_display_set_gray_game_view(packed, GAME_VIEW_CLASSIC);
}
void fast_display_set_gray_game_view(const uint8_t *packed, game_view_t view) {
    gray_game = packed;
    gray_view = view;
    if (gray_plan) gray_plan->view = view;
}
esp_err_t fast_display_set_gray_config(const gray_config_t *config)
{
    if (!gray_config_valid(config)) return ESP_ERR_INVALID_ARG;
    gray_settings = *config;
    if (gray_plan) gray_plan->config = *config;
    return ESP_OK;
}

static DMA_ATTR uint8_t previous[SSD1681_FRAME_BYTES];
static DMA_ATTR uint8_t black[SSD1681_FRAME_BYTES], white[SSD1681_FRAME_BYTES];
static ssd1681_handle_t owner;
static display_mode_t selected;
static unsigned updates;
static bool prepared;
static int64_t last_start;
static bool hold_end_pending;
static int64_t scan_cost_us = 40000;

unsigned fast_display_pending(ssd1681_handle_t d)
{
    if (!d || d != owner) return 0;
    if (selected == DISPLAY_GRAYSCALE)
        return gray_plan ? (gray_plan->pending ? gray_plan->pending : hold_end_pending ? 1 : 0) : 0;
    if (!drive_counters) return 0;
    if (hold_end_pending && !drive_counters->pending_pixels)
        return 1; /* Physical HOLD ending still needs delivery. */
    return drive_counters->pending_pixels;
}

static void wait_drive_period(ssd1681_handle_t d)
{
    if (!d || d != owner) return;
    unsigned period = fast_display_drive_period_us(selected);
    if (selected == DISPLAY_GRAYSCALE && (gray_settings.scan_policy || gray_immediate)) period = 30000;
    const int64_t remaining = last_start + period - esp_timer_get_time();
    if (last_start && remaining > 0) vTaskDelay(pdMS_TO_TICKS((remaining + 999) / 1000));
}

void fast_display_wait_slot(ssd1681_handle_t d, int64_t image_deadline)
{
    if (!d || d != owner) return;
    /* Completion and HOLD share the game-image slots and cannot start between them.
     * FreeRTOS can wake early; recheck the instant. */
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const int64_t ready = last_start ? last_start + fast_display_drive_period_us(selected) : now;
        int64_t wake = image_deadline > ready ? image_deadline : ready;
        if (selected == DISPLAY_GRAYSCALE && !gray_settings.scan_policy && !gray_immediate)
            wake = fast_display_wake_time(now, ready, image_deadline,
                                     fast_display_pending(d) != 0, scan_cost_us);
        if (now >= wake) return;
        vTaskDelay(pdMS_TO_TICKS((wake - now + 999) / 1000));
    }
}

esp_err_t fast_display_full(ssd1681_handle_t d, const uint8_t *frame, display_mode_t mode)
{
    if (!d || !frame || mode >= DISPLAY_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    owner = NULL; prepared = false;
    const bool gray = mode == DISPLAY_GRAYSCALE;
    if (gray) {
        esp_err_t reserve=gray_resources(gray_adaptive.history_mode,gray_adaptive.weak_wl);
        if (reserve) return reserve;
    }
    /* The grayscale accelerator and dithered counters compete for scarce
     * internal RAM: free the old mode's workspace before allocating the next. */
    if (!gray && gray_parallel_active) {
        gray_plan_parallel_deinit(); gray_parallel_active=false;
        ESP_LOGI("fast_display","PLAYER_GRAY_MEMORY,freed,1,free_after,%u,largest_after,%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    if (gray) { heap_caps_free(drive_counters); drive_counters=NULL; }
    if (gray && !gray_parallel_active) {
        size_t free_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest_before=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        bool accelerated=gray_plan_parallel_init();
        gray_parallel_active=true;
        size_t free_after=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest_after=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI("fast_display","PLAYER_GRAY_MEMORY,accelerated,%u,parallel,%u,bytes,%u,free_before,%u,largest_before,%u,free_after,%u,largest_after,%u",
                 accelerated,gray_plan_parallel_active(),(unsigned)gray_plan_parallel_acceleration_bytes(),(unsigned)free_before,(unsigned)largest_before,(unsigned)free_after,(unsigned)largest_after);
    }
    hold_end_pending = false; scan_cost_us = 40000;
    if (!gray) {
        if (!drive_counters) {
            drive_counters = heap_caps_calloc(1, sizeof(*drive_counters), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            /* Audio/BLE startup can fragment internal RAM below this 25 KB allocation; use PSRAM counters
             * rather than abort before Home. Masks stay in DMA-capable SRAM. */
            bool internal=drive_counters!=NULL;
            size_t free_internal=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t largest_internal=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if (!drive_counters) drive_counters=heap_caps_calloc(1,sizeof(*drive_counters),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ESP_LOGI("fast_display","PLAYER_DRIVE_MEMORY,mode,%u,bytes,%u,internal,%u,free_internal,%u,largest_internal,%u,ready,%u",
                     mode,(unsigned)sizeof(*drive_counters),internal,(unsigned)free_internal,(unsigned)largest_internal,drive_counters!=NULL);
        }
        if (!drive_counters) return ESP_ERR_NO_MEM;
        heap_caps_free(gray_plan); gray_plan = NULL;
    } else {
        if (!gray_plan) {
            gray_plan = heap_caps_calloc(1, sizeof(*gray_plan), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            bool internal = gray_plan != NULL;
            if (!gray_plan) gray_plan = heap_caps_calloc(1, sizeof(*gray_plan), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ESP_LOGI("fast_display", "PLAYER_GRAY_MEMORY,bytes,%u,internal,%u", (unsigned)sizeof(*gray_plan), internal);
        }
        if (!gray_plan) return ESP_ERR_NO_MEM;
        scan_cost_us = 14000;
        memset(gray_costs, 0, sizeof(gray_costs)); gray_cost_next = 0;
    }
    ssd1681_timing_t timing;
    esp_err_t error = ssd1681_vendor_full(d, frame, SSD1681_FRAME_BYTES, &timing);
    if (!error) error = ssd1681_vendor_enable_mixed(d);
    if (!error) error = ssd1681_vendor_set_mixed_frames(d, 1);
    if (!error) error = ssd1681_vendor_set_fast_transport(d, true);
    if (!error && gray) error = ssd1681_vendor_gray_frame_rate(d, 7);
    if (!error && gray) error = ssd1681_vendor_gray_strength(d, gray_strength);
    if (!error && gray && gray_effective_waveform())
        error=ssd1681_vendor_gray_wave_mode(d,gray_effective_waveform());
    if (!error && gray) {
        gray_applied_strength = gray_strength;
        gray_waveform_applied=gray_effective_waveform();
        gray_plan->config = gray_settings;
        gray_plan->view = gray_view;
        gray_attach_policy();
        gray_plan_parallel_seed(gray_plan, frame);
        gray_plan_parallel_target(gray_plan, frame, gray_game);
    }
    if (error) return error;
    if (!gray) drive_counters_seed(drive_counters, frame);
    memcpy(previous, frame, sizeof(previous));
    owner = d; selected = mode; updates = 0; last_start = 0;
    ESP_LOGI("fast_display", "PLAYER_DISPLAY_FULL,mode,%u,busy_us,%" PRId64, mode, timing.busy_us);
    return ESP_OK;
}

esp_err_t fast_display_finish(ssd1681_handle_t d, const uint8_t *frame)
{
    if (!d || !frame) return ESP_ERR_INVALID_ARG;
    owner = NULL; prepared = false;
    hold_end_pending = false;
    /* The final full refresh is reserved by the lifetime cap, so preparing another fast waveform
     * can be rejected here even though cleaning succeeded. */
    ssd1681_timing_t timing = {0};
    esp_err_t error = ssd1681_vendor_full(d, frame, SSD1681_FRAME_BYTES, &timing);
    ESP_LOGI("fast_display", "PLAYER_DISPLAY_FINISH,busy_us,%" PRId64 ",error,%d", timing.busy_us, error);
    return error;
}

static esp_err_t gray_scan(ssd1681_handle_t d,const uint8_t *b,const uint8_t *w,ssd1681_timing_t *timing)
{
    if (gray_effective_waveform()<SSD1681_GRAY_WAVE_WEAK11) return ssd1681_vendor_mixed_scan(d,b,w,SSD1681_FRAME_BYTES,timing);
    /* Frozen per-transition eligibility avoids a 23k-pixel planner walk between the primary and
     * follow-up; HOLD and other black pairs keep their codes, only settled W->L is special. */
    uint32_t any=0;
    for(unsigned i=0;i<SSD1681_FRAME_BYTES;i+=sizeof(uint32_t)) {
        uint32_t black_bits,eligible;
        memcpy(&black_bits,b+i,4);memcpy(&eligible,gray_plan->weak_wl+i,4);
        uint32_t special=black_bits&eligible,ordinary=black_bits&~special;
        memcpy(gray_weak_black+i,&ordinary,4);memcpy(gray_weak_special+i,&special,4);
        any|=special;
    }
    if(!any) return ssd1681_vendor_mixed_scan(d,b,w,SSD1681_FRAME_BYTES,timing);
    return ssd1681_vendor_gray_wave_scan(d,gray_weak_black,w,gray_weak_special,SSD1681_FRAME_BYTES,timing);
}

static esp_err_t gray_update(ssd1681_handle_t d, const uint8_t *frame)
{
    gray_last_scans = 0;
    wait_drive_period(d);
    const int64_t start = esp_timer_get_time();
    bool immediate=gray_immediate;
    unsigned holds=gray_holds;
    unsigned driven = immediate ?
        gray_plan_parallel_prepare_pair(gray_plan, frame, gray_game, black, white,
                                 gray_follow_black, gray_follow_white, holds) :
        gray_plan_parallel_prepare(gray_plan, frame, gray_game, black, white);
    if (!gray_plan->pending && !hold_end_pending) return ESP_OK;
    ssd1681_timing_t timing = {0};
    const int64_t resume_start = esp_timer_get_time();
    esp_err_t error = ESP_OK;
    unsigned strength=gray_strength;
    if (gray_applied_strength != strength) {
        ESP_LOGI("fast_display", "PLAYER_GRAY_STRENGTH_BEGIN,from,%u,to,%u",
                 gray_applied_strength, strength);
        error = ssd1681_analog_power(d, false, &timing);
        ESP_LOGI("fast_display", "PLAYER_GRAY_STRENGTH_OFF,error,%d,activation_us,%" PRId64,
                 error, timing.activation_us);
        if (!error) error = ssd1681_vendor_gray_strength(d, strength);
        if (!error) {
            gray_applied_strength = strength;
            ESP_LOGI("fast_display", "PLAYER_GRAY_STRENGTH,preset,%u", strength);
        }
    }
    if (!error && gray_waveform_applied!=gray_effective_waveform()) {
        error=ssd1681_analog_power(d,false,&timing);
        if (!error) error=ssd1681_vendor_gray_wave_mode(d,gray_effective_waveform());
        if (!error) gray_waveform_applied=gray_effective_waveform();
    }
    if (!error && prepared) error = ssd1681_vendor_mixed_resume(d, &timing);
    const int64_t masks_done = esp_timer_get_time();
    if (!error) {
        ++gray_last_scans;
        error = gray_scan(d,black,white,&timing);
    }
    if (error) { owner = NULL; return error; }
    const int64_t delivered = esp_timer_get_time();
    int64_t hold_start = 0, hold_done = 0;
    if (immediate && driven) {
        /* Do not log, sleep or walk the planner between drive and HOLD. */
        ssd1681_timing_t hold_timing = {0};
        hold_start = esp_timer_get_time();
        ++gray_last_scans;
        error = gray_scan(d,gray_follow_black,gray_follow_white,&hold_timing);
        hold_done = esp_timer_get_time();
    }
    unsigned follow_driven=0;
    if (immediate && driven) {
        for (unsigned i=0;i<SSD1681_FRAME_BYTES;++i)
            follow_driven += __builtin_popcount((unsigned)(gray_follow_black[i] | gray_follow_white[i]));
    }
    if (!error && follow_driven)
        gray_plan_parallel_complete_pair(gray_plan, black, white, gray_follow_black, gray_follow_white);
    else gray_plan_parallel_complete(gray_plan, black, white);
    /* A follow-up that still drove pixels needs a subsequent HOLD ending.
     * A zero-mask follow-up has already delivered that ending. */
    hold_end_pending = driven != 0 && (!immediate || error || follow_driven);
    const int64_t end = esp_timer_get_time();
    if (immediate)
        ESP_LOGI("fast_display", "PLAYER_GRAY_PAIR,start_us,%" PRId64 ",end_us,%" PRId64
                 ",drive_done_us,%" PRId64 ",hold_gap_us,%" PRId64 ",hold_us,%" PRId64
                 ",scans,%u,driven,%u,pending,%u,error,%d,follow_driven,%u,hold_mask,%u",
                 start,end,delivered,hold_start ? hold_start-delivered : 0,
                 hold_start ? hold_done-hold_start : 0,gray_last_scans,driven,gray_plan->pending,error,
                 follow_driven,holds);
    updates += gray_last_scans;
    if (error) { owner = NULL; return error; }
    /* Exclude first preparation and idle analog startup from the steady scan
     * estimate; their pauses are logged, not presented as 30 fps samples. */
    if (prepared && masks_done-resume_start < 10000) {
        /* Recent high water mark: an expensive scene change must not disable
         * completion slots for the rest of an otherwise light session. */
        gray_costs[gray_cost_next++ % 8] = end-start+1000;
        scan_cost_us = 14000;
        for (unsigned i=0; i<8; ++i)
            if (gray_costs[i] > scan_cost_us) scan_cost_us = gray_costs[i];
    }
    ESP_LOGI("fast_display", "PLAYER_GRAY_SCAN,start_us,%" PRId64 ",end_us,%" PRId64
             ",gap_us,%" PRId64 ",prepare_us,%" PRId64 ",transport_us,%" PRId64
             ",complete_us,%" PRId64 ",busy_us,%" PRId64 ",driven,%u,pending,%u,hold,%u,immediate,%u,scans,%u",
             start, end, last_start ? start-last_start : 0, masks_done-start,
             delivered-masks_done, end-(hold_done ? hold_done : delivered), timing.busy_us,
             driven, gray_plan->pending, !driven, immediate, gray_last_scans);
    if(gray_adaptive.history_mode || gray_adaptive.weak_wl)
        ESP_LOGI("fast_display","PLAYER_GRAY_ADAPTIVE_SCAN,history,%u,weak_wl,%u,work_us,%" PRId64
                 ",returns,%u,reversals,%u",gray_adaptive.history_mode,gray_adaptive.weak_wl,end-start,
                 gray_plan->history_returns,gray_plan->history_reversals);
    prepared = true; last_start = start;
    return ESP_OK;
}

esp_err_t fast_display_update(ssd1681_handle_t d, const uint8_t *frame)
{
    if (!d || !frame) return ESP_ERR_INVALID_ARG;
    if (d != owner) return ESP_ERR_INVALID_STATE;
    if (selected == DISPLAY_GRAYSCALE) return gray_update(d, frame);
    if (!hold_end_pending && !drive_counters->pending_pixels &&
        !memcmp(previous, frame, sizeof(previous))) return ESP_OK;
    const int64_t remaining = last_start + fast_display_drive_period_us(selected) - esp_timer_get_time();
    if (last_start && remaining > 0)
        vTaskDelay(pdMS_TO_TICKS((remaining + 999) / 1000));
    const int64_t start = esp_timer_get_time();
    const bool was_prepared = prepared;
    ssd1681_timing_t timing;
    esp_err_t error = ESP_OK;
    /* No-op if already powered; after idle, resuming does not reset RAM. */
    if (prepared) error = ssd1681_vendor_mixed_resume(d, &timing);
    /* Anchor cadence after an idle power startup, so its time cannot shorten
     * the interval between the resumed image and the following image. */
    const int64_t activation_start = esp_timer_get_time();
    const int64_t gap = last_start ? activation_start - last_start : 0;
    unsigned driven = drive_counters_prepare(drive_counters, frame, dither_black, dither_white, black, white);
    const int64_t masks_done = esp_timer_get_time();
    if (!error) error = ssd1681_vendor_mixed_scan(d, black, white, sizeof(previous), &timing);
    if (error) { owner = NULL; return error; }
    const int64_t delivered = esp_timer_get_time();
    drive_counters_complete(drive_counters, black, white);
    /* Keep a successor pending until an all-HOLD mask is delivered.
     * Fresh target changes produce active masks and defer this ending. */
    hold_end_pending = driven != 0;
    if (!driven) ESP_LOGI("fast_display", "PLAYER_DITHER_HOLD_END,mode,%u", selected);
    const int64_t cost = esp_timer_get_time() - activation_start + 2000;
    if (was_prepared && cost > scan_cost_us) scan_cost_us = cost;
    ESP_LOGI("fast_display", "PLAYER_DITHER_DRIVE,mode,%u,black,%u,white,%u,driven,%u,pending_pixels,%u,prepare_us,%" PRId64 ",transport_us,%" PRId64 ",complete_us,%" PRId64,
             selected, dither_black, dither_white, driven, drive_counters->pending_pixels,
             masks_done - activation_start, delivered - masks_done, esp_timer_get_time() - delivered);
    memcpy(previous, frame, sizeof(previous));
    prepared = true; last_start = activation_start; ++updates;
    ESP_LOGI("fast_display", "PLAYER_DITHER_SCAN,start_us,%" PRId64 ",end_us,%" PRId64 ",driven,%u",
             activation_start, esp_timer_get_time(), driven);
    ESP_LOGI("fast_display", "PLAYER_DISPLAY_UPDATE,mode,%u,n,%u,driven,%u,gap_us,%" PRId64
             ",busy_us,%" PRId64 ",total_us,%" PRId64,
             selected, updates, driven, gap, timing.busy_us, esp_timer_get_time() - start);
    return ESP_OK;
}
