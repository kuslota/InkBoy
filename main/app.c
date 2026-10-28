#include "clock.h"
#include "controller.h"
#include "display_config.h"
#include "display_preset.h"
#include "game_audio.h"
#include "game_session.h"
#include "gb_emulator.h"
#include "gray_settings_ui.h"
#include "input.h"
#include "menu.h"
#include "power.h"
#include "screen.h"
#include "settings_ui.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "player";

static void run_player(void)
{
    /* Power management is an optimisation, never a startup dependency: on
     * failure the player runs at boot clocks exactly as it did before. */
    esp_err_t pm_error = power_pm_init();
    if (pm_error) ESP_LOGW(TAG, "PLAYER_PM_UNAVAILABLE,error,%s", esp_err_to_name(pm_error));
    esp_err_t error = power_enable_battery();
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(GPIO_NUM_6);
    rtc_gpio_deinit(GPIO_NUM_0);
    const gpio_config_t boot = {.pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    if (!error) error = gpio_config(&boot);
    /* Consume the wake press so it cannot select an item on the startup menu. */
    while (!error && gpio_get_level(GPIO_NUM_0) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    if (error == ESP_OK) error = input_start_hardware_watch();
    nvs_handle_t settings;
    if (error == ESP_OK) error = nvs_flash_init(); /* Never erase user NVS on an init error. */
    if (error == ESP_OK) error = nvs_open("gamepad", NVS_READWRITE, &settings);
    if (error != ESP_OK) { screen_message("STARTUP ERROR", "RESET TO RETRY", "CHECK USB LOG", ""); return; }
    input_load_mapping(settings);
    controller_load(settings);
    if (gpio_get_level(GPIO_NUM_18)==0) controller_request_console();
    settings_ui_init(settings);
    (void)display_preset_load_active();
    settings_ui_load_display();
    gray_settings_ui_load();
    settings_ui_load_toggles();
    const bool sound_enabled = settings_ui_sound_enabled();
    /* Before audio: both use I2C port 0. Clock cartridges follow this time across power-off. */
    clock_init();
    gb_emulator_set_clock(clock_now);
    esp_err_t audio_error = game_audio_init(32000);
    game_audio_set_enabled(sound_enabled);
    ESP_LOGI(TAG, "PLAYER_SOUND,enabled,%u", sound_enabled);
    if (audio_error) ESP_LOGW(TAG, "AUDIO_UNAVAILABLE,error,%d", audio_error);
    /* Transport and emulator task need internal RAM; the first screen gives the grayscale planner
     * whatever is left, so it comes after them. */
    error = controller_start();
    if (error) ESP_LOGE(TAG, "PLAYER_CONTROLLER_START_FAILED,error,%s", esp_err_to_name(error));
    if (error || !game_session_start()) {
        screen_message("STARTUP ERROR", "CANNOT START PLAYER", "CHECK USB LOG", "RESET TO RETRY"); return;
    }
    if (!screen_message("ON", "STARTING GAME BOY", "LOADING MENU", "")) return;
    input_wait_release();
    unsigned selected = 0;
    while (!screen_failed()) {
        const char *items[] = {"RESUME GAME", "GAMES", "SETTINGS", "POWER OFF"};
        const char *no_game[] = {"GAMES","SETTINGS","POWER OFF"};
        const bool game_ready = game_session_ready();
        int choice = menu_choose("HOME", game_ready ? game_session_name() : "CHOOSE A GAME TO START",
                                 game_ready ? items : no_game, game_ready ? 4 : 3, &selected, NULL);
        if (choice == -2) break;
        if(choice>=0 && !game_ready) {const unsigned indices[]={1,2,3};choice=indices[choice];}
        bool play = false;
        if (choice == 0) play = game_session_ensure_saved();
        else if (choice == 1) play = game_session_select();
        else if (choice == 2) settings_ui_open();
        else if (choice == 3) power_off();
        if (play && !screen_failed() && input_wait_controller()) {
            if (!screen_close()) break;
            input_wait_release(); input_clear_events();
            /* The one place full clocks and no light sleep are required. */
            power_performance_hold();
            (void)game_session_play();
            power_performance_release();
            if (screen_failed()) break;
            game_session_ensure_saved();
            selected = 0;
        }
    }
    ESP_LOGE(TAG, "Player stopped; panel off. Reset required.");
}

void app_main(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    /* A crash/watchdog must not create an unattended refresh/reboot loop. */
    if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
        reason == ESP_RST_BROWNOUT) {
        ESP_LOGE("inkboy", "Reset reason %d: refusing automatic start after a crash; power-cycle to retry", reason);
        return;
    }
    run_player();
}
