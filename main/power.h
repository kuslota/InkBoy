#pragma once
#include "esp_err.h"

esp_err_t power_enable_battery(void);
void power_off(void);

/* Configure dynamic frequency scaling and automatic light sleep for menu
 * idle (240 MHz down to the 40 MHz XTAL floor). Non-fatal: a failure leaves
 * the chip at its boot frequency with no light sleep, as before. */
esp_err_t power_pm_init(void);
/* Hold/release full performance: CPU at its maximum frequency and no light
 * sleep. Calls nest; gameplay holds this for the whole session. */
void power_performance_hold(void);
void power_performance_release(void);
