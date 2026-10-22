#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gray_config.h"
#include "gray_adaptive.h"
#include "game_view.h"

#define GRAY_PIXELS 40000u
/* K,D,L,W = 0,1,2,3. Level is a model, not measured reflectance. */
/* ESP32 and host test targets are little-endian. The word view batches the
 * two external-RAM byte accesses; the named fields retain their layout. */
typedef union {
    struct { uint8_t level; uint8_t remaining:4, target:2, source:2; };
    uint16_t packed;
} gray_pixel_t;
/* Optional provenance. origin=4 means unclassified/clean, not a fifth shade.
 * Counts advance only after acknowledged delivery, including paired scans. */
typedef struct { uint8_t origin, black_steps, white_steps; } gray_history_t;
typedef struct {
    gray_pixel_t pixel[GRAY_PIXELS];
    uint8_t active[GRAY_PIXELS/8], previous_binary[GRAY_PIXELS/8], previous_game[GAME_VIEW_MAX_BYTES];
    unsigned pending;
    gray_config_t config;
    bool had_game;
    /* view: layout of the `game` argument, set by the caller (zeroed = classic).
     * had_view: layout previous_game was recorded in. */
    game_view_t view, had_view;
    gray_history_t *history;
    unsigned history_mode; /* bit0 completed returns; bit1 interrupted returns */
    unsigned history_returns, history_reversals;
    gray_adaptive_t adaptive;
    bool adaptive_configured; /* false uses the fixed L>K>L defaults instead of `adaptive` */
    uint8_t weak_wl[GRAY_PIXELS/8]; /* frozen eligibility for a settled W->L */
} gray_plan_t;
_Static_assert(sizeof(gray_pixel_t) == 2, "Gray state must fit two bytes per pixel");
void gray_plan_seed(gray_plan_t *p, const uint8_t *binary);
/* Optional packed game image replaces just the p->view viewport. */
void gray_plan_target(gray_plan_t *p, const uint8_t *binary, const uint8_t *game);
unsigned gray_plan_masks(const gray_plan_t *p, uint8_t *black, uint8_t *white);
/* Commit only after successful delivery of those exact masks. */
void gray_plan_complete(gray_plan_t *p, const uint8_t *black, const uint8_t *white);

unsigned gray_plan_prepare(gray_plan_t *p, const uint8_t *binary, const uint8_t *game, uint8_t *black, uint8_t *white);
/* Predict follow-up masks before either scan. Only HOLD-Off transitions
 * with at least two remaining pulses may drive in the second scan. */
unsigned gray_plan_prepare_pair(gray_plan_t *p, const uint8_t *binary, const uint8_t *game,
                                uint8_t *black, uint8_t *white, uint8_t *follow_black,
                                uint8_t *follow_white, unsigned hold_mask);
/* Both scans succeeded: commit their doses in one traversal of optical state. */
void gray_plan_complete_pair(gray_plan_t *p, const uint8_t *black, const uint8_t *white,
                             const uint8_t *follow_black, const uint8_t *follow_white);
