#pragma once
#include "fast_display.h"

/* The grayscale setters of components/esp32-ws154/components/epd_fast/fast_display.c, modelled in memory: they keep
 * what was applied and can fail the optional history/weak-drive allocation. */
typedef struct {
    gray_config_t config;
    gray_adaptive_t adaptive;
    unsigned strength, holds, applies;
    bool immediate, adaptive_alloc_error, history_memory, weak_memory;
} fake_gray_backend_t;

extern fake_gray_backend_t fake_gray;
void fake_gray_reset(void);
