#pragma once
#include <stdint.h>
#include "esp_err.h"

#define GPIO_NUM_0 0
#define GPIO_NUM_6 6
#define GPIO_NUM_9 9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#define GPIO_NUM_42 42
#define GPIO_NUM_46 46
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_ANYEDGE 3
#define GPIO_INTR_LOW_LEVEL 4
#define GPIO_INTR_HIGH_LEVEL 5

typedef int gpio_num_t;
typedef int gpio_int_type_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef void (*gpio_isr_t)(void *);

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_hold_en(gpio_num_t pin);
esp_err_t gpio_hold_dis(gpio_num_t pin);
void gpio_deep_sleep_hold_en(void);
void gpio_deep_sleep_hold_dis(void);
esp_err_t gpio_set_direction(gpio_num_t pin, int mode);
esp_err_t gpio_install_isr_service(int flags);
void gpio_uninstall_isr_service(void);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void *argument);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_wakeup_disable(gpio_num_t pin);
