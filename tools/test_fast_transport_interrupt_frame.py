#!/usr/bin/env python3
"""Check the opt-in interrupt-backed route for full SPI planes only."""
from host_build import run

CODE = r'''
#define CONFIG_SSD1681_EXPERIMENTAL_INTERRUPT_FRAME_TX 1
#include "ssd1681_fast.c"
#include <assert.h>
#include "host_fakes.h"

int main(void)
{
    host_fakes_reset();
    uint8_t command = 0;
    uint8_t frame[SSD1681_FRAME_BYTES] = {0};
    struct ssd1681_device d = {.spi=(spi_device_handle_t)1,.active_spi_hz=10000000};

    assert(transfer(&d,false,&command,1)==ESP_OK);
    assert(host_spi_polling_transactions==1 && host_spi_interrupt_transactions==0);
    assert(host_spi_last.length==8 && host_spi_last.buffer==&command && host_spi_last.override_hz==0);

    assert(transfer(&d,true,frame,sizeof(frame))==ESP_OK);
    assert(host_spi_interrupt_transactions==1 && host_spi_polling_transactions==1);
    assert(host_spi_last.length==SSD1681_FRAME_BYTES*8 && host_spi_last.buffer==frame);
    assert(host_spi_last.override_hz==0 && d.active_spi_hz==10000000);

    d.fast_transport=true;
    assert(transfer(&d,true,frame,sizeof(frame))==ESP_OK);
    assert(host_spi_interrupt_transactions==2 && host_spi_last.override_hz==20000000);
    assert(d.active_spi_hz==20000000);
    assert(transfer(&d,false,&command,1)==ESP_OK);
    assert(host_spi_polling_transactions==2 && host_spi_interrupt_transactions==2);
    assert(host_spi_last.length==8 && host_spi_last.override_hz==0);

    d.fast_transport=false;
    assert(transfer(&d,true,frame,sizeof(frame))==ESP_OK);
    assert(host_spi_interrupt_transactions==3 && host_spi_last.override_hz==10000000);
    assert(d.active_spi_hz==10000000);

    d.fast_transport=true;
    host_spi_error=99;
    assert(transfer(&d,true,frame,sizeof(frame))==99);
    assert(d.fault && d.active_spi_hz==10000000);
    assert(host_spi_interrupt_transactions==4 && host_spi_polling_transactions==2);
    assert(host_spi_last.override_hz==20000000);
    return 0;
}
'''

run(CODE)
print('PASS: opt-in full-frame interrupt path preserves small-transfer polling, clock override, and fault handling')
