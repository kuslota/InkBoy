#!/usr/bin/env python3
"""Defaults, active-state persistence and validation of display presets over the fake NVS."""
from host_build import run

CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "display_preset.h"
#include "host_fakes.h"

static display_preset_t dithered(const char *name, unsigned black, unsigned white)
{
    display_preset_t p = {.mode = DISPLAY_DITHERED, .black_pulses = (uint8_t)black, .white_pulses = (uint8_t)white};
    snprintf(p.name, sizeof(p.name), "%s", name);
    return p;
}

static display_preset_t gray(const char *name)
{
    display_preset_t p = *display_preset_default(DISPLAY_GRAYSCALE);
    snprintf(p.name, sizeof(p.name), "%s", name);
    p.gray.tuning.dose[0][1] = 3;
    p.gray.tuning.retarget_gain = 4;
    p.gray.strength = 2;
    p.gray.adaptive.history_mode = 1;
    return p;
}

static void seed_blob(const char *key, const void *data, size_t size) { host_nvs_seed("inkboy", key, HOST_NVS_BLOB, data, size); }
static void seed_name(const char *key, const char *name) { host_nvs_seed("inkboy", key, HOST_NVS_STR, name, strlen(name) + 1); }
static void seed_flag(const char *key, uint8_t value) { host_nvs_seed("inkboy", key, HOST_NVS_U8, &value, 1); }

static void expect_default(display_mode_t mode)
{
    assert(display_preset_same_settings(display_preset_active(mode), display_preset_default(mode)));
    assert(!strcmp(display_preset_active(mode)->name, "Default") && !display_preset_active_modified(mode));
    assert(!display_preset_active_is_user(mode));
}

static void defaults(void)
{
    const display_preset_t *d = display_preset_default(DISPLAY_DITHERED);
    assert(d->mode == DISPLAY_DITHERED && d->black_pulses == 8 && d->white_pulses == 8 && !strcmp(d->name, "Default"));
    assert(display_preset_valid(d));
    const display_preset_t *g = display_preset_default(DISPLAY_GRAYSCALE);
    static const uint8_t expected_doses[4][4] = {{0, 2, 4, 7}, {4, 0, 1, 6}, {4, 1, 0, 5}, {7, 3, 2, 0}};
    assert(g->mode == DISPLAY_GRAYSCALE && !strcmp(g->name, "Default") && display_preset_valid(g));
    assert(!memcmp(g->gray.tuning.dose, expected_doses, sizeof(expected_doses)));
    assert(g->gray.tuning.retarget_gain == 2 && !g->gray.tuning.scan_policy && !g->gray.strength && g->gray.immediate);
    assert(gray_profile_holds(&g->gray) == 50791 && g->gray.adaptive.history_mode == 3 && !g->gray.adaptive.weak_wl);
    assert(g->gray.adaptive.return_delta[0] == -1 && g->gray.adaptive.reverse_percent[0] == 150);
    assert(!g->black_pulses && !g->white_pulses && d->gray.version == 0);
}

static void fresh_device(void)
{
    host_fakes_reset();
    assert(display_preset_load_active() == ESP_OK);
    expect_default(DISPLAY_DITHERED);
    expect_default(DISPLAY_GRAYSCALE);
    assert(host_nvs_commits == 0 && !host_nvs_find("inkboy", "d_values") && !host_nvs_find("inkboy", "g_values"));
    char label[40];
    display_preset_active_label(DISPLAY_DITHERED, label, sizeof(label));
    assert(!strcmp(label, "Default"));
}

static void persists_active_state(void)
{
    host_fakes_reset();
    assert(display_preset_load_active() == ESP_OK);
    display_preset_t d = dithered("User 12", 3, 5), g = gray("User 3");
    assert(display_preset_set_active(&d, true) == ESP_OK && display_preset_set_active(&g, false) == ESP_OK);
    assert(host_nvs_commits == 2);
    const host_nvs_entry_t *values = host_nvs_find("inkboy", "d_values");
    assert(values && values->type == HOST_NVS_BLOB && values->size == 3 && values->data[0] == 1 && values->data[1] == 3 && values->data[2] == 5);
    assert(host_nvs_find("inkboy", "d_name")->type == HOST_NVS_STR && !strcmp((const char *)host_nvs_find("inkboy", "d_name")->data, "User 12"));
    assert(host_nvs_u8("inkboy", "d_modified") == 1 && host_nvs_u8("inkboy", "g_modified") == 0);
    values = host_nvs_find("inkboy", "g_values");
    assert(values->type == HOST_NVS_BLOB && values->size == sizeof(gray_profile_t) && values->data[0] == GRAY_PROFILE_VERSION);
    assert(!strcmp((const char *)host_nvs_find("inkboy", "g_name")->data, "User 3"));
    assert(display_preset_active_is_user(DISPLAY_DITHERED) && display_preset_active_modified(DISPLAY_DITHERED));

    host_nvs_entry_t kept[6];
    const char *keys[6] = {"d_values", "d_name", "d_modified", "g_values", "g_name", "g_modified"};
    for (unsigned i = 0; i < 6; ++i) kept[i] = *host_nvs_find("inkboy", keys[i]);
    host_nvs_reset();
    assert(display_preset_load_active() == ESP_OK);
    expect_default(DISPLAY_DITHERED);
    expect_default(DISPLAY_GRAYSCALE);
    for (unsigned i = 0; i < 6; ++i) host_nvs_restore("inkboy", keys[i], &kept[i]);
    assert(display_preset_load_active() == ESP_OK);
    const display_preset_t *loaded = display_preset_active(DISPLAY_DITHERED);
    assert(loaded->mode == DISPLAY_DITHERED && loaded->black_pulses == 3 && loaded->white_pulses == 5 && !strcmp(loaded->name, "User 12"));
    assert(display_preset_active_modified(DISPLAY_DITHERED) && !display_preset_active_modified(DISPLAY_GRAYSCALE));
    assert(display_preset_same_settings(display_preset_active(DISPLAY_GRAYSCALE), &g) && !strcmp(display_preset_active(DISPLAY_GRAYSCALE)->name, "User 3"));
    char label[40];
    display_preset_active_label(DISPLAY_DITHERED, label, sizeof(label));
    assert(!strcmp(label, "User 12 *"));
    display_preset_active_label(DISPLAY_GRAYSCALE, label, sizeof(label));
    assert(!strcmp(label, "User 3"));
    d = dithered("User 12", 3, 5);
    assert(display_preset_set_active(&d, false) == ESP_OK && !display_preset_active_modified(DISPLAY_DITHERED) && host_nvs_u8("inkboy", "d_modified") == 0);
}

