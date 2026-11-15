#pragma once
#include "input.h"

/* Scripted stand-in for main/input.c. Hooks default to "nothing pressed". */
extern uint8_t (*fake_input_buttons)(void);
extern unsigned (*fake_input_boot_event)(void);
extern bool (*fake_input_connected)(void);
extern bool fake_input_controller_ready, fake_input_mapping_valid;
extern unsigned fake_input_remaps, fake_input_releases;
/* input_wait(): last deadline, call count; by default each call advances the clock by at most 10 ms. */
extern int64_t fake_input_last_deadline, fake_input_last_wait_at;
extern unsigned fake_input_waits;
extern bool fake_input_wait_to_deadline;
