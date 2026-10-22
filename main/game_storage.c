#include "game_storage.h"
#include "rom_file.h"
#include "gb_emulator.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "save_file.h"
#include "state_file.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#define ROOT "/sdcard"
#define ROM_DIR ROOT "/roms"
static sdmmc_card_t *card;
static const char *TAG = "storage";

static esp_err_t state_error(state_file_status_t status)
{
    switch (status) {
    case STATE_FILE_OK: return ESP_OK;
    case STATE_FILE_MISSING: return ESP_ERR_NOT_FOUND;
    case STATE_FILE_WRONG_SIZE: return ESP_ERR_INVALID_SIZE;
    case STATE_FILE_WRONG_ROM: return ESP_ERR_NOT_SUPPORTED;
    case STATE_FILE_INVALID: return ESP_ERR_INVALID_CRC;
    default: return ESP_FAIL;
    }
}

static bool state_path_for(const game_entry_t *entry, char path[GAME_PATH_BYTES])
{
    char rom_path[GAME_PATH_BYTES], save_path[GAME_PATH_BYTES];
    if (!game_catalog_paths(ROM_DIR, entry, rom_path, save_path)) return false;
    int n = snprintf(path, GAME_PATH_BYTES, "%s.state", rom_path);
    return n >= 0 && n < GAME_PATH_BYTES;
}

esp_err_t game_storage_mount(void)
{
    if (card) return ESP_OK;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1; slot.clk = GPIO_NUM_39; slot.cmd = GPIO_NUM_41; slot.d0 = GPIO_NUM_40;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 3,
    };
    return esp_vfs_fat_sdmmc_mount(ROOT, &host, &slot, &mount, &card);
}

const char *game_storage_root(void)
{
    return ROOT;
}

esp_err_t game_storage_close(void)
{
    if (!card) return ESP_OK;
    esp_err_t error = esp_vfs_fat_sdcard_unmount(ROOT, card);
    if (error == ESP_OK) card = NULL;
    return error;
}

esp_err_t game_storage_list(game_entry_t *entries, size_t capacity, size_t *count, bool *truncated)
{
    esp_err_t error = game_storage_mount();
    if (error != ESP_OK) return error;
    return game_catalog_scan(ROM_DIR, entries, capacity, count, truncated) ? ESP_OK : ESP_FAIL;
}

esp_err_t game_storage_read(const game_entry_t *entry, uint8_t **rom, size_t *size)
{
    *rom = NULL; *size = 0;
    char rom_path[GAME_PATH_BYTES], save_path[GAME_PATH_BYTES];
    if (!card || !game_catalog_paths(ROM_DIR, entry, rom_path, save_path)) return ESP_ERR_INVALID_STATE;
    esp_err_t result;
    struct stat info;
    if (stat(rom_path, &info) != 0) { result = ESP_ERR_NOT_FOUND; goto fail; }
    if (!S_ISREG(info.st_mode) || info.st_size < 32768 || info.st_size > GB_MAX_ROM_BYTES) {
        result = ESP_ERR_INVALID_SIZE; goto fail;
    }
    *size = info.st_size;
    *rom = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*rom) { result = ESP_ERR_NO_MEM; goto fail; }
    if (rom_file_read(rom_path, *rom, *size) != ROM_FILE_OK) { result = ESP_FAIL; goto fail; }
    if (!gb_emulator_validate_rom(*rom, *size)) { result = ESP_ERR_NOT_SUPPORTED; goto fail; }
    ESP_LOGI(TAG, "GAME_ROM_LOADED,path,%s,bytes,%zu,mapper,%02x,ram_code,%u",
             rom_path, *size, (*rom)[0x147], (*rom)[0x149]);
    /* Keep mounted for autosaves; only this frontend task performs card I/O. */
    return ESP_OK;
fail:
    ESP_LOGW(TAG,"GAME_ROM_LOAD_FAILED,error,%s",esp_err_to_name(result));
    heap_caps_free(*rom); *rom = NULL; *size = 0;
    return result;
}