static void default_is_never_changed(void)
{
    host_fakes_reset();
    assert(display_preset_load_active() == ESP_OK);
    const display_preset_t *d = display_preset_default(DISPLAY_DITHERED), *g = display_preset_default(DISPLAY_GRAYSCALE);
    const display_preset_t d_before = *d, g_before = *g;
    display_preset_t edited = *display_preset_active(DISPLAY_DITHERED);
    edited.black_pulses = 1;
    assert(display_preset_set_active(&edited, true) == ESP_OK);
    edited = *display_preset_active(DISPLAY_GRAYSCALE);
    edited.gray.tuning.dose[1][0] = 12;
    edited.gray.adaptive.history_mode = 0;
    assert(display_preset_set_active(&edited, true) == ESP_OK);
    assert(display_preset_active(DISPLAY_DITHERED) != d && display_preset_active(DISPLAY_GRAYSCALE) != g);
    assert(!memcmp(d, &d_before, sizeof(d_before)) && !memcmp(g, &g_before, sizeof(g_before)));
    assert(display_preset_load_active() == ESP_OK);
    assert(!memcmp(display_preset_default(DISPLAY_DITHERED), &d_before, sizeof(d_before)));
    assert(!memcmp(display_preset_default(DISPLAY_GRAYSCALE), &g_before, sizeof(g_before)));
    edited = *g;
    assert(display_preset_set_active(&edited, false) == ESP_OK);
    edited.gray.strength = 3;
    assert(!memcmp(display_preset_default(DISPLAY_GRAYSCALE), &g_before, sizeof(g_before)));
    expect_default(DISPLAY_GRAYSCALE);
}

static void damaged_storage(void)
{
    const uint8_t bad_blobs[][3] = {{2, 3, 4}, {1, 0, 4}, {1, 3, 9}, {1, 3, 0}};
    for (unsigned i = 0; i < 4; ++i) {
        host_fakes_reset();
        seed_blob("d_values", bad_blobs[i], 3);
        seed_name("d_name", "User 1");
        seed_flag("d_modified", 1);
        assert(display_preset_load_active() == ESP_ERR_INVALID_ARG);
        expect_default(DISPLAY_DITHERED);
    }
    host_fakes_reset();
    seed_blob("d_values", bad_blobs[0], 2);
    assert(display_preset_load_active() == ESP_ERR_INVALID_ARG);
    expect_default(DISPLAY_DITHERED);
    gray_profile_t broken = display_preset_default(DISPLAY_GRAYSCALE)->gray;
    broken.strength = 4;
    host_fakes_reset();
    seed_blob("g_values", &broken, sizeof(broken));
    seed_name("g_name", "User 2");
    assert(display_preset_load_active() == ESP_ERR_INVALID_ARG);
    expect_default(DISPLAY_GRAYSCALE);
    expect_default(DISPLAY_DITHERED);
    host_fakes_reset();
    host_nvs_open_error = ESP_ERR_NVS_IO;
    assert(display_preset_load_active() == ESP_ERR_NVS_IO);
    expect_default(DISPLAY_DITHERED);
    expect_default(DISPLAY_GRAYSCALE);
}

