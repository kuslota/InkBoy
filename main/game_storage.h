#pragma once
#include "esp_err.h"
#include "game_catalog.h"
#include <stdint.h>
#include <stddef.h>
esp_err_t game_storage_mount(void);
const char *game_storage_root(void);
esp_err_t game_storage_close(void);
esp_err_t game_storage_list(game_entry_t *entries, size_t capacity, size_t *count, bool *truncated);
esp_err_t game_storage_read(const game_entry_t *entry, uint8_t **rom, size_t *size);
/* Reads the save (or its .bak) into data[capacity]. Any size the ROM accepts is loaded: clock
 * cartridges also take saves from other emulators with or without a clock footer. */
esp_err_t game_storage_load_save(const game_entry_t *entry, const uint8_t *rom, size_t rom_size,
                                 uint8_t *data, size_t capacity, size_t *loaded);
esp_err_t game_storage_save(const game_entry_t *entry, const uint8_t *data, size_t size);
uint32_t game_storage_crc32(const void *data, size_t size);
esp_err_t game_storage_save_state(const game_entry_t *entry, uint32_t rom_crc32,
                                  uint32_t rom_size, const uint8_t *payload,
                                  size_t payload_size);
esp_err_t game_storage_load_state(const game_entry_t *entry, uint32_t rom_crc32,
                                  uint32_t rom_size, uint8_t *payload,
                                  size_t exact_payload_size);
esp_err_t game_storage_has_state(const game_entry_t *entry, uint32_t rom_crc32,
                                 uint32_t rom_size, size_t exact_payload_size,
                                 bool *exists);
