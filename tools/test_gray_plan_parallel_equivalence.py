#!/usr/bin/env python3
"""Differential host test for the parallel grayscale planner against the serial one.

Compares current main/gray_plan.c directly with
main/gray_plan_parallel.c over deterministic stateful workloads and lifecycle paths.
"""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''#include "gray_plan_parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gray_plan_t old_plan, new_plan;
static gray_history_t old_history[GRAY_PIXELS], new_history[GRAY_PIXELS];
static uint8_t binary[GRAY_PIXELS/8], game[GAME_VIEW_MAX_BYTES];
static uint8_t b[GRAY_PIXELS/8], w[GRAY_PIXELS/8], fb[GRAY_PIXELS/8], fw[GRAY_PIXELS/8];
static uint8_t rb[GRAY_PIXELS/8], rw[GRAY_PIXELS/8], rfb[GRAY_PIXELS/8], rfw[GRAY_PIXELS/8];
static uint32_t rng=0x91e10da5u;
static unsigned async_toggle, async_used;
static int async_always;

static uint32_t rand32(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}
static void fail(unsigned step, const char *what, unsigned index) {
    fprintf(stderr, "Mismatch at step %u: %s[%u]\n", step, what, index);
    exit(1);
}
static void eqmem(unsigned step, const char *name, const void *a, const void *b, size_t n) {
    if (memcmp(a,b,n)) {
        const unsigned char *x=a, *y=b;
        size_t i=0; while(i<n && x[i]==y[i]) ++i;
        fail(step,name,(unsigned)i);
    }
}
static void compare(unsigned step) {
    eqmem(step,"pixel",old_plan.pixel,new_plan.pixel,sizeof(old_plan.pixel));
    eqmem(step,"active",old_plan.active,new_plan.active,sizeof(old_plan.active));
    eqmem(step,"previous_binary",old_plan.previous_binary,new_plan.previous_binary,sizeof(old_plan.previous_binary));
    eqmem(step,"previous_game",old_plan.previous_game,new_plan.previous_game,sizeof(old_plan.previous_game));
    eqmem(step,"weak_wl",old_plan.weak_wl,new_plan.weak_wl,sizeof(old_plan.weak_wl));
    eqmem(step,"history",old_history,new_history,sizeof(old_history));
    eqmem(step,"config",&old_plan.config,&new_plan.config,sizeof(old_plan.config));
    eqmem(step,"adaptive",&old_plan.adaptive,&new_plan.adaptive,sizeof(old_plan.adaptive));
    if(old_plan.pending!=new_plan.pending) fail(step,"pending",0);
    if(old_plan.had_game!=new_plan.had_game) fail(step,"had_game",0);
    if(!game_view_equal(old_plan.had_view,new_plan.had_view)) fail(step,"had_view",0);
    if(old_plan.history_mode!=new_plan.history_mode) fail(step,"history_mode",0);
    if(old_plan.history_returns!=new_plan.history_returns) fail(step,"history_returns",0);
    if(old_plan.history_reversals!=new_plan.history_reversals) fail(step,"history_reversals",0);
    if(old_plan.adaptive_configured!=new_plan.adaptive_configured) fail(step,"adaptive_configured",0);
}
/* Alternates synchronous and begin+join completion; NULL follow masks select complete(). */
static void new_complete(const uint8_t *nb, const uint8_t *nw, const uint8_t *nfb, const uint8_t *nfw) {
    if((async_always || (async_toggle++&1)) &&
       gray_plan_parallel_complete_begin(&new_plan,nb,nw,nfb,nfw)) {
        gray_plan_parallel_complete_join(); ++async_used; return;
    }
    if(nfb) gray_plan_parallel_complete_pair(&new_plan,nb,nw,nfb,nfw);
    else gray_plan_parallel_complete(&new_plan,nb,nw);
}
static void init_config(unsigned variant) {
    gray_config_t c=(gray_config_t)GRAY_CONFIG_DEFAULT;
    c.retarget_gain=variant%5; c.scan_policy=variant%2;
    for(unsigned s=0;s<4;s++) for(unsigned t=0;t<4;t++)
        if(s!=t) c.dose[s][t]=(uint8_t)(1+((variant*7+s*3+t*5)%GRAY_DOSE_MAX));
    old_plan.config=new_plan.config=c;
}
static void mutate_inputs(unsigned step) {
    /* Mostly sparse edits, with periodic broad changes to exercise dirty rows. */
    unsigned n=(step%31==0)?300:1+(rand32()%12);
    while(n--) {
        unsigned i=rand32()%sizeof(binary); binary[i]^=(uint8_t)(1u<<(rand32()%8));
    }
    n=(step%29==0)?250:rand32()%9;
    while(n--) { unsigned i=rand32()%sizeof(game); game[i]=(uint8_t)rand32(); }
}
static const uint8_t *choose_game(unsigned step) {
    switch(step%7) { case 0: case 1: case 5: return NULL; default: return game; }
}
static unsigned valid_hold(unsigned step) {
    /* Diagonal hold bits are mandatory by the public mask contract. */
    return GRAY_HOLD_DIAGONAL | ((step*0x1397u) & ~GRAY_HOLD_DIAGONAL & 0xffffu);
}
static void compare_masks(unsigned step, unsigned count, unsigned ref_count,
                          int pair, int follow) {
    if(count!=ref_count) fail(step,"active_mask_count",0);
    eqmem(step,"black mask",b,rb,sizeof(b)); eqmem(step,"white mask",w,rw,sizeof(w));
    if(pair && follow) {
        eqmem(step,"follow black mask",fb,rfb,sizeof(fb));
        eqmem(step,"follow white mask",fw,rfw,sizeof(fw));
    }
}
static void set_game_target(unsigned index, unsigned target) {
    unsigned x=index%200, y=index/200;
    if(x<20 || x>=180 || y<28 || y>=172) abort();
    unsigned j=(y-28)*160+x-20, shift=(j%4)*2;
    game[j/4]=(uint8_t)((game[j/4]&~(3u<<shift))|((3u-target)<<shift));
}
static void set_view(game_view_t view) {
    old_plan.view=new_plan.view=view;
}
static void set_view_pixel(game_view_t view, unsigned x, unsigned y, unsigned target) {
    if(x<view.x || x>=view.x+view.width || y<view.y || y>=view.y+view.height) abort();
    unsigned col=x-view.x, row=y-view.y;
    unsigned j=row*(view.width/4)+col/4, shift=(col%4)*2;
    game[j]=(uint8_t)((game[j]&~(3u<<shift))|((3u-target)<<shift));
}
static void check_layout(unsigned step, bool has_game, game_view_t expected) {
    if(old_plan.had_game!=has_game) fail(step,"layout had_game",0);
    if(!game_view_equal(game_view_or_classic(old_plan.had_view),expected))
        fail(step,"layout cached view",0);
    compare(step);
}
static void layout_delivery(unsigned step, const uint8_t *image, game_view_t view,
                            bool has_game, game_view_t expected_cached_view) {
    set_view(view);
    unsigned rc=gray_plan_prepare(&old_plan,binary,image,rb,rw);
    unsigned nc=gray_plan_parallel_prepare(&new_plan,binary,image,b,w);
    compare_masks(step,nc,rc,0,0);
    gray_plan_complete(&old_plan,rb,rw);
    new_complete(b,w,NULL,NULL);
    check_layout(step,has_game,expected_cached_view);
}
static void variable_layout_workload(unsigned base) {
    const game_view_t classic=GAME_VIEW_CLASSIC, fill=GAME_VIEW_FILL, zero={0};
    memset(&old_plan,0,sizeof(old_plan)); memset(&new_plan,0,sizeof(new_plan));
    memset(old_history,0,sizeof(old_history)); memset(new_history,0,sizeof(new_history));
    memset(binary,0,sizeof(binary)); memset(game,0xff,sizeof(game));
    old_plan.history=old_history; new_plan.history=new_history;
    init_config(base%5);
    set_view(classic);
    gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary);

    /* Classic game pixels, then a layout change to fill-screen. Dirty rows include
     * areas that were game-driven only in one of the two layouts. */
    set_view_pixel(classic,20,28,3); set_view_pixel(classic,179,171,0);
    layout_delivery(base+1,game,classic,true,classic);
    set_view_pixel(fill,0,10,0); set_view_pixel(fill,199,189,3);
    binary[10*25] ^= 0x80; binary[189*25+24] ^= 0x01;
    layout_delivery(base+2,game,fill,true,fill);

    /* Leaving gameplay restores binary beneath the old fill viewport; the
     * cached game layout must remain the last one that actually had a game. */
    binary[10*25] ^= 0x40; binary[189*25+24] ^= 0x02;
    layout_delivery(base+3,NULL,classic,false,fill);

    /* Re-enter with classic, then leave again while p->view changes to fill. */
    set_view_pixel(classic,20,28,1); set_view_pixel(classic,179,171,2);
    layout_delivery(base+4,game,classic,true,classic);
    layout_delivery(base+5,NULL,fill,false,classic);

    /* A zeroed view is the public classic-layout spelling. Exercise async
     * completion as well as synchronous completion across that normalization. */
    set_view_pixel(classic,40,40,2);
    async_always=1;
    layout_delivery(base+6,game,zero,true,classic);
    set_view_pixel(fill,100,100,1);
    layout_delivery(base+7,game,fill,true,fill);
    set_view_pixel(fill,99,98,0);
    set_view(fill);
    unsigned hold=GRAY_HOLD_DIAGONAL;
    unsigned rc=gray_plan_prepare_pair(&old_plan,binary,game,rb,rw,rfb,rfw,hold);
    unsigned nc=gray_plan_parallel_prepare_pair(&new_plan,binary,game,b,w,fb,fw,hold);
    compare_masks(base+8,nc,rc,1,1);
    gray_plan_complete_pair(&old_plan,rb,rw,rfb,rfw);
    /* Async begin+join also receives and commits the predicted follow masks. */
    new_complete(b,w,fb,fw);
    check_layout(base+8,true,fill);
    async_always=0;
}
static void prime_route_state(unsigned index, unsigned origin, unsigned route_step) {
    memset(&old_plan,0,sizeof(old_plan)); memset(&new_plan,0,sizeof(new_plan));
    memset(old_history,0,sizeof(old_history)); memset(new_history,0,sizeof(new_history));
    memset(binary,0,sizeof(binary)); memset(game,0xff,sizeof(game));
    old_plan.history=old_history; new_plan.history=new_history;
    init_config(0);
    gray_adaptive_t a=(gray_adaptive_t)GRAY_ADAPTIVE_DEFAULT;
    a.history_mode=3; a.weak_wl=1;
    for(unsigned r=0;r<GRAY_ROUTES;r++) { a.return_delta[r]=1; a.reverse_percent[r]=150; }
    old_plan.adaptive=new_plan.adaptive=a;
    old_plan.adaptive_configured=new_plan.adaptive_configured=true;
    old_plan.history_mode=new_plan.history_mode=3;
    gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary);
    gray_plan_target(&old_plan,binary,game); gray_plan_parallel_target(&new_plan,binary,game);
    /* Establish the origin through acknowledged delivery, keeping the cached
     * source image coherent with the model just as production does. */
    set_game_target(index,origin);
    gray_plan_target(&old_plan,binary,game); gray_plan_parallel_target(&new_plan,binary,game);
    while(old_plan.pending) {
        unsigned rc=gray_plan_masks(&old_plan,rb,rw);
        unsigned nc=gray_plan_parallel_masks(&new_plan,b,w);
        compare_masks(799,nc,rc,0,0);
        gray_plan_complete(&old_plan,rb,rw);
        new_complete(b,w,NULL,NULL);
    }
    compare(800+route_step*20);
}
static void target_route(unsigned step) {
    gray_plan_target(&old_plan,binary,game);
    gray_plan_parallel_target(&new_plan,binary,game);
    compare(step);
}
static void one_delivery(unsigned step) {
    unsigned rc=gray_plan_prepare(&old_plan,binary,game,rb,rw);
    unsigned nc=gray_plan_parallel_prepare(&new_plan,binary,game,b,w);
    compare_masks(step,nc,rc,0,0);
    gray_plan_complete(&old_plan,rb,rw);
    new_complete(b,w,NULL,NULL);
    compare(step);
}
static void history_route(unsigned route_step, unsigned origin, unsigned via,
                          int complete_first, unsigned expected_counter) {
    const unsigned index=28u*200u+20u, base=800u+route_step*20u;
    prime_route_state(index,origin,route_step);
    /* Force the cached source row dirty even when the first route shade is K,
     * which is already the packed game's initial value for this pixel. */
    binary[index/8] |= (uint8_t)(128u>>(index%8));
    set_game_target(index,via); target_route(base+1);
    if(complete_first) {
        unsigned guard=0;
        while(old_plan.pixel[index].remaining && guard++<GRAY_DOSE_MAX+1) one_delivery(base+2+guard);
        if(old_plan.pixel[index].remaining || new_plan.pixel[index].remaining) fail(base,"route did not settle",index);
        set_game_target(index,origin); target_route(base+12);
        if(expected_counter==1 && old_plan.history_returns==0) fail(base,"completed return counter",index);
        if(expected_counter==2 && old_plan.history_reversals==0) fail(base,"interrupted reversal counter",index);
    } else {
        one_delivery(base+2);
        set_game_target(index,origin); target_route(base+3);
        if(old_plan.history_reversals==0) fail(base,"interrupted reversal counter",index);
    }
}
static void boundary_workload(unsigned step) {
    const unsigned low=19999u, high=20000u;
    memset(&old_plan,0,sizeof(old_plan)); memset(&new_plan,0,sizeof(new_plan));
    memset(old_history,0,sizeof(old_history)); memset(new_history,0,sizeof(new_history));
    memset(binary,0,sizeof(binary)); memset(game,0xff,sizeof(game));
    old_plan.history=old_history; new_plan.history=new_history;
    init_config(step%5);
    gray_adaptive_t adaptive=(gray_adaptive_t)GRAY_ADAPTIVE_DEFAULT;
    adaptive.history_mode=3; adaptive.weak_wl=1;
    for(unsigned r=0;r<GRAY_ROUTES;r++) {
        adaptive.return_delta[r]=(int8_t)((int)(step%7)-3);
        adaptive.reverse_percent[r]=(uint8_t)(50+25*((r+step)%7));
    }
    old_plan.adaptive=new_plan.adaptive=adaptive;
    old_plan.adaptive_configured=new_plan.adaptive_configured=true;
    old_plan.history_mode=new_plan.history_mode=3;
    gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary);
    compare(step);

    /* Touch the last pixel/byte in row 99 and the first in row 100. These
     * straddle both the worker split and the 40,000-pixel mask-byte boundary. */
    binary[low/8] |= (uint8_t)(128u>>(low%8));
    binary[high/8] |= (uint8_t)(128u>>(high%8));
    /* Game rows 71 and 72 map to screen rows 99 and 100. */
    set_game_target(99u*200u+20u,3);
    set_game_target(99u*200u+179u,2);
    set_game_target(100u*200u+20u,1);
    set_game_target(100u*200u+179u,3);
    unsigned hold=GRAY_HOLD_DIAGONAL|0x1080u;
    unsigned rc=gray_plan_prepare_pair(&old_plan,binary,game,rb,rw,rfb,rfw,hold);
    unsigned nc=gray_plan_parallel_prepare_pair(&new_plan,binary,game,b,w,fb,fw,hold);
    compare_masks(step+1,nc,rc,1,1);
    gray_plan_complete_pair(&old_plan,rb,rw,rfb,rfw);
    new_complete(b,w,fb,fw);
    compare(step+2);

    binary[low/8] &= (uint8_t)~(128u>>(low%8));
    binary[high/8] &= (uint8_t)~(128u>>(high%8));
    set_game_target(99u*200u+20u,0);
    set_game_target(99u*200u+179u,0);
    set_game_target(100u*200u+20u,0);
    set_game_target(100u*200u+179u,0);
    rc=gray_plan_prepare(&old_plan,binary,game,rb,rw);
    nc=gray_plan_parallel_prepare(&new_plan,binary,game,b,w);
    compare_masks(step+3,nc,rc,0,0);
    gray_plan_complete(&old_plan,rb,rw);
    new_complete(b,w,NULL,NULL);
    compare(step+4);
}

