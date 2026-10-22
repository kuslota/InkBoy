#include "fake_game_storage.h"
#include "host_fakes.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

fake_storage_t fake_storage;

void fake_storage_reset(void)
{
    memset(&fake_storage, 0, sizeof(fake_storage));
    fake_storage.rom_size = 32768;
}

esp_err_t game_storage_mount(void)
{
    return fake_storage.mount_error;
}

const char *game_storage_root(void)
{
    return fake_storage.root;
}

esp_err_t game_storage_close(void)
{
    host_trace("game_storage_close");
    return fake_storage.close_error;
}

esp_err_t game_storage_list(game_entry_t *entries, size_t capacity, size_t *count, bool *truncated)
{
    assert(!host_mutexes_held && fake_storage.list_count <= capacity);
    *count = fake_storage.list_count;
    *truncated = false;
    memcpy(entries, fake_storage.listed, fake_storage.list_count * sizeof(*entries));
    return fake_storage.list_error;
}

esp_err_t game_storage_read(const game_entry_t *entry, uint8_t **rom, size_t *size)
{
    (void)entry;
    assert(!host_mutexes_held);
    if (fake_storage.read_error) return fake_storage.read_error;
    *size = fake_storage.rom_size;
    *rom = calloc(1, *size);
    assert(*rom);
    return ESP_OK;
}

esp_err_t game_storage_load_save(const game_entry_t *entry, const uint8_t *rom, size_t rom_size,
                                 uint8_t *data, size_t capacity, size_t *loaded)
{
    (void)entry; (void)rom; (void)rom_size;
    assert(!host_mutexes_held);
    memset(data, fake_storage.save_fill, capacity);
    *loaded = capacity;
    return fake_storage.load_save_error;
}

esp_err_t game_storage_save(const game_entry_t *entry, const uint8_t *data, size_t size)
{
    (void)entry;
    assert(!host_mutexes_held && size <= sizeof(fake_storage.disk_save));
    ++fake_storage.save_writes;
    if (fake_storage.save_error) return fake_storage.save_error;
    memcpy(fake_storage.disk_save, data, size);
    fake_storage.disk_save_size = size;
    return ESP_OK;
}

uint32_t game_storage_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = (uint32_t)size;
    for (size_t i = 0; i < size; ++i) crc = (crc * 33u) ^ bytes[i];
    return crc;
}

esp_err_t game_storage_save_state(const game_entry_t *entry, uint32_t rom_crc32, uint32_t rom_size,
                                  const uint8_t *payload, size_t payload_size)
{
    (void)entry; (void)rom_crc32; (void)rom_size;
    assert(!host_mutexes_held && payload_size <= sizeof(fake_storage.disk_state));
    ++fake_storage.state_writes;
    if (fake_storage.save_state_error) return fake_storage.save_state_error;
    memcpy(fake_storage.disk_state, payload, payload_size);
    return ESP_OK;
}

esp_err_t game_storage_load_state(const game_entry_t *entry, uint32_t rom_crc32, uint32_t rom_size,
                                  uint8_t *payload, size_t exact_payload_size)
{
    (void)entry; (void)rom_crc32; (void)rom_size;
    assert(!host_mutexes_held);
    ++fake_storage.state_reads;
    if (fake_storage.load_state_error) return fake_storage.load_state_error;
    memcpy(payload, fake_storage.disk_state, exact_payload_size);
    return ESP_OK;
}

esp_err_t game_storage_has_state(const game_entry_t *entry, uint32_t rom_crc32, uint32_t rom_size,
                                 size_t exact_payload_size, bool *exists)
{
    (void)entry; (void)rom_crc32; (void)rom_size; (void)exact_payload_size;
    *exists = fake_storage.has_state;
    return fake_storage.has_state_error;
}
