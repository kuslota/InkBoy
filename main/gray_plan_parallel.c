#include "gray_plan_parallel.h"
#include <string.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
#include <pthread.h>
#endif

#define GRAY_PARALLEL_SPLIT_ROW 100u
#define GRAY_PARALLEL_WORKER_STACK_BYTES 4096u
/* Just below the gameplay display task (20): it preempts the worker the moment SPI/BUSY completes,
 * while the worker still outranks the background tasks that the inline completion used to outrank. */
#define GRAY_PARALLEL_FIRST_PRIORITY 19

/* Policy and counters are copied to internal task memory for the pixel walk; read/modify/write
 * on the PSRAM planner's bitfields is too slow on dense updates. */
typedef struct {
    gray_config_t config;
    gray_adaptive_t adaptive;
    bool adaptive_configured, weak_enabled;
    unsigned origin_targets; /* bit t: a settled retarget to shade t records its origin */
    /* budget[1] is budget[0] already rescaled for an interrupted transition. */
    uint8_t budget[2][193][4], scaled[GRAY_ROUTES+1][GRAY_DOSE_MAX+1];
} target_policy_t;

typedef struct {
    int pending_delta;
    unsigned returns, reversals, active_count;
} parallel_result_t;

typedef struct {
    gray_plan_t *plan;
    const uint8_t *binary, *game;
    const uint8_t *follow_black_in, *follow_white_in;
    uint8_t *black_out, *white_out, *follow_black_out, *follow_white_out;
    const uint8_t *black_in, *white_in;
    unsigned hold_mask;
    bool do_target, do_masks, do_complete;
} parallel_job_t;

typedef struct gray_parallel_context gray_parallel_context_t;
typedef struct {
    gray_parallel_context_t *ctx;
    unsigned begin, end;
    parallel_result_t result;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t start, done;
    TaskHandle_t task;
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
    pthread_t thread;
    bool started;
#endif
} parallel_worker_t;

struct gray_parallel_context {
    target_policy_t policy;
    bool policy_valid;
    uint8_t pulse[4][193][GRAY_DOSE_MAX+1];
    parallel_job_t job;
    /* second (rows 100-199, core 1) serves every job; first (rows 0-99, core 0) only async completion. */
    parallel_worker_t first, second;
    bool async_pending;
};
static gray_parallel_context_t *parallel_context;
#ifndef ESP_PLATFORM
bool gray_plan_parallel_host_init_fails;
#endif

static void build_policy(target_policy_t *policy, const gray_plan_t *p)
{
    policy->config=p->config;
    policy->adaptive=p->adaptive;
    policy->adaptive_configured=p->adaptive_configured;
    policy->weak_enabled=p->adaptive_configured && p->adaptive.weak_wl;
    policy->origin_targets=p->adaptive_configured && gray_adaptive_gray_routes(&p->adaptive) ? 0xfu : 0x9u;
    for(unsigned route=0;route<=GRAY_ROUTES;++route) {
        unsigned gain=route ? (policy->adaptive_configured ? policy->adaptive.reverse_percent[route-1] : 150) :
                              gray_retarget_percent(&policy->config);
        for(unsigned budget=0;budget<=GRAY_DOSE_MAX;++budget) {
            unsigned value=(budget*gain+50)/100;
            policy->scaled[route][budget]=value<1 ? 1 : value>GRAY_DOSE_MAX ? GRAY_DOSE_MAX : value;
        }
    }
    for(unsigned level=0;level<=192;++level) {
        unsigned low=level/64, fraction=level%64, high=low<3 ? low+1 : low;
        for(unsigned target=0;target<4;++target) {
            unsigned base=(policy->config.dose[low][target]*(64-fraction)+
                           policy->config.dose[high][target]*fraction+63)/64;
            policy->budget[0][level][target]=(uint8_t)base;
            policy->budget[1][level][target]=base ? policy->scaled[0][base] : 0;
        }
    }
}

static void ensure_policy(gray_parallel_context_t *ctx, const gray_plan_t *p)
{
    target_policy_t *policy=&ctx->policy;
    if(!ctx->policy_valid || memcmp(&policy->config,&p->config,sizeof(p->config)) ||
       memcmp(&policy->adaptive,&p->adaptive,sizeof(p->adaptive)) ||
       policy->adaptive_configured!=p->adaptive_configured) {
        build_policy(policy,p);
        ctx->policy_valid=true;
    }
}

