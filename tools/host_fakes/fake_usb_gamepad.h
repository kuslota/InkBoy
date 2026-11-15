#pragma once
#include "gameboy/usb_gamepad.h"

/* Stand-in for the esp32-gameboy USB host. Callbacks run on the caller's thread. */
extern esp_err_t fake_usb_start_error;
extern unsigned fake_usb_stop_timeouts, fake_usb_starts, fake_usb_stops;
/* Runs inside every usb_gamepad_stop() call, before it completes. */
extern void (*fake_usb_stop_hook)(void);
void fake_usb_reset(void);
void fake_usb_report(uint8_t mask);
void fake_usb_unplug(void);
void fake_usb_release_console(void);
