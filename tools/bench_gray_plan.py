#!/usr/bin/env python3
"""Host timing of the parallel grayscale planner: a git revision against the working tree.

Usage: bench_gray_plan.py [BASE_REV]   (default HEAD). Host timing is only a rough proxy for the ESP32.
"""

from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

BENCH = r'''#include "gray_plan_parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static gray_plan_t plan;
static uint8_t binary[GRAY_PIXELS/8], game[GAME_VIEW_MAX_BYTES];
static uint8_t b[GRAY_PIXELS/8], w[GRAY_PIXELS/8], fb[GRAY_PIXELS/8], fw[GRAY_PIXELS/8];
static uint32_t rng=0x1234567u;
static uint32_t rand32(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

static gray_history_t history[GRAY_PIXELS];

/* Device gameplay setup: adaptive defaults with history mode 3, paired scans, default holds. */
static void run(const char *name, int scroll, unsigned edits, int with_history) {
    memset(&plan,0,sizeof(plan)); memset(binary,0,sizeof(binary));
    plan.config=(gray_config_t)GRAY_CONFIG_DEFAULT;
    plan.adaptive=(gray_adaptive_t)GRAY_ADAPTIVE_DEFAULT;
    plan.adaptive_configured=true;
    plan.history=with_history ? history : NULL;
    plan.history_mode=with_history ? plan.adaptive.history_mode : 0;
    for(unsigned i=0;i<sizeof(game);i++) game[i]=(uint8_t)(rand32()&rand32());
    gray_plan_parallel_seed(&plan,binary);
    double target=0, mask=0, comp=0, t;
    const unsigned frames=600;
    for(unsigned f=0;f<frames;f++) {
        if(scroll) { memmove(game,game+40,sizeof(game)-40); for(unsigned i=0;i<40;i++) game[sizeof(game)-40+i]=(uint8_t)rand32(); }
        else for(unsigned e=0;e<edits;e++) game[rand32()%sizeof(game)]=(uint8_t)rand32();
        t=now();
        gray_plan_parallel_target(&plan,binary,game);
        target+=now()-t;
        /* Every row is now clean, so prepare_pair is the mask pass plus a row compare. */
        t=now();
        gray_plan_parallel_prepare_pair(&plan,binary,game,b,w,fb,fw,GRAY_HOLDS_DEFAULT);
        mask+=now()-t;
        t=now();
        gray_plan_parallel_complete_pair(&plan,b,w,fb,fw);
        comp+=now()-t;
    }
    printf("%s %.1f %.1f %.1f\n",name,target/frames*1e6,mask/frames*1e6,comp/frames*1e6);
}
int main(void) {
    gray_plan_parallel_init();
    run("static_sprite",0,6,1);
    run("busy",0,150,1);
    run("dense",0,1650,1);
    run("scrolling",1,0,1);
    run("dense_nohist",0,1650,0);
    return 0;
}
'''


def build(cc, tmp, name, main_dir):
    src = tmp / f"{name}.c"
    src.write_text(BENCH)
    out = tmp / name
    subprocess.run(cc + ["-std=c11", "-O2", "-I", str(main_dir), str(src), str(main_dir / "gray_plan.c"),
                         str(main_dir / "gray_plan_parallel.c"), "-o", str(out)], check=True)
    return out


def main() -> int:
    base = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="gray-plan-bench-") as t:
        tmp = Path(t)
        old_dir = tmp / "old_main"
        old_dir.mkdir()
        for f in ("gray_plan.c", "gray_plan_parallel.c"):
            for where in (f"main/{f}", f"components/esp32-ws154/components/epd_fast/{f}", f"components/epd_fast/{f}"):
                shown = subprocess.run(["git", "show", f"{base}:{where}"], cwd=ROOT, capture_output=True, text=True)
                if shown.returncode == 0:
                    (old_dir / f).write_text(shown.stdout)
                    break
            else:
                raise SystemExit(f"{f} not found at {base}")
        new_dir = tmp / "new_sources"
        new_dir.mkdir()
        for f in (*(ROOT / "components/esp32-ws154/components/epd_fast").glob("*.c"), *(ROOT / "components/esp32-ws154/components/epd_fast/include").glob("*.h")):
            (new_dir / f.name).write_text(f.read_text())
        for f in (ROOT / "main").glob("*.h"):
            (old_dir / f.name).write_text(f.read_text())
            (new_dir / f.name).write_text(f.read_text())
        for f in (ROOT / "components/esp32-ws154/components/epd_fast/include").glob("*.h"):
            (old_dir / f.name).write_text(f.read_text())
        bins = {"old": build(cc, tmp, "old", old_dir), "new": build(cc, tmp, "new", new_dir)}
        results = {}
        for _ in range(5):
            for name, exe in bins.items():
                for line in subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split("\n"):
                    if line:
                        seq, *times = line.split()
                        best = results.get((name, seq), [1e9] * len(times))
                        results[(name, seq)] = [min(b, float(t)) for b, t in zip(best, times)]
        print(f"{'sequence':<14}{'pass':<10}{'old us/frame':>14}{'new us/frame':>14}{'speed-up':>10}")
        for seq in ("static_sprite", "busy", "dense", "scrolling", "dense_nohist"):
            for i, phase in enumerate(("target", "mask", "complete")):
                o, n = results[("old", seq)][i], results[("new", seq)][i]
                print(f"{seq:<14}{phase:<10}{o:>14.1f}{n:>14.1f}{o / n:>9.2f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