typedef struct {
    unsigned history_mode, returns, reversals;
    int pending_delta;
} target_counters_t;

/* retarget()'s budget when the pixel returns to its recorded origin (to == origin, 1 or 2); every
 * other retarget has route -1 and keeps the policy budget. */
static __attribute__((noinline)) unsigned route_budget(const target_policy_t *restrict plan,
                                                       target_counters_t *restrict counters,
                                                       unsigned packed, const gray_history_t *restrict h,
                                                       unsigned target)
{
    const gray_pixel_t p={.packed=(uint16_t)packed};
    const bool gray_routes=plan->adaptive_configured && gray_adaptive_gray_routes(&plan->adaptive);
    unsigned base=plan->budget[0][p.level][target];
    unsigned budget=plan->budget[p.remaining!=0][p.level][target];
    unsigned steps=p.target==0 ? h->black_steps : p.target==3 ? h->white_steps :
        gray_routes && h->origin<4 ? (p.target<h->origin ? h->black_steps : h->white_steps) : 0;
    int route=steps ? gray_adaptive_route(h->origin,p.target,target) : -1;
    if (!plan->adaptive_configured && route!=GRAY_ROUTE_LKL) route=-1;
    if(route>=0) {
        if(!p.remaining && (counters->history_mode&1) &&
           p.level==p.target*64 && steps==plan->config.dose[h->origin][p.target]) {
            int delta=plan->adaptive_configured ? plan->adaptive.return_delta[route] : -1;
            if(delta) {
                int adjusted=(int)base+delta;
                budget=adjusted<1 ? 1 : adjusted>(int)GRAY_DOSE_MAX ? GRAY_DOSE_MAX : (unsigned)adjusted;
                ++counters->returns;
            }
        } else if(p.remaining && base && (counters->history_mode&2)) {
            unsigned gain=plan->adaptive_configured ? plan->adaptive.reverse_percent[route] : 150;
            if(gain) { budget=plan->scaled[route+1][base]; ++counters->reversals; }
        }
    }
    return budget;
}

#define PIXEL_LEVEL 0x00ffu
#define PIXEL_REMAINING 0x0f00u
#define PIXEL_SETTLED_W 0x30c0u /* target W, nothing remaining, level 192 */

/* Same result as retarget() for the 4 pixels of one source group; targets holds 2 bits per pixel. */
static inline void retarget_group(const target_policy_t *restrict policy, target_counters_t *restrict counters,
                                  gray_pixel_t *restrict pixel, gray_history_t *restrict history,
                                  unsigned targets, unsigned first_bit, uint8_t *restrict active,
                                  uint8_t *restrict weak_wl)
{
    unsigned active_bits=*active, weak_bits=*weak_wl;
    int pending_delta=0;
    for(unsigned k=0;k<4;++k,targets>>=2) {
        const unsigned target=targets&3u, packed=pixel[k].packed;
        if(target==((packed>>12)&3u)) continue;
        const unsigned level=packed&PIXEL_LEVEL, pending=(packed&PIXEL_REMAINING)!=0;
        unsigned budget=policy->budget[pending][level][target];
        if(history) {
            gray_history_t *h=&history[k];
            if(h->origin==target && (target==1 || target==2))
                budget=route_budget(policy,counters,packed,h,target);
            const bool settled=!(packed&(PIXEL_REMAINING|63u)) && (policy->origin_targets>>target)&1u;
            *h=(gray_history_t){.origin=settled ? (uint8_t)(level>>6) : 4};
        }
        pixel[k].packed=(uint16_t)(level|budget<<8|target<<12|((level+32u)>>6)<<14);
        const unsigned bit=128u>>(first_bit+k);
        const bool weak=target==2 && policy->weak_enabled && (packed&0x3fffu)==PIXEL_SETTLED_W;
        weak_bits=weak ? weak_bits|bit : weak_bits&~bit;
        active_bits=budget ? active_bits|bit : active_bits&~bit;
        pending_delta+=(int)(budget!=0)-(int)pending;
    }
    if(weak_bits!=*weak_wl) *weak_wl=(uint8_t)weak_bits;
    if(active_bits!=*active) *active=(uint8_t)active_bits;
    counters->pending_delta+=pending_delta;
}

