#!/usr/bin/env python3
"""Row-windowed mixed uploads leave both controller RAM planes equal to the full masks.

The real driver runs against the SPI-level SSD1681 model (tools/host_fakes/host_ssd1681.c),
which applies the window, cursor and entry-mode address counter to every RAM byte."""
from host_build import run

CODE = r'''
#define CONFIG_SSD1681_EXPERIMENTAL_INTERRUPT_FRAME_TX 1
#include "ssd1681_fast.c"
#include "host_fakes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define ROWS 200
#define RB 25
static ssd1681_handle_t panel;
static uint32_t seed;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static unsigned below(unsigned n) { return rnd() % n; }

static void busy_pulse(void)
{
    host_gpio_level[PIN_BUSY] = 1; busy_isr(panel); host_clock_us += 100;
    host_gpio_level[PIN_BUSY] = 0; busy_isr(panel);
}

static uint8_t black[5000], white[5000], special[5000], frame[5000];
static bool gray, weak;
static uint64_t scans, windowed_scans, bytes_sent, full_uploads;

static void check_ram(const uint8_t *r, const uint8_t *bw)
{
    assert(!host_ssd1681.outside_writes);
    for (unsigned row = 0; row < ROWS; ++row) {
        /* Vendor profile: entry 0x01 from Y=199, the mapping every full upload produces. */
        assert(!memcmp(host_ssd1681.scanned[1][ROWS - 1 - row], r + row * RB, RB));
        assert(!memcmp(host_ssd1681.scanned[0][ROWS - 1 - row], bw + row * RB, RB));
    }
}

static void start_session(void)
{
    ssd1681_timing_t t;
    for (unsigned i = 0; i < sizeof frame; ++i) frame[i] = (uint8_t)rnd();
    assert(!ssd1681_vendor_full(panel, frame, sizeof frame, &t));
    assert(!ssd1681_vendor_enable_mixed(panel));
    assert(!ssd1681_vendor_set_mixed_frames(panel, 1));
    assert(!ssd1681_vendor_set_fast_transport(panel, true));
    weak = false;
    if (gray) {
        assert(!ssd1681_vendor_gray_frame_rate(panel, 7));
        assert(!ssd1681_vendor_gray_strength(panel, below(4)));
        if (below(2)) { assert(!ssd1681_vendor_gray_wave_mode(panel, SSD1681_GRAY_WAVE_WEAK11)); weak = true; }
    }
}

static void open_panel(void)
{
    assert(!ssd1681_create(&panel));
    assert(!ssd1681_enable_continuous_play(panel));
    assert(!ssd1681_power_on(panel));
    start_session();
}

/* Sets the pixel in one mask and clears it in the other, keeping the masks disjoint. */
static void set_pixel(uint8_t *mask, unsigned row, unsigned column)
{
    const uint8_t bit = (uint8_t)(0x80u >> (column % 8));
    uint8_t *other = mask == black ? white : black;
    mask[row * RB + column / 8] |= bit; other[row * RB + column / 8] &= (uint8_t)~bit;
}

/* Disjoint masks in rows [first, last]; density in 1/256 units, kind 0 mixed, 1 black, 2 white. */
static void fill_rows(unsigned first, unsigned last, unsigned density, unsigned kind)
{
    for (unsigned row = first; row <= last; ++row)
        for (unsigned column = 0; column < 200; ++column) {
            if ((rnd() & 255) >= density) continue;
            const bool to_black = kind == 1 || (kind == 0 && (rnd() & 1));
            set_pixel(to_black ? black : white, row, column);
        }
}

static unsigned band_first = 60, band_rows = 10;
static void make_masks(void)
{
    const bool repeat = below(12) == 0;
    if (!repeat) { memset(black, 0, sizeof black); memset(white, 0, sizeof white); }
    memset(special, 0, sizeof special);
    if (!repeat) switch (below(11)) {
    case 0: break;                                                /* All HOLD. */
    case 1: for (unsigned n = 1 + below(60); n--;) set_pixel(rnd() & 1 ? black : white, below(ROWS), below(200));
            break;                                                /* Sparse anywhere. */
    case 2: fill_rows(28, 171, 1 + below(255), below(3)); break;  /* Game rows only. */
    case 3: fill_rows(28 + below(20), 171 - below(20), 1 + below(40), 0); break;
    case 4: { const unsigned row = below(ROWS); fill_rows(row, row, 1 + below(255), below(3)); break; }
    case 5: /* A band that moves, grows and shrinks: rows appear and disappear. */
        band_first = (band_first + ROWS + below(31) - 15) % ROWS;
        band_rows = 1 + (band_rows + below(9) + 36) % 40;
        fill_rows(band_first, band_first + band_rows - 1 < ROWS ? band_first + band_rows - 1 : ROWS - 1, 80, 0);
        break;
    case 6: for (unsigned i = 0; i < sizeof black; ++i) { white[i] = (uint8_t)rnd(); black[i] = (uint8_t)(rnd() & ~white[i]); }
            break;                                                /* Dense full frame. */
    case 7: if (rnd() & 1) set_pixel(black, 0, below(200));      /* Edge rows. */
            if (rnd() & 1) set_pixel(white, ROWS - 1, below(200));
            if (rnd() & 1) set_pixel(black, ROWS - 1, 199 - below(8));
            break;
    case 8: { /* First or last byte of a row: words straddle row boundaries. */
        const unsigned row = below(ROWS);
        (rnd() & 1 ? black : white)[row * RB + (rnd() & 1 ? 0 : RB - 1)] = (uint8_t)(1u << below(8));
        break; }
    case 9: fill_rows(0, ROWS - 1, 2, below(3)); break;
    default: fill_rows(28, 171, 128, 0); break;
    }
    if (weak && below(2)) { /* Special pixels in a random row band, disjoint from black and white. */
        const unsigned first = below(ROWS), last = first + below(ROWS - first);
        for (unsigned i = first * RB; i < (last + 1) * RB; ++i)
            if (below(3) == 0) special[i] = (uint8_t)(rnd() & ~(black[i] | white[i]) & rnd());
    }
}

static void scan(void)
{
    make_masks();
    static uint8_t r[5000], bw[5000];
    for (unsigned i = 0; i < sizeof r; ++i) { r[i] = black[i] | special[i]; bw[i] = white[i] | special[i]; }
    ssd1681_timing_t t;
    const uint64_t before = host_ssd1681.ram_bytes;
    const unsigned activations = host_ssd1681.activations;
    esp_err_t error = weak ? ssd1681_vendor_gray_wave_scan(panel, black, white, special, 5000, &t)
                           : ssd1681_vendor_mixed_scan(panel, black, white, 5000, &t);
    if (error) fprintf(stderr, "scan %llu error %d weak %d partial %d analog %d fault %d powered %d\n", (unsigned long long)scans, error, weak, panel->vendor_partial, panel->analog_on, panel->fault, panel->powered);
    assert(!error);
    assert(host_ssd1681.activations > activations && host_ssd1681.control == 0x0c);
    assert(host_ssd1681.ram_bytes - before == t.upload_bytes);
    assert(t.upload_bytes % 100 == 0 && t.upload_bytes <= 10000);
    check_ram(r, bw);
    ++scans; bytes_sent += t.upload_bytes;
    if (t.upload_bytes < 10000) ++windowed_scans; else ++full_uploads;
}

static void rejected_scan(void)
{
    const unsigned transactions = host_spi_transactions, updates = panel->updates;
    uint8_t saved[2][ROWS][RB]; memcpy(saved, host_ssd1681.ram, sizeof saved);
    memset(black, 0, sizeof black); memset(white, 0, sizeof white);
    const unsigned i = below(5000); black[i] = white[i] = 0x10;
    ssd1681_timing_t t;
    assert(ssd1681_vendor_mixed_scan(panel, black, white, 5000, &t) == ESP_ERR_INVALID_ARG);
    assert(host_spi_transactions == transactions && panel->updates == updates);
    assert(!memcmp(saved, host_ssd1681.ram, sizeof saved));
    black[i] = white[i] = 0;
}

static void event(void)
{
    ssd1681_timing_t t;
    switch (below(9)) {
    case 0: /* Idle: analog off, then resume. */
        if (!panel->vendor_partial) break;
        assert(!ssd1681_analog_power(panel, false, &t));
        assert(!ssd1681_vendor_mixed_resume(panel, &t));
        break;
    case 1: /* Strength or waveform change reloads the LUT with analog off. */
        if (!gray) break;
        assert(!ssd1681_analog_power(panel, false, &t));
        if (rnd() & 1) assert(!ssd1681_vendor_gray_strength(panel, below(4)));
        else { weak = !weak; assert(!ssd1681_vendor_gray_wave_mode(panel, weak ? SSD1681_GRAY_WAVE_WEAK11 : SSD1681_GRAY_WAVE_NORMAL)); }
        if (panel->vendor_partial) assert(!ssd1681_vendor_mixed_resume(panel, &t));
        break;
    case 2: start_session(); break; /* Full refresh. */
    case 3: /* Panel power cycle. */
        assert(!ssd1681_power_off(panel)); assert(!ssd1681_power_on(panel)); start_session();
        break;
    case 4: /* Return to the stock full profile, then resume mixed scans. */
        assert(!ssd1681_vendor_full(panel, frame, 5000, &t));
        start_session();
        break;
    case 5: { /* Transport fault: the session dies and a new driver instance starts over. */
        host_spi_error = ESP_FAIL;
        memset(black, 0, sizeof black); memset(white, 0xff, sizeof white);
        assert(ssd1681_vendor_mixed_scan(panel, black, white, 5000, &t) == ESP_FAIL);
        host_spi_error = ESP_OK;
        assert(panel->fault && !panel->powered);
        assert(!ssd1681_destroy(panel));
        open_panel();
        break; }
    case 6: rejected_scan(); break;
    default: break;
    }
}

static void exercise(bool paranoid, bool gray_mode, uint32_t start_seed)
{
    host_fakes_reset();
    host_ssd1681_reset(paranoid);
    host_semaphore_wait_hook = busy_pulse;
    seed = start_seed; gray = gray_mode;
    open_panel();
    for (unsigned n = 0; n < 3000; ++n) {
        if (below(25) == 0) event();
        scan();
    }
    assert(!host_ssd1681.irq_rule_breaks);
    assert(!ssd1681_destroy(panel));
}

static uint32_t upload(const uint8_t *b, const uint8_t *w)
{
    ssd1681_timing_t t;
    assert(!ssd1681_vendor_mixed_scan(panel, b, w, 5000, &t));
    check_ram(b, w);
    return t.upload_bytes;
}

/* Exact byte counts for the cases the windowing is meant to win. */
static void directed(void)
{
    host_fakes_reset();
    host_semaphore_wait_hook = busy_pulse;
    gray = false; seed = 7;
    open_panel();
    static uint8_t zero[5000], b[5000], w[5000];
    assert(upload(zero, zero) == 10000);           /* RAM unknown after the full refresh. */
    assert(upload(zero, zero) == 0);               /* Nothing nonzero anywhere: no plane traffic. */
    const unsigned transactions = host_spi_transactions;
    assert(upload(zero, zero) == 0 && host_spi_transactions - transactions == 3); /* 0x22, data, 0x20 only. */
    b[50 * RB + 3] = 1;                            /* Row 50 -> rows 48..51 of both planes. */
    assert(upload(b, zero) == 200);
    assert(upload(zero, zero) == 200);             /* Row 50 must be cleared in RAM. */
    assert(upload(zero, zero) == 0);
    memset(b + 28 * RB, 0x0f, 144 * RB); memset(w + 28 * RB, 0xf0, 144 * RB);
    assert(upload(b, w) == 2 * 144 * RB);          /* Game rows 28..171 only. */
    const unsigned interrupts = host_spi_interrupt_transactions;
    assert(upload(b, w) == 2 * 144 * RB && host_spi_interrupt_transactions - interrupts == 2);
    memset(b, 0, sizeof b); b[199 * RB + 24] = 0x80;
    assert(upload(b, zero) == 2 * 172 * RB);      /* Both: RAM 28..171 + row 199. */
    assert(upload(b, zero) == 2 * 4 * RB);          /* Both: rows 196..199 only. */
    ssd1681_timing_t t;
    assert(!ssd1681_analog_power(panel, false, &t) && !ssd1681_vendor_mixed_resume(panel, &t));
    assert(upload(zero, zero) == 10000);           /* Any non-scan activation forgets RAM. */
    assert(!host_ssd1681.irq_rule_breaks);
    assert(!ssd1681_destroy(panel));
}

int main(void)
{
    directed();
    for (unsigned variant = 0; variant < 4; ++variant) exercise(variant & 1, variant & 2, 0x1234567u + variant);
    printf("scans %llu, windowed %llu, full %llu, mean bytes %.0f\n", (unsigned long long)scans,
           (unsigned long long)windowed_scans, (unsigned long long)full_uploads, (double)bytes_sent / scans);
    assert(windowed_scans > full_uploads);
    return 0;
}
'''

run(CODE)
print('PASS: windowed mixed uploads keep both RAM planes equal to the masks across resets, refreshes, '
      'power cycles, LUT reloads, faults and rejected masks; IRQ threshold')
