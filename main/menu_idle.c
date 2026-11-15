#include "menu_idle.h"
#include "ws154_pins.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#ifndef CONFIG_PM_ENABLE
bool menu_idle_wait(int64_t deadline) { input_wait(deadline); return false; }
#else
#include "controller.h"
#include "game_session.h"
#include "ws154_power.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdint.h>

static const char *TAG = "player";
static const gpio_num_t held[] = WS154_POWER_LIGHT_SLEEP_HOLDS;
static bool buttons_armed, plug_at_wake;

static bool possible(void *unused)
{
    (void)unused;
    /* A started Bluetooth controller and a PC on USB-Serial-JTAG keep the chip awake or lose
     * their link in light sleep; those menus stay awake exactly as before. */
    bool ok = input_board_idle() && !controller_connected() && !controller_host_running() &&
              !controller_bluetooth_started() && !usb_serial_jtag_is_connected() && game_session_save_idle();
    return ok;
}

static esp_err_t arm(bool on, void *unused)
{
    (void)unused;
    if (on) {
        const esp_err_t error = input_sleep_wakeup(true);
        buttons_armed = !error;
        return error ? error : controller_plug_wakeup(true);
    }
    (void)controller_plug_wakeup(false);
    if (buttons_armed) (void)input_sleep_wakeup(false);
    buttons_armed = false;
    return ESP_OK;
}

static bool idle(void *unused) { (void)unused; return input_board_idle(); }
static bool plug_pending(void *unused) { (void)unused; return controller_plug_pending(); }
static void wait(int64_t until, void *unused) { (void)unused; input_wait(until); }

/* A press the GPIO wake missed still holds the button down at the backstop wake. */
static bool button_down(void *unused)
{
    (void)unused;
    if (gpio_get_level(WS154_BTN_BOOT) != 0 && gpio_get_level(WS154_BTN_PWR) != 0) return false;
    input_watch_wake();
    return true;
}

static void asleep(void *unused)
{
    (void)unused;
    ESP_LOGI(TAG, "PLAYER_MENU_IDLE,sleep,1,transport,%s,parked,%u", controller_transport_name(controller_transport()),
             controller_parked());
}

static void waking(void *unused) { (void)unused; plug_at_wake = controller_plug_pending(); }

static const ws154_idle_config_t idle_config = {
    .held = held, .held_count = sizeof(held) / sizeof(held[0]), .latch = WS154_BATTERY_LATCH,
    .backstop_us = 1000000, .can_sleep = possible, .arm = arm, .idle = idle, .woken = plug_pending, .wait = wait,
    .poll = button_down, .asleep = asleep, .waking = waking,
};

bool menu_idle_wait(int64_t deadline)
{
    if (esp_timer_get_time() >= deadline || controller_plug_pending()) return false;
    const ws154_idle_result_t result = ws154_idle_wait(&idle_config, deadline);
    if (result.tried) {
        ESP_LOGI(TAG, "PLAYER_MENU_IDLE,wake,%u,ms,%lld,poll,%u,plug,%u,error,%s,pm,%s", result.slept,
                 (long long)((esp_timer_get_time() - result.began_us) / 1000), result.polled, plug_at_wake,
                 esp_err_to_name(result.error), esp_err_to_name(result.restored));
    }
    if (result.slept) return true;
    /* Awake while parked, the ignored D+ has no wake of its own: recheck it like the backstop. */
    if (!controller_plug_pending())
        input_wait(controller_parked() ? ws154_idle_next_check(&idle_config, deadline) : deadline);
    return false;
}

#endif