static void exhaustive_completion_variant(int with_history, int gray_routes) {
    enum { COMBINATIONS=193*4*GRAY_DOSE_MAX, PATTERNS=20 };
    /* The first four patterns exercise complete(); all sixteen patterns then
     * exercise complete_pair(), including conflicting and otherwise invalid
     * first/follow mask combinations. Every batch contains the full 40,000
     * pixel traversal, while its first COMBINATIONS pixels enumerate every
     * valid level/target/remaining tuple. */
    memset(binary,0,sizeof(binary));
    memset(&old_plan,0,sizeof(old_plan)); memset(&new_plan,0,sizeof(new_plan));
    memset(old_history,0,sizeof(old_history)); memset(new_history,0,sizeof(new_history));
    old_plan.history=new_plan.history=NULL; init_config(0);
    if(with_history) old_plan.history=old_history, new_plan.history=new_history;
    if(gray_routes) {
        gray_adaptive_t a=(gray_adaptive_t)GRAY_ADAPTIVE_DEFAULT;
        a.return_delta[GRAY_ROUTE_LDL]=1;
        old_plan.adaptive=new_plan.adaptive=a;
        old_plan.adaptive_configured=new_plan.adaptive_configured=true;
    }
    gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary);
    unsigned pattern_id=0;
    for(unsigned paired=0;paired<=1;paired++) {
        unsigned limit=paired ? 16 : 4;
        for(unsigned mask_code=0;mask_code<limit;mask_code++,pattern_id++) {
            memset(b,0,sizeof(b)); memset(w,0,sizeof(w));
            memset(fb,0,sizeof(fb)); memset(fw,0,sizeof(fw));
            memset(rb,0,sizeof(rb)); memset(rw,0,sizeof(rw));
            memset(rfb,0,sizeof(rfb)); memset(rfw,0,sizeof(rfw));
            memset(old_plan.active,0xff,sizeof(old_plan.active));
            memset(new_plan.active,0xff,sizeof(new_plan.active));
            old_plan.pending=new_plan.pending=GRAY_PIXELS;
            for(unsigned i=0;i<GRAY_PIXELS;i++) {
                unsigned id=i%COMBINATIONS;
                unsigned remaining=id%GRAY_DOSE_MAX+1;
                unsigned target=(id/GRAY_DOSE_MAX)%4;
                unsigned level=id/(GRAY_DOSE_MAX*4);
                unsigned source=(level+target+remaining)&3;
                old_plan.pixel[i]=(gray_pixel_t){.level=(uint8_t)level,
                    .remaining=remaining,.target=target,.source=source};
                new_plan.pixel[i].packed=(uint16_t)(level|(remaining<<8)|(target<<12)|(source<<14));
                /* Counters at 0, mid-range, one or two below saturation, and saturated. */
                static const uint8_t counts[]={0,128,253,254,255};
                old_history[i]=new_history[i]=(gray_history_t){.origin=(uint8_t)(i/COMBINATIONS%5),
                    .black_steps=counts[(i/COMBINATIONS+pattern_id)%5],.white_steps=counts[(i/COMBINATIONS+pattern_id+2)%5]};
                unsigned bit=128u>>(i%8), byte=i/8;
                if(mask_code&1) { b[byte]|=bit; rb[byte]|=bit; }
                if(mask_code&2) { w[byte]|=bit; rw[byte]|=bit; }
                if(paired && (mask_code&4)) { fb[byte]|=bit; rfb[byte]|=bit; }
                if(paired && (mask_code&8)) { fw[byte]|=bit; rfw[byte]|=bit; }
            }
            if(paired) {
                gray_plan_complete_pair(&old_plan,rb,rw,rfb,rfw);
                new_complete(b,w,fb,fw);
            } else {
                gray_plan_complete(&old_plan,rb,rw);
                new_complete(b,w,NULL,NULL);
            }
            compare(1000+pattern_id);
        }
    }
    printf("exhaustive completion: %u state tuples across %u mask patterns\n",
           COMBINATIONS,PATTERNS);
}
static void exhaustive_completion(void) {
    exhaustive_completion_variant(1,0);
    exhaustive_completion_variant(1,1);
    exhaustive_completion_variant(0,0);
}