static rom_file_status_t read_save(const char *path, const uint8_t *rom, size_t rom_size,
                                   uint8_t *data, size_t capacity, size_t *loaded)
{
    rom_file_status_t status = rom_file_read_upto(path, data, capacity, loaded);
    if (status == ROM_FILE_OK && !gb_emulator_rom_save_accepted(rom, rom_size, *loaded)) status = ROM_FILE_SIZE;
    return status;
}

esp_err_t game_storage_load_save(const game_entry_t *entry, const uint8_t *rom, size_t rom_size,
                                 uint8_t *data, size_t capacity, size_t *loaded)
{
    char rom_path[GAME_PATH_BYTES], save_path[GAME_PATH_BYTES];
    if (!card || !capacity || !loaded || !game_catalog_paths(ROM_DIR, entry, rom_path, save_path)) return ESP_ERR_INVALID_STATE;
    rom_file_status_t status = read_save(save_path, rom, rom_size, data, capacity, loaded);
    if (status != ROM_FILE_OK) {
        char backup_path[GAME_PATH_BYTES + 4];
        snprintf(backup_path, sizeof(backup_path), "%s.bak", save_path);
        rom_file_status_t backup = read_save(backup_path, rom, rom_size, data, capacity, loaded);
        if (backup == ROM_FILE_OK) { ESP_LOGW(TAG, "Recovered backup save"); return ESP_OK; }
        if (status == ROM_FILE_MISSING && backup == ROM_FILE_MISSING) return ESP_ERR_NOT_FOUND;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SAVE_LOADED,bytes,%zu", *loaded);
    return ESP_OK;
}

esp_err_t game_storage_save(const game_entry_t *entry, const uint8_t *data, size_t size)
{
    char rom_path[GAME_PATH_BYTES], save_path[GAME_PATH_BYTES];
    if (!card || !size || !game_catalog_paths(ROM_DIR, entry, rom_path, save_path)) return ESP_ERR_INVALID_STATE;
    if (!save_file_replace(save_path, data, size)) return ESP_FAIL;
    ESP_LOGI(TAG, "SAVE_WRITTEN,bytes,%zu", size);
    return ESP_OK;
}

uint32_t game_storage_crc32(const void *data, size_t size)
{
    return state_file_crc32(data, size);
}

esp_err_t game_storage_save_state(const game_entry_t *entry, uint32_t rom_crc32,
                                  uint32_t rom_size, const uint8_t *payload,
                                  size_t payload_size)
{
    char path[GAME_PATH_BYTES];
    if (!card || !state_path_for(entry, path)) return ESP_ERR_INVALID_STATE;
    state_file_status_t status = state_file_write(path, rom_crc32, rom_size, payload, payload_size);
    if (status == STATE_FILE_OK) ESP_LOGI(TAG, "STATE_WRITTEN,bytes,%zu", payload_size);
    return state_error(status);
}

esp_err_t game_storage_load_state(const game_entry_t *entry, uint32_t rom_crc32,
                                  uint32_t rom_size, uint8_t *payload,
                                  size_t exact_payload_size)
{
    char path[GAME_PATH_BYTES];
    if (!card || !state_path_for(entry, path)) return ESP_ERR_INVALID_STATE;
    state_file_status_t status = state_file_read(path, rom_crc32, rom_size,
                                                 payload, exact_payload_size);
    if (status == STATE_FILE_OK) ESP_LOGI(TAG, "STATE_LOADED,bytes,%zu", exact_payload_size);
    return state_error(status);
}

esp_err_t game_storage_has_state(const game_entry_t *entry, uint32_t rom_crc32,
                                 uint32_t rom_size, size_t exact_payload_size,
                                 bool *exists)
{
    if (!exists || !exact_payload_size || exact_payload_size > STATE_FILE_MAX_PAYLOAD)
        return ESP_ERR_INVALID_ARG;
    *exists = false;
    char path[GAME_PATH_BYTES];
    if (!card || !state_path_for(entry, path)) return ESP_ERR_INVALID_STATE;
    uint8_t *probe = malloc(exact_payload_size);
    if (!probe) return ESP_ERR_NO_MEM;
    state_file_status_t status = state_file_read(path, rom_crc32, rom_size,
                                                 probe, exact_payload_size);
    free(probe);
    if (status == STATE_FILE_MISSING) return ESP_OK;
    if (status == STATE_FILE_OK) *exists = true;
    return state_error(status);
}
