#ifndef REQUEST_CACHE_H
#define REQUEST_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "nightshift_config.h"

typedef struct {
    uint16_t sequence;
    uint16_t command;
    uint16_t status;
    uint16_t data_len;
    uint8_t data[NIGHTSHIFT_DEDUP_DATA_SIZE];
    bool valid;
} request_cache_entry_t;

typedef struct {
    request_cache_entry_t entries[NIGHTSHIFT_DEDUP_CACHE_SIZE];
    uint16_t next;
} request_cache_t;

void request_cache_init(request_cache_t *cache);
const request_cache_entry_t *request_cache_find(
    const request_cache_t *cache, uint16_t sequence, uint16_t command);
void request_cache_store(request_cache_t *cache,
                         uint16_t sequence, uint16_t command,
                         uint16_t status,
                         const uint8_t *data, uint16_t data_len);

#endif /* REQUEST_CACHE_H */
