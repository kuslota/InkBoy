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

/* Policy and counters are copied to internal task memory for the pixel walk; read/modify/write
 * on the PSRAM planner's bitfields is too slow on dense updates. */
typedef struct {
    gray_config_t config;
    gray_adaptive_t adaptive;
    bool adaptive_configured;
    unsigned history_mode, history_returns, history_reversals;
    uint8_t budget[193][4], scaled[GRAY_ROUTES+1][GRAY_DOSE_MAX+1];
} target_policy_t;

static void retarget(target_policy_t *plan, gray_pixel_t *p, gray_history_t *h,
                     unsigned target, bool *weak)
{
    const gray_config_t *config=&plan->config;
    /* Interpolate doses between adjacent model shades, using delivered progress rather than an
     * abandoned target as the settled source. */
    unsigned base = plan->budget[p->level][target];
    unsigned budget=p->remaining && base ? plan->scaled[0][base] : base;
    if (h) {
        bool gray_routes=plan->adaptive_configured && gray_adaptive_gray_routes(&plan->adaptive);
        unsigned steps=p->target==0 ? h->black_steps : p->target==3 ? h->white_steps :
            gray_routes && h->origin<4 ? (p->target<h->origin ? h->black_steps : h->white_steps) : 0;
        int route=steps ? gray_adaptive_route(h->origin,p->target,target) : -1;
        if (!plan->adaptive_configured && route!=GRAY_ROUTE_LKL) route=-1;
        if(route>=0) {
            if(!p->remaining && (plan->history_mode&1) &&
               p->level==p->target*64 && steps==config->dose[h->origin][p->target]) {
                int delta=plan->adaptive_configured ? plan->adaptive.return_delta[route] : -1;
                if(delta) {
                    int adjusted=(int)base+delta;
                    budget=adjusted<1 ? 1 : adjusted>(int)GRAY_DOSE_MAX ? GRAY_DOSE_MAX : (unsigned)adjusted;
                    ++plan->history_returns;
                }
            } else if(p->remaining && base && (plan->history_mode&2)) {
                unsigned gain=plan->adaptive_configured ? plan->adaptive.reverse_percent[route] : 150;
                if(gain) { budget=plan->scaled[route+1][base]; ++plan->history_reversals; }
            }
        }
        h->origin=(target==0 || target==3 || gray_routes) && !p->remaining && p->level%64==0 ? p->level/64 : 4;
        h->black_steps=h->white_steps=0;
    }
    /* A model source rounded up to W during an interrupted transition must not
     * receive weakened darkening. Require the previous white dose to finish. */
    *weak = plan->adaptive_configured && plan->adaptive.weak_wl && target==2 &&
        p->target==3 && !p->remaining && p->level==192;
    /* Freeze the nearest estimated source for this transition. Do not change
     * pair policy as each pulse advances the intermediate level. */
    p->source = (p->level + 32u) / 64u;
    p->remaining = budget;
    p->target = target;
}

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

typedef struct {
    target_policy_t policy;
    bool policy_valid;
    uint8_t pulse[4][193][GRAY_DOSE_MAX+1];
    parallel_job_t job;
    parallel_result_t worker_result;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t start, done;
    TaskHandle_t worker;
#endif
} gray_parallel_context_t;
static gray_parallel_context_t *parallel_context;
#ifndef ESP_PLATFORM
bool gray_plan_parallel_host_init_fails;
#endif