static void set_both_pixel(unsigned i, unsigned level, unsigned remaining, unsigned target, unsigned source) {
    old_plan.pixel[i]=(gray_pixel_t){.level=(uint8_t)level,.remaining=remaining,.target=target,.source=source};
    new_plan.pixel[i].packed=(uint16_t)(level|(remaining<<8)|(target<<12)|(source<<14));
    uint8_t bit=(uint8_t)(128u>>(i%8));
    if(remaining) { old_plan.active[i/8]|=bit; new_plan.active[i/8]|=bit; ++old_plan.pending; ++new_plan.pending; }
}
static void reset_both(gray_history_t *history_old, gray_history_t *history_new) {
    memset(&old_plan,0,sizeof(old_plan)); memset(&new_plan,0,sizeof(new_plan));
    memset(old_history,0,sizeof(old_history)); memset(new_history,0,sizeof(new_history));
    old_plan.history=history_old; new_plan.history=history_new;
}
/* Every packed state, history tuple and new target the retarget decision reads, under one policy:
 * level 0..192, remaining 0/1/7/12, old target, origin 0..4 and step counts that are zero, equal
 * to the completed-return dose, off by one or saturated, each sent to all four new targets through
 * the fill viewport. The binary rows above and below it get random tuples and K/W targets. */
