#include "controller.h"
#include "diagnostics.h"
#include "ble_gamepad.h"
#include "gameboy/usb_gamepad.h"
#include "input.h"
#include "usb_detect.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>

static const char *TAG = "player";
enum { USB_STOP_ATTEMPTS = 3 };
/* Allow the controller two seconds to enumerate before parking the host. */
#define PROBE_US 2000000

static nvs_handle_t settings;
static controller_transport_t saved, active;
static bool running, console_mode, bluetooth_started;
/* ignore_dp: D+ is held high by a device that did not connect; acting on it again would loop. */
static bool parked, ignore_dp, plug_wake;
/* End of the attempt that started the host; 0 once it connected, INT64_MAX: do not park. */
static int64_t probe_until;
static vprintf_like_t previous_log;
static _Atomic uint8_t usb_mask;
static _Atomic bool usb_report_seen;
static _Atomic uint32_t usb_reports;

const char *controller_transport_name(controller_transport_t transport)
{
    return transport == CONTROLLER_USB ? "usb" : "ble";
}

static void usb_report(uint8_t mask, void *unused)
{
    (void)unused;
    atomic_store(&usb_mask, mask);
    atomic_store(&usb_report_seen, true);
    atomic_fetch_add(&usb_reports, 1);
}

static void usb_disconnected(void *unused)
{
    (void)unused;
    atomic_store(&usb_mask, 0);
    atomic_store(&usb_report_seen, false);
}

static int discard_log(const char *format, va_list args)
{
    (void)format; (void)args;
    return 0;
}

static void release_console(void *unused)
{
    (void)unused;
    if (!previous_log) previous_log = esp_log_set_vprintf(discard_log);
}

static void restore_console(void *unused)
{
    (void)unused;
    if (!previous_log) return;
    esp_log_set_vprintf(previous_log);
    previous_log = NULL;
}

static esp_err_t start_transport(controller_transport_t transport)
{
    if (transport == CONTROLLER_BLUETOOTH) { bluetooth_started = true; return ble_gamepad_start(); }
    if (console_mode) return ESP_OK;
    const usb_gamepad_config_t config = {
        .report = usb_report, .disconnected = usb_disconnected,
        .release_console = release_console, .restore_console = restore_console,
    };
    const esp_err_t error = usb_gamepad_start(&config);
    if (!error) probe_until = esp_timer_get_time() + PROBE_US;
    return error;
}

static esp_err_t stop_transport(controller_transport_t transport)
{
    if (transport == CONTROLLER_BLUETOOTH) return ble_gamepad_stop();
    esp_err_t error = ESP_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < USB_STOP_ATTEMPTS && error == ESP_ERR_TIMEOUT; ++attempt)
        error = usb_gamepad_stop();
    if (error == ESP_OK) usb_disconnected(NULL);
    return error;
}

static esp_err_t start_and_log(controller_transport_t transport)
{
    esp_err_t error = start_transport(transport);
    running = error == ESP_OK;
    if (running) active = transport;
    ESP_LOGI(TAG, "PLAYER_CONTROLLER,transport,%s,result,%s", controller_transport_name(transport), esp_err_to_name(error));
    return error;
}

static esp_err_t save(controller_transport_t transport)
{
    esp_err_t error = nvs_set_u8(settings, "controller", (uint8_t)transport);
    if (!error) error = nvs_commit(settings);
    if (!error) saved = transport;
    return error;
}

void controller_load(nvs_handle_t nvs)
{
    settings = nvs;
    console_mode = false;
    uint8_t stored;
    saved = nvs_get_u8(settings, "controller", &stored) == ESP_OK && stored < CONTROLLER_TRANSPORT_COUNT
                ? (controller_transport_t)stored : CONTROLLER_BLUETOOTH;
    active = saved;
}

controller_transport_t controller_transport(void) { return active; }
bool controller_running(void) { return running || parked; }

esp_err_t controller_start(void)
{
    esp_err_t error = start_and_log(saved);
    if (error && saved == CONTROLLER_USB) error = start_and_log(CONTROLLER_BLUETOOTH);
    return error;
}

static void attach(void)
{
    if (!parked) return;
    usb_detect_watch(false, NULL, NULL);
    usb_detect_end();
    parked = false;
}

void controller_wake(void)
{
    if (!parked) return;
    attach();
    (void)start_and_log(CONTROLLER_USB);
}

esp_err_t controller_select(controller_transport_t transport)
{
    controller_wake();
    const controller_transport_t previous = active;
    if (transport == previous && running) return transport == saved ? ESP_OK : save(transport);
    esp_err_t error = running ? stop_transport(previous) : ESP_OK;
    if (error) {
        ESP_LOGW(TAG, "PLAYER_CONTROLLER,transport,%s,result,%s", controller_transport_name(transport), esp_err_to_name(error));
        return error;
    }
    running = false;
    error = save(transport);
    if (!error) { console_mode = false; error = start_and_log(transport); }
    if (error) {
        (void)save(previous);
        (void)start_and_log(previous);
    }
    return error;
}

