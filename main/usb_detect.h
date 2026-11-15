#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Plug detection with the USB host stopped: a plugged device's pull-up drives D+ (GPIO20) high
 * against the pull-down set here (USB LAB). Saves the pads, then detaches GPIO19/20 from the PHY. */
esp_err_t usb_detect_begin(void);
/* Restores the saved pad registers; call before the host starts again. */
void usb_detect_end(void);
bool usb_detect_plugged(void);
/* High-level interrupt on D+ that wakes input_wait(); it disarms itself once it fires. */
void usb_detect_watch(bool on);
/* Light-sleep GPIO wake on D+ high (needs esp_sleep_enable_gpio_wakeup()). */
esp_err_t usb_detect_wakeup(bool on);
