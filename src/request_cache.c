#include "request_cache.h"

#include <string.h>

void request_cache_init(request_cache_t *cache)
{
    if (cache) memset(cache, 0, sizeof(*cache));
}

const request_cache_entry_t *request_cache_find(
    const request_cache_t *cache, uint16_t sequence, uint16_t command)
{
    if (!cache) return NULL;
    for (uint16_t i = 0; i < NIGHTSHIFT_DEDUP_CACHE_SIZE; ++i) {
        const request_cache_entry_t *entry = &cache->entries[i];
        if (entry->valid && entry->sequence == sequence &&
            entry->command == command) {
            return entry;
        }
    }
    return NULL;
}

void request_cache_store(request_cache_t *cache,
                         uint16_t sequence, uint16_t command,
                         uint16_t status,
                         const uint8_t *data, uint16_t data_len)
{
    if (!cache) return;
    request_cache_entry_t *entry = &cache->entries[cache->next];
    entry->sequence = sequence;
    entry->command = command;
    entry->status = status;
    entry->data_len = data_len;
    if (entry->data_len > NIGHTSHIFT_DEDUP_DATA_SIZE) {
        entry->data_len = NIGHTSHIFT_DEDUP_DATA_SIZE;
    }
    if (entry->data_len != 0 && data) {
        memcpy(entry->data, data, entry->data_len);
    }
    entry->valid = true;
    cache->next = (uint16_t)((cache->next + 1U) %
                             NIGHTSHIFT_DEDUP_CACHE_SIZE);
}
