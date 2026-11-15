#include "host_panel.h"
#include "host_fakes.h"
#include <assert.h>
#include <string.h>

host_panel_t host_panel;

void host_panel_reset(void)
{
    memset(&host_panel, 0, sizeof(host_panel));
    host_panel.frame_rate = 2;
    host_panel.scan_us = 13000;
    host_panel.resume_us = 140000;
    host_panel.full_us = 1800000;
}

#define P host_panel

esp_err_t ssd1681_create(ssd1681_handle_t *out)
{
    *out = (ssd1681_handle_t)1;
    P.updates = 0;
    P.continuous_play = false;
    P.powered = false;
    return ESP_OK;
}

esp_err_t ssd1681_enable_continuous_play(ssd1681_handle_t d)
{
    assert(d && !P.powered && !P.updates);
    P.continuous_play = true;
    ++P.continuous_enables;
    return ESP_OK;
}

esp_err_t ssd1681_power_on(ssd1681_handle_t d)
{
    assert(d);
    P.powered = true;
    return ESP_OK;
}

esp_err_t ssd1681_destroy(ssd1681_handle_t d)
{
    if (d) ++P.destroys;
    P.powered = P.analog = false;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_full(ssd1681_handle_t d, const uint8_t *frame, size_t length, ssd1681_timing_t *timing)
{
    assert(d && frame && length == SSD1681_FRAME_BYTES);
    if (!P.continuous_play && P.updates >= SSD1681_MAX_UPDATES) return ESP_ERR_INVALID_STATE;
    ++P.updates;
    ++P.fulls;
    P.analog = P.prepared = P.fast_transport = P.explicit_masks = false;
    P.frame_rate = 2;
    P.strength = P.wave_mode = 0;
    P.last_scan_start = 0;
    memcpy(P.physical, frame, length);
    host_clock_us += P.full_us;
    *timing = (ssd1681_timing_t){.busy_us = 1750000};
    if (P.on_full) P.on_full(frame);
    return ESP_OK;
}

esp_err_t ssd1681_vendor_enable_mixed(ssd1681_handle_t d)
{
    assert(d && !P.analog && !P.prepared);
    if (!P.continuous_play && P.updates >= SSD1681_MAX_UPDATES - 1) return ESP_ERR_INVALID_STATE;
    P.explicit_masks = true;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_set_mixed_frames(ssd1681_handle_t d, unsigned frames)
{
    assert(d && !P.analog && !P.prepared && frames == 1);
    if (!P.continuous_play && P.updates >= SSD1681_MAX_UPDATES - 1) return ESP_ERR_INVALID_STATE;
    P.mixed_frames = frames;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_set_fast_transport(ssd1681_handle_t d, bool enabled)
{
    assert(d && P.explicit_masks && !P.analog && !P.prepared && enabled);
    P.fast_transport = enabled;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_gray_frame_rate(ssd1681_handle_t d, unsigned code)
{
    assert(d && !P.analog && P.fast_transport && code == 7);
    P.frame_rate = code;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_gray_strength(ssd1681_handle_t d, unsigned preset)
{
    assert(d && !P.analog && P.frame_rate == 7 && preset <= 3);
    P.strength = preset;
    ++P.strength_changes;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_gray_wave_mode(ssd1681_handle_t d, unsigned mode)
{
    assert(d && !P.analog && mode < SSD1681_GRAY_WAVE_COUNT);
    P.wave_mode = mode;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_mixed_resume(ssd1681_handle_t d, ssd1681_timing_t *timing)
{
    (void)timing;
    assert(d && P.prepared);
    if (!P.analog) {
        ++P.resumes;
        host_clock_us += P.resume_us;
    }
    P.analog = true;
    return ESP_OK;
}

void ssd1681_vendor_mixed_upload_all_next(ssd1681_handle_t d) { (void)d; }

esp_err_t ssd1681_analog_power(ssd1681_handle_t d, bool on, ssd1681_timing_t *timing)
{
    (void)timing;
    assert(d && !on && (P.analog || !P.strict_power_off));
    P.analog = false;
    ++P.analog_offs;
    host_clock_us += 500;
    return ESP_OK;
}

esp_err_t ssd1681_vendor_mixed_scan(ssd1681_handle_t d, const uint8_t *black, const uint8_t *white,
                                    size_t length, ssd1681_timing_t *timing)
{
    assert(d && length == SSD1681_FRAME_BYTES && P.explicit_masks && P.fast_transport);
    assert(!P.require_gray_frame_rate || P.frame_rate == 7);
    for (unsigned i = 0; i < length; ++i) assert(!(black[i] & white[i]));
    if (P.before_scan) P.before_scan(black, white);
    if (!P.continuous_play && P.updates >= SSD1681_MAX_UPDATES - 1) return ESP_ERR_INVALID_STATE;
    ++P.updates;
    if (P.prepared) assert(P.analog);
    unsigned minimum = P.fast_transport ? P.min_gap_fast_us : P.min_gap_slow_us;
    if (P.last_scan_start) assert(host_clock_us - P.last_scan_start >= minimum);
    P.last_scan_start = host_clock_us;
    bool hold = true;
    for (unsigned i = 0; i < length; ++i) if (black[i] || white[i]) hold = false;
    if (P.scans < HOST_PANEL_SCANS) {
        P.scan_start[P.scans] = host_clock_us;
        P.scan_hold[P.scans] = hold;
        P.any_hold |= hold;
    }
    ++P.scans;
    esp_err_t error = ESP_OK;
    if (P.scan_fails || P.scans == P.fail_scan_at) {
        P.powered = P.analog = false;
        error = HOST_PANEL_ERROR;
    } else {
        if (!P.prepared) host_clock_us += P.first_scan_extra_us;
        P.prepared = P.analog = true;
        host_clock_us += P.scan_us;
        if (P.scans <= HOST_PANEL_SCANS) P.scan_end[P.scans - 1] = host_clock_us;
        *timing = (ssd1681_timing_t){.busy_us = 5735};
        for (unsigned i = 0; i < length; ++i) P.physical[i] = (P.physical[i] & (uint8_t)~black[i]) | white[i];
    }
    if (P.after_scan) P.after_scan(error);
    return error;
}

esp_err_t ssd1681_vendor_gray_wave_scan(ssd1681_handle_t d, const uint8_t *black, const uint8_t *white,
                                        const uint8_t *special, size_t length, ssd1681_timing_t *timing)
{
    assert(ssd1681_gray_wave_weak(P.wave_mode));
    for (unsigned i = 0; i < length; ++i) {
        assert(!((black[i] & white[i]) | ((black[i] | white[i]) & special[i])));
        P.special_pixels += (unsigned)__builtin_popcount(special[i]);
    }
    for (unsigned i = 0; i < 40000; ++i)
        if (special[i / 8] & (128u >> (i % 8))) assert(i % 200 >= 20 && i % 200 < 180 && i / 200 >= 28 && i / 200 < 172);
    return ssd1681_vendor_mixed_scan(d, black, white, length, timing);
}
