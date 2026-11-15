idf_component_register(SRCS "app.c" "clock.c" "controller.c" "display_config.c" "display_preset.c" "game_session.c"
    "game_storage.c" "gray_motion.c" "gray_profile_store.c" "gray_settings_ui.c"
    "input.c" "latest_frame.c" "menu.c" "menu_idle.c" "power.c" "preset_json.c" "preset_store.c" "preset_ui.c" "screen.c" "screen_draw.c" "settings_ui.c"
    INCLUDE_DIRS "." PRIV_REQUIRES esp_driver_usb_serial_jtag esp_pm REQUIRES espressif__cjson game_audio ble_gamepad nvs_flash fatfs esp32-gameboy epd_fast ssd1681_fast ws154_pins ws154_power esp_timer esp_driver_gpio esp_driver_i2c esp_hw_support)

if(INKBOY_DIAGNOSTICS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE INKBOY_DIAGNOSTICS=1)
    target_sources(${COMPONENT_LIB} PRIVATE "diagnostics.c")
endif()
