#include "ble_gamepad.h"
#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "gamepad";
static _Atomic bool connected, stopping, task_running;
static _Atomic uint32_t raw_buttons, last_report_ms, reports;
static _Atomic(esp_hidh_dev_t *) device;
static bool stack_ready;

enum { STOP_WAIT_MS = 10000, CLOSE_WAIT_MS = 2000, POLL_MS = 20 };

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }

static void hid_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_hidh_event_data_t *event = data;
    switch (id) {
    case ESP_HIDH_OPEN_EVENT:
        atomic_store(&raw_buttons, 0);
        atomic_store(&last_report_ms, now_ms());
        atomic_store(&connected, event->open.status == ESP_OK);
        ESP_LOGI(TAG, "PAD_OPEN,status,%d,name,%s", event->open.status,
                 event->open.status == ESP_OK ? esp_hidh_dev_name_get(event->open.dev) : "failed");
        if (event->open.status == ESP_OK) esp_hidh_dev_dump(event->open.dev, stdout);
        break;
    case ESP_HIDH_INPUT_EVENT:
        /* ESP32-BLE-Gamepad 0.7.4: report 3 begins with the little-endian
         * button bitmap. esp_hidh reports ID separately, outside data[]. */
        if (event->input.report_id == 3 && event->input.length >= 2) {
            uint8_t buttons = event->input.data[0];
            uint32_t previous = atomic_exchange(&raw_buttons, buttons);
            atomic_store(&last_report_ms, now_ms());
            atomic_fetch_add(&reports, 1);
            if (previous != buttons) ESP_LOGI(TAG, "PAD_BUTTONS,%02x", buttons);
        }
        break;
    case ESP_HIDH_CLOSE_EVENT:
        atomic_store(&raw_buttons, 0);
        atomic_store(&connected, false);
        atomic_store(&device, NULL);
        ESP_LOGI(TAG, "PAD_DISCONNECTED: released all buttons; rescanning");
        break;
    default: break;
    }
}

static void connect_task(void *unused)
{
    (void)unused;
    /* Rest between scans. It starts at the historical 2 s, so a controller
     * switched on near boot is found as fast as before, and doubles after
     * every scan that did not see the pad, up to 20 s: a board left sitting
     * in the menus with its controller off otherwise spends most of its time
     * on the radio, scanning 3 s out of every 5. Finding the pad (or being
     * connected) resets the rest. Stop requests still land within 100 ms. */
    unsigned rest_ms = 2000;
    while (!atomic_load(&stopping)) {
        bool found = false;
        if (!atomic_load(&connected)) {
            size_t count = 0;
            esp_hid_scan_result_t *results = NULL;
            ESP_LOGI(TAG, "PAD_SCAN: looking for ESP32 Gamepad");
            esp_err_t error = esp_hid_scan(3, &count, &results);
            if (error == ESP_OK) {
                for (esp_hid_scan_result_t *r = results; r && !atomic_load(&stopping); r = r->next) {
                    if (r->transport == ESP_HID_TRANSPORT_BLE && r->name &&
                        strcmp(r->name, "ESP32 Gamepad") == 0) {
                        found = true;
                        ESP_LOGI(TAG, "PAD_CONNECT,rssi,%d", r->rssi);
                        /* esp_hidh's BLE open waits for discovery completion. */
                        esp_hidh_dev_t *opened = esp_hidh_dev_open(r->bda, r->transport, r->ble.addr_type);
                        if (opened) atomic_store(&device, opened);
                        else ESP_LOGW(TAG, "PAD_CONNECT_FAILED; retrying");
                        break;
                    }
                }
            } else ESP_LOGW(TAG, "PAD_SCAN_FAILED,%s", esp_err_to_name(error));
            esp_hid_scan_results_free(results);
        }
        if (found || atomic_load(&connected)) rest_ms = 2000;
        for (unsigned waited = 0; waited < rest_ms && !atomic_load(&stopping); waited += 100)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (!found && !atomic_load(&connected))
            rest_ms = rest_ms >= 10000 ? 20000 : rest_ms * 2;
    }
    atomic_store(&task_running, false);
    vTaskDelete(NULL);
}

static esp_err_t init_stack(void)
{
    esp_err_t error = esp_hid_gap_init(HIDH_BLE_MODE);
    if (error != ESP_OK) return error;
    error = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (error != ESP_OK) return error;
    esp_ble_auth_req_t auth = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t io = ESP_IO_CAP_NONE;
    error = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    if (error != ESP_OK) return error;
    error = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io));
    if (error != ESP_OK) return error;
    const esp_hidh_config_t config = {
        .callback = hid_event, .event_stack_size = 4096, .callback_arg = NULL,
    };
    return esp_hidh_init(&config);
}

esp_err_t ble_gamepad_start(void)
{
    if (!stack_ready) {
        esp_err_t error = init_stack();
        if (error != ESP_OK) return error;
        stack_ready = true;
    }
    atomic_store(&stopping, false);
    if (atomic_exchange(&task_running, true)) return ESP_OK;
    if (xTaskCreatePinnedToCore(connect_task, "pad_connect", 6144, NULL, 2, NULL, 0) == pdPASS) return ESP_OK;
    atomic_store(&task_running, false);
    return ESP_ERR_NO_MEM;
}

static bool wait_for(bool (*done)(void), unsigned timeout_ms)
{
    for (unsigned waited = 0; !done(); waited += POLL_MS) {
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    return true;
}
static bool task_stopped(void) { return !atomic_load(&task_running); }
static bool device_closed(void) { return atomic_load(&device) == NULL; }

/* The Bluetooth stack and esp_hidh stay initialised: deinitialising them
 * is not needed to stop scanning or release the controller, and restarting is cheap. */
esp_err_t ble_gamepad_stop(void)
{
    atomic_store(&stopping, true);
    if (!wait_for(task_stopped, STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_hidh_dev_t *opened = atomic_load(&device);
    if (opened) {
        if (esp_hidh_dev_close(opened) != ESP_OK) atomic_store(&device, NULL);
        else if (!wait_for(device_closed, CLOSE_WAIT_MS)) return ESP_ERR_TIMEOUT;
    }
    atomic_store(&raw_buttons, 0);
    atomic_store(&connected, false);
    return ESP_OK;
}

bool ble_gamepad_connected(void)
{
    return atomic_load(&connected) && atomic_load(&reports) > 0 &&
           now_ms() - atomic_load(&last_report_ms) < 2000;
}
uint8_t ble_gamepad_raw(void) { return ble_gamepad_connected() ? atomic_load(&raw_buttons) : 0; }
uint32_t ble_gamepad_reports(void) { return atomic_load(&reports); }
