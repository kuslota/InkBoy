#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Stand-in for main/menu_idle.c: records each wait; the hook, when set, decides what happens. */
extern bool (*fake_menu_idle_hook)(void);
extern unsigned fake_menu_idle_calls;
extern int64_t fake_menu_idle_last_deadline, fake_menu_idle_last_at;
