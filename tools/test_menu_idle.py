#!/usr/bin/env python3
"""Menu light sleep and USB plug detection over the real input, controller and idle modules: parking
the host on D+, entry conditions, what is held, exact PM restore, wakes by button, D+ and deadline."""
from host_build import run

CODE = r'''
#define CONFIG_PM_ENABLE 1
#include "menu_idle.c"
#include <assert.h>
#include <string.h>
#include "fake_ble_gamepad.h"
#include "fake_usb_gamepad.h"
#include "host_fakes.h"

static bool save_idle = true;
bool game_session_save_idle(void) { return save_idle; }

static const int pins[] = {17, 6, 9, 10, 11, 12, 13, 42, 46};
/* What happens during each blocked wait: a timer wake, a tap/hold, D+ with or without its interrupt. */
enum { TIMER, PRESS, SILENT_PRESS, DP_HIGH, DP_HIGH_SILENT, DP_LOW_SILENT };
static int actions[8];
static unsigned action_count, hooks, awake_waits;
static int press_pin = 0;
static int64_t press_us = 80000;
static bool plug_wake_expected;

static TaskHandle_t watch(void) { TaskHandle_t t = host_task_find("player_buttons"); assert(t); return t; }
static int64_t release_at;
static void let_go(void) { if (release_at && host_clock_us >= release_at) { release_at = 0; host_gpio_drive(press_pin, 1); } }
static void script(int a, int b) { actions[0] = a; actions[1] = b; action_count = b >= 0 ? 2 : 1; hooks = awake_waits = 0; }

static void blocked(void)
{
    if (!host_pm_config.light_sleep_enable) { ++awake_waits; host_clock_us += (int64_t)host_semaphore_wait_ticks * 1000; return; }
    assert(host_pm_config.max_freq_mhz == 160 && host_pm_config.min_freq_mhz == 40);
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; ++i) {
        char hold[32]; snprintf(hold, sizeof hold, "gpio_hold_en(%d)", pins[i]); assert(host_trace_has(hold));
    }
    assert(host_gpio_wake[0] && host_gpio_wake[18] && !host_trace_has("gpio_hold_dis"));
    assert(host_gpio_wake[20] == plug_wake_expected && !usb_gamepad_running());
    assert(host_semaphore_wait_ticks <= 1000); /* The 1 s missed-wake backstop. */
    const int action = hooks < action_count ? actions[hooks] : TIMER;
    ++hooks;
    if (action == PRESS || action == TIMER || action == SILENT_PRESS) host_clock_us += (int64_t)host_semaphore_wait_ticks * 1000;
    if (action == SILENT_PRESS) host_gpio_level[press_pin] = 0; /* A wake the GPIO interrupt missed. */
    else if (action == PRESS) {
        release_at = host_clock_us + press_us; host_delay_hook = let_go;
        host_gpio_drive(press_pin, 0);
        host_task_run(watch());
    } else if (action == DP_HIGH) { host_clock_us += 300000; host_gpio_drive(20, 1); }
    else if (action == DP_HIGH_SILENT || action == DP_LOW_SILENT) {
        host_clock_us += (int64_t)host_semaphore_wait_ticks * 1000; host_gpio_level[20] = action == DP_HIGH_SILENT;
    }
}

static void start(unsigned transport)
{
    host_fakes_reset(); fake_usb_reset();
    host_nvs_seed("gamepad", "controller", HOST_NVS_U8, &(uint8_t){(uint8_t)transport}, 1);
    nvs_handle_t handle; assert(nvs_open("gamepad", NVS_READWRITE, &handle) == ESP_OK);
    controller_load(handle); assert(controller_start() == ESP_OK);
    assert(input_start_hardware_watch() == ESP_OK);
    host_task_run(watch());
    host_pm_config = (esp_pm_config_t){.max_freq_mhz = 160, .min_freq_mhz = 40};
    host_semaphore_wait_hook = blocked;
    host_trace_clear(); script(TIMER, -1);
}

static bool before(const char *a, const char *b) { int x = host_trace_position(a), y = host_trace_position(b); return x >= 0 && y > x; }

/* The boot attempt finds nothing within 2 s: the host stops and D+ is watched. */
static void park(void)
{
    assert(usb_gamepad_running() && !controller_menu_service()); /* Still inside the attempt. */
    host_clock_us += 2000000;
    assert(controller_menu_service() && controller_parked() && !usb_gamepad_running() && host_usb_pads_detached);
    assert(before("usb_stop", "usb_pads_detach") && host_gpio_interrupt_armed(20, GPIO_INTR_HIGH_LEVEL));
    assert(controller_status() == CONTROLLER_USB_WAIT && controller_running() && !controller_plug_pending());
    assert(!controller_menu_service() && controller_menu_deadline() == INT64_MAX);
    plug_wake_expected = true;
    host_trace_clear(); host_log_clear();
}

static void restored(void)
{
    assert(!host_pm_config.light_sleep_enable && host_pm_config.max_freq_mhz == 160 && host_pm_config.min_freq_mhz == 40);
    assert(!host_gpio_wake[0] && !host_gpio_wake[18] && !host_gpio_wake[20] && host_trace_has("esp_sleep_disable_wakeup_source(7)"));
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; ++i) {
        char hold[32]; snprintf(hold, sizeof hold, "gpio_hold_dis(%d)", pins[i]); assert(host_trace_has(hold));
    }
    assert(host_gpio_level[17] == 1 && host_nvs_commits == 0);
}

static void untouched(void)
{
    host_trace_clear(); script(TIMER, -1);
    assert(!menu_idle_wait(host_clock_us + 50000) && !hooks);
    assert(!host_trace_has("esp_pm_configure") && !host_trace_has("gpio_hold_en") && !host_trace_has("gpio_wakeup_enable"));
}

static void press_during_stop(void) { host_gpio_drive(0, 0); host_task_run(watch()); }
static void connect_on_delay(void) { if (usb_gamepad_running()) fake_usb_report(0); }

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *s = argv[1];
    if (!strcmp(s, "usb_tap") || !strcmp(s, "usb_hold")) {
        /* Sleeps at once with nothing due; a button wake is a normal press and never starts the host. */
        if (!strcmp(s, "usb_hold")) { press_pin = 18; press_us = 1500000; }
        start(CONTROLLER_USB); park();
        script(TIMER, PRESS);
        assert(menu_idle_wait(INT64_MAX) && hooks == 2);
        restored();
        assert(before("gpio_hold_en(46)", "gpio_wakeup_enable(0,4)") && before("gpio_wakeup_enable(18,4)", "gpio_wakeup_enable(20,5)"));
        assert(before("gpio_wakeup_enable(20,5)", "esp_pm_configure(160,40,ls=1)"));
        assert(before("esp_pm_configure(160,40,ls=1)", "esp_pm_configure(160,40,ls=0)"));
        assert(before("esp_pm_configure(160,40,ls=0)", "gpio_wakeup_disable(20)") && before("gpio_wakeup_disable(18)", "gpio_hold_dis(17)"));
        assert(!controller_menu_service() && controller_parked() && !host_trace_has("usb_start") && !host_trace_has("usb_pads_restore"));
        if (press_pin == 0) assert(input_hardware_event() == INPUT_SELECT && !input_hardware_event()); /* The tap that woke it. */
        else {
            assert(!input_board_idle() && input_hardware_event() == INPUT_BACK);
            host_gpio_drive(18, 1); host_task_run(watch()); assert(!input_hardware_event() && input_board_idle());
        }
    } else if (!strcmp(s, "dp_asleep")) {
        /* D+ rises while asleep: its interrupt ends the sleep, the host starts and a controller keeps it awake. */
        start(CONTROLLER_USB); park();
        script(DP_HIGH, -1);
        assert(menu_idle_wait(INT64_MAX) && hooks == 1 && host_log_count("plug,1") == 1);
        restored();
        host_trace_clear(); script(TIMER, -1);
        assert(controller_plug_pending() && !menu_idle_wait(INT64_MAX) && !host_trace_has("gpio_hold_en") && !awake_waits); /* Not slept on. */
        assert(controller_menu_service() && before("usb_pads_restore", "usb_start") && usb_gamepad_running() && !controller_parked());
        assert(controller_menu_deadline() == host_clock_us + 2000000 && controller_status() == CONTROLLER_USB_WAIT);
        untouched(); /* The attempt keeps the host running. */
        fake_usb_report(0);
        assert(!controller_menu_service() && controller_status() == CONTROLLER_USB_CONNECTED);
        host_clock_us += 10000000;
        assert(!controller_menu_service() && usb_gamepad_running() && controller_menu_deadline() == INT64_MAX);
        untouched();
    } else if (!strcmp(s, "dp_missed")) {
        /* D+ high without its interrupt is found by the 1 s backstop. */
        start(CONTROLLER_USB); park();
        script(TIMER, DP_HIGH_SILENT);
        assert(menu_idle_wait(INT64_MAX) && hooks == 2 && host_log_count("plug,1") == 1);
        restored();
        assert(controller_menu_service() && usb_gamepad_running());
    } else if (!strcmp(s, "dp_awake")) {
        /* D+ already high when parking, e.g. a controller that dropped off: one attempt at once. */
        start(CONTROLLER_USB); fake_usb_report(0); assert(!controller_menu_service());
        host_clock_us += 5000000;
        fake_usb_unplug(); host_gpio_level[20] = 1;
        assert(controller_menu_service() && before("usb_stop", "usb_pads_detach") && before("usb_pads_restore", "usb_start"));
        assert(usb_gamepad_running() && fake_usb_starts == 2 && controller_status() == CONTROLLER_USB_WAIT);
    } else if (!strcmp(s, "dp_no_controller")) {
        /* D+ high but nothing connects within 2 s: stop again and ignore D+ until it reads low once. */
        start(CONTROLLER_USB); park();
        host_gpio_drive(20, 1);
        assert(controller_plug_pending() && controller_menu_service() && usb_gamepad_running() && fake_usb_starts == 2);
        host_clock_us += 1999999;
        assert(!controller_menu_service() && usb_gamepad_running());
        host_clock_us += 1;
        assert(controller_menu_service() && controller_parked() && !usb_gamepad_running() && fake_usb_stops == 2);
        assert(!host_gpio_interrupt_armed(20, GPIO_INTR_HIGH_LEVEL) && !controller_plug_pending());
        for (unsigned i = 0; i < 5; ++i) assert(!controller_menu_service());
        assert(fake_usb_starts == 2 && controller_status() == CONTROLLER_USB_WAIT);
        plug_wake_expected = false; host_trace_clear(); host_log_clear();
        script(TIMER, DP_LOW_SILENT); /* No D+ wake while ignored; the backstop sees it go low. */
        assert(menu_idle_wait(INT64_MAX) && hooks == 2 && fake_usb_starts == 2 && !host_trace_has("gpio_wakeup_enable(20"));
        restored();
        assert(controller_plug_pending() && !controller_menu_service() && host_gpio_interrupt_armed(20, GPIO_INTR_HIGH_LEVEL));
        host_gpio_drive(20, 1); /* Plugged again: a fresh attempt. */
        assert(controller_menu_service() && fake_usb_starts == 3);
    } else if (!strcmp(s, "disconnect")) {
        start(CONTROLLER_USB); fake_usb_report(0);
        assert(!controller_menu_service() && controller_status() == CONTROLLER_USB_CONNECTED);
        host_clock_us += 30000000;
        untouched(); /* Connected: awake. */
        fake_usb_unplug();
        assert(controller_menu_deadline() <= host_clock_us); /* Handled on the next loop pass. */
        host_trace_clear();
        assert(controller_menu_service() && controller_parked() && before("usb_stop", "usb_pads_detach") && !host_trace_has("usb_start"));
        assert(host_gpio_interrupt_armed(20, GPIO_INTR_HIGH_LEVEL) && controller_status() == CONTROLLER_USB_WAIT);
        plug_wake_expected = true; script(PRESS, -1);
        assert(menu_idle_wait(INT64_MAX) && hooks == 1);
    } else if (!strcmp(s, "gameplay")) {
        /* Gameplay and controller waits start a parked host first, as before. */
        start(CONTROLLER_USB); park();
        input_set_gameplay(true);
        assert(before("usb_pads_restore", "usb_start") && usb_gamepad_running() && !controller_parked());
        input_set_gameplay(false);
        host_clock_us += 2000000;
        assert(controller_menu_service() && controller_parked());
        host_trace_clear(); host_delay_hook = connect_on_delay;
        assert(input_wait_controller() && before("usb_pads_restore", "usb_start") && controller_connected());
        host_clock_us += 2000000; host_delay_hook = NULL;
        input_set_gameplay(true); input_set_gameplay(false); /* Not parked: nothing restarts. */
        assert(fake_usb_starts == 3 && !controller_menu_service());
    } else if (!strcmp(s, "deadline")) {
        /* Analog power-down due in 750 ms: a timer wake ends the sleep on time with pins released and
         * PM restored for the panel transaction; the next wait sleeps again. Long waits use the backstop. */
        start(CONTROLLER_USB); park();
        const int64_t due = host_clock_us + 750000;
        assert(menu_idle_wait(due) && hooks == 1 && host_clock_us >= due && host_clock_us < due + 1000);
        restored();
        host_trace_clear(); host_log_clear();
        const int64_t later = host_clock_us + 3500000;
        script(TIMER, -1);
        assert(menu_idle_wait(later) && hooks == 4 && host_clock_us >= later && host_clock_us < later + 1000);
        restored(); assert(host_log_count("PLAYER_MENU_IDLE,sleep") == 1); /* The backstop does not leave sleep. */
    } else if (!strcmp(s, "pc_mode")) {
        /* USB: PC without a PC: today's pads, no D+ detection. */
        start(CONTROLLER_USB);
        assert(controller_toggle_usb() == ESP_OK && controller_status() == CONTROLLER_USB_PC);
        host_clock_us += 5000000;
        assert(!controller_menu_service() && !controller_parked() && !host_trace_has("usb_pads"));
        host_trace_clear(); plug_wake_expected = false; script(PRESS, -1);
        assert(menu_idle_wait(INT64_MAX) && hooks == 1);
        restored();
        assert(!host_trace_has("usb_") && controller_status() == CONTROLLER_USB_PC && input_hardware_event() == INPUT_SELECT);
        assert(controller_toggle_usb() == ESP_OK && usb_gamepad_running()); /* Back to the controller: a new attempt. */
        host_clock_us += 2000000;
        assert(controller_menu_service() && controller_parked());
        host_trace_clear();
        assert(controller_toggle_usb() == ESP_OK && controller_status() == CONTROLLER_USB_PC && !controller_parked());
        assert(host_trace_has("usb_pads_restore") && !host_trace_has("usb_start") && !usb_gamepad_running());
    } else if (!strcmp(s, "pc_connected")) {
        start(CONTROLLER_USB); assert(controller_toggle_usb() == ESP_OK); host_usj_connected = true; untouched();
    } else if (!strcmp(s, "bluetooth")) {
        start(CONTROLLER_BLUETOOTH); untouched();
        assert(!controller_menu_service() && !controller_parked());
        assert(controller_select(CONTROLLER_USB) == ESP_OK); host_clock_us += 3000000;
        assert(controller_menu_service() && controller_parked()); /* Switched to USB: detection applies, */
        untouched(); /* but the Bluetooth controller stays enabled after a switch, so no sleep. */
    } else if (!strcmp(s, "select")) {
        /* Switching transport while parked restores the pads before the host is touched. */
        start(CONTROLLER_USB); park();
        assert(controller_select(CONTROLLER_BLUETOOTH) == ESP_OK && !controller_parked() && !host_usb_pads_detached);
        assert(before("usb_pads_restore", "usb_start") && !usb_gamepad_running() && controller_transport() == CONTROLLER_BLUETOOTH);
    } else if (!strcmp(s, "saving")) {
        start(CONTROLLER_USB); park(); save_idle = false; untouched();
    } else if (!strcmp(s, "held")) {
        start(CONTROLLER_USB); park(); host_gpio_drive(0, 0); host_task_run(watch()); untouched();
    } else if (!strcmp(s, "already_light")) {
        start(CONTROLLER_USB); park(); host_pm_config.light_sleep_enable = true; host_semaphore_wait_hook = NULL;
        assert(!menu_idle_wait(host_clock_us + 50000) && !host_trace_has("esp_pm_configure") && !host_trace_has("gpio_hold_en"));
    } else if (!strcmp(s, "press_during_stop")) {
        /* A press while the host stops reaches the menu and is not slept on. */
        start(CONTROLLER_USB); host_clock_us += 2000000; fake_usb_stop_hook = press_during_stop;
        assert(controller_menu_service() && controller_parked());
        untouched();
        assert(input_hardware_event() == INPUT_PREVIOUS);
    } else if (!strcmp(s, "pm_error")) {
        start(CONTROLLER_USB); park(); host_pm_configure_error = ESP_ERR_NOT_SUPPORTED;
        assert(!menu_idle_wait(host_clock_us + 50000) && !hooks && awake_waits == 1);
        assert(host_trace_has("esp_pm_configure(160,40,ls=1)") && !host_trace_has("ls=0"));
        restored(); assert(controller_parked() && !host_trace_has("usb_start"));
    } else if (!strcmp(s, "usb_stop_fails")) {
        /* A host that will not stop stays running; parking is not retried in a loop. */
        start(CONTROLLER_USB); host_clock_us += 2000000; fake_usb_stop_timeouts = 3;
        assert(!controller_menu_service() && !controller_parked() && usb_gamepad_running() && fake_usb_stops == 3);
        assert(!controller_menu_service() && fake_usb_stops == 3 && controller_menu_deadline() == INT64_MAX);
        untouched();
    } else if (!strcmp(s, "detach_fails")) {
        start(CONTROLLER_USB); host_clock_us += 2000000; host_usb_detect_error = ESP_FAIL;
        assert(!controller_menu_service() && !controller_parked() && usb_gamepad_running() && fake_usb_starts == 2);
        assert(!controller_menu_service() && fake_usb_stops == 1);
    } else if (!strcmp(s, "missed_interrupt")) {
        start(CONTROLLER_USB); park(); script(SILENT_PRESS, -1);
        assert(menu_idle_wait(INT64_MAX) && hooks == 1 && host_log_count("poll,1") == 1);
        restored();
        host_task_run(watch()); /* The backstop woke the button task, which now samples the press. */
        assert(!input_board_idle());
    } else assert(0);
    return 0;
}
'''

run(CODE, modules=['input.c', 'controller.c'], fakes=['fake_screen.c', 'fake_ble_gamepad.c', 'fake_usb_gamepad.c'],
    scenarios=[[s] for s in ('usb_tap', 'usb_hold', 'dp_asleep', 'dp_missed', 'dp_awake', 'dp_no_controller', 'disconnect',
                             'gameplay', 'deadline', 'pc_mode', 'pc_connected', 'bluetooth', 'select', 'saving', 'held',
                             'already_light', 'press_during_stop', 'pm_error', 'usb_stop_fails', 'detach_fails',
                             'missed_interrupt')])
print('PASS: menus sleep at once; no controller parks the USB host on D+; D+ high (interrupt, backstop or awake) starts one '
      '2 s attempt and a controller keeps it awake, a non-controller is ignored until D+ is low; disconnect returns to '
      'detection; button wakes never start the host; gameplay/controller waits start it; timer wakes honour deadlines '
      'with exact PM restore; PC mode, Bluetooth, PC connected, save, held button, PM and stop errors stay safe')
