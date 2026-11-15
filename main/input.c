#include "input.h"
#include "diagnostics.h"
#include "controller.h"
#include "gb_emulator.h"
#include "screen.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "player";
/* Indexed by the Game Boy button bit: A,B,Select,Start,Right,Left,Up,Down. */
static uint8_t mapping[8];
static nvs_handle_t settings;
static int read_pin(int pin)
{
#ifdef INKBOY_DIAGNOSTICS
    int simulated = diagnostics_gpio(pin);
    if (simulated >= 0) return simulated;
#endif
    return gpio_get_level(pin);
}

uint8_t input_buttons(void)
{
    uint8_t raw = controller_raw(), result = 0;
    for (unsigned i = 0; i < 8; ++i) if (raw & mapping[i]) result |= 1u << i;
    return result;
}

typedef struct { bool raw, stable, handled; int64_t changed, pressed; } button_state_t;
static button_state_t board_buttons[2];
static _Atomic bool gameplay;
static bool chord, chord_reported;
static int64_t chord_started;
static unsigned events[32];
static _Atomic unsigned event_read, event_write;
static _Atomic bool board_quiet;
static SemaphoreHandle_t waiter;

void input_notify(void) { if (waiter) xSemaphoreGive(waiter); }

void input_wait(int64_t deadline)
{
    const int64_t now = esp_timer_get_time();
    if (!waiter || deadline <= now) return;
    const int64_t ms = (deadline - now + 999) / 1000;
    (void)xSemaphoreTake(waiter, deadline == INT64_MAX || ms >= 86400000 ? portMAX_DELAY : pdMS_TO_TICKS(ms));
}

static void enqueue(unsigned event)
{
    unsigned write = atomic_load(&event_write), read = atomic_load(&event_read);
    if (write - read < 32) { events[write % 32] = event;
#ifdef INKBOY_DIAGNOSTICS
        ESP_LOGI("test", "PLAYER_BUTTON,event,%u,game,%u",event,atomic_load(&gameplay));
#endif
         atomic_store(&event_write, write + 1); input_notify(); }
}

unsigned input_hardware_event(void)
{
    unsigned read = atomic_load(&event_read);
    if (read == atomic_load(&event_write)) return INPUT_NONE;
    unsigned event = events[read % 32];
    atomic_store(&event_read, read + 1);
    if (event == INPUT_PC) (void)controller_toggle_usb();
    return event;
}

void input_set_gameplay(bool enabled)
{
    if (enabled) controller_wake();
    atomic_store(&gameplay, enabled);
}
void input_clear_events(void) { while (input_hardware_event()) {} }
void input_discard_menu_events(void)
{
    atomic_store(&event_read, atomic_load(&event_write));
    for (unsigned i = 0; i < 2; ++i) board_buttons[i].handled = true;
}

/* True while a debounce, the 0.7 s hold, the 2 s chord or a simulated press still needs 10 ms samples. */
static bool sampling_needed(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        const button_state_t *b = &board_buttons[i];
        if (b->raw != b->stable || (b->stable && !b->handled)) return true;
    }
    if (board_buttons[0].raw && board_buttons[1].raw && !chord_reported) return true;
    return false;
}

bool input_board_idle(void)
{
    return atomic_load(&board_quiet) && !atomic_load(&gameplay) &&
           atomic_load(&event_read) == atomic_load(&event_write);
}

void input_sample_hardware(void)
{
    int64_t now = esp_timer_get_time();
    bool down[2] = {read_pin(GPIO_NUM_0) == 0, read_pin(GPIO_NUM_18) == 0};
    if (down[0] && down[1]) {
        if (!chord) {
            if (atomic_load(&gameplay)) enqueue(INPUT_BACK);
            chord=true;chord_reported=false;
        }
        if (!chord_started) chord_started=now;
        if (!chord_reported && now-chord_started>=2000000) {
            enqueue(INPUT_PC);chord_reported=true;
        }
    } else chord_started=0;
    for (unsigned i = 0; i < 2; ++i) {
        button_state_t *b = &board_buttons[i];
        bool raw = down[i];
        if (raw != b->raw) { b->raw = raw; b->changed = now; }
        if (now - b->changed >= 30000 && b->stable != raw) {
            b->stable = raw;
            if (raw) { b->pressed = now; b->handled = chord; }
            else if (!b->handled) { enqueue(i ? INPUT_NEXT : INPUT_SELECT); b->handled = true; }
        }
        if (chord) b->handled = true;
        if (!b->stable || b->handled) continue;
        if (atomic_load(&gameplay)) {
            enqueue(i ? INPUT_NEXT : INPUT_SELECT); b->handled = true;
        } else if (now - b->pressed >= 700000) {
            enqueue(i ? INPUT_BACK : INPUT_PREVIOUS); b->handled = true;
        }
    }
    if (!down[0] && !down[1] && !board_buttons[0].stable && !board_buttons[1].stable) chord = false;
    atomic_store(&board_quiet, !down[0] && !down[1] && !sampling_needed());
}

