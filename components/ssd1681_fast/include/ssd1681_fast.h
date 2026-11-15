#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SSD1681_WIDTH 200
#define SSD1681_HEIGHT 200
#define SSD1681_FRAME_BYTES 5000
#define SSD1681_SPI_HZ 10000000
#define SSD1681_LUT_BYTES 153
#define SSD1681_MAX_UPDATES 2402
#define CLEAN_REFRESH_INTERVAL 2400

typedef struct ssd1681_device *ssd1681_handle_t;
/* Select only after full cleaning, before partial preparation. Full cleaning
 * restores 10 MHz. SSD1681 Rev 0.13, p5/p37: maximum SPI write clock 20 MHz. */
esp_err_t ssd1681_vendor_set_fast_transport(ssd1681_handle_t dev, bool enabled);
typedef struct {
    int64_t busy_us;       /* ISR timestamps: rising to falling BUSY edge. */
    int64_t activation_us; /* Master activation through task completion. */
    int64_t upload_us;     /* Mixed scans only: both plane writes, including their window setup. */
    uint32_t upload_bytes; /* Mixed scans only: plane bytes sent; rows already zero in RAM are skipped. */
} ssd1681_timing_t;

/* Single task owns this synchronous driver. No public raw-command interface. */
esp_err_t ssd1681_create(ssd1681_handle_t *out);
/* Set before power-on for sustained gameplay; otherwise the driver enforces SSD1681_MAX_UPDATES. */
esp_err_t ssd1681_enable_continuous_play(ssd1681_handle_t dev);
esp_err_t ssd1681_power_on(ssd1681_handle_t dev);
esp_err_t ssd1681_write_frame(ssd1681_handle_t dev, const uint8_t *frame,
                            size_t length, bool previous, int64_t *upload_us);
esp_err_t ssd1681_analog_power(ssd1681_handle_t dev, bool on,
                             ssd1681_timing_t *timing);
esp_err_t ssd1681_power_off(ssd1681_handle_t dev);
esp_err_t ssd1681_destroy(ssd1681_handle_t dev);

/* Fixed Waveshare V2 packages with their volatile voltage/end settings. No call accepts a
 * caller-supplied LUT. Partial scans need a completed full; errors latch a fault and cut panel power. */
esp_err_t ssd1681_vendor_full(ssd1681_handle_t dev, const uint8_t *frame,
                            size_t length, ssd1681_timing_t *timing);
#define SSD1681_MIXED_MAX_DRIVEN_PIXELS (SSD1681_WIDTH * SSD1681_HEIGHT)
/* Fixed BB/WW-VSS waveform, power retained between scans; select after full, before partial entry.
 * Both planes equal the masks in RAM at every activation: (R,BW) 00 HOLD, 10 BLACK, 01 WHITE, masks disjoint.
 * Both planes get the row span nonzero in either mask or either RAM plane; other activity forces full uploads. */
esp_err_t ssd1681_vendor_enable_mixed(ssd1681_handle_t dev);
esp_err_t ssd1681_vendor_mixed_scan(ssd1681_handle_t dev, const uint8_t *black_mask,
                                  const uint8_t *white_mask, size_t length,
                                  ssd1681_timing_t *timing);
/* Fixed 4/2/1-frame timing; select after mixed mode, before partial setup. 2 and 1 shorten only G0;
 * 1 also zeroes G1 B, keeping G1 A. Full/power-off restore the 17-frame timing. */
esp_err_t ssd1681_vendor_set_mixed_frames(ssd1681_handle_t dev, unsigned frames);
/* Resume a prepared mixed session after analog-off, preserving LUT and RAM: power startup only,
 * no pixel activation, counter increment or hardware reset. */
esp_err_t ssd1681_vendor_mixed_resume(ssd1681_handle_t dev, ssd1681_timing_t *timing);
/* The next mixed scan uploads every row, so its upload takes the same time whatever changed. */
void ssd1681_vendor_mixed_upload_all_next(ssd1681_handle_t dev);

/* FR[1] code 0..7 for one-frame explicit masks, analog off only. Full refresh resets to 2. */
esp_err_t ssd1681_vendor_gray_frame_rate(ssd1681_handle_t d, unsigned code);
#include "ssd1681_gray_waveform.h"
/* VSL presets: 0 stock -15V, 1 -13V, 2 -11V, 3 -9V. Needs FR7 explicit one-frame masks and analog off;
 * full refresh or power reset restores stock. There is no raw voltage or OTP API. */
esp_err_t ssd1681_vendor_gray_strength(ssd1681_handle_t d, unsigned preset);
/* Needs analog off and the FR7 explicit base; full refresh resets NORMAL. Special masks are only
 * allowed in a weak mode that assigns code 11, and the three masks must be pairwise disjoint. */
esp_err_t ssd1681_vendor_gray_wave_mode(ssd1681_handle_t d, unsigned mode);
esp_err_t ssd1681_vendor_gray_wave_scan(ssd1681_handle_t d,
    const uint8_t *black, const uint8_t *white, const uint8_t *special,
    size_t length, ssd1681_timing_t *timing);
