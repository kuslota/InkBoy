#include "menu_idle.h"
#include "fake_menu_idle.h"
#include "host_fakes.h"

bool (*fake_menu_idle_hook)(void);
unsigned fake_menu_idle_calls;
int64_t fake_menu_idle_last_deadline, fake_menu_idle_last_at;

/* Without a hook it "sleeps" to a finite deadline; INT64_MAX returns at once, as if pressed. */
bool menu_idle_wait(int64_t deadline)
{
    ++fake_menu_idle_calls;
    fake_menu_idle_last_deadline = deadline; fake_menu_idle_last_at = host_clock_us;
    if (fake_menu_idle_hook) return fake_menu_idle_hook();
    if (deadline != INT64_MAX && deadline > host_clock_us) host_clock_us = deadline;
    return true;
}
