#pragma once
#include "display_mode.h"
#include "game_view.h"
#include "gray_adaptive.h"
#include "gray_config.h"
#include "ssd1681_fast.h"

#define DITHER_PULSES_DEFAULT 8u
#define DITHER_PULSES_MAX 8u
#define FAST_DISPLAY_IMAGE_PERIOD_US 33334u

/* Select a wake time that leaves room for a whole completion scan. A new
 * image takes priority when no completion scan fits; never queue old images. */
static inline int64_t fast_display_wake_time(int64_t now, int64_t ready, int64_t deadline,
                                     bool pending, int64_t cost) {
    int64_t earliest = now > ready ? now : ready;
    if (now >= deadline || (pending && earliest + cost <= deadline)) return earliest;
    return deadline > earliest ? deadline : earliest;
}
/* A 30 ms lower bound tolerates small per-slot preparation differences without accumulating drift. */
static inline unsigned fast_display_drive_period_us(display_mode_t mode) { return mode == DISPLAY_GRAYSCALE ? 15000 : 30000; }
static inline const char *fast_display_drive_name(display_mode_t mode) { return mode == DISPLAY_GRAYSCALE ? "GRAY" : "DITHER"; }
static inline int64_t fast_display_next_image(int64_t deadline, int64_t now) {
    if (!deadline) return now + FAST_DISPLAY_IMAGE_PERIOD_US;
    if (now < deadline) return deadline;
    /* Retain the image clock's phase; skip missed deadlines without queuing
     * old frames. A 34 ms scan loop can then admit about 20 images/s. */
    return deadline + ((now - deadline) / FAST_DISPLAY_IMAGE_PERIOD_US + 1) * FAST_DISPLAY_IMAGE_PERIOD_US;
}

/* Borrowed stable snapshot, NULL for menus. Display-task owner only. */
void fast_display_set_gray_game(const uint8_t *packed);
/* Same, for a packed image laid out as `view` instead of the classic viewport. */
void fast_display_set_gray_game_view(const uint8_t *packed, game_view_t view);
esp_err_t fast_display_set_gray_config(const gray_config_t *config);
/* Applies live without a clean or reseed. Allocation may fail. */
esp_err_t fast_display_reserve_gray_adaptive(const gray_adaptive_t *config);
esp_err_t fast_display_set_gray_adaptive(const gray_adaptive_t *config);
/* Deferred to next grayscale scan, with analog off/reload/resume, no cleaning. */
esp_err_t fast_display_set_gray_strength(unsigned preset);
/* 0 keeps existing pacing; 1 follows each drive with the per-pair HOLD masks. */
esp_err_t fast_display_set_gray_immediate(unsigned enabled);
esp_err_t fast_display_set_gray_holds(unsigned mask);
/* Physical partial attempts in the last grayscale update, including its HOLD. */
unsigned fast_display_gray_last_scans(void);

esp_err_t fast_display_set_dither_pulses(unsigned black, unsigned white);
void fast_display_wait_slot(ssd1681_handle_t d, int64_t image_deadline);
/* Nonzero while work remains: the pending pixel count, or 1 when only the
 * HOLD ending remains. Zero when settled or context invalid. */
unsigned fast_display_pending(ssd1681_handle_t d);

/* One task/display owner shared by menus and gameplay. */
esp_err_t fast_display_full(ssd1681_handle_t d, const uint8_t *frame, display_mode_t mode);
/* Final cleaning only: invalidate the fast context without preparing more scans. */
esp_err_t fast_display_finish(ssd1681_handle_t d, const uint8_t *frame);
esp_err_t fast_display_update(ssd1681_handle_t d, const uint8_t *frame);
