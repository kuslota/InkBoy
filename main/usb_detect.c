#include "usb_detect.h"
#include "input.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/usb_serial_jtag_reg.h"

enum { PINS = 2 };
static const gpio_num_t pins[PINS] = {GPIO_NUM_19, GPIO_NUM_20};
#define PAD_MASK (BIT(GPIO_NUM_19) | BIT(GPIO_NUM_20))
/* The pull-down only has to discharge the line after the PHY lets go; USB LAB read it after 300 ms. */
#define SETTLE_MS 20

static struct { uint32_t usj_conf0, iomux[PINS], pin[PINS], out_sel[PINS], enable; } saved;

/* gpio_config() selects the GPIO function, which clears USB_SERIAL_JTAG USB_PAD_ENABLE, and its
 * pull-up disable on GPIO20 sets PAD_PULL_OVERRIDE with DP_PULLUP clear (IDF 6.1 gpio_ll.h). */
esp_err_t usb_detect_begin(void)
{
    saved.usj_conf0 = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
    saved.enable = GPIO.enable;
    for (unsigned i = 0; i < PINS; ++i) {
        saved.iomux[i] = REG_READ(GPIO_PIN_MUX_REG[pins[i]]);
        saved.pin[i] = GPIO.pin[pins[i]].val;
        saved.out_sel[i] = GPIO.func_out_sel_cfg[pins[i]].val;
    }
    const gpio_config_t io = {.pin_bit_mask = BIT64(GPIO_NUM_19) | BIT64(GPIO_NUM_20), .mode = GPIO_MODE_INPUT,
                              .pull_down_en = GPIO_PULLDOWN_ENABLE};
    const esp_err_t error = gpio_config(&io);
    if (error) { usb_detect_end(); return error; }
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    return ESP_OK;
}

/* USB pad enable goes back last, so the PHY never sees the GPIO pulls. */
void usb_detect_end(void)
{
    for (unsigned i = 0; i < PINS; ++i) {
        GPIO.pin[pins[i]].val = saved.pin[i];
        GPIO.func_out_sel_cfg[pins[i]].val = saved.out_sel[i];
        REG_WRITE(GPIO_PIN_MUX_REG[pins[i]], saved.iomux[i]);
    }
    GPIO.enable_w1ts = saved.enable & PAD_MASK;
    GPIO.enable_w1tc = ~saved.enable & PAD_MASK;
    GPIO.status_w1tc = PAD_MASK;
    REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, saved.usj_conf0);
}

bool usb_detect_plugged(void) { return gpio_get_level(GPIO_NUM_20) != 0; }

static void dp_isr(void *unused)
{
    (void)unused;
    gpio_intr_disable(GPIO_NUM_20);
    input_notify_from_isr();
}

void usb_detect_watch(bool on)
{
    if (!on) { gpio_intr_disable(GPIO_NUM_20); (void)gpio_isr_handler_remove(GPIO_NUM_20); return; }
    (void)gpio_set_intr_type(GPIO_NUM_20, GPIO_INTR_HIGH_LEVEL);
    (void)gpio_isr_handler_add(GPIO_NUM_20, dp_isr, NULL);
    (void)gpio_intr_enable(GPIO_NUM_20);
}

esp_err_t usb_detect_wakeup(bool on)
{
    return on ? gpio_wakeup_enable(GPIO_NUM_20, GPIO_INTR_HIGH_LEVEL) : gpio_wakeup_disable(GPIO_NUM_20);
}