static void damaged_labels(void)
{
    const uint8_t values[3] = {1, 3, 4};
    /* A missing or unusable name falls back to Default, and then the values must show as edited. */
    const char *names[] = {NULL, "", "bad\"name", "123456789012345678901"};
    for (unsigned i = 0; i < 4; ++i) {
        host_fakes_reset();
        seed_blob("d_values", values, 3);
        if (names[i]) seed_name("d_name", names[i]);
        assert(display_preset_load_active() == ESP_OK);
        assert(!strcmp(display_preset_active(DISPLAY_DITHERED)->name, "Default") && display_preset_active_modified(DISPLAY_DITHERED));
        assert(display_preset_active(DISPLAY_DITHERED)->black_pulses == 3);
    }
    host_fakes_reset();
    const uint8_t same[3] = {1, 8, 8};
    seed_blob("d_values", same, 3);
    assert(display_preset_load_active() == ESP_OK);
    expect_default(DISPLAY_DITHERED);
    host_fakes_reset();
    seed_blob("d_values", values, 3);
    seed_name("d_name", "default");
    seed_flag("d_modified", 0);
    assert(display_preset_load_active() == ESP_OK && display_preset_active_modified(DISPLAY_DITHERED));
    host_fakes_reset();
    seed_blob("d_values", values, 3);
    seed_name("d_name", "User 7");
    seed_flag("d_modified", 2);
    assert(display_preset_load_active() == ESP_OK);
    assert(!strcmp(display_preset_active(DISPLAY_DITHERED)->name, "User 7") && !display_preset_active_modified(DISPLAY_DITHERED));
    host_fakes_reset();
    seed_blob("d_values", values, 3);
    seed_name("d_name", "User 7");
    assert(display_preset_load_active() == ESP_OK && !display_preset_active_modified(DISPLAY_DITHERED));
}

static void rejects_invalid_and_failed_writes(void)
{
    host_fakes_reset();
    assert(display_preset_load_active() == ESP_OK);
    display_preset_t bad[6] = {dithered("Ok", 0, 2), dithered("Ok", 2, 9), dithered("", 2, 2), dithered("bad\"", 2, 2), gray("Ok"), gray("Ok")};
    bad[4].gray.strength = 4;
    bad[5].mode = DISPLAY_MODE_COUNT;
    for (unsigned i = 0; i < 6; ++i) assert(display_preset_set_active(&bad[i], true) == ESP_ERR_INVALID_ARG);
    assert(display_preset_set_active(NULL, true) == ESP_ERR_INVALID_ARG && host_nvs_commits == 0);
    expect_default(DISPLAY_DITHERED);
    expect_default(DISPLAY_GRAYSCALE);

    display_preset_t good = dithered("User 1", 4, 4);
    assert(display_preset_set_active(&good, false) == ESP_OK);
    display_preset_t next = dithered("User 1", 5, 5);
    for (unsigned fault = 0; fault < 3; ++fault) {
        host_nvs_open_error = fault == 0 ? ESP_ERR_NVS_IO : ESP_OK;
        host_nvs_set_error = fault == 1 ? ESP_ERR_NVS_IO : ESP_OK;
        host_nvs_commit_error = fault == 2 ? ESP_ERR_NVS_IO : ESP_OK;
        assert(display_preset_set_active(&next, true) == ESP_ERR_NVS_IO);
        host_nvs_open_error = host_nvs_set_error = host_nvs_commit_error = ESP_OK;
        assert(display_preset_active(DISPLAY_DITHERED)->black_pulses == 4 && !display_preset_active_modified(DISPLAY_DITHERED));
        assert(host_nvs_find("inkboy", "d_values")->data[1] == 4 && host_nvs_u8("inkboy", "d_modified") == 0);
    }
}

static void name_rules(void)
{
    assert(display_preset_name_valid("Default") && display_preset_name_valid("User 12") && display_preset_name_valid("a-b_c.d/e:f"));
    assert(display_preset_name_valid("12345678901234567890") && !display_preset_name_valid("123456789012345678901"));
    assert(!display_preset_name_valid(NULL) && !display_preset_name_valid("") && !display_preset_name_valid(" a") && !display_preset_name_valid("a "));
    assert(!display_preset_name_valid("a\"b") && !display_preset_name_valid("a\\b") && !display_preset_name_valid("a\tb") && !display_preset_name_valid("caf\xc3\xa9"));
    assert(display_preset_is_default_name("Default") && display_preset_is_default_name("DEFAULT") && !display_preset_is_default_name("Default 2"));
    assert(!display_preset_is_default_name(NULL));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "defaults")) defaults();
    else if (!strcmp(argv[1], "fresh")) fresh_device();
    else if (!strcmp(argv[1], "persist")) persists_active_state();
    else if (!strcmp(argv[1], "immutable")) default_is_never_changed();
    else if (!strcmp(argv[1], "damaged")) damaged_storage();
    else if (!strcmp(argv[1], "labels")) damaged_labels();
    else if (!strcmp(argv[1], "failures")) rejects_invalid_and_failed_writes();
    else if (!strcmp(argv[1], "names")) name_rules();
    else assert(0);
    return 0;
}
'''

run(CODE, modules=['display_preset.c', 'gray_profile_store.c'],
    scenarios=[[name] for name in ('defaults', 'fresh', 'persist', 'immutable', 'damaged', 'labels', 'failures', 'names')])
print('PASS: compiled Defaults, fresh device, NVS persistence of values/name/modified, Default immutability, damaged storage, invalid and failed writes')
