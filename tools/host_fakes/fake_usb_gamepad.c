#include "fake_usb_gamepad.h"
#include "host_fakes.h"

esp_err_t fake_usb_start_error;
unsigned fake_usb_stop_timeouts, fake_usb_starts, fake_usb_stops;
void (*fake_usb_stop_hook)(void);
static usb_gamepad_config_t config;
static bool running;

void fake_usb_reset(void)
{
    fake_usb_start_error = ESP_OK;
    fake_usb_stop_timeouts = fake_usb_starts = fake_usb_stops = 0;
    config = (usb_gamepad_config_t){0};
    running = false;
    fake_usb_stop_hook = NULL;
}

esp_err_t usb_gamepad_start(const usb_gamepad_config_t *requested)
{
    host_trace("usb_start");
    ++fake_usb_starts;
    if (fake_usb_start_error) return fake_usb_start_error;
    if (running) return ESP_OK;
    config = *requested;
    running = true;
    if (config.release_console) config.release_console(config.ctx);
    return ESP_OK;
}

esp_err_t usb_gamepad_stop(void)
{
    host_trace("usb_stop");
    ++fake_usb_stops;
    if (fake_usb_stop_hook) fake_usb_stop_hook();
    if (fake_usb_stop_timeouts) { --fake_usb_stop_timeouts; return ESP_ERR_TIMEOUT; }
    if (!running) return ESP_OK;
    running = false;
    if (config.disconnected) config.disconnected(config.ctx);
    if (config.restore_console) config.restore_console(config.ctx);
    return ESP_OK;
}

bool usb_gamepad_running(void) { return running; }
void fake_usb_report(uint8_t mask) { if (running) config.report(mask, config.ctx); }
void fake_usb_unplug(void) { if (running && config.disconnected) config.disconnected(config.ctx); }
void fake_usb_release_console(void) { if (running && config.release_console) config.release_console(config.ctx); }
