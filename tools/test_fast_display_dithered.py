#!/usr/bin/env python3
"""Exercise the real dithered backend and menu presentation over a fake panel."""
from host_build import run, DISPLAY_MODULES

CODE = r'''
#include "fast_display.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display_config.h"
#include "host_fakes.h"
#include "host_panel.h"
#include "screen.h"

#define H ((ssd1681_handle_t)1)

static uint8_t model_target[40000], model_remaining[40000];
static unsigned last_driven;
static uint8_t *frame;
static unsigned black_budget, white_budget;

static void model_full(const uint8_t *p)
{
    for (unsigned i = 0; i < 40000; ++i) { model_target[i] = (p[i / 8] >> (7 - i % 8)) & 1; model_remaining[i] = 0; }
}

static void model_before(const uint8_t *black, const uint8_t *white)
{
    uint8_t decoded[5000];
    last_driven = 0;
    for (unsigned i = 0; i < 5000; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            unsigned pixel = i * 8 + bit, mask = 0x80u >> bit;
            bool target = (frame[i] & mask) != 0;
            if (target != model_target[pixel]) { model_target[pixel] = target; model_remaining[pixel] = target ? white_budget : black_budget; }
            assert(((black[i] & mask) != 0) == (model_remaining[pixel] > 0 && !target));
            assert(((white[i] & mask) != 0) == (model_remaining[pixel] > 0 && target));
        }
        last_driven += (unsigned)__builtin_popcount(black[i] | white[i]);
        decoded[i] = (host_panel.physical[i] & (uint8_t)~black[i]) | white[i];
    }
    assert(!memcmp(decoded, frame, 5000));
}

static void model_after(esp_err_t error)
{
    if (!error) for (unsigned i = 0; i < 40000; ++i) if (model_remaining[i]) --model_remaining[i];
}

static void set_config(display_mode_t mode, unsigned black, unsigned white)
{
    display_config_t config = {mode, (uint8_t)black, (uint8_t)white, false, BORDER_BLACK_LOGO};
    display_config_set(&config);
    black_budget = black;
    white_budget = white;
}

static void set_mode(display_mode_t mode) { set_config(mode, black_budget, white_budget); }

static void start(void)
{
    host_fakes_reset();
    host_panel_reset();
    host_panel.scan_us = 20000;
    host_panel.first_scan_extra_us = 250000;
    host_panel.resume_us = 120000;
    host_panel.min_gap_fast_us = 30000;
    host_panel.min_gap_slow_us = 50000;
    host_panel.strict_power_off = true;
    host_panel.on_full = model_full;
    host_panel.before_scan = model_before;
    host_panel.after_scan = model_after;
    frame = screen_panel();
    set_config(DISPLAY_DITHERED, DITHER_PULSES_DEFAULT, DITHER_PULSES_DEFAULT);
}

static void arguments_and_session_cap(void)
{
    start();
    assert(fast_display_full(NULL, frame, DISPLAY_DITHERED) == ESP_ERR_INVALID_ARG);
    assert(fast_display_full(H, frame, DISPLAY_MODE_COUNT) == ESP_ERR_INVALID_ARG);
    assert(fast_display_update(H, frame) == ESP_ERR_INVALID_STATE);
    /* At the session cap, cleaning still fits but preparing another fast waveform does not. */
    assert(fast_display_finish(NULL, frame) == ESP_ERR_INVALID_ARG);
    for (display_mode_t mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        ssd1681_handle_t d;
        assert(ssd1681_create(&d) == 0 && ssd1681_power_on(d) == 0);
        assert(fast_display_full(d, frame, mode) == 0);
        host_panel.updates = SSD1681_MAX_UPDATES - 2;
        unsigned f = host_panel.fulls;
        assert(fast_display_full(d, frame, mode) == ESP_ERR_INVALID_STATE && host_panel.fulls == f + 1);
        assert(ssd1681_destroy(d) == 0);
        assert(ssd1681_create(&d) == 0 && ssd1681_power_on(d) == 0);
        assert(fast_display_full(d, frame, mode) == 0);
        host_panel.updates = SSD1681_MAX_UPDATES - 2; f = host_panel.fulls;
        assert(fast_display_finish(d, frame) == 0 && host_panel.fulls == f + 1 && host_panel.updates == SSD1681_MAX_UPDATES - 1);
        assert(owner == NULL && !host_panel.analog && fast_display_update(d, frame) == ESP_ERR_INVALID_STATE);
        assert(fast_display_pending(d) == 0);
        /* Exhausted/faulted hardware must still propagate a real failure. */
        host_panel.updates = SSD1681_MAX_UPDATES; f = host_panel.fulls;
        assert(fast_display_finish(d, frame) == ESP_ERR_INVALID_STATE && host_panel.fulls == f);
        assert(ssd1681_destroy(d) == 0);
    }
    assert(screen_close());
    assert(fast_display_set_dither_pulses(0, 2) == ESP_ERR_INVALID_ARG);
    assert(fast_display_set_dither_pulses(2, 9) == ESP_ERR_INVALID_ARG);
}

static void every_budget(void)
{
    start();
    for (unsigned black_pulses = 1; black_pulses <= 8; ++black_pulses)
    for (unsigned white_pulses = 1; white_pulses <= 8; ++white_pulses) {
        set_config(DISPLAY_DITHERED, black_pulses, white_pulses);
        assert(screen_close());
        memset(frame, 85, 5000);
        assert(screen_present(true) && host_panel.mixed_frames == 1u && host_panel.explicit_masks && !fast_display_pending(H));
        unsigned f = host_panel.fulls, s = host_panel.scans;
        for (unsigned i = 1000; i < 5000; ++i) frame[i] ^= 255;
        assert(screen_present(true));
        while (fast_display_pending(H)) assert(screen_present(false));
        unsigned max = black_pulses > white_pulses ? black_pulses : white_pulses;
        assert(host_panel.scans == s + max + 1 && host_panel.fulls == f);
        /* Completion of a budget ends with one all-HOLD scan, then no rescans. */
        assert(last_driven == 0 && !hold_end_pending);
        for (unsigned i = 0; i < 5000; ++i) assert(!black[i] && !white[i]);
        unsigned before = host_panel.scans, resume_before = host_panel.resumes;
        assert(fast_display_update(H, frame) == 0 && host_panel.scans == before && host_panel.resumes == resume_before);
        frame[1111] ^= 128; assert(screen_present(true));
        frame[1111] ^= 128; frame[2222] ^= 8; assert(screen_present(true));
        while (fast_display_pending(H)) assert(screen_present(false));
        frame[3456] ^= 2; assert(screen_present(true));
        assert(screen_full_refresh(H) == 0 && !fast_display_pending(H));
    }
}

static void failed_and_superseded_endings(void)
{
    start();
    set_config(DISPLAY_DITHERED, 4, 4);
    assert(screen_present(true));
    frame[1234] ^= 16; assert(screen_present(true));
    drive_counters_t before = *drive_counters;
    host_panel.fail_scan_at = host_panel.scans + 1;
    assert(!screen_failed() && !screen_present(false) && screen_failed() && !host_panel.powered);
    assert(!memcmp(&before, drive_counters, sizeof(before))); /* No successful delivery, no count consumed. */
    assert(!fast_display_pending(H)); host_panel.fail_scan_at = 0;
    /* Completion of a budget is not idle until its all-HOLD successor succeeds. */
    set_config(DISPLAY_DITHERED, 1, 1);
    assert(screen_present(true));
    frame[1234] ^= 16; assert(screen_present(true));
    assert(!drive_counters->pending_pixels && hold_end_pending && fast_display_pending(H) == 1);
    /* New input supersedes the ending; it must not receive an all-HOLD mask. */
    frame[2345] ^= 8; assert(screen_present(true)); assert(last_driven == 1 && hold_end_pending);
    drive_counters_t ending_before = *drive_counters;
    host_panel.fail_scan_at = host_panel.scans + 1;
    assert(!screen_present(false) && !host_panel.powered && last_driven == 0);
    assert(hold_end_pending && owner == NULL && !memcmp(&ending_before, drive_counters, sizeof(ending_before)));
    host_panel.fail_scan_at = 0; assert(screen_present(true)); assert(!hold_end_pending && !fast_display_pending(H));
    frame[1234] ^= 16; assert(screen_present(true)); assert(hold_end_pending);
    assert(screen_full_refresh(H) == 0 && !hold_end_pending && !fast_display_pending(H));
}

static void mode_and_pulse_changes(void)
{
    start();
    set_config(DISPLAY_DITHERED, 1, 1);
    assert(screen_present(true));
    /* A user mode change gets a clean reference without a gameplay scan. */
    unsigned f = host_panel.fulls, s = host_panel.scans;
    set_mode(DISPLAY_GRAYSCALE); assert(screen_present(true));
    assert(host_panel.fulls == f + 1 && host_panel.scans == s && !drive_counters && gray_plan);
    f = host_panel.fulls; set_mode(DISPLAY_DITHERED); assert(screen_present(true));
    assert(host_panel.fulls == f + 1 && drive_counters && !gray_plan);
    /* Changing a pulse budget in the dithered mode also re-establishes the reference. */
    f = host_panel.fulls; set_config(DISPLAY_DITHERED, 5, 1); assert(screen_present(true) && host_panel.fulls == f + 1);
    f = host_panel.fulls; assert(screen_present(true) && host_panel.fulls == f); /* Unchanged settings reuse the context. */
    for (unsigned i = screen_updates(); i < SSD1681_MAX_UPDATES - 4; ++i) assert(screen_present(true));
    unsigned before_destroy = host_panel.destroys;
    assert(screen_present(true) && host_panel.destroys == before_destroy + 1 && screen_updates() == 1);
    /* Switching away frees the counters, so allocation failure is observable on return. */
    set_mode(DISPLAY_GRAYSCALE); assert(screen_present(true)); assert(screen_close());
    host_heap_fail_all = true; set_mode(DISPLAY_DITHERED); unsigned df = host_panel.fulls;
    assert(!screen_present(true) && screen_failed() && !host_panel.powered && host_panel.fulls == df);
    host_heap_fail_all = false;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "arguments")) arguments_and_session_cap();
    else if (!strcmp(argv[1], "budgets")) every_budget();
    else if (!strcmp(argv[1], "endings")) failed_and_superseded_endings();
    else if (!strcmp(argv[1], "modes")) mode_and_pulse_changes();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=[m for m in DISPLAY_MODULES if m not in ('fast_display.c', 'latest_frame.c')], fakes=['host_panel.c'],
    scenarios=[[name] for name in ('arguments', 'budgets', 'endings', 'modes')])
print('PASS: dithered controls; all 64 polarity budgets, one all-HOLD ending, new-input priority, ending failure/reset, no idle rescan, mode and pulse changes, allocation failure and the session cap for both modes')
