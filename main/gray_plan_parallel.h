#pragma once

#include <stddef.h>
#include "gray_plan.h"

/* Optional parallel worker. Init may fail when internal RAM is tight, and every planner call then
 * uses the serial path. Call init/deinit on the display task when entering/leaving grayscale. */
bool gray_plan_parallel_init(void);
#ifndef ESP_PLATFORM
/* Lets host tests exercise the serial fallback by making init fail. */
extern bool gray_plan_parallel_host_init_fails;
#endif
void gray_plan_parallel_deinit(void);
/* Zero when inactive; otherwise the context size (worker stack is separate). */
size_t gray_plan_parallel_acceleration_bytes(void);
bool gray_plan_parallel_active(void);

void gray_plan_parallel_seed(gray_plan_t *p, const uint8_t *binary);
void gray_plan_parallel_target(gray_plan_t *p, const uint8_t *binary, const uint8_t *game);
unsigned gray_plan_parallel_masks(const gray_plan_t *p, uint8_t *black, uint8_t *white);
unsigned gray_plan_parallel_prepare(gray_plan_t *p, const uint8_t *binary, const uint8_t *game,
                             uint8_t *black, uint8_t *white);
unsigned gray_plan_parallel_prepare_pair(gray_plan_t *p, const uint8_t *binary, const uint8_t *game,
                                  uint8_t *black, uint8_t *white, uint8_t *follow_black,
                                  uint8_t *follow_white, unsigned hold_mask);
void gray_plan_parallel_complete(gray_plan_t *p, const uint8_t *black, const uint8_t *white);
void gray_plan_parallel_complete_pair(gray_plan_t *p, const uint8_t *black, const uint8_t *white,
                               const uint8_t *follow_black, const uint8_t *follow_white);
/* False (nothing started) when the core-0 worker is missing or cannot be preempted by the caller.
 * Until join, keep the masks unchanged and do not read plan pixel/active/history/pending. */
bool gray_plan_parallel_complete_begin(gray_plan_t *p, const uint8_t *black, const uint8_t *white,
                                       const uint8_t *follow_black, const uint8_t *follow_white);
void gray_plan_parallel_complete_join(void);