static unsigned exhaustive_retarget_policy(unsigned variant, int with_history) {
    static const unsigned remaining_values[]={0,1,7,12};
    enum { TUPLES=193*4*4*5*4, GAME_FIRST=10*200, GAME_PIXELS=180*200 };
    const game_view_t fill=GAME_VIEW_FILL;
    const gray_config_t config=old_plan.config;
    const gray_adaptive_t adaptive=old_plan.adaptive;
    const bool configured=old_plan.adaptive_configured;
    const unsigned mode=old_plan.history_mode;
    unsigned routes_taken=0;
    for(unsigned batch=0;batch*GAME_PIXELS<TUPLES*4u;batch++) {
        reset_both(with_history ? old_history : NULL, with_history ? new_history : NULL);
        old_plan.config=new_plan.config=config;
        old_plan.adaptive=new_plan.adaptive=adaptive;
        old_plan.adaptive_configured=new_plan.adaptive_configured=configured;
        old_plan.history_mode=new_plan.history_mode=mode;
        old_plan.view=new_plan.view=fill;
        for(unsigned i=0;i<sizeof(binary);i++) binary[i]=(uint8_t)rand32();
        /* Pixel k of every 4-pixel group is driven to shade k. */
        memset(game,0x1b,sizeof(game));
        /* Every group is dirty: previous_binary differs everywhere and had_game is false. */
        for(unsigned i=0;i<sizeof(binary);i++) old_plan.previous_binary[i]=new_plan.previous_binary[i]=(uint8_t)~binary[i];
        for(unsigned i=0;i<GRAY_PIXELS;i++) {
            unsigned slot=i>=GAME_FIRST && i<GAME_FIRST+GAME_PIXELS ? batch*GAME_PIXELS+i-GAME_FIRST : rand32();
            unsigned id=slot/4%TUPLES;
            unsigned level=id%193, rest=id/193;
            unsigned remaining=remaining_values[rest%4]; rest/=4;
            unsigned target=rest%4; rest/=4;
            unsigned origin=rest%5; rest/=5;
            unsigned steps_code=rest%4;
            unsigned dose=origin<4 ? config.dose[origin][target] : 3;
            unsigned steps=steps_code==0 ? 0 : steps_code==1 ? dose : steps_code==2 ? dose+1 : 255;
            unsigned black=steps, white=steps_code==2 ? dose : steps;
            if((slot/4/TUPLES+i)&1) { unsigned t=black; black=white; white=t; }
            set_both_pixel(i,level,remaining,target,(level+i)&3);
            old_history[i]=new_history[i]=(gray_history_t){(uint8_t)origin,(uint8_t)black,(uint8_t)white};
            if(rand32()&1) { old_plan.weak_wl[i/8]|=(uint8_t)(128u>>(i%8)); new_plan.weak_wl[i/8]|=(uint8_t)(128u>>(i%8)); }
        }
        unsigned hold=valid_hold(batch+variant), step=4000+variant*16+batch;
        unsigned rc=gray_plan_prepare_pair(&old_plan,binary,game,rb,rw,rfb,rfw,hold);
        unsigned nc=gray_plan_parallel_prepare_pair(&new_plan,binary,game,b,w,fb,fw,hold);
        compare_masks(step,nc,rc,1,1);
        compare(step);
        routes_taken+=old_plan.history_returns+old_plan.history_reversals;
        gray_plan_complete_pair(&old_plan,rb,rw,rfb,rfw);
        new_complete(b,w,fb,fw);
        compare(step);
    }
    return routes_taken;
}
static void exhaustive_retarget(void) {
    unsigned variant=0, routes_taken=0;
    for(unsigned configured=0;configured<=1;configured++)
    for(unsigned mode=0;mode<=3;mode++)
    for(unsigned routes=0;routes<=2;routes++)
    for(unsigned with_history=0;with_history<=1;with_history++,variant++) {
        init_config(variant);
        gray_adaptive_t a=(gray_adaptive_t)GRAY_ADAPTIVE_DEFAULT;
        a.history_mode=(uint8_t)mode; a.weak_wl=(uint8_t)(1+variant%2);
        for(unsigned r=0;r<GRAY_ROUTES;r++) {
            a.return_delta[r]=(int8_t)((int)((r+variant)%7)-3);
            a.reverse_percent[r]=(uint8_t)((r+variant)%3 ? 50+25*((r+variant)%7) : 0);
        }
        /* routes 0: gray routes off; 1: gray routes on; 2: everything neutral except L>K>L. */
        if(routes==0) a.return_delta[GRAY_ROUTE_DLD]=a.return_delta[GRAY_ROUTE_LDL]=0,
                      a.reverse_percent[GRAY_ROUTE_DLD]=a.reverse_percent[GRAY_ROUTE_LDL]=0;
        if(routes==1) a.return_delta[GRAY_ROUTE_DLD]=2, a.reverse_percent[GRAY_ROUTE_LDL]=75;
        if(routes==2) for(unsigned r=1;r<GRAY_ROUTES;r++) a.return_delta[r]=0, a.reverse_percent[r]=0;
        if(!gray_adaptive_valid(&a)) abort();
        old_plan.adaptive=new_plan.adaptive=a;
        old_plan.adaptive_configured=new_plan.adaptive_configured=configured;
        old_plan.history_mode=new_plan.history_mode=mode;
        routes_taken+=exhaustive_retarget_policy(variant,with_history);
    }
    if(!routes_taken) { fprintf(stderr,"exhaustive retarget never took a route\n"); exit(1); }
    printf("exhaustive retarget: %u policy variants\n",variant);
}
/* Every packed state under several hold masks: the mask pass reads nothing else. */
static void exhaustive_masks(void) {
    static const unsigned holds[]={GRAY_HOLD_DIAGONAL,GRAY_HOLDS_DEFAULT,GRAY_HOLD_ALL,0x8421u|0x5a5au,0x8421u|0x2184u};
    reset_both(old_history,new_history);
    init_config(0);
    memset(binary,0,sizeof(binary));
    for(unsigned i=0;i<GRAY_PIXELS;i++) {
        unsigned id=i%(193*16*16), level=id%193, remaining=(id/193)%16, rest=id/(193*16);
        set_both_pixel(i,level,remaining,rest&3,rest>>2);
    }
    memcpy(old_plan.previous_binary,binary,sizeof(binary)); memcpy(new_plan.previous_binary,binary,sizeof(binary));
    for(unsigned h=0;h<sizeof(holds)/sizeof(holds[0]);h++) {
        unsigned rc=gray_plan_prepare_pair(&old_plan,binary,NULL,rb,rw,rfb,rfw,holds[h]);
        unsigned nc=gray_plan_parallel_prepare_pair(&new_plan,binary,NULL,b,w,fb,fw,holds[h]);
        compare_masks(4900+h,nc,rc,1,1);
        rc=gray_plan_masks(&old_plan,rb,rw);
        nc=gray_plan_parallel_masks(&new_plan,b,w);
        compare_masks(4910+h,nc,rc,0,0);
        compare(4900+h);
    }
}

