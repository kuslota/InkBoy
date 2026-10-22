#!/usr/bin/env python3
"""Run the real play session over the real fast display and a fake panel, clock and controller."""
import argparse
from host_build import run, DISPLAY_MODULES, SESSION_FAKES

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wake-early-us', type=int, default=0,
                    help='Probe steady motion with a wake this many us before a whole-ms delay (0..999)')
args = parser.parse_args()
if not 0 <= args.wake_early_us < 1000:
    parser.error('--wake-early-us must be 0..999')

CODE = r'''
#include "game_session.c"
#include <assert.h>
#include <stdlib.h>
#include "fake_game_audio.h"
#include "fake_gb_emulator.h"
#include "fake_input.h"
#include "host_fakes.h"
#include "host_panel.h"

static int64_t origin, sample_time, sample_epoch, last_bucket;
static int64_t session_limit_us = 100000000LL;
static unsigned samples, scenario, manual_count, steady_intervals;
static bool manual;
static int64_t steady_max_gap, steady_min_gap;
static unsigned wake_early_us;

static unsigned boot_event(void)
{
    int64_t now = host_clock_us;
    if (scenario == 5 || scenario == 6) return now - origin >= session_limit_us ? 2 : 0;
    if (scenario == 1 && !manual_count && now - origin >= 120000) { manual = true; ++manual_count; return 1; }
    return now - origin >= 2500000 ? INPUT_SELECT : 0;
}

static bool scripted_controller_connected(void) { return !(scenario == 3 && host_clock_us - origin >= 300000); }

static unsigned convert(uint8_t *dst)
{
    int64_t now = host_clock_us;
    if (!atomic_load(&paused) && !manual) {
        int64_t bucket = (now - sample_epoch) / FAST_DISPLAY_IMAGE_PERIOD_US;
        assert(bucket > last_bucket);
        last_bucket = bucket;
        if (scenario == 4 && samples && sample_time - origin >= 500000) {
            int64_t gap = now - sample_time;
            if (!steady_intervals || gap > steady_max_gap) steady_max_gap = gap;
            if (!steady_intervals || gap < steady_min_gap) steady_min_gap = gap;
            ++steady_intervals;
        }
        sample_time = now;
        ++samples;
    }
    uint8_t next = 255;
    if (!atomic_load(&paused)) {
        int64_t t = now - origin;
        next = t < 400000 ? (uint8_t)~(0x80u >> ((t / 100000) % 4)) : t < 1800000 ? 255 : 127;
        if (scenario >= 4) next = (uint8_t)~(0x80u >> ((t / 50000) % 8));
    }
    bool changed = dst[0] != next;
    dst[0] = next;
    return changed ? 1 : 0;
}

unsigned gb_video_convert_fast(gb_video_t *video, const uint8_t *packed, uint8_t *panel) { (void)video; (void)packed; return convert(panel); }
unsigned gb_video_prepare_gray(gb_video_t *video, const uint8_t *packed, uint8_t *panel) { (void)video; (void)packed; return convert(panel); }

static void on_full(const uint8_t *frame)
{
    (void)frame;
    manual = false;
    sample_epoch = host_clock_us;
    last_bucket = -1;
    if (host_panel.fulls == 1) origin = host_clock_us;
}

static void only_first_byte_drives(const uint8_t *black, const uint8_t *white)
{
    for (unsigned i = 1; i < SSD1681_FRAME_BYTES; ++i) assert(!(black[i] | white[i]));
}

static void start_session(display_mode_t mode, unsigned budget, unsigned which)
{
    assert(screen_close());
    host_fakes_reset();
    host_panel_reset();
    host_delay_early_us = wake_early_us;
    host_panel.scan_us = mode == DISPLAY_GRAYSCALE ? 14000 : 30000; /* Assumes the 20 MHz transfer saves 4 ms. */
    host_panel.resume_us = 120000;
    host_panel.full_us = 10000;
    /* Grayscale spaces update starts, not scan starts, so its panel-level gap can be one scan. */
    host_panel.min_gap_fast_us = mode == DISPLAY_DITHERED ? fast_display_drive_period_us(mode) : 0;
    host_panel.on_full = on_full;
    host_panel.fail_scan_at = which == 2 ? 3 : 0;
    host_panel.before_scan = mode == DISPLAY_DITHERED ? only_first_byte_drives : NULL;
    fake_input_boot_event = boot_event;
    fake_input_connected = scripted_controller_connected;
    display_config_t config = {mode, (uint8_t)budget, (uint8_t)budget, false, BORDER_BLACK_LOGO};
    display_config_set(&config);
    scenario = which;
    origin = sample_time = 0;
    samples = manual_count = steady_intervals = 0;
    manual = false;
    steady_max_gap = steady_min_gap = 0;
    core_lock = xSemaphoreCreateMutex();
    atomic_store(&paused, true);
    fake_gb_reset();
    fake_audio_playing_on = fake_audio_playing_off = 0;
    state_dirty = false;
    save_size = 4;
    assert(save_worker_start());
    save_worker_issued = 0;
}

static void queue_alone(display_mode_t mode, unsigned which, unsigned budget)
{
    start_session(mode, budget, which);
    unsigned result = game_session_play();
    assert(host_task_priority == 1);
    if (which == 2) { assert(screen_failed()); return; }
    assert(!screen_failed() && atomic_load(&paused));
    assert(screen_updates() == 0);
    assert(host_panel.fulls == 1u);
    assert(result == (which == 3 ? 3u : 1u));
    if (which == 0) {
        assert(host_panel.analog_offs >= 1 && host_panel.resumes >= 1);
        assert(samples >= 42 && host_panel.any_hold);
    }
    if (which == 4)
        assert(steady_intervals >= 18 && steady_min_gap >= FAST_DISPLAY_IMAGE_PERIOD_US - 1000 &&
               steady_max_gap <= FAST_DISPLAY_IMAGE_PERIOD_US + 1000);
    const char *home[]={"RESUME GAME","GAMES","SETTINGS","POWER OFF"};
    unsigned fulls_before=host_panel.fulls;
    host_panel.before_scan=NULL;
    screen_draw_menu(screen_panel(),"HOME","",home,4,0);
    assert(screen_present(true) && host_panel.fulls==fulls_before);
    unsigned remaining=0;
    while(screen_pending()) { assert(screen_service()>=0 && ++remaining<20); }
    assert(screen_close());

}

static void long_session(display_mode_t mode, unsigned which)
{
    session_limit_us = which == 5 ? 260000000LL : 300000000LL;
    start_session(mode, 1, which);
    assert(game_session_play() == 1 && !screen_failed() && atomic_load(&paused) && !host_panel.analog);
    assert(host_task_priority == 1);
    assert(host_panel.fulls == 1 && host_panel.scans > 2400 && host_clock_us - origin > 120000000LL);
    assert(state_dirty && !fake_audio_playing && fake_audio_playing_on == 1 && fake_audio_playing_off == 1);
    assert(host_panel.continuous_enables == 1 && host_panel.continuous_play);
    assert(save_worker_issued >= 4 && save_worker_issued <= (which == 5 ? 4u : 5u)); /* One queued battery save a minute, never waited for. */
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    wake_early_us = (unsigned)atoi(argv[1]);
    display_mode_t mode = (display_mode_t)atoi(argv[2]);
    unsigned which = (unsigned)atoi(argv[3]), budget = (unsigned)atoi(argv[4]);
    int64_t deadline = 0;
    unsigned admitted = 0;
    for (int64_t t = 0; t < 1000000; t += 34000) if (t >= deadline) {
        deadline = fast_display_next_image(deadline, t);
        ++admitted;
        assert(deadline > t);
    }
    assert(admitted == 30); /* A 34 ms loop keeps one image per 33.334 ms slot. */
    assert(fast_display_next_image(33334, 100000) == 100002);
    assert(fast_display_next_image(100002, 100000) == 100002);
    assert(fast_display_wake_time(34000, 33334, 50000, true, 40000) == 50000);
    assert(fast_display_wake_time(34000, 33334, 100000, true, 40000) == 34000);
    assert(fast_display_wake_time(34000, 33334, 100000, false, 40000) == 100000);
    assert(fast_display_wake_time(80000, 90000, 50000, true, 40000) == 90000);
    if (which < 5) {
        queue_alone(mode, which, budget);
        return 0;
    }
    /* Complete long sessions up to the 2,399-scan limit. A second session models Resume. */
    for (unsigned replay = 0; replay < 2; ++replay) long_session(mode, which);
    return 0;
}
'''

# Every session starts a fresh process: a delivery failure latches the screen as failed until reset.
early = str(args.wake_early_us)
scenarios = [[early, str(mode), str(which), str(budget)]
             for mode in (0, 1) for which in ((4,) if args.wake_early_us else range(5)) for budget in range(1, 9)]
if not args.wake_early_us:
    scenarios += [[early, str(mode), str(which), '1'] for mode in (0, 1) for which in (5, 6)]
run(CODE, modules=DISPLAY_MODULES, fakes=SESSION_FAKES, optimize='-O2', scenarios=scenarios)
print('PASS: dithered and grayscale scheduling and continuous-play entry; scripted BOOT exits after 2400+ drives and 2+ minutes; entry-only full refresh with live menu handover, state/audio lifecycle, image priority, HOLD ending, idle stop/resume, immediate tap exit, disconnect, delivery failure and the one-minute autosave queue')
