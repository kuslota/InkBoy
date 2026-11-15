#include "host_fakes.h"
#include <string.h>

host_ssd1681_t host_ssd1681;
static uint8_t arguments[8];
static unsigned argument_count;
static uint32_t noise = 0x9e3779b9u;

static void scramble(void)
{
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned y = 0; y < HOST_SSD1681_ROWS; ++y)
            for (unsigned x = 0; x < HOST_SSD1681_ROW_BYTES; ++x) {
                noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                host_ssd1681.ram[p][y][x] = (uint8_t)noise;
            }
}

static void registers_por(void)
{
    host_ssd1681.entry = 0x03;
    host_ssd1681.x_start = 0; host_ssd1681.x_end = HOST_SSD1681_ROW_BYTES - 1;
    host_ssd1681.y_start = 0; host_ssd1681.y_end = HOST_SSD1681_ROWS - 1;
    host_ssd1681.x = host_ssd1681.y = 0;
    host_ssd1681.control = 0; host_ssd1681.plane = -1;
}

void host_ssd1681_reset(bool paranoid)
{
    memset(&host_ssd1681, 0, sizeof(host_ssd1681));
    host_ssd1681.paranoid = paranoid;
    registers_por();
    scramble();
    argument_count = 0;
}

/* Counter steps toward the window end and wraps to the window start after reaching it. */
static bool step(unsigned *counter, unsigned start, unsigned end, bool increment, unsigned modulo)
{
    if (*counter == end) { *counter = start; return true; }
    *counter = (*counter + (increment ? 1 : modulo - 1)) % modulo;
    return false;
}

static void ram_write(uint8_t byte)
{
    host_ssd1681_t *c = &host_ssd1681;
    if (c->x < HOST_SSD1681_ROW_BYTES && c->y < HOST_SSD1681_ROWS) c->ram[c->plane][c->y][c->x] = byte;
    else ++c->outside_writes;
    ++c->ram_bytes;
    if (c->entry & 4) {
        if (step(&c->y, c->y_start, c->y_end, c->entry & 2, 512)) step(&c->x, c->x_start, c->x_end, c->entry & 1, 64);
    } else if (step(&c->x, c->x_start, c->x_end, c->entry & 1, 64)) step(&c->y, c->y_start, c->y_end, c->entry & 2, 512);
}

static void command(uint8_t value)
{
    host_ssd1681_t *c = &host_ssd1681;
    c->command = value; argument_count = 0;
    c->plane = value == 0x24 ? 0 : value == 0x26 ? 1 : -1;
    if (value == 0x12) { registers_por(); if (c->paranoid) scramble(); }
    if (value == 0x20) {
        ++c->activations;
        memcpy(c->scanned, c->ram, sizeof(c->scanned));
        if (c->paranoid && c->control != 0x0c) scramble();
        /* Some controllers copy one plane into the other after an update; alternate directions. */
        else if (c->paranoid) memcpy(c->ram[c->activations & 1], c->ram[!(c->activations & 1)], sizeof(c->ram[0]));
    }
}

static void data(uint8_t value)
{
    host_ssd1681_t *c = &host_ssd1681;
    if (c->plane >= 0) { ram_write(value); return; }
    if (argument_count < sizeof(arguments)) arguments[argument_count] = value;
    const unsigned n = ++argument_count;
    switch (c->command) {
    case 0x11: c->entry = value & 7; break;
    case 0x22: c->control = value; break;
    case 0x44: if (n == 1) c->x_start = value & 0x3f; else if (n == 2) c->x_end = value & 0x3f; break;
    case 0x45:
        if (n == 2) c->y_start = arguments[0] | (arguments[1] & 1u) << 8;
        if (n == 4) c->y_end = arguments[2] | (arguments[3] & 1u) << 8;
        break;
    case 0x4e: c->x = value & 0x3f; break;
    case 0x4f: if (n == 1) c->y = value; else if (n == 2) c->y = (c->y & 0xff) | (value & 1u) << 8; break;
    default: break;
    }
}

void host_ssd1681_spi(const uint8_t *bytes, size_t length, bool interrupt)
{
    if (interrupt != (length >= HOST_SSD1681_IRQ_MIN_BYTES)) ++host_ssd1681.irq_rule_breaks;
    const bool is_data = host_gpio_level[HOST_SSD1681_PIN_DC] != 0;
    for (size_t i = 0; i < length; ++i) {
        if (is_data) data(bytes[i]);
        else command(bytes[i]);
    }
}

void host_ssd1681_gpio(int pin, uint32_t level)
{
    if (pin == HOST_SSD1681_PIN_PWR && level) { registers_por(); scramble(); }
    /* Assumed, as the driver's mixed path already relies on it: a hardware reset keeps
     * registers (entry mode, windows) and RAM. Paranoid mode still drops the RAM. */
    if (pin == HOST_SSD1681_PIN_RST && !level && host_ssd1681.paranoid) scramble();
}
