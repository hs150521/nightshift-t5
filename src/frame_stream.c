#include "frame_stream.h"

#include <string.h>

void frame_stream_init(frame_stream_t *stream)
{
    if (stream) memset(stream, 0, sizeof(*stream));
}

frame_stream_result_t frame_stream_push(frame_stream_t *stream, uint8_t byte)
{
    if (!stream) return FRAME_STREAM_DROPPED;
    if (byte == 0) {
        if (stream->dropping) {
            stream->dropping = false;
            stream->length = 0;
            return FRAME_STREAM_DROPPED;
        }
        if (stream->length == 0) {
            stream->empty_count++;
            return FRAME_STREAM_NONE;
        }
        return FRAME_STREAM_READY;
    }
    if (stream->dropping) return FRAME_STREAM_NONE;
    if (stream->length >= sizeof(stream->bytes)) {
        stream->dropping = true;
        stream->length = 0;
        stream->overflow_count++;
        return FRAME_STREAM_NONE;
    }
    stream->bytes[stream->length++] = byte;
    return FRAME_STREAM_NONE;
}

const uint8_t *frame_stream_data(const frame_stream_t *stream)
{
    return stream ? stream->bytes : NULL;
}

uint16_t frame_stream_length(const frame_stream_t *stream)
{
    return stream ? stream->length : 0;
}

void frame_stream_consume(frame_stream_t *stream)
{
    if (stream) stream->length = 0;
}
