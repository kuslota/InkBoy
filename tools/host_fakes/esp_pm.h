#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdio.h>
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
esp_err_t esp_pm_configure(const void *config);
esp_err_t esp_pm_get_configuration(void *config);
esp_err_t esp_pm_dump_locks(FILE *stream);
