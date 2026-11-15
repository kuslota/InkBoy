#include "display_preset.h"
#include "gray_profile_store.h"
#include "fast_display.h"
#include "gray_default_profile.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "presets";
static const char *const NAMESPACE = "inkboy";
static const char *const VALUES_KEY[DISPLAY_MODE_COUNT] = {"d_values", "g_values"};
static const char *const NAME_KEY[DISPLAY_MODE_COUNT] = {"d_name", "g_name"};
static const char *const MODIFIED_KEY[DISPLAY_MODE_COUNT] = {"d_modified", "g_modified"};
enum { DITHER_BLOB_VERSION = 1, DITHER_BLOB_BYTES = 3 };

static display_preset_t defaults[DISPLAY_MODE_COUNT];
static display_preset_t active[DISPLAY_MODE_COUNT];
static bool modified[DISPLAY_MODE_COUNT];
static bool ready;

static bool pulses_valid(unsigned pulses) { return pulses >= 1 && pulses <= DITHER_PULSES_MAX; }

bool display_preset_name_valid(const char *name)
{
    size_t length = name ? strnlen(name, PRESET_NAME_MAX + 1) : 0;
    if (!length || length > PRESET_NAME_MAX || name[0] == ' ' || name[length - 1] == ' ') return false;
    for (size_t i = 0; i < length; ++i)
        if (name[i] < ' ' || name[i] > '~' || name[i] == '"' || name[i] == '\\') return false;
    return true;
}

bool display_preset_is_default_name(const char *name)
{
    return name && !strcasecmp(name, PRESET_DEFAULT_NAME);
}

bool display_preset_valid(const display_preset_t *preset)
{
    if (!preset || !display_preset_name_valid(preset->name)) return false;
    if (preset->mode == DISPLAY_DITHERED) return pulses_valid(preset->black_pulses) && pulses_valid(preset->white_pulses);
    return preset->mode == DISPLAY_GRAYSCALE && gray_profile_valid(&preset->gray);
}

bool display_preset_same_settings(const display_preset_t *a, const display_preset_t *b)
{
    if (a->mode != b->mode) return false;
    if (a->mode == DISPLAY_DITHERED) return a->black_pulses == b->black_pulses && a->white_pulses == b->white_pulses;
    return !memcmp(&a->gray, &b->gray, sizeof(a->gray));
}

static void build_defaults(void)
{
    defaults[DISPLAY_DITHERED] = (display_preset_t){.mode = DISPLAY_DITHERED, .name = PRESET_DEFAULT_NAME,
                                                    .black_pulses = DITHER_PULSES_DEFAULT,
                                                    .white_pulses = DITHER_PULSES_DEFAULT};
    defaults[DISPLAY_GRAYSCALE] = (display_preset_t){.mode = DISPLAY_GRAYSCALE, .name = PRESET_DEFAULT_NAME,
                                                     .gray = *gray_default_profile()};
}

static void ensure_ready(void)
{
    if (ready) return;
    build_defaults();
    memcpy(active, defaults, sizeof(active));
    ready = true;
}

const display_preset_t *display_preset_default(display_mode_t mode)
{
    ensure_ready();
    return &defaults[mode];
}

static esp_err_t read_values(nvs_handle_t h, display_mode_t mode, display_preset_t *preset)
{
    *preset = defaults[mode];
    if (mode == DISPLAY_GRAYSCALE) return gray_profile_nvs_get(h, VALUES_KEY[mode], &preset->gray);
    uint8_t blob[DITHER_BLOB_BYTES];
    size_t size = sizeof(blob);
    esp_err_t error = nvs_get_blob(h, VALUES_KEY[mode], blob, &size);
    if (error) return error;
    if (size != sizeof(blob) || blob[0] != DITHER_BLOB_VERSION || !pulses_valid(blob[1]) || !pulses_valid(blob[2]))
        return ESP_ERR_INVALID_ARG;
    preset->black_pulses = blob[1];
    preset->white_pulses = blob[2];
    return ESP_OK;
}

