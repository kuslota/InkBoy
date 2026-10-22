#include "gray_plan.h"
#include <string.h>
#include <stdbool.h>

/* Exact one/two-pulse progression table in internal BSS (20 KB); signed rounding runs once at
 * startup instead of per pixel on every scan. */
static uint8_t pulse_level[4][193][GRAY_DOSE_MAX+1][2];
static bool pulse_step_ready;
static void init_pulse_steps(void)
{
    if(pulse_step_ready) return;
    for(int target=0;target<4;++target)
        for(int level=0;level<=192;++level)
            for(int n=1;n<=(int)GRAY_DOSE_MAX;++n) {
                int first=level+(target*64-level)/n;
                pulse_level[target][level][n][0]=first;
                pulse_level[target][level][n][1]=n>1 ? first+(target*64-first)/(n-1) : first;
            }
    pulse_step_ready=true;
}

void gray_plan_seed(gray_plan_t *p, const uint8_t *binary)
{
    init_pulse_steps();
    if (!gray_config_valid(&p->config)) p->config = (gray_config_t)GRAY_CONFIG_DEFAULT;
    for (unsigned i=0; i<GRAY_PIXELS; ++i) {
        unsigned shade = binary[i/8] & (128u >> (i%8)) ? 3 : 0;
        p->pixel[i] = (gray_pixel_t){.level=shade*64, .target=shade, .source=shade};
        if (p->history) p->history[i]=(gray_history_t){.origin=4};
    }
    p->pending = 0;
    memset(p->active, 0, sizeof(p->active));
    memset(p->weak_wl, 0, sizeof(p->weak_wl));
    memcpy(p->previous_binary, binary, sizeof(p->previous_binary));
    p->had_game = false;
    p->history_returns=p->history_reversals=0;
}

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

void gray_plan_target(gray_plan_t *p, const uint8_t *binary, const uint8_t *game)
{
    target_policy_t policy = {.config=p->config,.adaptive=p->adaptive,
        .adaptive_configured=p->adaptive_configured,.history_mode=p->history_mode,
        .history_returns=p->history_returns,.history_reversals=p->history_reversals};
    for(unsigned level=0;level<=192;++level) {
        unsigned low=level/64, fraction=level%64, high=low<3 ? low+1 : low;
        for(unsigned target=0;target<4;++target)
            policy.budget[level][target]=(policy.config.dose[low][target]*(64-fraction)+
                policy.config.dose[high][target]*fraction+63)/64;
    }
    for(unsigned route=0;route<=GRAY_ROUTES;++route) {
        unsigned gain=route ? (policy.adaptive_configured ? policy.adaptive.reverse_percent[route-1] : 150) :
                              gray_retarget_percent(&policy.config);
        for(unsigned budget=0;budget<=GRAY_DOSE_MAX;++budget) {
            unsigned value=(budget*gain+50)/100;
            policy.scaled[route][budget]=value<1 ? 1 : value>GRAY_DOSE_MAX ? GRAY_DOSE_MAX : value;
        }
    }
    gray_history_t *history=p->history;
    unsigned pending=p->pending;
    const game_view_t view=game_view_or_classic(p->view), old=game_view_or_classic(p->had_view);
    const unsigned row=view.width/4;
    /* previous_game is comparable only while the layout is unchanged. */
    const bool same=game && p->had_game && game_view_equal(view,old);
    /* Dirty source rows avoid traversing all optical state on every scan.
     * Cached inputs are owned copies: the caller may reuse its snapshot buffer. */
    for (unsigned y=0; y<200; ++y) {
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
                    if (!was_pending) ++pending;
                } else {
                    if(p->active[i/8]&bit) p->active[i/8] &= (uint8_t)~bit;
                    if (was_pending) --pending;
                }
            }
        }
        memcpy(p->previous_binary+y*25, binary+y*25, 25);
        if (in_new) memcpy(p->previous_game+(y-view.y)*row, game+(y-view.y)*row, row);
    }
    p->had_game = game != NULL;
    if (game) p->had_view = view;
    p->pending=pending;
    p->history_returns=policy.history_returns;
    p->history_reversals=policy.history_reversals;
}

static unsigned prepare_masks(const gray_plan_t *p, uint8_t *black, uint8_t *white,
                               uint8_t *follow_black, uint8_t *follow_white, unsigned hold_mask)
{
    unsigned active = 0;
    for (unsigned byte=0; byte<GRAY_PIXELS/8; ++byte) {
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

unsigned gray_plan_masks(const gray_plan_t *p, uint8_t *black, uint8_t *white)
{
    return prepare_masks(p,black,white,NULL,NULL,GRAY_HOLD_ALL);
}

unsigned gray_plan_prepare_pair(gray_plan_t *p, const uint8_t *binary, const uint8_t *game,
                                uint8_t *black, uint8_t *white, uint8_t *follow_black,
                                uint8_t *follow_white, unsigned hold_mask)
{
    gray_plan_target(p,binary,game);
    return prepare_masks(p,black,white,follow_black,follow_white,hold_mask);
}

unsigned gray_plan_prepare(gray_plan_t *p, const uint8_t *binary, const uint8_t *game,
                           uint8_t *black, uint8_t *white)
{
    gray_plan_target(p, binary, game);
    return gray_plan_masks(p, black, white);
}

static void complete_masks(gray_plan_t *p, const uint8_t *black, const uint8_t *white,
                            const uint8_t *follow_black, const uint8_t *follow_white)
{
    unsigned pending=p->pending;
    gray_history_t *history=p->history;
    bool gray_routes=p->adaptive_configured && gray_adaptive_gray_routes(&p->adaptive);
    for (unsigned byte=0; byte<GRAY_PIXELS/8; ++byte) {
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
                level=pulse_level[shade][level][remaining][delivered-1];
                remaining-=delivered;
                if(!remaining) { active &= ~bit; --pending; }
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
    p->pending=pending;
}

void gray_plan_complete(gray_plan_t *p, const uint8_t *black, const uint8_t *white)
{
    complete_masks(p,black,white,NULL,NULL);
}

void gray_plan_complete_pair(gray_plan_t *p, const uint8_t *black, const uint8_t *white,
                             const uint8_t *follow_black, const uint8_t *follow_white)
{
    complete_masks(p,black,white,follow_black,follow_white);
}
