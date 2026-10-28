#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Header-only stand-in for ESP-IDF's esp_pm: configuration and locks are
 * recorded nowhere and always succeed, matching how the other fakes treat
 * hardware that host tests do not exercise. Handles are fabricated distinct
 * integers, never dereferenced. */
typedef struct {
    int max_freq_mhz;
    int min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;

typedef enum { ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP } esp_pm_lock_type_t;
typedef struct esp_pm_lock *esp_pm_lock_handle_t;

static inline esp_err_t esp_pm_configure(const esp_pm_config_t *config)
{
    (void)config;
    return ESP_OK;
}

static inline esp_err_t esp_pm_lock_create(esp_pm_lock_type_t type, int arg, const char *name,
                                           esp_pm_lock_handle_t *out)
{
    static int next_handle = 1;
    (void)type; (void)arg; (void)name;
    *out = (esp_pm_lock_handle_t)(uintptr_t)next_handle++;
    return ESP_OK;
}

static inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) { (void)lock; return ESP_OK; }
static inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) { (void)lock; return ESP_OK; }
static inline esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t lock) { (void)lock; return ESP_OK; }