static inline unsigned group_ceil(unsigned x)
{
    return (x+3)/4<50 ? (x+3)/4 : 50;
}

static void target_range(const target_policy_t *cached, gray_plan_t *p,
                         const uint8_t *binary, const uint8_t *game,
                         unsigned begin, unsigned end, parallel_result_t *result)
{
    /* Binary nibble (MSB = leftmost pixel) to 2-bit targets, leftmost pixel in the low bits. */
#define NIBBLE_TARGETS(n) ((((n) >> 3 & 1) | ((n) >> 2 & 1) << 2 | ((n) >> 1 & 1) << 4 | ((n) & 1) << 6) * 3)
    static const uint8_t binary_targets[16]={
        NIBBLE_TARGETS(0),NIBBLE_TARGETS(1),NIBBLE_TARGETS(2),NIBBLE_TARGETS(3),
        NIBBLE_TARGETS(4),NIBBLE_TARGETS(5),NIBBLE_TARGETS(6),NIBBLE_TARGETS(7),
        NIBBLE_TARGETS(8),NIBBLE_TARGETS(9),NIBBLE_TARGETS(10),NIBBLE_TARGETS(11),
        NIBBLE_TARGETS(12),NIBBLE_TARGETS(13),NIBBLE_TARGETS(14),NIBBLE_TARGETS(15)};
#undef NIBBLE_TARGETS
    const target_policy_t *restrict policy=cached;
    target_counters_t counters={.history_mode=p->history_mode};
    gray_history_t *restrict history=p->history;
    gray_pixel_t *restrict pixels=p->pixel;
    uint8_t *restrict active=p->active, *restrict weak_wl=p->weak_wl;
    uint8_t *restrict previous_binary=p->previous_binary, *restrict previous_game=p->previous_game;
    const game_view_t view=game_view_or_classic(p->view), old=game_view_or_classic(p->had_view);
    const unsigned row=view.width/4;
    /* previous_game is comparable only while the layout is unchanged. */
    const bool same=game && p->had_game && game_view_equal(view,old);
    /* Dirty source rows avoid traversing all optical state on every scan.
     * Cached inputs are owned copies: the caller may reuse its snapshot buffer. */
    for (unsigned y=begin; y<end; ++y) {
        /* in_new: the game now drives this row; in_old: it did on the previous target. */
        bool in_new = game && y>=view.y && y<view.y+view.height;
        bool in_old = p->had_game && y>=old.y && y<old.y+old.height;
        bool changed = memcmp(p->previous_binary+y*25, binary+y*25, 25) != 0;
        if (in_new || in_old) {
            if (same) changed |= memcmp(p->previous_game+(y-view.y)*row, game+(y-view.y)*row, row) != 0;
            else changed = true;
        }
        if (!changed) continue;
        /* Groups [game_begin, game_end) take the game image; [old_begin, old_end) took it last time. */
        const unsigned game_begin=in_new ? group_ceil(view.x) : 50, game_end=in_new ? group_ceil(view.x+view.width) : 50;
        const unsigned old_begin=in_old ? group_ceil(old.x) : 0, old_end=in_old ? group_ceil(old.x+old.width) : 0;
        /* A 4-pixel group never straddles a bitmap byte (200 % 8 == 0, x0 % 4 == 0). */
        gray_pixel_t *restrict row_pixels=pixels+y*200;
        gray_history_t *restrict row_history=history ? history+y*200 : NULL;
        uint8_t *restrict row_active=active+y*25, *restrict row_weak=weak_wl+y*25;
        for(unsigned group=0;group<50;++group) {
            if(group==game_begin) {
                const uint8_t *incoming=game+(y-view.y)*row, *before=previous_game+(y-view.y)*row;
                for(;group<game_end;++group,++incoming,++before) {
                    if(same && *incoming==*before) continue;
                    retarget_group(policy,&counters,row_pixels+group*4,row_history ? row_history+group*4 : NULL,
                                   ~*incoming&0xffu,group%2*4,row_active+group/2,row_weak+group/2);
                }
                if(group>=50) break;
            }
            const unsigned shift=group&1 ? 0 : 4, incoming=(binary[y*25+group/2]>>shift)&15;
            if((group<old_begin || group>=old_end) && incoming==((previous_binary[y*25+group/2]>>shift)&15)) continue;
            retarget_group(policy,&counters,row_pixels+group*4,row_history ? row_history+group*4 : NULL,
                           binary_targets[incoming],group%2*4,row_active+group/2,row_weak+group/2);
        }
        memcpy(previous_binary+y*25, binary+y*25, 25);
        if (in_new) memcpy(previous_game+(y-view.y)*row, game+(y-view.y)*row, row);
    }
    result->pending_delta=counters.pending_delta;
    result->returns=counters.returns;
    result->reversals=counters.reversals;
}