static esp_err_t write_values(nvs_handle_t h, const display_preset_t *preset)
{
    if (preset->mode == DISPLAY_GRAYSCALE) return gray_profile_nvs_set(h, VALUES_KEY[preset->mode], &preset->gray);
    const uint8_t blob[DITHER_BLOB_BYTES] = {DITHER_BLOB_VERSION, preset->black_pulses, preset->white_pulses};
    return nvs_set_blob(h, VALUES_KEY[preset->mode], blob, sizeof(blob));
}

static esp_err_t load_mode(nvs_handle_t h, display_mode_t mode)
{
    display_preset_t loaded;
    esp_err_t error = read_values(h, mode, &loaded);
    if (error) {
        active[mode] = defaults[mode];
        modified[mode] = false;
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
    }
    char name[PRESET_NAME_MAX + 1];
    size_t size = sizeof(name);
    if (nvs_get_str(h, NAME_KEY[mode], name, &size) == ESP_OK && display_preset_name_valid(name))
        snprintf(loaded.name, sizeof(loaded.name), "%s", name);
    uint8_t flag;
    bool is_modified = nvs_get_u8(h, MODIFIED_KEY[mode], &flag) == ESP_OK && flag == 1;
    /* The compiled Default is never a stored name for different values. */
    if (display_preset_is_default_name(loaded.name) && !display_preset_same_settings(&loaded, &defaults[mode]))
        is_modified = true;
    active[mode] = loaded;
    modified[mode] = is_modified;
    return ESP_OK;
}

esp_err_t display_preset_load_active(void)
{
    ensure_ready();
    nvs_handle_t h;
    esp_err_t result = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (result) {
        memcpy(active, defaults, sizeof(active));
        memset(modified, 0, sizeof(modified));
        return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
    }
    for (display_mode_t mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        esp_err_t error = load_mode(h, mode);
        if (error) ESP_LOGW(TAG, "stored %s settings unusable: %s; using Default", mode == DISPLAY_DITHERED ? "dithered" : "grayscale",
                            esp_err_to_name(error));
        if (!result) result = error;
        ESP_LOGI(TAG, "PRESET_ACTIVE,mode,%u,name,%s,modified,%u", mode, active[mode].name, modified[mode]);
    }
    nvs_close(h);
    return result;
}

const display_preset_t *display_preset_active(display_mode_t mode)
{
    ensure_ready();
    return &active[mode];
}

bool display_preset_active_modified(display_mode_t mode)
{
    return modified[mode];
}

bool display_preset_active_is_user(display_mode_t mode)
{
    return !display_preset_is_default_name(display_preset_active(mode)->name);
}

esp_err_t display_preset_set_active(const display_preset_t *preset, bool is_modified)
{
    ensure_ready();
    if (!display_preset_valid(preset)) return ESP_ERR_INVALID_ARG;
    display_preset_t stored = {.mode = preset->mode};
    snprintf(stored.name, sizeof(stored.name), "%s", preset->name);
    if (preset->mode == DISPLAY_DITHERED) {
        stored.black_pulses = preset->black_pulses;
        stored.white_pulses = preset->white_pulses;
    } else {
        stored.gray = preset->gray;
    }
    nvs_handle_t h;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (error) return error;
    error = write_values(h, &stored);
    if (!error) error = nvs_set_str(h, NAME_KEY[stored.mode], stored.name);
    if (!error) error = nvs_set_u8(h, MODIFIED_KEY[stored.mode], is_modified);
    if (!error) error = nvs_commit(h);
    nvs_close(h);
    if (error) return error;
    active[stored.mode] = stored;
    modified[stored.mode] = is_modified;
    return ESP_OK;
}

void display_preset_active_label(display_mode_t mode, char *label, size_t size)
{
    snprintf(label, size, "%s%s", display_preset_active(mode)->name, modified[mode] ? " *" : "");
}