static void sparse_delivery(unsigned step, int pair) {
    memset(b,0xa5,sizeof(b)); memset(w,0xa5,sizeof(w)); memset(fb,0xa5,sizeof(fb)); memset(fw,0xa5,sizeof(fw));
    memset(rb,0x5a,sizeof(rb)); memset(rw,0x5a,sizeof(rw)); memset(rfb,0x5a,sizeof(rfb)); memset(rfw,0x5a,sizeof(rfw));
    unsigned hold=valid_hold(step), rc, nc;
    if(pair) {
        rc=gray_plan_prepare_pair(&old_plan,binary,NULL,rb,rw,rfb,rfw,hold);
        nc=gray_plan_parallel_prepare_pair(&new_plan,binary,NULL,b,w,fb,fw,hold);
    } else {
        rc=gray_plan_prepare(&old_plan,binary,NULL,rb,rw);
        nc=gray_plan_parallel_prepare(&new_plan,binary,NULL,b,w);
    }
    compare_masks(step,nc,rc,pair,pair);
    if(pair) { gray_plan_complete_pair(&old_plan,rb,rw,rfb,rfw); new_complete(b,w,fb,fw); }
    else { gray_plan_complete(&old_plan,rb,rw); new_complete(b,w,NULL,NULL); }
    compare(step);
}
/* Settled panel with isolated changes: whole mask words are inactive before, during and after. */
static void sparse_workload(unsigned base) {
    static const unsigned bytes[]={0,1,2,3,4,2498,2499,2500,2501,2503,2504,4996,4997,4998,4999,1234};
    memset(&old_plan,0,sizeof(old_plan)); memset(&new_plan,0,sizeof(new_plan));
    memset(old_history,0,sizeof(old_history)); memset(new_history,0,sizeof(new_history));
    memset(binary,0,sizeof(binary));
    old_plan.history=old_history; new_plan.history=new_history;
    init_config(base%5);
    gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary);
    for(unsigned round=0;round<2*sizeof(bytes)/sizeof(bytes[0]);round++) {
        unsigned byte=bytes[round%(sizeof(bytes)/sizeof(bytes[0]))];
        binary[byte]^=(uint8_t)(1u<<(round%8));
        if(round%3==0) binary[(byte+7)%sizeof(binary)]^=0x10;
        for(unsigned settle=0;settle<GRAY_DOSE_MAX+3;settle++)
            sparse_delivery(base+round*20+settle,(round+settle)&1);
        if(round%4==1) { binary[byte]^=(uint8_t)(1u<<(round%8)); sparse_delivery(base+round*20+19,1); }
    }
    for(unsigned i=0;i<4;i++) sparse_delivery(base+900+i,i&1);
}

