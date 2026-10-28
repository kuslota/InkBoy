#include "power.h"
#include "game_audio.h"
#include "game_session.h"
#include "game_storage.h"
#include "menu.h"
#include "screen.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "player";

static esp_pm_lock_handle_t performance_lock, no_sleep_lock;
static unsigned performance_depth;

esp_err_t power_pm_init(void)
{
    /* Menus are the idle state of this device, and the panel holds its image
     * for free, so let the SoC live like it: scale down to the XTAL floor
     * when nothing holds a lock, and light-sleep between tickless-idle
     * wakeups. Gameplay takes both locks below, so nothing changes there. */
    const esp_pm_config_t config = {.max_freq_mhz = 240, .min_freq_mhz = 40,
                                    .light_sleep_enable = true};
    esp_err_t error = esp_pm_configure(&config);
    if (!error) error = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "performance", &performance_lock);
    if (!error) error = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "performance", &no_sleep_lock);
    if (error) {
        if (no_sleep_lock) { esp_pm_lock_delete(no_sleep_lock); no_sleep_lock = NULL; }
        if (performance_lock) { esp_pm_lock_delete(performance_lock); performance_lock = NULL; }
        return error;
    }
    ESP_LOGI(TAG, "PLAYER_PM,max_mhz,240,min_mhz,40,light_sleep,1");
    return ESP_OK;
}

void power_performance_hold(void)
{
    if (!performance_lock || !no_sleep_lock) return;
    if (performance_depth++) return;
    esp_err_t error = esp_pm_lock_acquire(performance_lock);
    if (!error) error = esp_pm_lock_acquire(no_sleep_lock);
    if (error) {
        esp_pm_lock_release(performance_lock);
        performance_depth = 0;
        ESP_LOGW(TAG, "PLAYER_PM_HOLD_FAILED,error,%s", esp_err_to_name(error));
    }
}

void power_performance_release(void)
{
    if (!performance_lock || !no_sleep_lock || !performance_depth) return;
    if (--performance_depth) return;
    esp_pm_lock_release(no_sleep_lock);
    esp_pm_lock_release(performance_lock);
}

void power_off(void)
{
    if (!game_session_ensure_saved()) return;
    esp_err_t error = esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    if (!error) error = rtc_gpio_pullup_en(GPIO_NUM_0);
    if (!error) error = rtc_gpio_pulldown_dis(GPIO_NUM_0);
    if (!error) error = game_storage_close();
    if (error) { menu_notice("POWER ERROR", "COULD NOT PREPARE SLEEP", "TRY AGAIN"); return; }
    if (!screen_message("OFF", "GAME PROGRESS SAVED", "PRESS BOOT TO TURN ON", "SAFE TO REMOVE SD CARD")) return;
    /* Panel supply is active-low. Keep it off while digital GPIOs sleep. */
    error = gpio_hold_en(GPIO_NUM_6);
    if (!error) error = gpio_hold_en(GPIO_NUM_17); /* Keep the battery latch on for BOOT wake. */
    if (error) {
        gpio_hold_dis(GPIO_NUM_6); gpio_hold_dis(GPIO_NUM_17);
        menu_notice("POWER ERROR", "COULD NOT HOLD POWER STATE", "TRY AGAIN"); return;
    }
    gpio_deep_sleep_hold_en();
    while (gpio_get_level(GPIO_NUM_0) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    game_audio_shutdown();
    ESP_LOGI(TAG, "PLAYER_OFF,saved,1,wake,BOOT");
    esp_deep_sleep_start();
}

/* GPIO 17 latches the battery on when high. Preload HIGH before releasing a retained sleep hold
 * or enabling the output. */
esp_err_t power_enable_battery(void)
{
    esp_err_t error = gpio_set_level(GPIO_NUM_17, 1);
    const gpio_config_t battery = {.pin_bit_mask = 1ULL << GPIO_NUM_17,
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    if (!error) error = gpio_config(&battery);
    if (!error) error = gpio_hold_dis(GPIO_NUM_17);
    if (!error) ESP_LOGI(TAG, "PLAYER_BATTERY_LATCH,gpio,17,level,1");
    return error;
}
