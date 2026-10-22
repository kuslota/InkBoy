#include "clock.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>
#include <time.h>

/* PCF85063 on the codec's I2C bus at its fixed address 0x51. Control_1 (0x00) bit 5 is STOP and
 * bit 1 selects 12-hour mode. 0x04-0x0A hold BCD seconds (bit 7 = oscillator stopped, so the time
 * is lost), minutes, hours, day, weekday, month and year. */
#define RTC_ADDRESS 0x51
#define REG_CONTROL_1 0x00
#define REG_SECONDS 0x04
#define CONTROL_STOP 0x20
#define CONTROL_12H 0x02
#define SECONDS_OS 0x80
#define PIN_SDA 47
#define PIN_SCL 48
/* Audio supply (active low) and amplifier enable, as sequenced in game_audio.c. The stock firmware
 * powers this rail before talking to the RTC, so it is tried if the chip does not answer. */
#define PIN_AUDIO_RAIL 42
#define PIN_AMP_ENABLE 46
/* Times before 2024 cannot be real: the clock was never set or survived nothing. */
#define PLAUSIBLE_EPOCH 1704067200
/* Written when the RTC lost its time and the system clock has nothing better: 2025-01-01. */
#define FALLBACK_EPOCH 1735689600

static const char *TAG = "clock";
static bool valid;

static unsigned from_bcd(uint8_t value) { return (value >> 4) * 10u + (value & 15u); }
static uint8_t to_bcd(unsigned value) { return (uint8_t)(((value / 10u) << 4) | (value % 10u)); }
static bool bcd_ok(uint8_t value, unsigned max) { return (value & 15u) < 10u && from_bcd(value) <= max; }

static esp_err_t read_registers(i2c_master_dev_handle_t rtc, uint8_t *control, uint8_t time[7])
{
    uint8_t reg = REG_CONTROL_1;
    esp_err_t error = i2c_master_transmit_receive(rtc, &reg, 1, control, 1, 100);
    reg = REG_SECONDS;
    if (!error) error = i2c_master_transmit_receive(rtc, &reg, 1, time, 7, 100);
    return error;
}

static bool decode(uint8_t control, const uint8_t time[7], time_t *out)
{
    const uint8_t sec = time[0] & 0x7f, min = time[1] & 0x7f, day = time[3] & 0x3f;
    const uint8_t month = time[5] & 0x1f, year = time[6];
    if ((time[0] & SECONDS_OS) || (control & CONTROL_STOP) || !bcd_ok(sec, 59) || !bcd_ok(min, 59) ||
        !bcd_ok(day, 31) || day == 0 || !bcd_ok(month, 12) || month == 0 || !bcd_ok(year, 99)) return false;
    unsigned hour;
    if (control & CONTROL_12H) {
        const uint8_t h12 = time[2] & 0x1f;
        if (!bcd_ok(h12, 12) || h12 == 0) return false;
        hour = from_bcd(h12) % 12u + (time[2] & 0x20 ? 12u : 0u);
    } else {
        if (!bcd_ok(time[2] & 0x3f, 23)) return false;
        hour = from_bcd(time[2] & 0x3f);
    }
    struct tm t = {.tm_sec = (int)from_bcd(sec), .tm_min = (int)from_bcd(min), .tm_hour = (int)hour,
                   .tm_mday = (int)from_bcd(day), .tm_mon = (int)from_bcd(month) - 1,
                   .tm_year = 100 + (int)from_bcd(year)};
    *out = mktime(&t); /* No TZ is configured, so this is UTC. */
    return *out != (time_t)-1;
}

/* Restarts a stopped clock at `when`, keeping Control_1's capacitor and mode bits. */
static esp_err_t write_time(i2c_master_dev_handle_t rtc, uint8_t control, time_t when)
{
    struct tm t;
    gmtime_r(&when, &t);
    const uint8_t start[2] = {REG_CONTROL_1, (uint8_t)(control & ~(CONTROL_STOP | CONTROL_12H))};
    const uint8_t time[8] = {REG_SECONDS, to_bcd((unsigned)t.tm_sec), to_bcd((unsigned)t.tm_min),
                             to_bcd((unsigned)t.tm_hour), to_bcd((unsigned)t.tm_mday), (uint8_t)t.tm_wday,
                             to_bcd((unsigned)t.tm_mon + 1u), to_bcd((unsigned)(t.tm_year % 100))};
    esp_err_t error = i2c_master_transmit(rtc, start, sizeof(start), 100);
    if (!error) error = i2c_master_transmit(rtc, time, sizeof(time), 100);
    return error;
}

static void audio_rail(bool on)
{
    const gpio_config_t outputs = {.pin_bit_mask = (1ULL << PIN_AUDIO_RAIL) | (1ULL << PIN_AMP_ENABLE),
                                   .mode = GPIO_MODE_OUTPUT};
    gpio_set_level(PIN_AMP_ENABLE, 0);
    gpio_set_level(PIN_AUDIO_RAIL, on ? 0 : 1);
    gpio_config(&outputs);
    gpio_hold_dis(PIN_AMP_ENABLE);
    gpio_hold_dis(PIN_AUDIO_RAIL);
    gpio_set_level(PIN_AUDIO_RAIL, on ? 0 : 1);
}

static void set_system_time(time_t when)
{
    const struct timeval now = {.tv_sec = when};
    settimeofday(&now, NULL);
}

void clock_init(void)
{
    const time_t kept = time(NULL); /* The system clock survives deep sleep ("power off"). */
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t rtc = NULL;
    const i2c_master_bus_config_t bus_cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    const i2c_device_config_t device_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                            .device_address = RTC_ADDRESS, .scl_speed_hz = 100000};
    esp_err_t error = i2c_new_master_bus(&bus_cfg, &bus);
    if (!error) error = i2c_master_bus_add_device(bus, &device_cfg, &rtc);
    uint8_t control = 0, regs[7] = {0};
    bool rail = false;
    if (!error && read_registers(rtc, &control, regs) != ESP_OK) {
        audio_rail(true); rail = true;
        vTaskDelay(pdMS_TO_TICKS(30));
        error = read_registers(rtc, &control, regs);
    }
    time_t when;
    if (error) {
        valid = kept >= PLAUSIBLE_EPOCH;
        ESP_LOGW(TAG, "CLOCK_RTC_ABSENT,error,%s,system_time_kept,%u", esp_err_to_name(error), valid);
    } else if (decode(control, regs, &when)) {
        set_system_time(when);
        valid = true;
        ESP_LOGI(TAG, "CLOCK_RTC,unix,%lld,rail,%u", (long long)when, rail);
    } else {
        /* Time was lost (battery removed or first power-up): restart it so later power-offs count. */
        when = kept >= PLAUSIBLE_EPOCH ? kept : FALLBACK_EPOCH;
        error = write_time(rtc, control, when);
        set_system_time(when);
        valid = error == ESP_OK || kept >= PLAUSIBLE_EPOCH;
        ESP_LOGW(TAG, "CLOCK_RTC_RESET,control,0x%02x,seconds,0x%02x,unix,%lld,write,%s",
                 control, regs[0], (long long)when, esp_err_to_name(error));
    }
    if (rtc) i2c_master_bus_rm_device(rtc);
    if (bus) i2c_del_master_bus(bus);
    if (rail) audio_rail(false);
}

bool clock_now(int64_t *seconds)
{
    if (!valid) return false;
    *seconds = (int64_t)time(NULL);
    return true;
}