static void run_suite(unsigned phase) {
    old_plan.history=old_history; new_plan.history=new_history;
    init_config(0);
    for(unsigned i=0;i<sizeof(binary);i++) binary[i]=(uint8_t)rand32();
    for(unsigned i=0;i<sizeof(game);i++) game[i]=(uint8_t)rand32();
    gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary); compare(0);

    /* Exercise one settled L-K-L return and all four interrupted routes with
     * non-neutral adaptive settings, then verify that the relevant counters
     * actually advance in both implementations. */
    history_route(0,2,0,1,1);       /* completed L-K-L */
    history_route(1,2,0,0,2);       /* interrupted L-K-L */
    history_route(2,1,0,0,2);       /* interrupted D-K-D */
    history_route(3,1,3,0,2);       /* interrupted D-W-D */
    history_route(4,2,3,0,2);       /* interrupted L-W-L */
    history_route(5,1,2,1,1);       /* completed D-L-D */
    history_route(6,2,1,1,1);       /* completed L-D-L */
    history_route(7,1,2,0,2);       /* interrupted D-L-D */
    history_route(8,2,1,0,2);       /* interrupted L-D-L */

    exhaustive_completion();
    boundary_workload(780);

    memset(&old_plan,0,sizeof(old_plan)); memset(&new_plan,0,sizeof(new_plan));
    memset(old_history,0,sizeof(old_history)); memset(new_history,0,sizeof(new_history));
    old_plan.history=old_history; new_plan.history=new_history;
    init_config(0);
    for(unsigned i=0;i<sizeof(binary);i++) binary[i]=(uint8_t)rand32();
    for(unsigned i=0;i<sizeof(game);i++) game[i]=(uint8_t)rand32();
    gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary); compare(899);

    const unsigned steps=850;
    for(unsigned step=1;step<=steps;step++) {
        if(step%73==0) init_config(step/73);
        if(step%61==0) {
            gray_adaptive_t a=(gray_adaptive_t)GRAY_ADAPTIVE_DEFAULT;
            a.history_mode=(step/61)%4; a.weak_wl=(step/61)%3;
            for(unsigned i=0;i<GRAY_ROUTES;i++) {
                a.return_delta[i]=(int8_t)((int)(rand32()%7)-3);
                a.reverse_percent[i]=(uint8_t)(50+25*(rand32()%7));
            }
            old_plan.adaptive=new_plan.adaptive=a;
            old_plan.adaptive_configured=new_plan.adaptive_configured=(step/61)%2;
            old_plan.history_mode=new_plan.history_mode=a.history_mode;
        }
        /* Detach history for short randomized windows, then reattach the same
         * storage to ensure optional provenance pointers remain equivalent. */
        if(step%151==110) old_plan.history=new_plan.history=NULL;
        if(step%151==121) old_plan.history=old_history, new_plan.history=new_history;
        mutate_inputs(step);
        const uint8_t *g=choose_game(step);
        unsigned action=rand32()%8;
        if(step%127==0 || action==0) {
            gray_plan_seed(&old_plan,binary); gray_plan_parallel_seed(&new_plan,binary);
        } else if(action==1) {
            gray_plan_target(&old_plan,binary,g); gray_plan_parallel_target(&new_plan,binary,g);
        } else if(action==2 || action==3) {
            unsigned rc=gray_plan_prepare(&old_plan,binary,g,rb,rw);
            unsigned nc=gray_plan_parallel_prepare(&new_plan,binary,g,b,w);
            compare_masks(step,nc,rc,0,0);
            /* Some prepared waveforms are intentionally aborted. */
            if(action==2 || step%5==0) {
                gray_plan_complete(&old_plan,rb,rw);
                new_complete(b,w,NULL,NULL);
            }
        } else if(action==4 || action==5) {
            unsigned hold=valid_hold(step);
            unsigned rc=gray_plan_prepare_pair(&old_plan,binary,g,rb,rw,rfb,rfw,hold);
            unsigned nc=gray_plan_parallel_prepare_pair(&new_plan,binary,g,b,w,fb,fw,hold);
            compare_masks(step,nc,rc,1,1);
            if(action==4 || step%4==0) {
                gray_plan_complete_pair(&old_plan,rb,rw,rfb,rfw);
                new_complete(b,w,fb,fw);
            }
        } else {
            unsigned rc=gray_plan_masks(&old_plan,rb,rw);
            unsigned nc=gray_plan_parallel_masks(&new_plan,b,w);
            compare_masks(step,nc,rc,0,0);
            if(action==7 && step%3==0) {
                gray_plan_complete(&old_plan,rb,rw);
                new_complete(b,w,NULL,NULL);
            }
        }
        compare(step);
    }
    sparse_workload(5000+phase*1000);
    printf("equivalent phase %u across %u deterministic operations (seed 0x91e10da5)\n",phase,steps);
}

