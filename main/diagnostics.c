#include "diagnostics.h"
#include "ws154_pins.h"
#ifdef INKBOY_DIAGNOSTICS
#include "controller.h"
#include "gameboy/usb_gamepad.h"
#include "nvs.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static _Atomic unsigned expiry[2], controller_expiry, raw;
static bool live(unsigned deadline) { return (int32_t)(deadline-(unsigned)(esp_timer_get_time()/1000))>0; }
int diagnostics_gpio(int pin) { unsigned i=pin==WS154_BTN_PWR?1:0;return live(atomic_load(&expiry[i]))?0:-1; }
int diagnostics_controller(void) { return live(atomic_load(&controller_expiry))?(int)atomic_load(&raw):-1; }
static void command(char *line) {
    ESP_LOGI("test","TEST_COMMAND,%s",line);
    unsigned duration=0,mask=0,now=(unsigned)(esp_timer_get_time()/1000);
    if(sscanf(line,"ib boot %u",&duration)==1 || sscanf(line,"ib pwr %u",&duration)==1) {
        if(duration && duration<=5000) atomic_store(&expiry[strstr(line,"pwr")?1:0],now+duration);
    } else if(sscanf(line,"ib controller %x",&mask)==1 && mask<=255) {
        atomic_store(&raw,mask);atomic_store(&controller_expiry,now+5000);
    } else if(!strcmp(line,"ib usbstate")) {
        nvs_handle_t h;uint8_t saved=0;
        if(nvs_open("gamepad",NVS_READONLY,&h)==ESP_OK) { (void)nvs_get_u8(h,"controller",&saved);nvs_close(h); }
        ESP_LOGI("test","TEST_USB,saved,%u,selected,%u,service,%u,host,%u,pc,%u,gamepad,%u",
            saved,controller_transport(),controller_running(),usb_gamepad_running(),usb_serial_jtag_is_connected(),controller_connected());
    }
}

static void console(void *unused) {
    (void)unused;char line[96];unsigned length=0;
    usb_serial_jtag_driver_config_t config={.tx_buffer_size=256,.rx_buffer_size=256};
    esp_err_t error=usb_serial_jtag_driver_install(&config);
    ESP_LOGI("test","TEST_CONSOLE,ready,%d",error);
    for(;;) { char c;
        if(usb_serial_jtag_read_bytes(&c,1,pdMS_TO_TICKS(10))>0) {
            if(c=='\n' || c=='\r') {line[length]=0;if(length) command(line);length=0;}
            else if(length<sizeof line-1) line[length++]=c;
            else length=0;
        } else vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void diagnostics_start(void) { xTaskCreate(console,"test_console",4096,NULL,2,NULL); }
#endif
