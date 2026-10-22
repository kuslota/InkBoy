#!/usr/bin/env python3
"""Check fill-screen scaling/conversion and grayscale retargeting across layout switches."""
from host_build import run
CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game_view.h"
#include "gb_emulator.h"
#include "gray_plan_parallel.h"
static uint32_t seed=1;
static uint8_t random_byte(void){seed=seed*1664525u+1013904223u;return seed>>24;}
static unsigned shade_at(const uint8_t *image, unsigned row_bytes, unsigned x, unsigned y)
{ return (image[y*row_bytes+x/4]>>((x%4)*2))&3; }
static uint8_t packed[GB_PACKED_BYTES], panel[5000], before[5000];

static void scaling(void)
{
    for(unsigned frame=0;frame<20;++frame){
        for(unsigned i=0;i<sizeof(packed);++i)packed[i]=random_byte();
        const uint8_t *scaled=game_view_scale(packed);
        for(unsigned y=0;y<180;++y)for(unsigned x=0;x<200;++x)
            assert(shade_at(scaled,50,x,y)==shade_at(packed,GB_PACKED_ROW,x*4/5,y*4/5));
    }
}

static void dithered(void)
{
    static const uint8_t bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    static const uint8_t thresholds[4]={0,5,11,16};
    game_view_reset();
    for(unsigned frame=0;frame<30;++frame){
        for(unsigned y=0;y<GB_HEIGHT;++y)if(frame==0 || random_byte()%4==0)
            for(unsigned x=0;x<GB_PACKED_ROW;++x)packed[y*GB_PACKED_ROW+x]=random_byte();
        for(unsigned i=0;i<5000;++i)panel[i]=before[i]=random_byte();
        const uint8_t *scaled=game_view_scale(packed);
        unsigned rows=game_view_convert_fast(scaled,panel);
        assert(frame==0 ? rows==180 : rows<=180);
        for(unsigned y=0;y<200;++y)for(unsigned x=0;x<200;++x){
            unsigned i=y*25+x/8, bit=128u>>(x%8);
            bool in_view=y>=10 && y<190, changed=memcmp(panel+y*25,before+y*25,25)!=0;
            if(!in_view){assert((panel[i]&bit)==(before[i]&bit));continue;}
            if(!changed)continue; /* an unchanged source row is skipped, not repainted */
            bool white=bayer[(y-10)%4][x%4]>=thresholds[shade_at(scaled,50,x,y-10)];
            assert(((panel[i]&bit)!=0)==white);
        }
        assert(game_view_convert_fast(scaled,panel)==0);
    }
    for(unsigned i=0;i<5000;++i)panel[i]=before[i]=random_byte();
    game_view_reset();
    assert(game_view_prepare_gray(game_view_scale(packed),panel)==180);
    for(unsigned y=0;y<200;++y)
        if(y>=10 && y<190){for(unsigned b=0;b<25;++b)assert(!panel[y*25+b]);}
        else assert(!memcmp(panel+y*25,before+y*25,25));
}

/* Target model: the game drives its viewport, the binary panel drives the rest. */
static gray_plan_t plan;
static uint8_t binary[5000];
static void expect_targets(const uint8_t *game, game_view_t view, unsigned step)
{
    for(unsigned y=0;y<200;++y)for(unsigned x=0;x<200;++x){
        bool in_game=game && y>=view.y && y<view.y+view.height && x>=view.x && x<view.x+view.width;
        unsigned want=in_game ? 3-shade_at(game,view.width/4,x-view.x,y-view.y) :
                                (binary[y*25+x/8]&(128u>>(x%8)) ? 3 : 0);
        if(plan.pixel[y*200+x].target!=want){
            fprintf(stderr,"step %u: pixel %u,%u target %u want %u\n",step,x,y,plan.pixel[y*200+x].target,want);
            assert(0);
        }
    }
}
static void target(bool parallel, const uint8_t *game, game_view_t view)
{
    plan.view=view;
    if(parallel) gray_plan_parallel_target(&plan,binary,game); else gray_plan_target(&plan,binary,game);
}
static void layout_switches(bool parallel)
{
    static uint8_t classic[GB_PACKED_BYTES];
    if(parallel) assert(gray_plan_parallel_init());
    memset(&plan,0,sizeof(plan));
    for(unsigned i=0;i<5000;++i)binary[i]=random_byte();
    gray_plan_seed(&plan,binary);
    /* classic -> fill -> menu (no game) -> fill -> classic, with a moving image throughout */
    const game_view_t sequence[]={GAME_VIEW_CLASSIC,GAME_VIEW_FILL,GAME_VIEW_FILL,{0,0,0,0},GAME_VIEW_FILL,GAME_VIEW_CLASSIC,GAME_VIEW_CLASSIC};
    for(unsigned step=0;step<sizeof(sequence)/sizeof(sequence[0]);++step){
        for(unsigned i=0;i<sizeof(classic);++i)classic[i]=random_byte();
        for(unsigned i=0;i<200;++i)binary[random_byte()%5000]^=(uint8_t)(1u<<(random_byte()%8));
        game_view_t view=sequence[step];
        const uint8_t *game=!view.width ? NULL : view.width==200 ? game_view_scale(classic) : classic;
        target(parallel,game,view.width ? view : plan.view);
        expect_targets(game,view,step);
    }
    if(parallel) gray_plan_parallel_deinit();
}

int main(void)
{
    scaling(); dithered(); layout_switches(false); layout_switches(true);
    puts("PASS: 1.25x nearest-neighbour scale, fill dither/gray conversion and margins, planner targets across classic/fill/menu switches (serial and parallel)");
}
'''
run(CODE, modules=['game_view.c', 'gray_plan.c', 'gray_plan_parallel.c'])
