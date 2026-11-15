#!/usr/bin/env python3
"""Test real GPIO gestures and mapping updates through the real screen context."""
from host_build import run
CODE = r'''
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include "input.h"
#include "screen.h"
#include "controller.h"
#include "display_config.h"
#include "driver/gpio.h"
#include "host_fakes.h"
static unsigned fulls, updates, prompts, cancel_step, console_requests;
static bool raw_pending;
static void sample(int pin, int level, int64_t delay) {
    host_gpio_level[pin]=level; host_clock_us+=delay; input_sample_hardware();
}
static void init(void) {
    host_fakes_reset(); input_set_gameplay(false); input_clear_events();
    assert(input_start_hardware_watch()==ESP_OK);
}
static void tap(unsigned pin, unsigned expected) {
    sample(pin,0,1); sample(pin,0,29999); assert(input_hardware_event()==0);
    sample(pin,0,1); sample(pin,1,100000); sample(pin,1,30000);
    assert(input_hardware_event()==expected && !input_hardware_event());
}
static void hold(unsigned pin, unsigned expected) {
    sample(pin,0,1); sample(pin,0,30000); sample(pin,0,699999); assert(!input_hardware_event());
    sample(pin,0,1); assert(input_hardware_event()==expected);
    sample(pin,0,1000000); sample(pin,1,1); sample(pin,1,30000); assert(!input_hardware_event());
}
typedef struct { int64_t at; int pin, level; } step_t;
static step_t script[8];
static unsigned script_length, script_next;
static void play(void) {
    while (script_next < script_length && host_clock_us >= script[script_next].at) {
        host_gpio_drive(script[script_next].pin, script[script_next].level); ++script_next;
    }
}
static void later(int64_t delay, int pin, int level) {
    script[script_length++] = (step_t){host_clock_us + delay, pin, level}; host_delay_hook = play;
}
static jmp_buf stop;
static int64_t stop_at;
/* A polling task never blocks, so leave it once the scripted presses are over. */
static void poll_until_stop(void) { play(); if (host_clock_us >= stop_at) longjmp(stop, 1); }
static bool armed(int pin, int type) { return host_gpio_interrupt_armed(pin, type); }
/* Runs the real button task until it blocks; returns how many 10 ms polls it made. */
static unsigned run_watch(void) {
    TaskHandle_t task = host_task_find("player_buttons"); assert(task);
    unsigned before = host_task_delays(); host_task_run(task); return host_task_delays() - before;
}
static void interrupts(const char *which) {
    /* host_task_run() restarts the task function, so each run repeats the start-up self-test interrupt. */
    assert(run_watch() == 0 && host_gpio_isr_calls == 1);
    assert(armed(0, GPIO_INTR_LOW_LEVEL) && armed(18, GPIO_INTR_LOW_LEVEL) && input_board_idle());
    host_clock_us += 60000000; assert(run_watch() == 0); /* Released: nothing polls. */
    if (!strcmp(which, "irq_tap")) {
        const unsigned calls = host_gpio_isr_calls;
        later(100000, 0, 1); host_gpio_drive(0, 0);
        assert(host_gpio_isr_calls == calls + 1 && !armed(0, GPIO_INTR_LOW_LEVEL) && !armed(18, GPIO_INTR_LOW_LEVEL));
        unsigned polls = run_watch();
        assert(polls >= 12 && polls <= 15 && input_hardware_event() == INPUT_SELECT && !input_hardware_event());
        assert(armed(0, GPIO_INTR_LOW_LEVEL) && armed(18, GPIO_INTR_LOW_LEVEL) && input_board_idle());
        later(20000, 18, 1); host_gpio_drive(18, 0); /* A 20 ms bounce never debounces into an event. */
        assert(run_watch() <= 6 && !input_hardware_event() && input_board_idle());
    } else if (!strcmp(which, "irq_hold")) {
        host_gpio_drive(18, 0); unsigned polls = run_watch();
        assert(polls >= 72 && polls <= 75 && input_hardware_event() == INPUT_BACK && !input_hardware_event());
        assert(armed(18, GPIO_INTR_HIGH_LEVEL) && armed(0, GPIO_INTR_LOW_LEVEL) && !input_board_idle()); /* Waits for release. */
        host_clock_us += 5000000; assert(run_watch() == 0);
        host_gpio_drive(18, 1); assert(run_watch() <= 4 && !input_hardware_event() && input_board_idle());
    } else if (!strcmp(which, "irq_chord")) {
        host_gpio_drive(0, 0); host_gpio_level[18] = 0; /* Second press while interrupts are off: polling sees it. */
        unsigned polls = run_watch();
        assert(polls >= 199 && polls <= 202 && input_hardware_event() == INPUT_PC && console_requests == 1 && !input_hardware_event());
        assert(armed(0, GPIO_INTR_HIGH_LEVEL) && armed(18, GPIO_INTR_HIGH_LEVEL));
        later(1, 18, 1); host_gpio_drive(0, 1); run_watch();
        assert(!input_hardware_event() && input_board_idle() && armed(0, GPIO_INTR_LOW_LEVEL));
    } else if (!strcmp(which, "irq_gameplay")) {
        input_set_gameplay(true); host_gpio_drive(0, 0);
        assert(run_watch() <= 4 && input_hardware_event() == INPUT_SELECT && armed(0, GPIO_INTR_HIGH_LEVEL));
        host_gpio_drive(0, 1); run_watch(); assert(!input_hardware_event() && !input_board_idle());
        input_set_gameplay(false); assert(input_board_idle());
    } else if (!strcmp(which, "irq_queue")) {
        host_gpio_drive(0, 0); later(100000, 0, 1); run_watch(); assert(!input_board_idle()); /* Queued event. */
        input_clear_events(); assert(input_board_idle());
        assert(input_sleep_wakeup(true) == ESP_OK && host_gpio_wake[0] && host_gpio_wake[18]);
        assert(host_trace_has("gpio_wakeup_enable(0,4)") && host_trace_has("gpio_wakeup_enable(18,4)") &&
               host_trace_has("esp_sleep_enable_gpio_wakeup"));
        assert(input_sleep_wakeup(false) == ESP_OK && !host_gpio_wake[0] && !host_gpio_wake[18] &&
               host_trace_has("esp_sleep_disable_wakeup_source(7)"));
        assert(gpio_install_isr_service(0) == ESP_ERR_INVALID_STATE); /* Service stays installed for the panel to share. */
    } else assert(0);
}
uint8_t controller_raw(void) {
    if (!raw_pending) return 0;
    raw_pending=false; return (uint8_t)(1u<<(prompts-1));
}
bool controller_connected(void) { return true; }
uint32_t controller_reports(void) { return 0; }
esp_err_t controller_toggle_usb(void) { ++console_requests; return ESP_OK; }
controller_transport_t controller_transport(void) { return CONTROLLER_BLUETOOTH; }
void controller_wake(void) {}
void screen_draw_message(uint8_t *p,const char *title,const char *a,const char *b,const char *c) {
    (void)p;(void)a;(void)b;(void)c;
    if (!strncmp(title,"PRESS ",6)) {
        ++prompts; raw_pending=true;
        if(prompts==cancel_step) { hold(18,INPUT_BACK); hold(18,INPUT_BACK); /* consumed checks; queue one final Back */
            sample(18,0,1); sample(18,0,30000); sample(18,0,700000); }
    }
}
esp_err_t ssd1681_create(const ssd1681_config_t *config, ssd1681_handle_t *d) { (void)config; *d=(ssd1681_handle_t)1; return ESP_OK; }
esp_err_t ssd1681_power_on(ssd1681_handle_t d) { (void)d;return ESP_OK; }
esp_err_t ssd1681_destroy(ssd1681_handle_t d) { (void)d;return ESP_OK; }
esp_err_t ssd1681_analog_power(ssd1681_handle_t d,bool enabled,ssd1681_timing_t *t) { (void)d;(void)enabled;(void)t;return ESP_OK; }
void fast_display_set_gray_game(const uint8_t *p) { (void)p; }
unsigned fast_display_pending(ssd1681_handle_t d) { (void)d;return 0; }
unsigned fast_display_gray_last_scans(void) { return 1; }
void fast_display_wait_slot(ssd1681_handle_t d,int64_t deadline) { (void)d;(void)deadline; }
esp_err_t fast_display_set_dither_pulses(unsigned b,unsigned w) { (void)b;(void)w;return ESP_OK; }
esp_err_t fast_display_full(ssd1681_handle_t d,const uint8_t *p,display_mode_t mode) { (void)d;(void)p;(void)mode; ++fulls; return ESP_OK; }
esp_err_t fast_display_update(ssd1681_handle_t d,const uint8_t *p) { (void)d;(void)p; ++updates; return ESP_OK; }
int main(int argc,char **argv) {
    assert(argc==2); init();
    if(!strcmp(argv[1],"gestures")) {
        tap(0,INPUT_SELECT); tap(18,INPUT_NEXT); hold(0,INPUT_PREVIOUS); hold(18,INPUT_BACK);
        input_set_gameplay(true);
        for(unsigned i=0;i<2;++i) {
            int pin=i?18:0; sample(pin,0,1);sample(pin,0,30000);
            assert(input_hardware_event()==(i?INPUT_NEXT:INPUT_SELECT));
            input_set_gameplay(false);sample(pin,1,10000);sample(pin,1,30000);assert(!input_hardware_event());
            input_set_gameplay(true);
        }
        input_set_gameplay(false);
        sample(0,0,1);sample(18,0,1);sample(0,0,30000);sample(18,0,700000);
        sample(0,1,1);sample(18,1,1);sample(0,1,30000);assert(!input_hardware_event());
        host_gpio_level[0]=0;assert(input_start_hardware_watch()==ESP_OK);
        sample(0,1,1000000);sample(0,1,30000);assert(!input_hardware_event());
    } else if(!strcmp(argv[1],"irq_broken")) {
        host_gpio_interrupts_broken = true; /* E.g. a masked interrupt: the self-test must catch it. */
        later(100000, 0, 0); later(200000, 0, 1); stop_at = host_clock_us + 400000;
        host_delay_hook = poll_until_stop;
        if (!setjmp(stop)) host_task_run(host_task_find("player_buttons"));
        assert(host_log_count("self_test_failed") == 1 && input_hardware_event() == INPUT_SELECT);
        assert(input_sleep_wakeup(true) == ESP_ERR_INVALID_STATE); /* Menu idle refuses while polling. */
    } else if(!strncmp(argv[1],"irq_",4)) {
        interrupts(argv[1]);
    } else if(!strcmp(argv[1],"console")) {
        sample(0,0,1);sample(18,0,1);sample(0,0,30000);
        sample(18,0,2000000);assert(input_hardware_event()==INPUT_PC && console_requests==1);
        sample(18,0,2000000);assert(!input_hardware_event() && console_requests==1);
        sample(0,1,1);sample(18,1,1);sample(0,1,30000);assert(!input_hardware_event());
    } else if(!strcmp(argv[1],"console_continuous")) {
        sample(0,0,1);sample(18,0,30000);sample(18,0,1000000);
        sample(0,1,1);sample(0,1,30000);sample(18,0,2000000);
        assert(!input_hardware_event() && console_requests==0);
        sample(0,0,1);sample(0,0,1999999);assert(!input_hardware_event());
        sample(0,0,1);assert(input_hardware_event()==INPUT_PC && console_requests==1);
    } else {
        nvs_handle_t n;assert(nvs_open("gamepad",NVS_READWRITE,&n)==ESP_OK);input_load_mapping(n);
        uint8_t old[8];size_t size=sizeof old;
        assert(input_mapping_valid());
        if(!strcmp(argv[1],"cancel")) cancel_step=4;
        if(!strcmp(argv[1],"failure")) host_nvs_commit_error=ESP_FAIL;
        bool ok=input_calibrate();
        if(!strcmp(argv[1],"map")) {
            assert(ok && prompts==8 && fulls==1 && updates==7 && host_nvs_commits==1);
            assert(nvs_get_blob(n,"buttons",old,&size)==ESP_OK);
            const uint8_t expected[]={16,32,128,64,8,4,1,2};assert(!memcmp(old,expected,8));
        } else assert(!ok && input_mapping_valid());
    }
    return 0;
}
'''
run(CODE, modules=['input.c','screen.c','display_config.c'], scenarios=[[x] for x in ['gestures','console','console_continuous','map','cancel','failure',
                                  'irq_tap','irq_hold','irq_chord','irq_gameplay','irq_queue','irq_broken']])
print('PASS: debounce, two-button short/hold actions, immediate gameplay events, chords/held wake, partial mapping and cancellation/save failure; '
      'interrupt-driven task: no polling while released, level re-arm per state, same tap/hold/chord/gameplay events, light-sleep wake arming, polling fallback when the self-test interrupt never arrives')
