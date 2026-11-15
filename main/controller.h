#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"

typedef enum { CONTROLLER_BLUETOOTH, CONTROLLER_USB, CONTROLLER_TRANSPORT_COUNT } controller_transport_t;

/* Loads the saved transport; an unknown stored value means Bluetooth. */
void controller_load(nvs_handle_t settings);
controller_transport_t controller_transport(void);
const char *controller_transport_name(controller_transport_t transport);

/* Starts the saved transport. A USB failure falls back to Bluetooth for this
 * boot without changing the saved choice. */
esp_err_t controller_start(void);

/* Switches transport and saves the choice. On failure the previous transport
 * is restarted; controller_running() reports whether that worked. */
esp_err_t controller_select(controller_transport_t transport);
bool controller_running(void);

bool controller_connected(void);
uint8_t controller_raw(void);
uint32_t controller_reports(void);

/* Startup serial bypass and runtime USB controller/serial toggle. Neither saves NVS. */
void controller_request_console(void);
esp_err_t controller_toggle_usb(void);

/* Menu plug detection (USB transport, not PC mode): without a controller the host is parked (stopped,
 * D+ read as a GPIO); D+ high starts it for one 2 s attempt. True when it stopped or started the host. */
bool controller_menu_service(void);
/* End of the current connection attempt, else INT64_MAX. */
int64_t controller_menu_deadline(void);
bool controller_parked(void);
/* D+ asks controller_menu_service() to act: high while watched, or low after a failed attempt. */
bool controller_plug_pending(void);
/* Light-sleep wake on D+ high while parked and watching; a no-op otherwise. */
esp_err_t controller_plug_wakeup(bool enable);
/* Before gameplay or a controller wait: restores the pads and starts the host if parked. */
void controller_wake(void);
/* The USB host cannot run in light sleep. */
bool controller_host_running(void);
/* The Bluetooth controller stays enabled after its first start and then blocks light sleep. */
bool controller_bluetooth_started(void);

/* Snapshot used by menu badges; selected transport and live connection are separate. */
typedef enum { CONTROLLER_BLE_OFF, CONTROLLER_BLE_WAIT, CONTROLLER_BLE_CONNECTED,
               CONTROLLER_USB_OFF, CONTROLLER_USB_WAIT, CONTROLLER_USB_CONNECTED,
               CONTROLLER_USB_PC } controller_status_t;
controller_status_t controller_status(void);
