/**
 * @file command_router.h
 * @brief T5-Link v1 command validation and display-state dispatch.
 */

#ifndef COMMAND_ROUTER_H
#define COMMAND_ROUTER_H

#include <stdint.h>

#include "t5_protocol.h"

#define COMMAND_RESPONSE_MAX NIGHTSHIFT_DEDUP_DATA_SIZE

uint16_t command_router_dispatch(uint16_t command,
                                 const uint8_t *payload,
                                 uint16_t payload_len,
                                 uint8_t *response_data,
                                 uint16_t *response_len);

#endif /* COMMAND_ROUTER_H */