static TaskHandle_t watch_task;
static _Atomic bool interrupts;

void input_watch_wake(void) { if (watch_task) xTaskNotifyGive(watch_task); }

static void button_isr(void *unused)
{
    (void)unused;
    /* Level types would retrigger while held; the task re-arms once sampling is done. */
    gpio_intr_disable(GPIO_NUM_0); gpio_intr_disable(GPIO_NUM_18);
    BaseType_t woken = pdFALSE;
    if (watch_task) vTaskNotifyGiveFromISR(watch_task, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* Level, not edge: a change between the last sample and arming fires at once instead of being lost. */
static void arm(bool on)
{
    for (unsigned i = 0; i < 2; ++i) {
        const gpio_num_t pin = i ? GPIO_NUM_18 : GPIO_NUM_0;
        if (!on) { gpio_intr_disable(pin); continue; }
        gpio_set_intr_type(pin, board_buttons[i].raw ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
        gpio_intr_enable(pin);
    }
}

esp_err_t input_sleep_wakeup(bool enable)
{
    if (!atomic_load(&interrupts)) return ESP_ERR_INVALID_STATE;
    if (!enable) {
        esp_err_t error = gpio_wakeup_disable(GPIO_NUM_0), other = gpio_wakeup_disable(GPIO_NUM_18);
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        return error ? error : other;
    }
    esp_err_t error = gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
    if (!error) error = gpio_wakeup_enable(GPIO_NUM_18, GPIO_INTR_LOW_LEVEL);
    if (!error) error = esp_sleep_enable_gpio_wakeup();
    if (error) (void)input_sleep_wakeup(false);
    return error;
}
void input_notify_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (waiter) xSemaphoreGiveFromISR(waiter, &woken);
    if (woken) portYIELD_FROM_ISR();
}


/* Arming the level BOOT already has must interrupt at once; otherwise keep the old 10 ms polling. */
static bool interrupts_work(void)
{
    gpio_set_intr_type(GPIO_NUM_0, gpio_get_level(GPIO_NUM_0) ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    gpio_intr_enable(GPIO_NUM_0);
    const bool fired = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) > 0;
    arm(false);
    return fired;
}

static void watch_buttons(void *unused)
{
    (void)unused;
    if (atomic_load(&interrupts) && !interrupts_work()) {
        atomic_store(&interrupts, false);
        ESP_LOGW(TAG, "PLAYER_BUTTONS,interrupts,self_test_failed,polling");
    }
    for (bool woken = false;;) {
        input_sample_hardware();
        if (woken) { input_notify(); woken = false; }
        if (!atomic_load(&interrupts) || sampling_needed()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        arm(true);
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        arm(false);
        woken = true;
    }
}

esp_err_t input_start_hardware_watch(void)
{
    const gpio_config_t buttons = {.pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_18),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    esp_err_t error = gpio_config(&buttons);
    if (error) return error;
    for (unsigned i = 0; i < 2; ++i) {
        bool down = read_pin(i ? GPIO_NUM_18 : GPIO_NUM_0) == 0;
        board_buttons[i] = (button_state_t){.raw = down, .stable = down, .handled = true};
    }
#ifdef INKBOY_DIAGNOSTICS
    diagnostics_start();
#endif
    atomic_store(&board_quiet, !board_buttons[0].raw && !board_buttons[1].raw);
    if (!waiter && !(waiter = xSemaphoreCreateBinary())) return ESP_ERR_NO_MEM;
    /* The panel driver tolerates an installed service and then never uninstalls it. */
    esp_err_t isr = gpio_install_isr_service(0);
    if (isr == ESP_ERR_INVALID_STATE) isr = ESP_OK;
    if (!isr) isr = gpio_isr_handler_add(GPIO_NUM_0, button_isr, NULL);
    if (!isr) isr = gpio_isr_handler_add(GPIO_NUM_18, button_isr, NULL);
    arm(false);
    atomic_store(&interrupts, isr == ESP_OK);
    if (isr) ESP_LOGW(TAG, "PLAYER_BUTTONS,interrupts,%s,polling", esp_err_to_name(isr));
    return xTaskCreate(watch_buttons, "player_buttons", 2048, NULL, 2, &watch_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool input_connected(void) { return controller_connected(); }
uint32_t input_report_count(void) { return controller_reports(); }

bool input_wait_controller(void)
{
    if (controller_connected()) return true;
    controller_wake();
    const bool usb = controller_transport() == CONTROLLER_USB;
    if (!screen_message("ON / CONNECT", usb ? "PLUG IN THE USB CONTROLLER" : "TURN ON ESP32 GAMEPAD",
                        usb ? "CONNECTING OVER USB" : "CONNECTING AUTOMATICALLY", "HOLD PWR TO RETURN")) return false;
    while (!controller_connected()) { if ((input_hardware_event() >= INPUT_BACK)) return false;
        (void)screen_service(); vTaskDelay(pdMS_TO_TICKS(20)); }
    return true;
}

void input_wait_release(void)
{
    while (controller_raw() || read_pin(GPIO_NUM_0) == 0 || read_pin(GPIO_NUM_18) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(40));
    input_clear_events();
}

bool input_mapping_valid(void)
{
    uint8_t used = 0;
    for (unsigned i=0;i<8;++i) {
        uint8_t b = mapping[i];
        if (!b || (b & (b-1)) || (used & b)) return false;
        used |= b;
    }
    return used == 255;
}

static bool mapping_release(void)
{
    while (controller_raw() || read_pin(GPIO_NUM_0) == 0 || read_pin(GPIO_NUM_18) == 0) {
        if (!controller_connected() || (input_hardware_event() >= INPUT_BACK)) return false;
        if (screen_service() < 0) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool input_calibrate(void)
{
    static const char *names[] = {"PRESS UP", "PRESS DOWN", "PRESS LEFT", "PRESS RIGHT",
                                  "PRESS A", "PRESS B", "PRESS START", "PRESS SELECT"};
    static const uint8_t order[] = {6,7,5,4,0,1,3,2};
    uint8_t used = 0;
    uint8_t candidate[8] = {0};
    for (unsigned step=0;step<8;++step) {
        if (!input_wait_controller()) return false;
        if (!mapping_release()) return false;
        if (!screen_prompt(names[step], "PRESS ONE CONTROLLER BUTTON")) return false;
        ESP_LOGI(TAG, "MAP_WAIT,step,%u,control,%s", step, names[step]);
        for (;;) {
            if (!controller_connected() || (input_hardware_event() >= INPUT_BACK)) return false;
            if (screen_service() < 0) return false;
            uint8_t raw = controller_raw();
            if (raw && !(raw & (raw-1)) && !(raw & used)) {
                candidate[order[step]] = raw;
                used |= raw;
                ESP_LOGI(TAG, "MAP_BUTTON,control,%s,raw,%02x", names[step], raw);
                if (!mapping_release()) return false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    esp_err_t error = nvs_set_blob(settings, "buttons", candidate, sizeof(candidate));
    if (!error) error = nvs_commit(settings);
    if (error) {
        (void)nvs_set_blob(settings, "buttons", mapping, sizeof(mapping));
        (void)nvs_commit(settings);
        screen_message("SETUP ERROR", "CANNOT SAVE BUTTON MAPPING", "PREVIOUS MAPPING KEPT", "");
        return false;
    }
    memcpy(mapping, candidate, sizeof(mapping));
    ESP_LOGI(TAG, "MAP_SAVED");
    return true;
}

void input_remap(void)
{
    uint8_t old_mapping[8]; memcpy(old_mapping, mapping, sizeof(mapping));
    if (!input_calibrate()) memcpy(mapping, old_mapping, sizeof(mapping));
}

void input_load_mapping(nvs_handle_t nvs)
{
    settings = nvs;
    size_t map_size = sizeof(mapping);
    if (nvs_get_blob(settings, "buttons", mapping, &map_size) != ESP_OK || map_size != sizeof(mapping) || !input_mapping_valid())
        memset(mapping, 0, sizeof(mapping));
    if (!input_mapping_valid()) {
        /* Down=GPIO0, Left=1, Up=2, Right=3, Select=4, B=5, A=6, Start=7 on the
         * controller. A saved mapping wins and nothing is written here. */
        const uint8_t published_wiring[] = {0x40,0x20,0x10,0x80,0x08,0x02,0x04,0x01};
        memcpy(mapping, published_wiring, sizeof(mapping));
        ESP_LOGI(TAG, "PLAYER_MAPPING: using the published controller wiring");
    }
}