static inline bool row_idle(const uint8_t *active, unsigned row)
{
    uint32_t words[6], any=active[row*25+24];
    memcpy(words,active+row*25,sizeof(words));
    for(unsigned i=0;i<6;++i) any|=words[i];
    return any==0;
}

static unsigned mask_range(const gray_plan_t *p, uint8_t *black, uint8_t *white,
                           uint8_t *follow_black, uint8_t *follow_white,
                           unsigned hold_mask, unsigned begin, unsigned end)
{
    unsigned active = 0;
    memset(black+begin*25,0,(end-begin)*25);
    memset(white+begin*25,0,(end-begin)*25);
    if(follow_black) {
        memset(follow_black+begin*25,0,(end-begin)*25);
        memset(follow_white+begin*25,0,(end-begin)*25);
    }
    const uint8_t *restrict active_map=p->active;
    const gray_pixel_t *restrict pixels=p->pixel;
    /* state>>12 is source*4+target, the hold bit index; holding everything disables follow-up. */
    const unsigned hold=follow_black ? hold_mask : GRAY_HOLD_ALL;
    for (unsigned y=begin; y<end; ++y) {
        if(row_idle(active_map,y)) {  continue; }
        for(unsigned byte=y*25; byte<y*25+25; ++byte) {
            unsigned bits=active_map[byte], b=0,w=0,fb=0,fw=0;
            const gray_pixel_t *group=pixels+byte*8+7;
            while (bits) {
                const unsigned shift=__builtin_ctz(bits), bit=1u<<shift, state=group[-(int)shift].packed;
                const bool lighten=((state>>6)&0xc0u) > (state&PIXEL_LEVEL);
                const bool follow=(state&PIXEL_REMAINING) > 0x100u && !((hold>>(state>>12))&1u);
                if(lighten) { w|=bit; if(follow) fw|=bit; }
                else { b|=bit; if(follow) fb|=bit; }
                bits &= bits-1;
                ++active;
            }
            black[byte]=b;white[byte]=w;
            if(follow_black) { follow_black[byte]=fb;follow_white[byte]=fw; }
        }
    }
    return active;
}

