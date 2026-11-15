#!/usr/bin/env python3
"""Exercise actual four-shade planner/backend, delivery faults and image scheduling."""
from host_build import run

CODE = r'''
#include "fast_display.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gray_plan.h"
#include "host_fakes.h"
#include "host_panel.h"

#define TEST_ADAPTIVE {0,0,{-1,0,0,0,0,0},{150,0,0,0,0,0}}
#define TEST_TUNING {1,{{0,4,8,12},{8,0,4,12},{8,2,0,12},{12,3,2,0}},2,0}
static uint8_t binary[5000], game[5760], bm[5000], wm[5000];
static gray_plan_t plan;
static void one_target(unsigned target){memset(game,(3-target)*85,sizeof(game));gray_plan_target(&plan,binary,game);}
static void deliver(void){gray_plan_masks(&plan,bm,wm);
 uint8_t b2[5000],w2[5000];
 assert(gray_plan_prepare(&plan,binary,game,b2,w2)==plan.pending);
 assert(!memcmp(bm,b2,5000) && !memcmp(wm,w2,5000));
 gray_plan_complete(&plan,bm,wm);}
static const unsigned first=28*200+20;
int main(void){
 host_fakes_reset();host_panel_reset();host_panel.require_gray_frame_rate=true;
 {gray_config_t base=TEST_TUNING;gray_adaptive_t quiet=TEST_ADAPTIVE;
  assert(!fast_display_set_gray_config(&base) && !fast_display_set_gray_adaptive(&quiet));
  assert(!fast_display_set_gray_immediate(0) && !fast_display_set_gray_holds(GRAY_HOLD_ALL));}
 memset(binary,255,sizeof(binary));plan.config=(gray_config_t)TEST_TUNING;
 const unsigned expected[4][4]={{0,4,8,12},{8,0,4,12},{8,2,0,12},{12,3,2,0}};
 for(unsigned src=0;src<4;++src)for(unsigned dst=0;dst<4;++dst){
   gray_plan_seed(&plan,binary);
   /* Establish source by actual delivered transitions, not target assignment. */
   one_target(src);while(plan.pending)deliver();
   assert(plan.pixel[first].level==src*64);
   one_target(dst);assert(plan.pixel[first].remaining==expected[src][dst]);
   for(unsigned n=0;n<expected[src][dst];++n){
     one_target(dst);assert(plan.pixel[first].remaining==expected[src][dst]-n);
     unsigned active=gray_plan_masks(&plan,bm,wm);assert(active==23040);
     assert(!bm[0] && !wm[0] && !bm[4999] && !wm[4999]);
     gray_plan_complete(&plan,bm,wm);
   }
   assert(!plan.pending && plan.pixel[first].level==dst*64);
   assert(!gray_plan_masks(&plan,bm,wm));
 }
 /* Per-pair follow-up: every settled source/target combination. The source
  * stays frozen while a dose progresses; remaining=1 always releases. */
 uint8_t fb[5000],fw[5000];
 for(unsigned src=0;src<4;++src)for(unsigned dst=0;dst<4;++dst){
  gray_plan_seed(&plan,binary);one_target(src);while(plan.pending)deliver();
  one_target(dst);
  unsigned remain=plan.pixel[first].remaining;
  if(src!=dst)assert(plan.pixel[first].source==src);
  unsigned all=gray_plan_prepare_pair(&plan,binary,game,bm,wm,fb,fw,GRAY_HOLD_ALL);
  for(unsigned k=0;k<5000;++k)assert(!fb[k] && !fw[k]);
  assert(all==(src==dst ? 0 : 23040));
  unsigned mask=src==dst ? GRAY_HOLD_ALL : GRAY_HOLD_ALL ^ (1u<<(src*4+dst));
  gray_plan_prepare_pair(&plan,binary,game,bm,wm,fb,fw,mask);
  for(unsigned k=0;k<5000;++k){assert(fb[k]==(remain>1 ? bm[k] : 0));assert(fw[k]==(remain>1 ? wm[k] : 0));}
  if(remain){
   gray_plan_complete(&plan,bm,wm);
   assert(plan.pixel[first].source==src && plan.pixel[first].remaining==remain-1);
   gray_plan_complete(&plan,fb,fw);
   assert(plan.pixel[first].remaining==(remain>1 ? remain-2 : 0));
  }
 }
 /* Different policies in the same scan, including two-step rounding.
  * Compare the fused commit against two independently committed scans. */
 gray_plan_seed(&plan,binary);memset(game,0xe4,sizeof(game));gray_plan_target(&plan,binary,game);
 while(plan.pending)deliver();
 memset(game,0x1b,sizeof(game));
 gray_plan_prepare_pair(&plan,binary,game,bm,wm,fb,fw,0xe667u); // only gray targets hold
 gray_plan_t *copy=malloc(sizeof(plan));assert(copy);*copy=plan;
 for(unsigned y=28;y<172;++y)for(unsigned x=20;x<180;++x){
  unsigned i=y*200+x,bit=128u>>(i%8);gray_pixel_t *v=&plan.pixel[i];
  bool continued=v->remaining>1 && (v->target==0 || v->target==3);
  assert(!!((fb[i/8]|fw[i/8])&bit)==continued);
 }
 gray_plan_complete(copy,bm,wm);gray_plan_complete(copy,fb,fw);
 gray_plan_complete_pair(&plan,bm,wm,fb,fw);
 assert(!memcmp(&plan,copy,sizeof(plan)));free(copy);
 /* Interrupted white->black at level 112 uses estimated L (2), not the
  * abandoned K target, for the next pair. Same estimated source/target holds. */
 gray_plan_seed(&plan,binary);one_target(0);while(plan.pixel[first].level>112)deliver();
 unsigned estimate=(plan.pixel[first].level+32)/64;one_target(3);
 assert(plan.pixel[first].source==estimate);
 /* Packed pixel order and all viewport/margin boundaries with four different
  * adjacent target shades. Unchanged black and white HUD pixels select HOLD. */
 for(unsigned i=0;i<5000;++i)binary[i]=(i%2) ? 0x55 : 0xaa;
 gray_plan_seed(&plan,binary);memset(game,0xe4,sizeof(game));
 gray_plan_target(&plan,binary,game);gray_plan_masks(&plan,bm,wm);
 for(unsigned y=0;y<200;++y)for(unsigned x=0;x<200;++x){
   unsigned i=y*200+x, bit=128u>>(i%8);
   if(y<28 || y>=172 || x<20 || x>=180)assert(!(bm[i/8]&bit) && !(wm[i/8]&bit));
   else assert(plan.pixel[i].target==3-((x-20)%4));
 }
 while(plan.pending)deliver();
 for(unsigned y=28;y<172;++y)for(unsigned x=20;x<180;++x)
   assert(plan.pixel[y*200+x].level==(3-((x-20)%4))*64);
 memset(binary,255,sizeof(binary));
 /* Mid-flight reversal uses delivered level, preserves other pixels, and
  * changes neither optical estimate nor count on an omitted/failed delivery. */
 gray_plan_seed(&plan,binary);one_target(0);deliver();deliver();
 assert(plan.pixel[first].level==160 && plan.pixel[first].remaining==10);
 one_target(3);assert(plan.pixel[first].remaining==6 && plan.pixel[first].level==160);
 memset(bm,0,sizeof(bm));memset(wm,0,sizeof(wm));gray_plan_complete(&plan,bm,wm);
 assert(plan.pixel[first].remaining==6 && plan.pixel[first].level==160);
 while(plan.pending)deliver();assert(plan.pixel[first].level==192);
 /* Runtime gain affects only retargeted unfinished work; settled pairs keep
  * their own table dose. Range validation keeps diagonals HOLD and doses <=12. */
 gray_config_t tuning=TEST_TUNING;
 assert(gray_config_valid(&tuning));
 tuning.dose[0][0]=1;assert(!gray_config_valid(&tuning));tuning.dose[0][0]=0;
 tuning.dose[0][1]=0;assert(!gray_config_valid(&tuning));tuning.dose[0][1]=4;
 tuning.retarget_gain=0;plan.config=tuning;
 gray_plan_seed(&plan,binary);one_target(0);deliver();deliver();one_target(3);
 assert(plan.pixel[first].remaining==3);
 while(plan.pending)deliver();
 tuning.dose[3][1]=1;plan.config=tuning;one_target(1);
 assert(plan.pixel[first].remaining==1);deliver();assert(plan.pixel[first].level==64);
 plan.config=(gray_config_t)TEST_TUNING;
 /* Many abrupt retargets: estimate remains bounded and settles when motion stops. */
 for(unsigned n=0;n<1000;++n){one_target((n*17+n/3)%4);deliver();assert(plan.pixel[first].level<=192 && plan.pixel[first].remaining<=12);}
 one_target(2);for(unsigned n=0;n<12;++n)deliver();assert(!plan.pending && plan.pixel[first].level==128);
 /* Real backend: full refresh seeds B/W then schedules the gray viewport. */
 ssd1681_handle_t d=(ssd1681_handle_t)1;
 memset(game,85,sizeof(game));fast_display_set_gray_game(game);
 assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE) && host_panel.frame_rate==7 && host_panel.fast_transport);
 assert(fast_display_pending(d)==23040);
 assert(!fast_display_update(d,binary));assert(gray_plan->pixel[first].level==160);
 assert(!fast_display_update(d,binary));assert(gray_plan->pixel[first].level==128);
 assert(fast_display_pending(d)==1);assert(!fast_display_update(d,binary));assert(!fast_display_pending(d));
 unsigned before=host_panel.scans;assert(!fast_display_update(d,binary) && host_panel.scans==before);
 /* Returning to a menu removes game overlay and finishes the white viewport. */
 fast_display_set_gray_game(NULL);assert(!fast_display_update(d,binary));
 while(fast_display_pending(d))assert(!fast_display_update(d,binary));
 assert(gray_plan->pixel[first].level==192);
 /* Full cleaning during a gray image re-seeds actual B/W, then resumes grays. */
 fast_display_set_gray_game(game);assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));
 assert(gray_plan->pixel[first].level==192 && fast_display_pending(d)==23040);
 assert(!fast_display_update(d,binary));
 unsigned old_level=gray_plan->pixel[first].level, old_remaining=gray_plan->pixel[first].remaining;
 unsigned scans_before_setting=host_panel.scans, updates_before_setting=host_panel.updates;
 tuning=(gray_config_t)TEST_TUNING;tuning.dose[0][1]=2;
 assert(!fast_display_set_gray_config(&tuning));
 assert(host_panel.scans==scans_before_setting && host_panel.updates==updates_before_setting);
 assert(gray_plan->pixel[first].remaining==old_remaining && gray_plan->pixel[first].level==old_level);
 tuning.dose[0][1]=13;assert(fast_display_set_gray_config(&tuning)==ESP_ERR_INVALID_ARG);
 assert(gray_plan->config.dose[0][1]==2);
 tuning=(gray_config_t)TEST_TUNING;assert(!fast_display_set_gray_config(&tuning));

 /* Completion overlaps the scans, so a failed scan may already have advanced the plan;
  * the session ends and the next full refresh reseeds it. */
 host_panel.scan_fails=true;assert(fast_display_update(d,binary)==99);
 assert(!fast_display_pending(d) && fast_display_update(d,binary)==ESP_ERR_INVALID_STATE);
 host_panel.scan_fails=false;
 assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));
 assert(gray_plan->pixel[first].level==192 && fast_display_pending(d)==23040);
 /* Actual waiter admits two 13ms scans per 33.334ms slot. A measured 22ms
  * workload falls back to one without intentionally blocking the next image. */
 for(unsigned slow=0;slow<3;++slow){
   tuning=(gray_config_t)TEST_TUNING;tuning.scan_policy=slow==2;
   assert(!fast_display_set_gray_config(&tuning));
   host_panel.updates=0;host_panel.scan_us=slow==1 ? 22000 : 13000;memset(game,255,sizeof(game));
   assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));assert(!fast_display_update(d,binary));
   prepared=true;scan_cost_us=host_panel.scan_us+1000;
   int64_t epoch=host_clock_us+33334, deadline=epoch, previous_sample=0;
   unsigned images=0, initial=host_panel.scans;host_clock_us=epoch;
   while(images<30){
     fast_display_wait_slot(d,deadline);
     int64_t now=host_clock_us;
     if(now>=deadline){
       if(previous_sample && !(now-previous_sample>=32334 && now-previous_sample<=34334)){fprintf(stderr,"slow=%u image=%u gap=%lld now=%lld deadline=%lld last=%lld cost=%lld\n",slow,images,(long long)(now-previous_sample),(long long)now,(long long)deadline,(long long)last_start,(long long)scan_cost_us);abort();}
       previous_sample=now;++images;
       memset(game,images%2 ? 255 : 0,sizeof(game));
       deadline=fast_display_next_image(deadline,now);
     }
     assert(!fast_display_update(d,binary));
   }
   assert(host_panel.scans-initial >= (slow ? 30 : 58));
   if(slow)assert(host_panel.scans-initial==30);
 }
 /* Voltage applies lazily with analog off, no full/reset and no lost budget. */
 unsigned before_voltage=host_panel.updates, before_changes=host_panel.strength_changes;
 assert(fast_display_set_gray_strength(4)==ESP_ERR_INVALID_ARG);
 assert(!fast_display_set_gray_strength(2));
 assert(host_panel.strength_changes==before_changes && host_panel.updates==before_voltage);
 memset(game,0,sizeof(game));assert(!fast_display_update(d,binary));
 assert(host_panel.strength==2 && host_panel.strength_changes==before_changes+1 && host_panel.analog_offs==1);
 assert(host_panel.updates==before_voltage+1 && gray_applied_strength==2);
 assert(!fast_display_set_gray_strength(0));
 memset(game,255,sizeof(game));assert(!fast_display_update(d,binary));
 assert(host_panel.strength==0 && host_panel.analog_offs==2);
 assert(!fast_display_set_gray_strength(3));
 /* Immediate HOLD has no planner/log/sleep between the two scans, consumes
  * one dose only, and suppresses the redundant trailing HOLD. */
 assert(!fast_display_set_gray_strength(0));
 assert(!fast_display_set_gray_immediate(1));
 assert(fast_display_set_gray_immediate(2)==ESP_ERR_INVALID_ARG);
 host_panel.updates=0;memset(binary,255,sizeof(binary));memset(game,255,sizeof(game));
 tuning=(gray_config_t)TEST_TUNING;assert(!fast_display_set_gray_config(&tuning));
 assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));
 unsigned pair_first=host_panel.scans;
 assert(!fast_display_update(d,binary));assert(fast_display_gray_last_scans()==2);
 assert(!host_panel.scan_hold[pair_first] && host_panel.scan_hold[pair_first+1]);
 assert(host_panel.scan_start[pair_first+1]==host_panel.scan_end[pair_first]);
 assert(gray_plan->pixel[first].remaining==11 && gray_plan->pixel[first].level==16*11);
 while(fast_display_pending(d))assert(!fast_display_update(d,binary));
 assert(!hold_end_pending && gray_plan->pixel[first].level==0);
 before=host_panel.scans;assert(!fast_display_update(d,binary));
 assert(host_panel.scans==before && fast_display_gray_last_scans()==0);
 /* One pair per image slot, even if the saved policy permits extras. */
 for(unsigned policy=0;policy<2;++policy){
  host_panel.updates=0;tuning.scan_policy=policy;assert(!fast_display_set_gray_config(&tuning));
  assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));assert(!fast_display_update(d,binary));
  int64_t deadline=host_clock_us+33334,previous_image=0;unsigned initial=host_panel.scans;
  for(unsigned im=0;im<30;++im){
   fast_display_wait_slot(d,deadline);int64_t now=host_clock_us;
   if(previous_image)assert(now-previous_image>=32334 && now-previous_image<=34334);
   previous_image=now;deadline=fast_display_next_image(deadline,now);
   memset(game,im%2 ? 255 : 0,sizeof(game));assert(!fast_display_update(d,binary));
   assert(fast_display_gray_last_scans()==2);
  }
  assert(host_panel.scans-initial==60);
 }
 /* HOLD failure still commits the physically delivered drive once, then
  * invalidates the context; the failed HOLD must not consume another dose. */
 host_panel.updates=0;memset(game,255,sizeof(game));assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));
 host_panel.fail_scan_at=host_panel.scans+2;
 assert(fast_display_update(d,binary)==99 && fast_display_gray_last_scans()==2);
 assert(gray_plan->pixel[first].remaining==11 && gray_plan->pixel[first].level==176);
 assert(!owner && !fast_display_pending(d));host_panel.fail_scan_at=0;
 /* Pair fits the frontend's last allowed slot, preserving one final full. */
 host_panel.updates=0;assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));host_panel.updates=SSD1681_MAX_UPDATES-3;
 assert(!fast_display_update(d,binary));assert(host_panel.updates==SSD1681_MAX_UPDATES-1);
 assert(!fast_display_finish(d,binary) && host_panel.updates==SSD1681_MAX_UPDATES);
 /* Toggling back is pure config, preserving pixel state and dose settings. */
 host_panel.updates=0;assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));
 unsigned old_scans=host_panel.scans;assert(!fast_display_set_gray_immediate(0));assert(host_panel.scans==old_scans);
 assert(!fast_display_update(d,binary) && fast_display_gray_last_scans()==1);
 assert(!fast_display_set_gray_immediate(1));
 /* Mixed second scans consume exactly two doses only for HOLD-Off pairs. */
 host_panel.updates=0;memset(binary,255,sizeof(binary));memset(game,255,sizeof(game));
 assert(!fast_display_set_gray_holds(GRAY_HOLD_ALL ^ (1u<<12))); // W -> K off
 assert(fast_display_set_gray_holds(0)==ESP_ERR_INVALID_ARG);
 assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));unsigned begin_pair=host_panel.scans;
 assert(!fast_display_update(d,binary));
 assert(gray_plan->pixel[first].remaining==10 && gray_plan->pixel[first].level==160);
 assert(!host_panel.scan_hold[begin_pair] && !host_panel.scan_hold[begin_pair+1]);
 assert(host_panel.scan_start[begin_pair+1]==host_panel.scan_end[begin_pair]);
 while(fast_display_pending(d))assert(!fast_display_update(d,binary));
 assert(gray_plan->pixel[first].level==0 && host_panel.scan_hold[host_panel.scans-1]);
 assert(host_panel.scans-begin_pair==13); // 12 drive pulses plus final HOLD next slot.
 /* Failure of a driving second scan ends the session; completion of the pair may already have run. */
 host_panel.updates=0;assert(!fast_display_full(d,binary,DISPLAY_GRAYSCALE));host_panel.fail_scan_at=host_panel.scans+2;
 assert(fast_display_update(d,binary)==99);
 assert(!fast_display_pending(d) && fast_display_update(d,binary)==ESP_ERR_INVALID_STATE);
 host_panel.fail_scan_at=0;assert(!fast_display_set_gray_holds(GRAY_HOLD_ALL));
 /* Switching back to dithered restores FR2 and binary counters. */
 assert(!fast_display_full(d,binary,DISPLAY_DITHERED) && host_panel.frame_rate==2 && host_panel.strength==0 && gray_plan==NULL && drive_counters);
 host_heap_fail_all=true;assert(fast_display_full(d,binary,DISPLAY_GRAYSCALE)==ESP_ERR_NO_MEM);
 assert(!fast_display_pending(d));
 heap_caps_free(drive_counters);
 puts("PASS: all 16 doses, partial retargets, HOLD margins/endings, gray/menu/full recovery, delivery faults, 30Hz scheduling and dithered isolation");
}
'''

run(CODE, modules=['gray_plan.c', 'gray_plan_parallel.c', 'drive_counters.c'], fakes=['host_panel.c'])
