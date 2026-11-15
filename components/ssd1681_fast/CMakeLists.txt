idf_component_register(SRCS "ssd1681_fast.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_spi esp_timer)

if(CONFIG_SSD1681_EXPERIMENTAL_INTERRUPT_FRAME_TX)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_SSD1681_EXPERIMENTAL_INTERRUPT_FRAME_TX=1)
endif()