static void complete_range(const gray_parallel_context_t *ctx, gray_plan_t *p,
                           const uint8_t *black, const uint8_t *white,
                           const uint8_t *follow_black, const uint8_t *follow_white,
                           unsigned begin, unsigned end, parallel_result_t *result)
{
    int pending_delta=0;
    uint8_t *restrict history=(uint8_t *)p->history;
    gray_pixel_t *restrict pixels=p->pixel;
    uint8_t *restrict active_map=p->active;
    const uint8_t *restrict black_in=black, *restrict white_in=white;
    const uint8_t (*restrict pulse)[193][GRAY_DOSE_MAX+1]=ctx->pulse;
    const unsigned counted_targets=!history ? 0 :
        p->adaptive_configured && gray_adaptive_gray_routes(&p->adaptive) ? 0xfu : 0x9u;
    _Static_assert(sizeof(gray_history_t)==3 && offsetof(gray_history_t,black_steps)==1 &&
                   offsetof(gray_history_t,white_steps)==2,"history counters are indexed by direction");
    for (unsigned y=begin; y<end; ++y) {
        if(row_idle(active_map,y)) {  continue; }
        for(unsigned byte=y*25; byte<y*25+25; ++byte) {
            const unsigned fb=follow_black ? follow_black[byte] : 0;
            const unsigned fw=follow_white ? follow_white[byte] : 0;
            const unsigned black_bits=black_in[byte], white_bits=white_in[byte];
            const unsigned active_before=active_map[byte];
            unsigned active=active_before;
            unsigned bits=(black_bits | white_bits | fb | fw) & active;
            /* A pulse moves a pixel only toward its target, and a scan whose black and white bits are
             * both set delivers nothing: bytes 0/1 hold lighten/darken first-scan steps, 2/3 follow-up. */
            const uint32_t steps_by_direction=(white_bits&~black_bits) | (black_bits&~white_bits)<<8 |
                                              (fw&~fb)<<16 | (fb&~fw)<<24;
            while (bits) {
                const unsigned shift=__builtin_ctz(bits), i=byte*8+7-shift;
                bits &= bits-1;
                const unsigned state=pixels[i].packed, shade=(state>>12)&3u;
                const unsigned level=state&PIXEL_LEVEL, goal=shade*64, remaining=(state>>8)&15u;
                if(level==goal) continue;
                const unsigned darken=level>goal;
                const uint32_t selected=steps_by_direction>>(shift+8*darken);
                const unsigned steps=(selected&1u)+((selected>>16)&1u);
                const unsigned delivered=steps<remaining ? steps : remaining;
                if(!delivered) continue;
                unsigned next=pulse[shade][level][remaining];
                if(delivered==2) next=pulse[shade][next][remaining-1];
                if(remaining==delivered) { active &= ~(1u<<shift); --pending_delta; }
                pixels[i].packed=(uint16_t)((state&0xf000u)|(remaining-delivered)<<8|next);
                if((counted_targets>>shade)&1u) {
                    uint8_t *counter=&history[i*3+2-darken];
                    const unsigned count=*counter+delivered;
                    *counter=(uint8_t)(count>255 ? 255 : count);
                }
            }
            if(active!=active_before) active_map[byte]=(uint8_t)active;
        }
    }
    result->pending_delta=pending_delta;
}

static void run_range(const gray_parallel_context_t *ctx, const parallel_job_t *job,
                      unsigned begin, unsigned end, parallel_result_t *result)
{
    *result=(parallel_result_t){0};
    if(job->do_target) {
        target_range(&ctx->policy,job->plan,job->binary,job->game,begin,end,result);
    }
    if(job->do_masks) {
        result->active_count=mask_range(job->plan,job->black_out,job->white_out,
                                        job->follow_black_out,job->follow_white_out,
                                        job->hold_mask,begin,end);
    }
    if(job->do_complete) {
        complete_range(ctx,job->plan,job->black_in,job->white_in,
                       job->follow_black_in,job->follow_white_in,begin,end,result);
    }
}

#ifdef ESP_PLATFORM
static void parallel_worker(void *arg)
{
    parallel_worker_t *w=arg;
    for(;;) {
        xSemaphoreTake(w->start,portMAX_DELAY);
        run_range(w->ctx,&w->ctx->job,w->begin,w->end,&w->result);
        xSemaphoreGive(w->done);
    }
}

static void worker_delete(parallel_worker_t *w)
{
    if(w->task) vTaskDelete(w->task);
    if(w->start) vSemaphoreDelete(w->start);
    if(w->done) vSemaphoreDelete(w->done);
    w->task=NULL; w->start=w->done=NULL;
}

static bool worker_create(parallel_worker_t *w, const char *name, UBaseType_t priority, BaseType_t core)
{
    w->start=xSemaphoreCreateBinary();
    w->done=xSemaphoreCreateBinary();
    w->task=NULL;
    if(w->start && w->done &&
       xTaskCreatePinnedToCore(parallel_worker,name,GRAY_PARALLEL_WORKER_STACK_BYTES,
                               w,priority,&w->task,core)==pdPASS) return true;
    worker_delete(w);
    return false;
}
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
static void *parallel_host_worker(void *arg)
{
    parallel_worker_t *w=arg;
    run_range(w->ctx,&w->ctx->job,w->begin,w->end,&w->result);
    return NULL;
}

static void host_start(parallel_worker_t *w)
{
    w->started=pthread_create(&w->thread,NULL,parallel_host_worker,w)==0;
}

static void host_join(parallel_worker_t *w)
{
    if(w->started) pthread_join(w->thread,NULL);
    else run_range(w->ctx,&w->ctx->job,w->begin,w->end,&w->result);
    w->started=false;
}
#endif

