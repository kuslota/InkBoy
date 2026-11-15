#!/usr/bin/env python3
"""Exercise the grayscale profile blob: validation, handle-based NVS round trip and failures."""
from host_build import run

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gray_profile_store.h"
#include "host_fakes.h"
#include "nvs.h"

static nvs_handle_t open_rw(void)
{
    nvs_handle_t h;
    assert(nvs_open("inkboy", NVS_READWRITE, &h) == ESP_OK);
    return h;
}

static const host_nvs_entry_t *slot(void) { return host_nvs_find("inkboy", "slot"); }

static void put_raw(const char *key, const void *bytes, size_t size) { host_nvs_seed("inkboy", key, HOST_NVS_BLOB, bytes, size); }

static gray_profile_t sentinel(void) { gray_profile_t p; memset(&p, 0xa5, sizeof(p)); return p; }

static void assert_unchanged(const gray_profile_t *p)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < sizeof(*p); ++i) assert(b[i] == 0xa5);
}

static gray_profile_t sample(void)
{
    gray_profile_t p = {.version = GRAY_PROFILE_VERSION,
        .tuning = {1, {{0, 1, 2, 7}, {3, 0, 1, 6}, {4, 2, 0, 5}, {7, 4, 3, 0}}, 3, 1},
        .strength = 2, .immediate = 1, .holds_lo = 0x27, .holds_hi = 0x86,
        .adaptive = {3, 2, {-2, 2, -1, 3, 2, -3}, {175, 125, 100, 200, 125, 200}}};
    assert(gray_profile_valid(&p));
    return p;
}

int main(void)
{
    host_fakes_reset();
    assert(sizeof(gray_config_t) == 19 && sizeof(gray_profile_t) == 38);
    nvs_handle_t h = open_rw();
    gray_profile_t p = sample(), out = sentinel();

    /* A written blob stays in the caller's transaction until it commits. */
    assert(gray_profile_nvs_set(h, "slot", &p) == ESP_OK);
    assert(!slot() && host_nvs_commits == 0);
    assert(gray_profile_nvs_get(h, "slot", &out) == ESP_OK && !memcmp(&out, &p, sizeof(p)));
    assert(nvs_commit(h) == ESP_OK && slot()->size == 38 && slot()->data[0] == GRAY_PROFILE_VERSION && host_nvs_commits == 1);
    nvs_close(h);
    h = open_rw();
    out = sentinel();
    assert(gray_profile_nvs_get(h, "slot", &out) == ESP_OK && !memcmp(&out, &p, sizeof(p)));

    /* Reject unsupported lengths, versions and fields without touching the output. */
    uint8_t malformed[39] = {0};
    for (size_t size = 0; size <= 37; size += 1) {
        put_raw("slot", &p, size);
        out = sentinel();
        assert(gray_profile_nvs_get(h, "slot", &out) == ESP_ERR_INVALID_ARG);
        assert_unchanged(&out);
    }
    put_raw("slot", malformed, 39);
    out = sentinel();
    assert(gray_profile_nvs_get(h, "slot", &out) == ESP_ERR_NVS_INVALID_LENGTH);
    assert_unchanged(&out);
    for (unsigned fault = 0; fault < 12; ++fault) {
        gray_profile_t bad = sample();
        if (fault == 0) bad.version = 2;
        if (fault == 1) bad.version = 4;
        if (fault == 2) bad.tuning.version = 2;
        if (fault == 3) bad.tuning.dose[0][1] = 0;
        if (fault == 4) bad.tuning.dose[1][1] = 1;
        if (fault == 5) bad.tuning.retarget_gain = 5;
        if (fault == 6) bad.strength = 4;
        if (fault == 7) bad.immediate = 2;
        if (fault == 8) bad.holds_lo = 0;
        if (fault == 9) bad.adaptive.history_mode = 4;
        if (fault == 10) bad.adaptive.reverse_percent[3] = 1;
        if (fault == 11) bad.adaptive.return_delta[5] = 4;
        put_raw("slot", &bad, sizeof(bad));
        out = sentinel();
        assert(gray_profile_nvs_get(h, "slot", &out) == ESP_ERR_INVALID_ARG);
        assert_unchanged(&out);
        unsigned before = host_nvs_commits;
        assert(gray_profile_nvs_set(h, "other", &bad) == ESP_ERR_INVALID_ARG && host_nvs_commits == before);
    }

    /* Store faults leave caller data intact and are reported. */
    put_raw("slot", &p, sizeof(p));
    host_nvs_get_error = ESP_ERR_NVS_IO;
    out = sentinel();
    assert(gray_profile_nvs_get(h, "slot", &out) == ESP_ERR_NVS_IO);
    assert_unchanged(&out);
    host_nvs_get_error = ESP_OK;
    out = sentinel();
    assert(gray_profile_nvs_get(h, "missing", &out) == ESP_ERR_NVS_NOT_FOUND);
    assert_unchanged(&out);
    assert(gray_profile_nvs_get(h, NULL, &out) == ESP_ERR_INVALID_ARG && gray_profile_nvs_get(h, "slot", NULL) == ESP_ERR_INVALID_ARG);
    assert(gray_profile_nvs_set(h, NULL, &p) == ESP_ERR_INVALID_ARG && gray_profile_nvs_set(h, "slot", NULL) == ESP_ERR_INVALID_ARG);
    host_nvs_set_error = ESP_ERR_NVS_IO;
    assert(gray_profile_nvs_set(h, "slot", &p) == ESP_ERR_NVS_IO);
    host_nvs_set_error = ESP_OK;
    nvs_close(h);
    puts("PASS: 38-byte blob round trip through a caller transaction, validation of every field, exact length, errors leave outputs unchanged");
}
'''

run(HARNESS, modules=['gray_profile_store.c'])