static void build_policy(target_policy_t *policy, const gray_plan_t *p)
{
    policy->config=p->config;
    policy->adaptive=p->adaptive;
    policy->adaptive_configured=p->adaptive_configured;
    for(unsigned level=0;level<=192;++level) {
        unsigned low=level/64, fraction=level%64, high=low<3 ? low+1 : low;
        for(unsigned target=0;target<4;++target)
            policy->budget[level][target]=(policy->config.dose[low][target]*(64-fraction)+
                policy->config.dose[high][target]*fraction+63)/64;
    }
    for(unsigned route=0;route<=GRAY_ROUTES;++route) {
        unsigned gain=route ? (policy->adaptive_configured ? policy->adaptive.reverse_percent[route-1] : 150) :
                              gray_retarget_percent(&policy->config);
        for(unsigned budget=0;budget<=GRAY_DOSE_MAX;++budget) {
            unsigned value=(budget*gain+50)/100;
            policy->scaled[route][budget]=value<1 ? 1 : value>GRAY_DOSE_MAX ? GRAY_DOSE_MAX : value;
        }
    }
    policy->history_mode=policy->history_returns=policy->history_reversals=0;
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

static void target_range(const target_policy_t *cached, gray_plan_t *p,
                         const uint8_t *binary, const uint8_t *game,
                         unsigned begin, unsigned end, parallel_result_t *result)
{
    target_policy_t policy=*cached;
    policy.history_mode=p->history_mode;
    policy.history_returns=policy.history_reversals=0;
    gray_history_t *history=p->history;
    int pending_delta=0;
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
        for(unsigned group=0;group<50;++group) {
            unsigned x0=group*4, byte=y*25+group/2;
            bool new_game=in_new && x0>=view.x && x0<view.x+view.width;
            bool old_game=in_old && x0>=old.x && x0<old.x+old.width;
            unsigned incoming;
            if(new_game) {
                unsigned j=(y-view.y)*row+(x0-view.x)/4;
                incoming=game[j];
                if(same && incoming==p->previous_game[j]) continue;
            } else {
                unsigned shift=group&1 ? 0 : 4;
                incoming=(binary[byte]>>shift)&15;
                if(!old_game && incoming==((p->previous_binary[byte]>>shift)&15)) continue;
            }
            for(unsigned k=0;k<4;++k) {
                unsigned x=x0+k, i=y*200+x, bit=128u >> (x%8);
                unsigned target=new_game ? 3-((incoming>>(2*k))&3) :
                                 (incoming&(8u>>k) ? 3 : 0);
                gray_pixel_t value={.packed=p->pixel[i].packed}, *v=&value;
                if (target == v->target) continue;
                gray_history_t h={0}, before={0};
                if(history) h=before=history[i];
                bool was_pending = v->remaining != 0, weak;
                retarget(&policy, v, history ? &h : NULL, target, &weak);
                p->pixel[i].packed=value.packed;
                if(history) {
                    if(before.origin!=h.origin) history[i].origin=h.origin;
                    if(before.black_steps!=h.black_steps) history[i].black_steps=h.black_steps;
                    if(before.white_steps!=h.white_steps) history[i].white_steps=h.white_steps;
                }
                unsigned previous_weak=p->weak_wl[i/8];
                unsigned next_weak=weak ? previous_weak|bit : previous_weak&~bit;
                if(previous_weak!=next_weak) p->weak_wl[i/8]=next_weak;
                if (v->remaining) {
                    if(!(p->active[i/8]&bit)) p->active[i/8] |= bit;
                    if (!was_pending) ++pending_delta;
                } else {
                    if(p->active[i/8]&bit) p->active[i/8] &= (uint8_t)~bit;
                    if (was_pending) --pending_delta;
                }
            }
        }
        memcpy(p->previous_binary+y*25, binary+y*25, 25);
        if (in_new) memcpy(p->previous_game+(y-view.y)*row, game+(y-view.y)*row, row);
    }
    result->pending_delta=pending_delta;
    result->returns=policy.history_returns;
    result->reversals=policy.history_reversals;
}

