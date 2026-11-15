#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"

uint8_t input_buttons(void);

enum { INPUT_NONE, INPUT_NEXT, INPUT_SELECT, INPUT_PREVIOUS, INPUT_BACK, INPUT_PC };
unsigned input_hardware_event(void);
void input_clear_events(void);
void input_discard_menu_events(void);
void input_set_gameplay(bool enabled);
void input_sample_hardware(void);
esp_err_t input_start_hardware_watch(void);
/* Blocks until a board-button change or event, input_notify() or the deadline (INT64_MAX: none). */
void input_wait(int64_t deadline);
void input_notify(void);
/* Makes the button task resample, e.g. after a simulated diagnostics press. */
void input_watch_wake(void);
/* Menu mode, both buttons released and debounced, no hold/chord timer and no queued event. */
bool input_board_idle(void);
/* Level-low light-sleep wake on BOOT and PWR; fails when the buttons fell back to polling. */
esp_err_t input_sleep_wakeup(bool enable);
void input_notify_from_isr(void);

void input_load_mapping(nvs_handle_t settings);
bool input_mapping_valid(void);
bool input_calibrate(void);
void input_remap(void);

bool input_connected(void);
uint32_t input_report_count(void);
bool input_wait_controller(void);
void input_wait_release(void);
