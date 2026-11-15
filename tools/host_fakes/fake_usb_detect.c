#include "usb_detect.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_fakes.h"
#include <assert.h>

bool host_usb_pads_detached;
esp_err_t host_usb_detect_error;

/* Tests without the real input module still link; input.c's definition wins where it is built. */
__attribute__((weak)) void input_notify_from_isr(void) {}

esp_err_t usb_detect_begin(void)
{
    assert(!host_usb_pads_detached);
    host_trace("usb_pads_detach");
    if (host_usb_detect_error) return host_usb_detect_error;
    const gpio_config_t io = {.pin_bit_mask = (1ULL << GPIO_NUM_19) | (1ULL << GPIO_NUM_20), .mode = GPIO_MODE_INPUT,
                              .pull_down_en = GPIO_PULLDOWN_ENABLE};
    (void)gpio_config(&io);
    host_usb_pads_detached = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

void usb_detect_end(void) { assert(host_usb_pads_detached); host_trace("usb_pads_restore"); host_usb_pads_detached = false; }
/* With the PHY attached the GPIO level means nothing; reading it then is a bug. */
bool usb_detect_plugged(void) { assert(host_usb_pads_detached); return host_gpio_level[GPIO_NUM_20] != 0; }

static void dp_isr(void *unused)
{
    (void)unused;
    gpio_intr_disable(GPIO_NUM_20);
    input_notify_from_isr();
}

void usb_detect_watch(bool on)
{
    if (!on) { gpio_intr_disable(GPIO_NUM_20); (void)gpio_isr_handler_remove(GPIO_NUM_20); return; }
    assert(host_usb_pads_detached);
    (void)gpio_set_intr_type(GPIO_NUM_20, GPIO_INTR_HIGH_LEVEL);
    (void)gpio_isr_handler_add(GPIO_NUM_20, dp_isr, NULL);
    (void)gpio_intr_enable(GPIO_NUM_20);
}

esp_err_t usb_detect_wakeup(bool on)
{
    return on ? gpio_wakeup_enable(GPIO_NUM_20, GPIO_INTR_HIGH_LEVEL) : gpio_wakeup_disable(GPIO_NUM_20);
}
