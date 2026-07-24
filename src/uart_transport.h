#ifndef UART_TRANSPORT_H
#define UART_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "t5_protocol.h"

int uart_transport_init(void);
int uart_transport_send_frame(const t5_frame_t *frame);
int uart_transport_send_ui_action(uint16_t action, uint8_t object_type,
                                  uint32_t object_id, int32_t value,
                                  const char *text);
int uart_transport_send_page_event(uint8_t page_id, uint8_t event,
                                   uint32_t object_id);
bool uart_transport_host_online(void);
uint32_t uart_transport_last_heartbeat_ms(void);

#endif /* UART_TRANSPORT_H */
