idf_component_register(
    SRCS "game_audio.c" "game_audio_i2s_pm.c" "CodecDriver.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "vendor/esp_codec_dev/device/es8311"
        "vendor/esp_codec_dev/device/priv_include"
        "vendor/esp_codec_dev/device/include"
        "vendor/esp_codec_dev/interface"
        "vendor/esp_codec_dev/include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s
    PRIV_REQUIRES esp_pm esp_driver_dma
)

idf_component_get_property(i2s_driver_dir esp_driver_i2s COMPONENT_DIR)
set_source_files_properties(game_audio_i2s_pm.c PROPERTIES INCLUDE_DIRECTORIES "${i2s_driver_dir}")
