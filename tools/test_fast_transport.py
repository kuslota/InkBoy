#!/usr/bin/env python3
"""Exercise the real SPI transfer's sticky clock override and fault handling."""
from host_build import run

CODE = r'''
#include "ssd1681_fast.c"
#include <assert.h>
#include "host_fakes.h"

int main(void)
{
    host_fakes_reset();
    uint8_t byte = 0;
    struct ssd1681_device d = {.spi = (spi_device_handle_t)1, .active_spi_hz = 10000000};
    assert(transfer(&d, true, &byte, 1) == 0 && host_spi_transactions == 1);
    assert(host_spi_last.length == 8 && host_spi_last.buffer == &byte && host_spi_last.override_hz == 0);
    d.fast_transport = true;
    assert(transfer(&d, true, &byte, 1) == 0 && d.active_spi_hz == 20000000 && host_spi_last.override_hz == 20000000);
    assert(transfer(&d, false, &byte, 1) == 0 && host_spi_last.override_hz == 0); /* The override is retained by the driver. */
    d.fast_transport = false;
    assert(transfer(&d, false, &byte, 1) == 0 && d.active_spi_hz == 10000000 && host_spi_last.override_hz == 10000000);
    d.fast_transport = true;
    host_spi_error = 99;
    assert(transfer(&d, true, &byte, 1) == 99 && d.fault && d.active_spi_hz == 10000000 && host_spi_last.override_hz == 20000000);

    /* The board buttons install the shared GPIO ISR service first: the panel uses it, never removes it. */
    host_fakes_reset();
    ssd1681_handle_t panel;
    assert(gpio_install_isr_service(0) == ESP_OK);
    assert(ssd1681_create(&panel) == ESP_OK && !panel->isr_service && panel->isr_handler);
    assert(ssd1681_destroy(panel) == ESP_OK && !host_trace_has("gpio_uninstall_isr_service"));
    host_fakes_reset();
    assert(ssd1681_create(&panel) == ESP_OK && panel->isr_service);
    assert(ssd1681_destroy(panel) == ESP_OK && host_trace_has("gpio_uninstall_isr_service"));
    return 0;
}
'''

run(CODE)
print('PASS: 10/20 MHz mode transitions, retained clock, failed transfer latches fault, shared GPIO ISR service kept installed')
