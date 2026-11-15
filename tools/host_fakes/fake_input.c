#include "fake_input.h"
#include "host_fakes.h"

uint8_t (*fake_input_buttons)(void);
unsigned (*fake_input_boot_event)(void);
bool (*fake_input_connected)(void);
bool fake_input_controller_ready = true, fake_input_mapping_valid = true;
unsigned fake_input_remaps, fake_input_releases;

uint8_t input_buttons(void) { return fake_input_buttons ? fake_input_buttons() : 0; }
unsigned input_hardware_event(void) { return fake_input_boot_event ? fake_input_boot_event() : 0; }
esp_err_t input_start_hardware_watch(void) { return ESP_OK; }
void input_load_mapping(nvs_handle_t settings) { (void)settings; }
bool input_mapping_valid(void) { return fake_input_mapping_valid; }
bool input_calibrate(void) { return true; }
void input_remap(void) { ++fake_input_remaps; }
bool input_connected(void) { return fake_input_connected ? fake_input_connected() : true; }
uint32_t input_report_count(void) { return 0; }
bool input_wait_controller(void) { return fake_input_controller_ready; }
void input_wait_release(void) { ++fake_input_releases; }

void input_clear_events(void) { (void)input_hardware_event(); }
void input_set_gameplay(bool enabled) { (void)enabled; }
void input_sample_hardware(void) {}

int64_t fake_input_last_deadline, fake_input_last_wait_at;
unsigned fake_input_waits;
bool fake_input_wait_to_deadline;
/* Each wait models one wake: at most 10 ms passes, like the old polling loop, unless a test
 * asks to jump straight to the deadline. */
void input_wait(int64_t deadline)
{
    ++fake_input_waits;
    fake_input_last_deadline = deadline; fake_input_last_wait_at = host_clock_us;
    const int64_t limit = fake_input_wait_to_deadline ? deadline : host_clock_us + 10000;
    if (deadline > host_clock_us) host_clock_us = deadline < limit ? deadline : limit;
}
void input_notify(void) {}
void input_watch_wake(void) {}
bool input_board_idle(void) { return true; }
esp_err_t input_sleep_wakeup(bool enable) { (void)enable; return ESP_OK; }