/* Startup bypass only: retain serial without changing the saved USB preference. */
void controller_request_console(void) { console_mode = true; }

esp_err_t controller_toggle_usb(void)
{
    if (active != CONTROLLER_USB) return ESP_OK;
    esp_err_t error;
    if (parked) {
        attach();
        running = console_mode = true;
        error = ESP_OK;
    } else if (!console_mode) {
        error = stop_transport(CONTROLLER_USB);
        if (!error) console_mode = true;
    } else {
        console_mode = false;
        error = start_transport(CONTROLLER_USB);
        if (error) console_mode = !usb_gamepad_running();
    }
    ESP_LOGI(TAG, "PLAYER_USB_ACCESS,serial,%u,result,%s", console_mode, esp_err_to_name(error));
    return error;
}

bool controller_bluetooth_started(void) { return bluetooth_started; }

bool controller_connected(void)
{
#ifdef INKBOY_DIAGNOSTICS
    if (diagnostics_controller() >= 0) return true;
#endif
    if (!running || (active == CONTROLLER_USB && console_mode)) return false;
    return active == CONTROLLER_USB ? usb_gamepad_running() && atomic_load(&usb_report_seen) : ble_gamepad_connected();
}

uint8_t controller_raw(void)
{
#ifdef INKBOY_DIAGNOSTICS
    int simulated = diagnostics_controller();
    if (simulated >= 0) return (uint8_t)simulated;
#endif
    if (!controller_connected()) return 0;
    return active == CONTROLLER_USB ? atomic_load(&usb_mask) : ble_gamepad_raw();
}

uint32_t controller_reports(void)
{
    return active == CONTROLLER_USB ? atomic_load(&usb_reports) : ble_gamepad_reports();
}

controller_status_t controller_status(void)
{
    if (active == CONTROLLER_USB && console_mode) return CONTROLLER_USB_PC;
    unsigned base = active == CONTROLLER_USB ? CONTROLLER_USB_OFF : CONTROLLER_BLE_OFF;
    return (controller_status_t)(base + (!running && !parked ? 0 : controller_connected() ? 2 : 1));
}

static bool detecting(void)
{
    bool on = active == CONTROLLER_USB && !console_mode;
    return on;
}

static void plugged_isr(void *unused)
{
    (void)unused;
    input_notify_from_isr();
}

/* A failed stop leaves the host running; parking stays off until the next connection. */
static bool park(bool after_probe)
{
    esp_err_t error = stop_transport(CONTROLLER_USB);
    if (!error) {
        running = false;
        if ((error = usb_detect_begin())) (void)start_and_log(CONTROLLER_USB);
    }
    if (error) probe_until = INT64_MAX;
    else {
        parked = true;
        ignore_dp = after_probe && usb_detect_plugged();
        if (!ignore_dp) usb_detect_watch(true, plugged_isr, NULL);
    }
    ESP_LOGI(TAG, "PLAYER_USB_DETECT,park,%s,after_probe,%u,ignore,%u", esp_err_to_name(error), after_probe, ignore_dp);
    return !error;
}

static void probe(void)
{
    attach();
    const esp_err_t error = start_and_log(CONTROLLER_USB);
    /* Back to detection so the next plug-in retries instead of leaving USB off. */
    if (error && usb_detect_begin() == ESP_OK) parked = ignore_dp = true;
}

bool controller_menu_service(void)
{
    if (!detecting()) return false;
    if (!parked) {
        if (!running) return false;
        if (controller_connected()) { probe_until = 0; return false; }
        if (esp_timer_get_time() < probe_until || !park(probe_until != 0)) return false;
        if (ignore_dp || !usb_detect_plugged()) return true;
    } else if (ignore_dp) {
        if (!usb_detect_plugged()) { ignore_dp = false; usb_detect_watch(true, plugged_isr, NULL); }
        return false;
    } else if (!usb_detect_plugged()) {
        usb_detect_watch(true, plugged_isr, NULL);
        return false;
    }
    probe();
    return true;
}

int64_t controller_menu_deadline(void)
{
    return detecting() && running && !controller_connected() ? probe_until : INT64_MAX;
}

bool controller_parked(void) { return parked; }
bool controller_plug_pending(void) { return parked && detecting() && usb_detect_plugged() != ignore_dp; }
bool controller_host_running(void) { return usb_gamepad_running(); }

esp_err_t controller_plug_wakeup(bool enable)
{
    if (!enable) {
        if (!plug_wake) return ESP_OK;
        plug_wake = false;
        return usb_detect_wakeup(false);
    }
    if (!parked || ignore_dp || !detecting()) return ESP_OK;
    const esp_err_t error = usb_detect_wakeup(true);
    plug_wake = !error;
    return error;
}
