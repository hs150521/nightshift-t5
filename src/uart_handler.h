/**
 * @file uart_handler.h
 * @brief UART transport: init, send, heartbeat tracking.
 */

#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "t5_protocol.h"

/**
 * @brief Initialise UART0 at 460800 8N1 and spawn the RX task.
 * @return 0 on success, negative on error.
 */
int uart_handler_init(void);

/**
 * @brief Encode and transmit a t5_frame_t over UART.
 */
int uart_send_frame(const t5_frame_t *frame);

/**
 * @brief Build a RESPONSE frame (flags=RESPONSE, seq echoed) with a
 *        u16LE status prefix followed by optional extra data.
 */
int uart_send_response(uint16_t seq, uint16_t cmd, uint16_t status,
                       const uint8_t *data, uint16_t data_len);

/**
 * @brief Build an EVENT frame (flags=EVENT) and transmit it.
 */
int uart_send_event(uint16_t cmd,
                    const uint8_t *payload, uint16_t payload_len);

/**
 * @brief True if a heartbeat has been received within the timeout window.
 */
bool uart_is_opi_online(void);

/**
 * @brief Timestamp (ms) of the last received HEARTBEAT.
 */
uint32_t uart_get_last_heartbeat_ms(void);

/**
 * @brief Write a plain-text debug string directly to the protocol UART.
 *        Use this for diagnostics visible on the same COM port as protocol data.
 */
void uart_debug_puts(const char *str);

#endif /* UART_HANDLER_H */