static unsigned mask_range(const gray_plan_t *p, uint8_t *black, uint8_t *white,
                           uint8_t *follow_black, uint8_t *follow_white,
                           unsigned hold_mask, unsigned begin, unsigned end)
{
    unsigned active = 0;
    for (unsigned byte=begin*25; byte<end*25; ++byte) {
        unsigned bits=p->active[byte], b=0,w=0,fb=0,fw=0;
        while (bits) {
            unsigned shift=__builtin_ctz(bits), bit=1u<<shift;
            unsigned state=p->pixel[byte*8+7-shift].packed;
            unsigned target=(state>>12)&3, source=state>>14, remaining=(state>>8)&15;
            bool lighten=target*64 > (state&255);
            if(lighten) w|=bit; else b|=bit;
            if (follow_black && remaining > 1 &&
                !(hold_mask & (1u << (source*4u+target)))) {
                if(lighten) fw|=bit; else fb|=bit;
            }
            bits &= ~bit;
            ++active;
        }
        black[byte]=b;white[byte]=w;
        if(follow_black) { follow_black[byte]=fb;follow_white[byte]=fw; }
    }
    return active;
}

static void complete_range(const gray_parallel_context_t *ctx, gray_plan_t *p,
                           const uint8_t *black, const uint8_t *white,
                           const uint8_t *follow_black, const uint8_t *follow_white,
                           unsigned begin, unsigned end, parallel_result_t *result)
{
    int pending_delta=0;
    gray_history_t *history=p->history;
    bool gray_routes=p->adaptive_configured && gray_adaptive_gray_routes(&p->adaptive);
    for (unsigned byte=begin*25; byte<end*25; ++byte) {
        unsigned fb=follow_black ? follow_black[byte] : 0;
        unsigned fw=follow_white ? follow_white[byte] : 0;
        unsigned active=p->active[byte];
        unsigned bits=(black[byte] | white[byte] | fb | fw) & active;
        while (bits) {
            unsigned shift=__builtin_ctz(bits), bit=1u<<shift, i=byte*8+7-shift;
            unsigned state=p->pixel[i].packed, shade=(state>>12)&3;
            unsigned b=black[byte]&bit, w=white[byte]&bit;
            int level=state&255, target=shade*64, distance=target-level;
            unsigned remaining=(state>>8)&15;
            unsigned steps=!(b && w) && ((distance>0 && w) || (distance<0 && b));
            if (!((fb & bit) && (fw & bit)) &&
                ((distance>0 && (fw & bit)) || (distance<0 && (fb & bit)))) ++steps;
            unsigned delivered=steps<remaining ? steps : remaining;
            if(delivered) {
                level=ctx->pulse[shade][level][remaining];
                if(delivered==2) level=ctx->pulse[shade][level][remaining-1];
                remaining-=delivered;
                if(!remaining) { active &= ~bit; --pending_delta; }
            }
            if(delivered) {
                p->pixel[i].packed=(state&0xf000u)|(remaining<<8)|(unsigned)level;
                if(history && (shade==0 || shade==3 || gray_routes)) {
                    uint8_t *counter=distance<0 ? &history[i].black_steps : &history[i].white_steps;
                    unsigned count=*counter+delivered;
                    *counter=count>255 ? 255 : count;
                }
            }
            bits &= ~bit;
        }
        if(active!=p->active[byte]) p->active[byte]=active;
    }
    result->pending_delta=pending_delta;
}

static void run_range(const gray_parallel_context_t *ctx, const parallel_job_t *job,
                      unsigned begin, unsigned end, parallel_result_t *result)
{
    *result=(parallel_result_t){0};
    if(job->do_target)
        target_range(&ctx->policy,job->plan,job->binary,job->game,begin,end,result);
    if(job->do_masks)
        result->active_count=mask_range(job->plan,job->black_out,job->white_out,
                                        job->follow_black_out,job->follow_white_out,
                                        job->hold_mask,begin,end);
    if(job->do_complete)
        complete_range(ctx,job->plan,job->black_in,job->white_in,
                       job->follow_black_in,job->follow_white_in,begin,end,result);
}

