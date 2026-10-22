#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Where a packed game image (four pixels/byte, leftmost pixel in the low two bits) sits on the
 * 200x200 panel. x and width are multiples of 4. */
typedef struct { uint8_t x, y, width, height; } game_view_t;
#define GAME_VIEW_CLASSIC ((game_view_t){20, 28, 160, 144})
/* 1.25x nearest neighbour: every fourth source pixel and row is doubled. */
#define GAME_VIEW_FILL ((game_view_t){0, 10, 200, 180})
#define GAME_VIEW_MAX_BYTES (200 * 180 / 4)

/* A zeroed view means the classic 160x144 viewport. */
static inline game_view_t game_view_or_classic(game_view_t v) { return v.width ? v : GAME_VIEW_CLASSIC; }
static inline bool game_view_equal(game_view_t a, game_view_t b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/* Fill-screen helpers. All state is static: call from the one display task. */
void game_view_reset(void);
/* Scales a 160x144 packed frame into an internal GAME_VIEW_FILL image. */
const uint8_t *game_view_scale(const uint8_t *packed);
/* Same contract as gb_video_convert_fast / gb_video_prepare_gray, for GAME_VIEW_FILL. */
unsigned game_view_convert_fast(const uint8_t *image, uint8_t *panel);
unsigned game_view_prepare_gray(const uint8_t *image, uint8_t *panel);
