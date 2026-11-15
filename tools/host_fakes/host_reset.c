#include "host_fakes.h"
#include <string.h>

void host_fakes_reset(void)
{
    host_clock_us = 1;
    host_semaphore_wait_hook=NULL;
    host_delay_early_us = 0;
    host_task_priority = 1;
    host_tasks_created = host_mutexes_held = 0;
    host_task_autorun_name[0] = 0;
    host_task_create_fails = host_mutex_create_fails = false;
    host_heap_fail_all = false;
    host_heap_fail_hook = NULL;
    host_log_clear();
    host_trace_clear();
    memset(host_gpio_level, 0, sizeof(host_gpio_level));
    host_gpio_level[0] = 1; host_gpio_level[18] = 1;
    host_wake_enable_error = ESP_OK;
    host_usb_pads_detached = false;
    host_usb_detect_error = ESP_OK;
    host_deep_sleep_hook = NULL;
    host_delay_hook = NULL;
    host_gpio_reset();
    host_pm_config = (esp_pm_config_t){.max_freq_mhz = 240, .min_freq_mhz = 40};
    host_pm_configure_error = ESP_OK;
    host_reset_reason = 0;
    host_spi_transactions = 0;
    host_spi_polling_transactions = host_spi_interrupt_transactions = 0;
    host_spi_error = ESP_OK;
    host_ssd1681_reset(false);
    host_nvs_reset();
}
