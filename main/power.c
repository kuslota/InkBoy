#include "power.h"
#include "ws154_pins.h"
#include "game_audio.h"
#include "game_session.h"
#include "game_storage.h"
#include "menu.h"
#include "screen.h"
#include "ws154_power.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "player";

void power_off(void)
{
    if (!game_session_ensure_saved()) return;
    esp_err_t error = ws154_power_wake_on_low(WS154_BTN_BOOT);
    if (!error) error = game_storage_close();
    if (error) { menu_notice("POWER ERROR", "COULD NOT PREPARE SLEEP", "TRY AGAIN"); return; }
    if (!screen_message("OFF", "GAME PROGRESS SAVED", "PRESS BOOT TO TURN ON", "SAFE TO REMOVE SD CARD")) return;
    /* Panel supply is active-low: keep it off. Keep the battery latch on for BOOT wake. */
    static const gpio_num_t held[] = {WS154_EPD_PWR, WS154_BATTERY_LATCH};
    if (ws154_power_hold_for_deep_sleep(held, sizeof(held) / sizeof(held[0]))) {
        menu_notice("POWER ERROR", "COULD NOT HOLD POWER STATE", "TRY AGAIN"); return;
    }
    ws154_power_wait_released(WS154_BTN_BOOT);
    game_audio_shutdown();
    ESP_LOGI(TAG, "PLAYER_OFF,saved,1,wake,BOOT");
    esp_deep_sleep_start();
}

esp_err_t power_enable_battery(void)
{
    const esp_err_t error = ws154_power_latch_on(WS154_BATTERY_LATCH);
    if (!error) ESP_LOGI(TAG, "PLAYER_BATTERY_LATCH,gpio,17,level,1");
    return error;
}
