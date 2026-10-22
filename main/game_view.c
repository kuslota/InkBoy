#include "game_view.h"
#include "gb_emulator.h"
#include <string.h>

#define FILL_ROW (200 / 4)
#define FILL_HEIGHT 180
#define FILL_TOP 10

static uint8_t scaled[GAME_VIEW_MAX_BYTES], previous[GAME_VIEW_MAX_BYTES];
static bool valid;

void game_view_reset(void) { valid = false; }

const uint8_t *game_view_scale(const uint8_t *packed)
{
    for (unsigned y = 0; y < FILL_HEIGHT; ++y) {
        uint8_t *dst = scaled + y * FILL_ROW;
        /* floor(y*4/5) repeats the source row at y%5==1. */
        if (y % 5 == 1) { memcpy(dst, dst - FILL_ROW, FILL_ROW); continue; }
        const uint8_t *src = packed + (y * 4 / 5) * GB_PACKED_ROW;
        memset(dst, 0, FILL_ROW);
        for (unsigned x = 0; x < 200; ++x) {
            unsigned sx = x * 4 / 5, shade = (src[sx / 4] >> ((sx % 4) * 2)) & 3;
            dst[x / 4] |= (uint8_t)(shade << ((x % 4) * 2));
        }
    }
    return scaled;
}

static bool row_changed(const uint8_t *image, unsigned y)
{
    const uint8_t *line = image + y * FILL_ROW;
    uint8_t *old = previous + y * FILL_ROW;
    if (valid && !memcmp(old, line, FILL_ROW)) return false;
    memcpy(old, line, FILL_ROW);
    return true;
}

unsigned game_view_convert_fast(const uint8_t *image, uint8_t *panel)
{
    /* Same ordered dither as gb_video_convert_fast. */
    static uint8_t lookup[4][256];
    static bool ready;
    if (!ready) {
        static const uint8_t bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
        static const uint8_t thresholds[4] = {0,5,11,16};
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned byte = 0; byte < 256; ++byte) {
                unsigned nibble = 0;
                for (unsigned x = 0; x < 4; ++x)
                    if (bayer[y][x] >= thresholds[(byte >> (x * 2)) & 3]) nibble |= 8u >> x;
                lookup[y][byte] = nibble;
            }
        ready = true;
    }
    unsigned rows = 0;
    for (unsigned y = 0; y < FILL_HEIGHT; ++y) {
        if (!row_changed(image, y)) continue;
        ++rows;
        const uint8_t *line = image + y * FILL_ROW, *map = lookup[y & 3];
        uint8_t *dst = panel + (y + FILL_TOP) * 25;
        for (unsigned x = 0; x < 25; ++x)
            dst[x] = (uint8_t)((map[line[2*x]] << 4) | map[line[2*x+1]]);
    }
    valid = true;
    return rows;
}

unsigned game_view_prepare_gray(const uint8_t *image, uint8_t *panel)
{
    unsigned rows = 0;
    for (unsigned y = 0; y < FILL_HEIGHT; ++y) rows += row_changed(image, y);
    memset(panel + FILL_TOP * 25, 0, FILL_HEIGHT * 25);
    valid = true;
    return rows;
}
