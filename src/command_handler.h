/**
 * @file command_handler.h
 * @brief Command dispatch: routes a decoded frame to the appropriate handler.
 */

#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include "t5_protocol.h"

/**
 * @brief Dispatch a decoded command frame to its handler.
 *
 * @param[in]  cmd          Command ID.
 * @param[in]  payload      Payload bytes (may be NULL if payload_len==0).
 * @param[in]  payload_len  Payload length.
 * @param[out] resp_buf     Buffer for optional response data (after the
 *                          u16LE status prefix).  Must be at least 64 bytes.
 * @param[out] resp_len     Set to the number of bytes written to resp_buf.
 *
 * @return Status code (T5_STATUS_OK etc.).
 */
uint16_t command_dispatch(uint16_t cmd,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t *resp_buf, uint16_t *resp_len);

#endif /* COMMAND_HANDLER_H */
