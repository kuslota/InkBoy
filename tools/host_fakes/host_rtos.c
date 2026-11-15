#include "esp_pm.h"
#include "fast_display.h"
#include "game_session.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_fakes.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int64_t host_clock_us = 1;
int64_t host_delay_early_us;
unsigned host_task_priority = 1, host_tasks_created, host_mutexes_held;
void (*host_semaphore_wait_hook)(void);
TickType_t host_semaphore_wait_ticks;
bool host_task_create_fails, host_mutex_create_fails;

struct host_task { TaskFunction_t function; void *argument; char name[16]; unsigned notifications; };
struct host_mutex { bool locked, binary, available; };
struct host_timer { esp_timer_create_args_t args; };
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out) {
    *out=calloc(1,sizeof(**out));if (!*out) return ESP_ERR_NO_MEM;
    (*out)->args=*args;return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t us) {
    host_clock_us+=(int64_t)us;timer->args.callback(timer->args.arg);return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer) { (void)timer;return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t timer) { free(timer);return ESP_OK; }

char host_task_autorun_name[16];
void (*host_delay_hook)(void);
esp_pm_config_t host_pm_config = {.max_freq_mhz = 240, .min_freq_mhz = 40};
esp_err_t host_pm_configure_error;
static struct host_task *running, *created[16];
static unsigned delays;
static jmp_buf *blocked;

/* A task runs on the caller's stack until it blocks on an empty notification
 * or deletes itself; that is where control returns. */
void host_task_run(TaskHandle_t task)
{
    jmp_buf here;
    jmp_buf *outer_jump = blocked;
    struct host_task *outer = running;
    blocked = &here;
    running = task;
    if (!setjmp(here)) task->function(task->argument);
    running = outer;
    blocked = outer_jump;
}

int64_t esp_timer_get_time(void) { return host_clock_us; }

void vTaskDelay(TickType_t ticks)
{
    host_clock_us += ticks ? (int64_t)ticks * 1000 - host_delay_early_us : 0;
    if (ticks) ++delays;
    if (ticks && host_delay_hook) host_delay_hook();
}

unsigned host_task_delays(void) { return delays; }

TaskHandle_t host_task_find(const char *name)
{
    for (unsigned k = 0; k < host_tasks_created && k < 16; ++k) {
        struct host_task *task = created[(host_tasks_created - 1 - k) % 16];
        if (!strcmp(task->name, name)) return task;
    }
    return NULL;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return running; }

UBaseType_t uxTaskPriorityGet(TaskHandle_t task)
{
    assert(!task);
    return host_task_priority;
}

void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority)
{
    assert(!task);
    host_task_priority = priority;
}

BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack, void *argument,
                       UBaseType_t priority, TaskHandle_t *handle)
{
    (void)stack; (void)priority;
    if (host_task_create_fails) return 0;
    ++host_tasks_created;
    struct host_task *task = calloc(1, sizeof(*task));
    task->function = function;
    task->argument = argument;
    snprintf(task->name, sizeof(task->name), "%s", name);
    created[(host_tasks_created - 1) % 16] = task;
    if (handle) *handle = task;
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack,
                                   void *argument, UBaseType_t priority, TaskHandle_t *handle, int core)
{
    (void)core;
    return xTaskCreate(function, name, stack, argument, priority, handle);
}

void vTaskDelete(TaskHandle_t task)
{
    assert(!task && running);
    longjmp(*blocked, 1);
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    ++task->notifications;
    if (task != running && !strcmp(task->name, host_task_autorun_name)) host_task_run(task);
    return pdPASS;
}

void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken)
{
    (void)xTaskNotifyGive(task);
    if (woken) *woken = 0;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    assert(running);
    if (!running->notifications && wait != portMAX_DELAY) { vTaskDelay(wait); return 0; } /* Timed out. */
    if (!running->notifications) longjmp(*blocked, 1);
    uint32_t taken = running->notifications;
    running->notifications = clear ? 0 : taken - 1;
    return taken;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return host_mutex_create_fails ? NULL : calloc(1, sizeof(struct host_mutex));
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{
    (void)wait;
    if (mutex->binary) {
        host_semaphore_wait_ticks = wait;
        if (!mutex->available && wait && host_semaphore_wait_hook) host_semaphore_wait_hook();
        bool had=mutex->available;mutex->available=false;return had ? pdTRUE : pdFALSE;
    }
    assert(!mutex->locked);
    mutex->locked = true;
    ++host_mutexes_held;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    if (mutex->binary) { mutex->available=true;return pdTRUE; }
    assert(mutex->locked);
    mutex->locked = false;
    --host_mutexes_held;
    return pdTRUE;
}

bool host_mutex_is_locked(SemaphoreHandle_t mutex) { return mutex->locked; }

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    SemaphoreHandle_t s=calloc(1,sizeof(*s));if (s) s->binary=true;return s;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { free(semaphore); }

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *woken)
{
    xSemaphoreGive(semaphore);
    *woken = 0;
    return pdTRUE;
}

UBaseType_t uxTaskGetNumberOfTasks(void) { return 1; }
UBaseType_t uxTaskGetSystemState(TaskStatus_t *array, UBaseType_t size, uint32_t *total_runtime)
{
    static struct host_task idle = {.name = "IDLE0"};
    if (total_runtime) *total_runtime = (uint32_t)host_clock_us;
    if (!size) return 0;
    array[0] = (TaskStatus_t){.xHandle = &idle, .pcTaskName = idle.name, .ulRunTimeCounter = (uint32_t)host_clock_us / 2};
    return 1;
}
BaseType_t xTaskGetCoreID(TaskHandle_t task) { (void)task; return 0; }
esp_err_t esp_pm_dump_locks(FILE *stream) { fputs("host pm locks\n", stream); return ESP_OK; }
esp_err_t esp_pm_configure(const void *config)
{
    const esp_pm_config_t *c = config;
    host_trace("esp_pm_configure(%d,%d,ls=%d)", c->max_freq_mhz, c->min_freq_mhz, c->light_sleep_enable);
    if (host_pm_configure_error) return host_pm_configure_error;
    host_pm_config = *c;
    return ESP_OK;
}
esp_err_t esp_pm_get_configuration(void *config) { *(esp_pm_config_t *)config = host_pm_config; return ESP_OK; }
__attribute__((weak)) void game_session_wake(void) {}
