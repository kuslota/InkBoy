#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "host_fakes.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char trace[4096];
int host_gpio_level[48] = {[0] = 1};
esp_err_t host_wake_enable_error;
bool host_usj_connected, host_gpio_wake[48], host_gpio_interrupts_broken;
unsigned host_gpio_isr_calls;
static bool isr_installed, intr_on[48];
static int intr_type[48];
static gpio_isr_t isr_handler[48];
static void *isr_argument[48];
void (*host_deep_sleep_hook)(void);
int host_reset_reason;

void host_trace(const char *format, ...)
{
    size_t used = strlen(trace);
    if (used + 160 > sizeof(trace)) return;
    va_list args;
    va_start(args, format);
    vsnprintf(trace + used, sizeof(trace) - used - 2, format, args);
    va_end(args);
    strcat(trace, ";");
}

void host_trace_clear(void) { trace[0] = 0; }
bool host_trace_has(const char *needle) { return strstr(trace, needle) != NULL; }

int host_trace_position(const char *needle)
{
    const char *found = strstr(trace, needle);
    return found ? (int)(found - trace) : -1;
}

void host_gpio_reset(void)
{
    host_usj_connected = isr_installed = host_gpio_interrupts_broken = false;
    host_gpio_isr_calls = 0;
    memset(host_gpio_wake, 0, sizeof(host_gpio_wake));
    memset(intr_on, 0, sizeof(intr_on));
    memset(intr_type, 0, sizeof(intr_type));
    memset(isr_handler, 0, sizeof(isr_handler));
}

/* Level types stay pending while their level holds, like the hardware; edges fire once per change. */
static void interrupt(int pin, bool changed)
{
    const int type = intr_type[pin], level = host_gpio_level[pin];
    const bool active = type == GPIO_INTR_LOW_LEVEL ? !level : type == GPIO_INTR_HIGH_LEVEL ? level :
                        type != GPIO_INTR_DISABLE && changed;
    if (host_gpio_interrupts_broken || !isr_installed || !isr_handler[pin] || !intr_on[pin] || !active) return;
    ++host_gpio_isr_calls;
    isr_handler[pin](isr_argument[pin]);
}

void host_gpio_drive(int pin, int level)
{
    const bool changed = host_gpio_level[pin] != level;
    host_gpio_level[pin] = level;
    interrupt(pin, changed);
}

bool host_gpio_interrupt_armed(int pin, int type) { return intr_on[pin] && intr_type[pin] == type && isr_handler[pin]; }

esp_err_t gpio_config(const gpio_config_t *config)
{
    host_trace("gpio_config(mask=%llx,mode=%d,pullup=%d)", (unsigned long long)config->pin_bit_mask, config->mode, config->pull_up_en);
    for (int pin = 0; pin < 48; ++pin)
        if (config->pin_bit_mask & (1ULL << pin)) { intr_type[pin] = config->intr_type; intr_on[pin] = config->intr_type != 0; }
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int flags)
{
    (void)flags;
    if (isr_installed) return ESP_ERR_INVALID_STATE;
    host_trace("gpio_install_isr_service");
    isr_installed = true;
    return ESP_OK;
}

void gpio_uninstall_isr_service(void)
{
    host_trace("gpio_uninstall_isr_service");
    isr_installed = false;
    memset(isr_handler, 0, sizeof(isr_handler));
}

esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void *argument)
{
    if (!isr_installed) return ESP_ERR_INVALID_STATE;
    isr_handler[pin] = handler; isr_argument[pin] = argument; intr_on[pin] = true;
    interrupt(pin, false);
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { isr_handler[pin] = NULL; return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type) { intr_type[pin] = type; return ESP_OK; }
esp_err_t gpio_intr_enable(gpio_num_t pin) { intr_on[pin] = true; interrupt(pin, false); return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { intr_on[pin] = false; return ESP_OK; }

esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type)
{
    host_trace("gpio_wakeup_enable(%d,%d)", pin, type);
    if (type != GPIO_INTR_LOW_LEVEL && type != GPIO_INTR_HIGH_LEVEL) return ESP_ERR_INVALID_ARG;
    intr_type[pin] = type; host_gpio_wake[pin] = true;
    return ESP_OK;
}

esp_err_t gpio_wakeup_disable(gpio_num_t pin)
{
    host_trace("gpio_wakeup_disable(%d)", pin);
    host_gpio_wake[pin] = false;
    return ESP_OK;
}

esp_err_t esp_sleep_enable_gpio_wakeup(void) { host_trace("esp_sleep_enable_gpio_wakeup"); return ESP_OK; }
esp_err_t esp_sleep_disable_wakeup_source(int source) { host_trace("esp_sleep_disable_wakeup_source(%d)", source); return ESP_OK; }
bool usb_serial_jtag_is_connected(void) { return host_usj_connected; }

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    host_gpio_level[pin]=(int)level;
    host_ssd1681_gpio(pin, level);
    host_trace("gpio_set_level(%d,%u)", pin, level);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) { return host_gpio_level[pin]; }

esp_err_t gpio_hold_en(gpio_num_t pin)
{
    host_trace("gpio_hold_en(%d)", pin);
    return ESP_OK;
}

esp_err_t gpio_hold_dis(gpio_num_t pin)
{
    host_trace("gpio_hold_dis(%d)", pin);
    return ESP_OK;
}

void gpio_deep_sleep_hold_en(void) { host_trace("gpio_deep_sleep_hold_en"); }
void gpio_deep_sleep_hold_dis(void) { host_trace("gpio_deep_sleep_hold_dis"); }

esp_err_t rtc_gpio_pullup_en(gpio_num_t pin)
{
    host_trace("rtc_gpio_pullup_en(%d)", pin);
    return ESP_OK;
}

esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin)
{
    host_trace("rtc_gpio_pulldown_dis(%d)", pin);
    return ESP_OK;
}

esp_err_t rtc_gpio_deinit(gpio_num_t pin)
{
    host_trace("rtc_gpio_deinit(%d)", pin);
    return ESP_OK;
}

esp_err_t esp_sleep_enable_ext0_wakeup(int pin, int level)
{
    host_trace("esp_sleep_enable_ext0_wakeup(%d,%d)", pin, level);
    return host_wake_enable_error;
}

void esp_deep_sleep_start(void)
{
    host_trace("esp_deep_sleep_start");
    if (host_deep_sleep_hook) host_deep_sleep_hook();
}

esp_reset_reason_t esp_reset_reason(void) { return (esp_reset_reason_t)host_reset_reason; }