bool gray_plan_parallel_init(void)
{
    if(parallel_context) return true;
#ifdef ESP_PLATFORM
    gray_parallel_context_t *ctx=heap_caps_malloc(sizeof(*ctx),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
    gray_parallel_context_t *ctx=gray_plan_parallel_host_init_fails ? NULL : malloc(sizeof(*ctx));
#endif
    if(!ctx) return false;
    ctx->policy_valid=false;
    ctx->async_pending=false;
    ctx->first=(parallel_worker_t){.ctx=ctx,.begin=0,.end=GRAY_PARALLEL_SPLIT_ROW};
    ctx->second=(parallel_worker_t){.ctx=ctx,.begin=GRAY_PARALLEL_SPLIT_ROW,.end=200};
    for(int target=0;target<4;++target)
        for(int level=0;level<=192;++level)
            for(int n=1;n<=(int)GRAY_DOSE_MAX;++n)
                ctx->pulse[target][level][n]=(uint8_t)(level+(target*64-level)/n);
#ifdef ESP_PLATFORM
    if(!worker_create(&ctx->second,"gray_parallel",5,1)) {
        heap_caps_free(ctx);
        return false;
    }
    (void)worker_create(&ctx->first,"gray_complete",GRAY_PARALLEL_FIRST_PRIORITY,0);
#endif
    parallel_context=ctx;
    return true;
}

void gray_plan_parallel_deinit(void)
{
    gray_plan_parallel_complete_join();
    gray_parallel_context_t *ctx=parallel_context;
    parallel_context=NULL;
    if(!ctx) return;
#ifdef ESP_PLATFORM
    worker_delete(&ctx->first);
    worker_delete(&ctx->second);
    heap_caps_free(ctx);
#else
    free(ctx);
#endif
}

size_t gray_plan_parallel_acceleration_bytes(void)
{
    return parallel_context ? sizeof(*parallel_context) : 0;
}

bool gray_plan_parallel_active(void)
{
#ifdef ESP_PLATFORM
    return parallel_context!=NULL;
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
    return parallel_context!=NULL;
#else
    return false;
#endif
}

static unsigned apply_results(const parallel_job_t *job, const parallel_result_t *first,
                              const parallel_result_t *second)
{
    gray_plan_t *p=job->plan;
    if(job->do_target || job->do_complete)
        p->pending+=(unsigned)(first->pending_delta+second->pending_delta);
    if(job->do_target) {
        p->history_returns+=first->returns+second->returns;
        p->history_reversals+=first->reversals+second->reversals;
    }
    if(job->do_target) {
        p->had_game=job->game!=NULL;
        if(job->game) p->had_view=game_view_or_classic(p->view);
    }
    return first->active_count+second->active_count;
}

static unsigned dispatch(parallel_job_t job)
{
    gray_plan_parallel_complete_join();
    gray_parallel_context_t *ctx=parallel_context;
    ctx->job=job;
    if(job.do_target) ensure_policy(ctx,job.plan);
    parallel_result_t main_result;
#ifdef ESP_PLATFORM
    xSemaphoreGive(ctx->second.start);
    run_range(ctx,&ctx->job,0,GRAY_PARALLEL_SPLIT_ROW,&main_result);
    xSemaphoreTake(ctx->second.done,portMAX_DELAY);
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
    host_start(&ctx->second);
    run_range(ctx,&ctx->job,0,GRAY_PARALLEL_SPLIT_ROW,&main_result);
    host_join(&ctx->second);
#else
    run_range(ctx,&ctx->job,0,GRAY_PARALLEL_SPLIT_ROW,&main_result);
    run_range(ctx,&ctx->job,GRAY_PARALLEL_SPLIT_ROW,200,&ctx->second.result);
#endif
    unsigned active=apply_results(&ctx->job,&main_result,&ctx->second.result);
    return active;
}

bool gray_plan_parallel_complete_begin(gray_plan_t *p, const uint8_t *black, const uint8_t *white,
                                       const uint8_t *follow_black, const uint8_t *follow_white)
{
    gray_parallel_context_t *ctx=parallel_context;
#ifdef ESP_PLATFORM
    if(!ctx || !ctx->first.task || uxTaskPriorityGet(NULL)<=GRAY_PARALLEL_FIRST_PRIORITY) return false;
#else
    if(!ctx) return false;
#endif
    gray_plan_parallel_complete_join();
    ctx->job=(parallel_job_t){.plan=p,.black_in=black,.white_in=white,.follow_black_in=follow_black,
                              .follow_white_in=follow_white,.do_complete=true};
    ctx->async_pending=true;
#ifdef ESP_PLATFORM
    xSemaphoreGive(ctx->first.start);
    xSemaphoreGive(ctx->second.start);
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
    host_start(&ctx->first);
    host_start(&ctx->second);
#endif
    return true;
}

void gray_plan_parallel_complete_join(void)
{
    gray_parallel_context_t *ctx=parallel_context;
    if(!ctx || !ctx->async_pending) return;
#ifdef ESP_PLATFORM
    xSemaphoreTake(ctx->first.done,portMAX_DELAY);
    xSemaphoreTake(ctx->second.done,portMAX_DELAY);
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
    host_join(&ctx->first);
    host_join(&ctx->second);
#else
    run_range(ctx,&ctx->job,ctx->first.begin,ctx->first.end,&ctx->first.result);
    run_range(ctx,&ctx->job,ctx->second.begin,ctx->second.end,&ctx->second.result);
#endif
    ctx->async_pending=false;
    (void)apply_results(&ctx->job,&ctx->first.result,&ctx->second.result);
}

void gray_plan_parallel_seed(gray_plan_t *p, const uint8_t *binary)
{
    gray_plan_parallel_complete_join();
    gray_plan_seed(p,binary);
}

void gray_plan_parallel_target(gray_plan_t *p, const uint8_t *binary, const uint8_t *game)
{
    if(!parallel_context) { gray_plan_target(p,binary,game); return; }
    (void)dispatch((parallel_job_t){.plan=p,.binary=binary,.game=game,.do_target=true});
}

unsigned gray_plan_parallel_masks(const gray_plan_t *p, uint8_t *black, uint8_t *white)
{
    if(!parallel_context) return gray_plan_masks(p,black,white);
    return dispatch((parallel_job_t){.plan=(gray_plan_t *)p,.black_out=black,.white_out=white,
                              .hold_mask=GRAY_HOLD_ALL,.do_masks=true});
}

unsigned gray_plan_parallel_prepare(gray_plan_t *p, const uint8_t *binary, const uint8_t *game,
                             uint8_t *black, uint8_t *white)
{
    if(!parallel_context) return gray_plan_prepare(p,binary,game,black,white);
    return dispatch((parallel_job_t){.plan=p,.binary=binary,.game=game,.black_out=black,
                              .white_out=white,.hold_mask=GRAY_HOLD_ALL,
                              .do_target=true,.do_masks=true});
}

unsigned gray_plan_parallel_prepare_pair(gray_plan_t *p, const uint8_t *binary, const uint8_t *game,
                                  uint8_t *black, uint8_t *white, uint8_t *follow_black,
                                  uint8_t *follow_white, unsigned hold_mask)
{
    if(!parallel_context) return gray_plan_prepare_pair(p,binary,game,black,white,follow_black,follow_white,hold_mask);
    return dispatch((parallel_job_t){.plan=p,.binary=binary,.game=game,.black_out=black,
                              .white_out=white,.follow_black_out=follow_black,
                              .follow_white_out=follow_white,.hold_mask=hold_mask,
                              .do_target=true,.do_masks=true});
}

void gray_plan_parallel_complete(gray_plan_t *p, const uint8_t *black, const uint8_t *white)
{
    if(!parallel_context) { gray_plan_complete(p,black,white); return; }
    (void)dispatch((parallel_job_t){.plan=p,.black_in=black,.white_in=white,.do_complete=true});
}

void gray_plan_parallel_complete_pair(gray_plan_t *p, const uint8_t *black, const uint8_t *white,
                               const uint8_t *follow_black, const uint8_t *follow_white)
{
    if(!parallel_context) { gray_plan_complete_pair(p,black,white,follow_black,follow_white); return; }
    (void)dispatch((parallel_job_t){.plan=p,.black_in=black,.white_in=white,
                              .follow_black_in=follow_black,.follow_white_in=follow_white,
                              .do_complete=true});
}
