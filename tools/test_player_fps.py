#!/usr/bin/env python3
"""Check wall-time FPS windows and overlay isolation from the game and logo."""
from host_build import run
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "player_fps.h"
#include "screen_draw.h"
int main(void) {
    player_fps_t fps;
    player_fps_reset(&fps,100000);
    for(unsigned n=0;n<30;++n)player_fps_presented(&fps);
    assert(!player_fps_poll(&fps,1099999));
    assert(player_fps_poll(&fps,1100000) && fps.tenths==300);
    for(unsigned n=0;n<30;++n)player_fps_presented(&fps);
    assert(!player_fps_poll(&fps,2100000)); /* stable text needs no redraw */
    for(unsigned n=0;n<15;++n)player_fps_presented(&fps);
    assert(player_fps_poll(&fps,3600000) && fps.tenths==100); /* actual elapsed, not nominal one second */
    assert(player_fps_poll(&fps,4600000) && !fps.tenths); /* static game */
    assert(!player_fps_poll(&fps,5600000)); /* idle overlay does not self-count */
    player_fps_reset(&fps,9000000);assert(!fps.frames && !fps.tenths);
    uint8_t guarded[5002],original[5000];memset(guarded,0x35,sizeof(guarded));
    memcpy(original,guarded+1,5000);
    screen_draw_fps(guarded+1,298,BORDER_BLACK_LOGO);
    assert(guarded[0]==0x35 && guarded[5001]==0x35);
    assert(!memcmp(guarded+1,original,172*25)); /* logo and viewport untouched */
    unsigned lit=0;
    for(unsigned y=172;y<200;++y)for(unsigned x=0;x<200;++x) {
        bool white=(guarded[1+y*25+x/8] & (128u>>(x%8)))!=0;
        if(white){assert(y>=180 && y<194 && x>=40 && x<160);++lit;}
    }
    assert(lit>100);
    memcpy(original,guarded+1,5000);screen_draw_fps(guarded+1,298,BORDER_BLACK_LOGO);
    assert(!memcmp(original,guarded+1,5000));
    screen_draw_fps(guarded+1,0,BORDER_BLACK_LOGO);
    assert(!memcmp(original,guarded+1,172*25) && memcmp(original+172*25,guarded+1+172*25,28*25));
    /* White border: black text on white, same bottom bezel. */
    memset(guarded,0x35,sizeof(guarded));memcpy(original,guarded+1,5000);
    screen_draw_fps(guarded+1,298,BORDER_WHITE);
    assert(guarded[0]==0x35 && guarded[5001]==0x35 && !memcmp(guarded+1,original,172*25));
    lit=0;
    for(unsigned y=172;y<200;++y)for(unsigned x=0;x<200;++x)
        if(!(guarded[1+y*25+x/8] & (128u>>(x%8)))){assert(y>=180 && y<194 && x>=40 && x<160);++lit;}
    assert(lit>100);
    /* Fill: small text confined to the 10px strip under the 180-row image. */
    memset(guarded,0x35,sizeof(guarded));memcpy(original,guarded+1,5000);
    screen_draw_fps(guarded+1,298,BORDER_FILL);
    assert(guarded[0]==0x35 && guarded[5001]==0x35 && !memcmp(guarded+1,original,190*25));
    lit=0;
    for(unsigned y=190;y<200;++y)for(unsigned x=0;x<200;++x)
        if(!(guarded[1+y*25+x/8] & (128u>>(x%8)))){assert(y>=192 && y<199);++lit;}
    assert(lit>20);
    puts("PASS: FPS elapsed-time averaging, idle zero/stable suppression, reset and bezel-only rendering");
}
'''
run(code, modules=['screen_draw.c'])