#ifdef ESP_PLATFORM
static void parallel_worker(void *arg)
{
    gray_parallel_context_t *ctx=arg;
    for(;;) {
        xSemaphoreTake(ctx->start,portMAX_DELAY);
        run_range(ctx,&ctx->job,GRAY_PARALLEL_SPLIT_ROW,200,&ctx->worker_result);
        xSemaphoreGive(ctx->done);
    }
}
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
static void *parallel_host_worker(void *arg)
{
    gray_parallel_context_t *ctx=arg;
    run_range(ctx,&ctx->job,GRAY_PARALLEL_SPLIT_ROW,200,&ctx->worker_result);
    return NULL;
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
    for(int target=0;target<4;++target)
        for(int level=0;level<=192;++level)
            for(int n=1;n<=(int)GRAY_DOSE_MAX;++n)
                ctx->pulse[target][level][n]=(uint8_t)(level+(target*64-level)/n);
#ifdef ESP_PLATFORM
    ctx->start=xSemaphoreCreateBinary();
    ctx->done=xSemaphoreCreateBinary();
    ctx->worker=NULL;
    if(!ctx->start || !ctx->done ||
       xTaskCreatePinnedToCore(parallel_worker,"gray_parallel",GRAY_PARALLEL_WORKER_STACK_BYTES,
                               ctx,5,&ctx->worker,1)!=pdPASS) {
        if(ctx->worker) vTaskDelete(ctx->worker);
        if(ctx->start) vSemaphoreDelete(ctx->start);
        if(ctx->done) vSemaphoreDelete(ctx->done);
        heap_caps_free(ctx);
        return false;
    }
#endif
    parallel_context=ctx;
    return true;
}

void gray_plan_parallel_deinit(void)
{
    gray_parallel_context_t *ctx=parallel_context;
    parallel_context=NULL;
    if(!ctx) return;
#ifdef ESP_PLATFORM
    vTaskDelete(ctx->worker);
    vSemaphoreDelete(ctx->start);
    vSemaphoreDelete(ctx->done);
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

static unsigned dispatch(parallel_job_t job)
{
    gray_parallel_context_t *ctx=parallel_context;
    ctx->job=job;
    if(job.do_target) ensure_policy(ctx,job.plan);
    parallel_result_t main_result, second;
#ifdef ESP_PLATFORM
    xSemaphoreGive(ctx->start);
    run_range(ctx,&ctx->job,0,GRAY_PARALLEL_SPLIT_ROW,&main_result);
    xSemaphoreTake(ctx->done,portMAX_DELAY);
    second=ctx->worker_result;
#elif defined(GRAY_PLAN_PARALLEL_HOST_THREADS)
    pthread_t thread;
    int started=pthread_create(&thread,NULL,parallel_host_worker,ctx)==0;
    run_range(ctx,&ctx->job,0,GRAY_PARALLEL_SPLIT_ROW,&main_result);
    if(started) { pthread_join(thread,NULL); second=ctx->worker_result; }
    else run_range(ctx,&ctx->job,GRAY_PARALLEL_SPLIT_ROW,200,&second);
#else
    run_range(ctx,&ctx->job,0,GRAY_PARALLEL_SPLIT_ROW,&main_result);
    run_range(ctx,&ctx->job,GRAY_PARALLEL_SPLIT_ROW,200,&second);
#endif
    gray_plan_t *p=job.plan;
    if(job.do_target || job.do_complete)
        p->pending+=(unsigned)(main_result.pending_delta+second.pending_delta);
    if(job.do_target) {
        p->history_returns+=main_result.returns+second.returns;
        p->history_reversals+=main_result.reversals+second.reversals;
    }
    if(job.do_target) {
        p->had_game=job.game!=NULL;
        if(job.game) p->had_view=game_view_or_classic(p->view);
    }
    return main_result.active_count+second.active_count;
}

void gray_plan_parallel_seed(gray_plan_t *p, const uint8_t *binary)
{
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
