#!/usr/bin/env python3
"""Exercise Settings groups, shared controller flow and persistence failures."""
from host_build import CJSON, PRESET_STORE_MODULES, SAVE_FILE, run
CODE = r'''
#include <assert.h>
#include <string.h>
#include "controller.h"
#include "display_config.h"
#include "display_preset.h"
#include "fake_game_audio.h"
#include "fake_game_storage.h"
#include "fake_input.h"
#include "fake_menu.h"
#include "fake_ble_gamepad.h"
#include "fake_usb_gamepad.h"
#include "fake_screen.h"
#include "gray_settings_ui.h"
#include "host_fakes.h"
#include "settings_ui.h"
static unsigned tuning, transitions, cursor, size;
static nvs_handle_t settings;
void gray_settings_ui_open(void) { ++tuning; }
void gray_settings_ui_transition_test(void) { ++transitions; }
esp_err_t gray_settings_ui_activate(const display_preset_t *p, bool modified) { return display_preset_set_active(p, modified); }
typedef struct {const char *title; int choice, direction;} event_t;
static event_t events[64];
static void push(const char *title, int choice) { events[size++] = (event_t){title,choice,1}; }
static int script(const char *title, const char *subtitle, const char *const *items, unsigned count, unsigned *selected, int *direction) {
    (void)subtitle; (void)selected;
    assert(cursor < size);
    event_t e = events[cursor++]; assert(!strcmp(title,e.title));
    if (!strcmp(title,"SETTINGS")) {
        assert(count == 4 && !strcmp(items[0],"DISPLAY") && !strcmp(items[1],"CONTROLLER") && !strcmp(items[2],"SOUND") && !strcmp(items[3],"FPS"));
    }
    if (!strcmp(title,"CONTROLLER")) assert(count == 2 && !strncmp(items[0],"CONNECTION:",11) && !strcmp(items[1],"MAP BUTTONS"));
    if (!strcmp(title,"CONNECTION")) assert(count == 2 && !strcmp(items[0],"BLUETOOTH") && !strcmp(items[1],"USB"));
    if (direction) *direction = e.direction;
    return e.choice;
}
static void start(void) {
    host_fakes_reset(); fake_storage_reset(); fake_usb_reset();
    fake_ble_started = false; fake_ble_starts = fake_ble_stops = 0;
    fake_ble_start_error = fake_ble_stop_error = ESP_OK;
    assert(nvs_open("gamepad",NVS_READWRITE,&settings)==ESP_OK);
    settings_ui_init(settings); controller_load(settings); assert(controller_start()==ESP_OK);
    display_config_t config = {DISPLAY_DITHERED,8,8,false,BORDER_BLACK_LOGO}; display_config_set(&config);
    assert(display_preset_load_active()==ESP_OK); settings_ui_load_toggles();
    fake_menu_hook = script; fake_menu_notices = fake_input_remaps = 0;
    size = cursor = 0;
}
static void finish(void) { settings_ui_open(); assert(cursor == size); }
static void groups(void) {
    start();
    push("SETTINGS",0); push("DISPLAY",3); push("DITHERED TUNING",0); events[size-1].direction=-1;
    push("DITHERED TUNING",-1); push("DISPLAY",0); push("DISPLAY MODE",1);
    push("DISPLAY",3); push("DISPLAY",4); push("DISPLAY",1); push("GAME BORDER",3); push("DISPLAY",-1);
    push("SETTINGS",2); push("SOUND",0); push("SOUND",1); push("SOUND",1); events[size-1].direction=-1;
    push("SOUND",1); events[size-1].direction=-1; push("SOUND",-1);
    push("SETTINGS",3); push("FPS",0); push("FPS",-1); push("SETTINGS",-1); finish();
    assert(tuning==1 && transitions==1 && display_config_get().mode==DISPLAY_GRAYSCALE);
    assert(display_config_get().dither_black_pulses==7 && !settings_ui_sound_enabled() && display_config_get().show_fps);
    assert(host_nvs_u8("gamepad","sound")==0 && host_nvs_u8("gamepad","show_fps")==1);
    /* 6 -> 7 -> 6 -> 5: right/A raises, left lowers; 3 dB per step from -30 dB. */
    assert(host_nvs_u8("gamepad","volume")==5 && fake_audio_volume_db==-15.0f);
    assert(display_config_get().border==BORDER_FILL && host_nvs_u8("gamepad","border")==BORDER_FILL);
    display_config_t reset={DISPLAY_DITHERED,2,2,false,BORDER_BLACK_LOGO}; display_config_set(&reset);
    assert(display_preset_load_active()==ESP_OK); settings_ui_load_display(); settings_ui_load_toggles();
    assert(display_config_get().mode==DISPLAY_GRAYSCALE && display_config_get().dither_black_pulses==7 && display_config_get().show_fps);
    assert(display_config_get().border==BORDER_FILL && fake_audio_volume_db==-15.0f);
}
static void transports(void) {
    start();
    push("SETTINGS",1); push("CONTROLLER",0); push("CONNECTION",1); push("USB CONTROLLER",-1);
    push("CONTROLLER",1); push("CONTROLLER",0); push("CONNECTION",1); push("USB CONTROLLER",1);
    push("CONTROLLER",1); push("CONTROLLER",0); push("CONNECTION",0); push("CONTROLLER",-1); push("SETTINGS",-1); finish();
    assert(fake_input_remaps==2 && fake_usb_starts==1 && fake_usb_stops==1 && fake_ble_starts==2);
    assert(controller_transport()==CONTROLLER_BLUETOOTH && host_nvs_u8("gamepad","controller")==0 && !fake_menu_notices);
}
static void failures(void) {
    start(); host_nvs_set_error=ESP_FAIL;
    push("SETTINGS",0); push("DISPLAY",0); push("DISPLAY MODE",1); push("DISPLAY",-1);
    push("SETTINGS",2); push("SOUND",0); push("SOUND",-1);
    push("SETTINGS",3); push("FPS",0); push("FPS",-1); push("SETTINGS",-1); finish();
    assert(fake_menu_notices==3 && display_config_get().mode==DISPLAY_DITHERED && settings_ui_sound_enabled() && !display_config_get().show_fps);
}
static void usb_failure(void) {
    start(); fake_usb_start_error=ESP_FAIL;
    push("SETTINGS",1); push("CONTROLLER",0); push("CONNECTION",1); push("USB CONTROLLER",1);
    push("CONTROLLER",-1); push("SETTINGS",-1); finish();
    assert(fake_menu_notices==1 && controller_transport()==CONTROLLER_BLUETOOTH && controller_running() && fake_ble_started);
}
static void startup_validation(void) {
    start();display_config_set(&(display_config_t){DISPLAY_GRAYSCALE,2,2,false,BORDER_BLACK_LOGO});
    settings_ui_load_display();settings_ui_load_toggles();
    assert(display_config_get().mode==DISPLAY_GRAYSCALE && display_config_get().dither_black_pulses==8 && settings_ui_sound_enabled());
    uint8_t invalid=255;
    for(unsigned i=0;i<5;++i) { const char *keys[]={"display_mode","show_fps","sound","border","volume"};host_nvs_seed("gamepad",keys[i],HOST_NVS_U8,&invalid,1); }
    settings_ui_load_display();settings_ui_load_toggles();
    assert(display_config_get().mode==DISPLAY_GRAYSCALE && !display_config_get().show_fps && settings_ui_sound_enabled());
    assert(display_config_get().border==BORDER_BLACK_LOGO && fake_audio_volume_db==-12.0f);
    uint8_t off=0,on=1;host_nvs_seed("gamepad","sound",HOST_NVS_U8,&off,1);host_nvs_seed("gamepad","show_fps",HOST_NVS_U8,&on,1);
    settings_ui_load_toggles();assert(!settings_ui_sound_enabled() && display_config_get().show_fps);
}
static void commit_failure(void) {
    start();uint8_t old_sound=1;host_nvs_seed("gamepad","sound",HOST_NVS_U8,&old_sound,1);host_nvs_commit_error=ESP_FAIL;
    push("SETTINGS",2);push("SOUND",0);push("SOUND",-1);push("SETTINGS",-1);finish();
    assert(settings_ui_sound_enabled() && fake_menu_notices==1);
    host_nvs_commit_error=ESP_OK;assert(nvs_commit(settings)==ESP_OK);assert(host_nvs_u8("gamepad","sound")==1);settings_ui_load_toggles();assert(settings_ui_sound_enabled());
}
static void transport_failures(void) {
    start();assert(controller_select(CONTROLLER_USB)==ESP_OK);fake_usb_stop_timeouts=3;
    push("SETTINGS",1);push("CONTROLLER",0);push("CONNECTION",0);push("CONTROLLER",-1);push("SETTINGS",-1);finish();
    assert(fake_menu_notices==1 && controller_transport()==CONTROLLER_USB && controller_running() && fake_ble_starts==1);
    start();host_nvs_set_error=ESP_FAIL;
    push("SETTINGS",1);push("CONTROLLER",0);push("CONNECTION",1);push("USB CONTROLLER",1);push("CONTROLLER",-1);push("SETTINGS",-1);finish();
    assert(fake_menu_notices==1 && controller_transport()==CONTROLLER_BLUETOOTH && controller_running() && fake_usb_starts==0);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"transport-failures")) transport_failures();
    else if(!strcmp(argv[1],"startup")) startup_validation(); else if(!strcmp(argv[1],"commit")) commit_failure();
    else if(!strcmp(argv[1],"groups")) groups(); else if(!strcmp(argv[1],"transports")) transports();
    else if(!strcmp(argv[1],"failures")) failures(); else usb_failure();
    return 0;
}
'''
run(CODE, modules=['settings_ui.c','preset_ui.c','controller.c','display_config.c',*PRESET_STORE_MODULES],
    fakes=['fake_menu.c','fake_input.c','fake_screen.c','fake_game_audio.c','fake_game_storage.c','fake_ble_gamepad.c','fake_usb_gamepad.c'],
    extra_sources=[SAVE_FILE], vendor_sources=[CJSON], scenarios=[[x] for x in ['groups','transports','failures','usb-failure','startup','commit','transport-failures']])
print('PASS: Settings groups, persisted edits, symmetrical transport/remap flow, cancellation and failed switch rollback')
