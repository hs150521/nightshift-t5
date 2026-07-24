#ifndef FRAME_STREAM_H
#define FRAME_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "nightshift_config.h"

typedef enum {
    FRAME_STREAM_NONE = 0,
    FRAME_STREAM_READY,
    FRAME_STREAM_DROPPED,
} frame_stream_result_t;

typedef struct {
    uint8_t bytes[NIGHTSHIFT_MAX_WIRE_FRAME];
    uint16_t length;
    bool dropping;
    uint32_t overflow_count;
    uint32_t empty_count;
} frame_stream_t;

void frame_stream_init(frame_stream_t *stream);
frame_stream_result_t frame_stream_push(frame_stream_t *stream, uint8_t byte);
const uint8_t *frame_stream_data(const frame_stream_t *stream);
uint16_t frame_stream_length(const frame_stream_t *stream);
void frame_stream_consume(frame_stream_t *stream);

#endif /* FRAME_STREAM_H */
