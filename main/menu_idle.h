#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A PC plugged in while asleep cannot enumerate on the sleeping USB-Serial-JTAG and nothing wakes
 * the chip for it, so USB: PC mode stays awake this long after input. */
#define MENU_PC_AWAKE_US 10000000LL

/* input_wait() in automatic light sleep when nothing can be lost; the waking press stays queued.
 * Ends on BOOT, PWR, a parked D+ change or the deadline. True if it slept. */
bool menu_idle_wait(int64_t deadline);
