#!/usr/bin/env python3
"""Exercise the production grayscale adaptive menu and route controls in isolation."""
from host_build import run, GB_VIDEO

CODE = r'''
#include "gray_settings_ui.c"
#include <assert.h>
#include <stdio.h>
#include "gray_profile_store.h"
#include "fake_gray_backend.h"
#include "fake_input.h"
#include "fake_menu.h"
#include "host_fakes.h"

static gray_profile_t stored;

static bool read_stored(void)
{
    nvs_handle_t h;
    if (nvs_open("inkboy", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t error = gray_profile_nvs_get(h, "g_values", &stored);
    nvs_close(h);
    return error == ESP_OK;
}

struct event { const char *title; unsigned count; int choice, direction; };
static struct event events[32];
static unsigned event_count, event_index;

static int scripted_menu(const char *title, const char *subtitle, const char *const *items,
                         unsigned count, unsigned *selected, int *direction)
{
    (void)subtitle; (void)items; (void)selected;
    assert(event_index < event_count);
    struct event e = events[event_index++];
    assert(!strcmp(title, e.title) && count == e.count);
    if (direction) *direction = e.direction;
    assert(e.choice < 0 || (unsigned)e.choice < count);
    return e.choice;
}

static void push(const char *title, unsigned count, int choice, int direction)
{
    events[event_count++] = (struct event){title, count, choice, direction};
}

static void reset_events(void) { event_count = event_index = 0; }
static void back_main(void) { push("GRAYSCALE HISTORY / DRIVE", 11, 10, 1); }

static void main_change(unsigned row, int direction)
{
    reset_events(); push("GRAYSCALE HISTORY / DRIVE", 11, (int)row, direction); back_main();
    gray_adaptive_settings(); assert(event_index == event_count);
}

int main(void){
    host_fakes_reset();fake_gray_reset();fake_menu_hook=scripted_menu;
    /* Start from neutral settings so each control's first change is observable. */
    display_preset_t neutral=*display_preset_default(DISPLAY_GRAYSCALE);
    neutral.gray.tuning=(gray_config_t)GRAY_CONFIG_DEFAULT;neutral.gray.strength=0;neutral.gray.immediate=0;
    neutral.gray.holds_lo=255;neutral.gray.holds_hi=255;neutral.gray.adaptive=(gray_adaptive_t){0,0,{-1,0,0,0,0,0},{150,0,0,0,0,0}};
    assert(display_preset_load_active()==ESP_OK && display_preset_set_active(&neutral,false)==ESP_OK);
    gray_settings_ui_load();host_nvs_erase("inkboy","g_values");host_nvs_commits=0;
    gray_adaptive_t defaults=GRAY_ADAPTIVE_DEFAULT;
    assert(gray_adaptive_valid(&defaults));
    for(unsigned i=1;i<GRAY_ROUTES;++i)
        assert(defaults.return_delta[i]==0 && defaults.reverse_percent[i]==0);

    // OFF -> BOTH wraps left; settled W>L weak drive cycles normal -> 11V -> 8.8V -> normal.
    main_change(0,-1);assert(gray_adaptive.history_mode==3 && fake_gray.adaptive.history_mode==3);
    assert(read_stored() && stored.adaptive.history_mode==3);
    main_change(1,1);assert(gray_adaptive.weak_wl==1);
    main_change(1,1);assert(gray_adaptive.weak_wl==2);
    main_change(1,1);assert(gray_adaptive.weak_wl==0);

    // Route menu rows map to the six named round trips in order.
    static const char *routes[]={"LIGHT > BLACK > LIGHT","DARK > BLACK > DARK",
        "DARK > WHITE > DARK","LIGHT > WHITE > LIGHT",
        "DARK > LIGHT > DARK","LIGHT > DARK > LIGHT"};
    for(unsigned i=0;i<GRAY_ROUTES;++i){
        reset_events();push("GRAYSCALE HISTORY / DRIVE",11,(int)i+2,1);
        push(routes[i],4,0,1);push(routes[i],4,1,1);push(routes[i],4,3,1);back_main();gray_adaptive_settings();
        assert(gray_adaptive.return_delta[i]==(i==0 ? 0 : 1));
        assert(gray_adaptive.reverse_percent[i]==(i==0 ? 175 : 50));
        gray_adaptive.return_delta[i]=defaults.return_delta[i];
        gray_adaptive.reverse_percent[i]=defaults.reverse_percent[i];fake_gray.adaptive=gray_adaptive;
        assert(event_index==event_count);
    }

    // L>K>L return delta and reverse gain change by one and 25% respectively.
    reset_events();push(routes[0],4,0,-1);push(routes[0],4,1,1);push(routes[0],4,3,1);
    gray_adaptive_route_settings(GRAY_ROUTE_LKL);assert(event_index==event_count);
    assert(gray_adaptive.return_delta[0]==-2 && gray_adaptive.reverse_percent[0]==175);
    assert(read_stored() && stored.adaptive.return_delta[0]==-2 && stored.adaptive.reverse_percent[0]==175);
    // Upper bounds are inert and do not write another profile.
    gray_adaptive.return_delta[0]=3;gray_adaptive.reverse_percent[0]=200;fake_gray.adaptive=gray_adaptive;
    unsigned before_write_count=host_nvs_commits;reset_events();push(routes[0],4,0,1);push(routes[0],4,1,1);push(routes[0],4,3,1);
    gray_adaptive_route_settings(GRAY_ROUTE_LKL);assert(event_index==event_count && host_nvs_commits==before_write_count);
    assert(gray_adaptive.return_delta[0]==3 && gray_adaptive.reverse_percent[0]==200);
    gray_adaptive.return_delta[0]=-3;gray_adaptive.reverse_percent[0]=0;fake_gray.adaptive=gray_adaptive;
    before_write_count=host_nvs_commits;reset_events();push(routes[0],4,0,-1);push(routes[0],4,1,-1);push(routes[0],4,3,1);
    gray_adaptive_route_settings(GRAY_ROUTE_LKL);assert(event_index==event_count && host_nvs_commits==before_write_count);
    assert(gray_adaptive.return_delta[0]==-3 && gray_adaptive.reverse_percent[0]==0);

    // Reset changes adaptive fields only; custom baseline doses survive.
    gray_config.dose[0][1]=6;gray_adaptive.history_mode=2;gray_adaptive.weak_wl=1;
    gray_adaptive.return_delta[3]=2;fake_gray.adaptive=gray_adaptive;
    reset_events();push("GRAYSCALE HISTORY / DRIVE",11,9,1);back_main();gray_adaptive_settings();
    assert(event_index==event_count && !memcmp(&gray_adaptive,&defaults,sizeof(defaults)));
    assert(!memcmp(&fake_gray.adaptive,&defaults,sizeof(defaults)) && gray_config.dose[0][1]==6);
    assert(read_stored() && stored.adaptive.history_mode==defaults.history_mode && stored.tuning.dose[0][1]==6);

    // Failed NVS save rolls the reserved adaptive policy back and leaves RAM/blob untouched.
    assert(read_stored());gray_profile_t before=gray_profile_current(),disk=stored;unsigned old_writes=host_nvs_commits;
    host_nvs_commit_error=ESP_ERR_NVS_IO;main_change(1,1);host_nvs_commit_error=ESP_OK;
    assert(!memcmp(&gray_adaptive,&before.adaptive,sizeof(gray_adaptive)));
    assert(!memcmp(&fake_gray.adaptive,&before.adaptive,sizeof(fake_gray.adaptive)) && read_stored() && !memcmp(&stored,&disk,sizeof(stored)));
    assert(host_nvs_commits==old_writes && fake_menu_notices==1);

    // Allocation failure fails before NVS and leaves the active state unchanged.
    fake_gray.adaptive_alloc_error=true;main_change(1,1);fake_gray.adaptive_alloc_error=false;
    assert(!memcmp(&gray_adaptive,&before.adaptive,sizeof(gray_adaptive)));
    assert(!memcmp(&fake_gray.adaptive,&before.adaptive,sizeof(fake_gray.adaptive)) && read_stored() && !memcmp(&stored,&disk,sizeof(stored)));
    assert(host_nvs_commits==old_writes && fake_menu_notices==2);
    assert(host_log_count("PLAYER_GRAY_TEST_BEGIN")==0);
    puts("PASS: adaptive mode/weak cycles, route mapping and bounds, neutral defaults, reset isolation, save rollback and allocation failure; no refresh API");
}
'''

run(CODE, modules=['gray_profile_store.c', 'gray_motion.c', 'screen_draw.c', 'display_config.c', 'display_preset.c'],
    fakes=['fake_menu.c', 'fake_screen.c', 'fake_input.c', 'fake_gray_backend.c'], extra_sources=[GB_VIDEO])
