#pragma once
#include "esp_err.h"

#define ESP_SLEEP_WAKEUP_GPIO 7
esp_err_t esp_sleep_enable_ext0_wakeup(int pin, int level);
esp_err_t esp_sleep_enable_gpio_wakeup(void);
esp_err_t esp_sleep_disable_wakeup_source(int source);
void esp_deep_sleep_start(void);
