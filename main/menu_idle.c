#include "menu_idle.h"
#include "input.h"
#include "freertos/FreeRTOS.h"
#ifndef CONFIG_PM_ENABLE
bool menu_idle_wait(int64_t deadline) { input_wait(deadline); return false; }
#else
#include "controller.h"
#include "game_session.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include <stdint.h>

static const char *TAG = "player";
/* Battery latch; panel supply, RST, DC, CS, SCK, MOSI; codec rail; amplifier enable. With
 * CONFIG_PM_SLP_DISABLE_GPIO light sleep would float them: power loss or a reset panel. */
static const gpio_num_t held[] = {GPIO_NUM_17, GPIO_NUM_6, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
                                  GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_42, GPIO_NUM_46};
#define HELD_COUNT (sizeof(held) / sizeof(held[0]))
/* Backstop if a GPIO wake is ever missed: one short wake per second instead of a stuck menu. */
#define CHECK_US 1000000

static void release(unsigned count)
{
    while (count--) {
        if (held[count] == GPIO_NUM_17) (void)gpio_set_level(GPIO_NUM_17, 1);
        (void)gpio_hold_dis(held[count]);
    }
}

static bool possible(void)
{
    /* A started Bluetooth controller and a PC on USB-Serial-JTAG keep the chip awake or lose
     * their link in light sleep; those menus stay awake exactly as before. */
    bool ok = input_board_idle() && !controller_connected() && !controller_host_running() &&
              !controller_bluetooth_started() && !usb_serial_jtag_is_connected() && game_session_save_idle();
    return ok;
}

static bool button_down(void) { return gpio_get_level(GPIO_NUM_0) == 0 || gpio_get_level(GPIO_NUM_18) == 0; }

static bool woken(int64_t deadline)
{
    return esp_timer_get_time() >= deadline || controller_plug_pending();
}

static int64_t next_check(int64_t deadline)
{
    const int64_t next = esp_timer_get_time() + CHECK_US;
    return deadline < next ? deadline : next;
}

static bool sleep_until(const esp_pm_config_t *saved, int64_t deadline)
{
    esp_err_t error = ESP_OK;
    unsigned holds = 0;
    while (!error && holds < HELD_COUNT && !(error = gpio_hold_en(held[holds]))) ++holds;
    const bool armed = !error && !(error = input_sleep_wakeup(true));
    if (armed) error = controller_plug_wakeup(true);
    /* A press or plug-in while arming must reach the menu, not be slept on. */
    const bool idle = !error && input_board_idle() && !woken(deadline);
    esp_pm_config_t light = *saved;
    light.light_sleep_enable = true;
    if (idle) error = esp_pm_configure(&light);
    const bool slept = idle && !error;
    const int64_t began = esp_timer_get_time();
    bool polled = false;
    if (slept) {
        ESP_LOGI(TAG, "PLAYER_MENU_IDLE,sleep,1,transport,%s,parked,%u", controller_transport_name(controller_transport()),
                 controller_parked());
        while (input_board_idle() && !woken(deadline)) {
            input_wait(next_check(deadline));
            if (input_board_idle() && button_down()) { polled = true; input_watch_wake(); break; }
        }
    }
    const bool plug = controller_plug_pending();
    const esp_err_t restored = slept ? esp_pm_configure(saved) : ESP_OK;
    (void)controller_plug_wakeup(false);
    if (armed) (void)input_sleep_wakeup(false);
    release(holds);
    ESP_LOGI(TAG, "PLAYER_MENU_IDLE,wake,%u,ms,%lld,poll,%u,plug,%u,error,%s,pm,%s", slept,
             (long long)((esp_timer_get_time() - began) / 1000), polled, plug, esp_err_to_name(error),
             esp_err_to_name(restored));
    return slept;
}

bool menu_idle_wait(int64_t deadline)
{
    if (esp_timer_get_time() >= deadline || controller_plug_pending()) return false;
    esp_pm_config_t saved;
    if (possible() && esp_pm_get_configuration(&saved) == ESP_OK && !saved.light_sleep_enable &&
        sleep_until(&saved, deadline)) return true;
    /* Awake while parked, the ignored D+ has no wake of its own: recheck it like the backstop. */
    if (!controller_plug_pending()) input_wait(controller_parked() ? next_check(deadline) : deadline);
    return false;
}

#endif
