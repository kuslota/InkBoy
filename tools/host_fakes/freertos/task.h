#pragma once
#include "freertos/FreeRTOS.h"

typedef struct host_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskPriorityGet(TaskHandle_t task);
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack, void *argument,
                       UBaseType_t priority, TaskHandle_t *handle);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack,
                                   void *argument, UBaseType_t priority, TaskHandle_t *handle, int core);
void vTaskDelete(TaskHandle_t task);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken);
TaskHandle_t xTaskGetCurrentTaskHandle(void);

#define configMAX_TASK_NAME_LEN 16
#define tskNO_AFFINITY 0x7FFFFFFF
typedef struct {
    TaskHandle_t xHandle;
    const char *pcTaskName;
    uint32_t ulRunTimeCounter;
} TaskStatus_t;
UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(TaskStatus_t *array, UBaseType_t size, uint32_t *total_runtime);
BaseType_t xTaskGetCoreID(TaskHandle_t task);