/* A begun completion left unjoined must be joined by the next planner call, not raced. */
static void implicit_join(unsigned step) {
    for(unsigned i=0;i<sizeof(binary);i++) binary[i]^=(uint8_t)rand32();
    unsigned rc=gray_plan_prepare(&old_plan,binary,NULL,rb,rw);
    unsigned nc=gray_plan_parallel_prepare(&new_plan,binary,NULL,b,w);
    compare_masks(step,nc,rc,0,0);
    gray_plan_complete(&old_plan,rb,rw);
    if(!gray_plan_parallel_complete_begin(&new_plan,b,w,NULL,NULL)) fail(step,"async begin refused",0);
    rc=gray_plan_masks(&old_plan,rb,rw);
    nc=gray_plan_parallel_masks(&new_plan,b,w);
    compare_masks(step+1,nc,rc,0,0);
    compare(step+1);
}
int main(void) {
    run_suite(0); /* Serial fallback before optional worker/cache initialization. */
    variable_layout_workload(720);
    if (!gray_plan_parallel_init()) { fprintf(stderr,"parallel init failed\n"); return 2; }
#ifdef GRAY_PLAN_PARALLEL_HOST_THREADS
    if (!gray_plan_parallel_active()) { fprintf(stderr,"parallel workers not active after init\n"); return 3; }
#endif
    if (!gray_plan_parallel_init()) { fprintf(stderr,"parallel repeated init failed\n"); return 4; }
    run_suite(1); /* Initialized path, with simultaneous row workers in pthread mode. */
    variable_layout_workload(740);
    if (!async_used) { fprintf(stderr,"asynchronous completion never ran\n"); return 11; }
    exhaustive_retarget();
    exhaustive_masks();
    async_always=1;
    exhaustive_completion();
    boundary_workload(3000);
    async_always=0;
    implicit_join(3100);
    gray_plan_parallel_deinit();
#ifdef GRAY_PLAN_PARALLEL_HOST_THREADS
    if (gray_plan_parallel_active()) { fprintf(stderr,"parallel workers active after deinit\n"); return 5; }
#endif
    run_suite(2); /* Fallback after worker/cache teardown. */
#ifdef GRAY_PLAN_PARALLEL_HOST_THREADS
    for(unsigned cycle=0;cycle<20;cycle++) {
        if (!gray_plan_parallel_init() || !gray_plan_parallel_active()) {
            fprintf(stderr,"parallel worker reinit failed at cycle %u\n",cycle); return 6;
        }
        boundary_workload(2000+cycle*10);
        gray_plan_parallel_deinit();
        if (gray_plan_parallel_active()) {
            fprintf(stderr,"parallel worker teardown failed at cycle %u\n",cycle); return 7;
        }
        boundary_workload(2005+cycle*10); /* Serial fallback without context. */
    }
#endif
    if (!gray_plan_parallel_init()) { fprintf(stderr,"parallel second init failed\n"); return 8; }
#ifdef GRAY_PLAN_PARALLEL_HOST_THREADS
    if (!gray_plan_parallel_active()) { fprintf(stderr,"parallel second worker init failed\n"); return 9; }
#endif
    run_suite(3); /* Recreated workers and cache. */
    gray_plan_parallel_deinit();
#ifdef GRAY_PLAN_PARALLEL_HOST_THREADS
    if (gray_plan_parallel_active()) { fprintf(stderr,"parallel workers survived second deinit\n"); return 10; }
#endif
    run_suite(4); /* Repeated fallback after second teardown. */
    return 0;
}
'''


def main() -> int:
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="gray-plan-equivalence-") as tmp:
        tmpdir = Path(tmp)
        harness = tmpdir / "equivalence.c"
        binary = tmpdir / "equivalence"
        harness.write_text(HARNESS)
        sanitizer = "thread" if os.environ.get("GRAY_PLAN_PARALLEL_TSAN") == "1" else "address,undefined"
        common = [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer",
            "-I", str(ROOT / "main"),
        ]
        if os.environ.get("GRAY_PLAN_PARALLEL_HOST_THREADS") == "1":
            common += ["-DGRAY_PLAN_PARALLEL_HOST_THREADS", "-pthread"]
        subprocess.run(cc + common + [str(harness), str(ROOT / "main/gray_plan.c"),
                       str(ROOT / "main/gray_plan_parallel.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, cwd=ROOT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
