/**
 * @file t5_protocol.h
 * @brief T5-Link v1 protocol constants, frame structure, and codec declarations.
 *
 * This header is shared by the UART transport layer, command handler, and any
 * future diagnostic tooling.  It is deliberately independent of LVGL and the
 * UI layer.
 */

#ifndef T5_PROTOCOL_H
#define T5_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "nightshift_config.h"

/* =========================================================================
 * Wire helpers — little-endian read / write
 * ======================================================================= */
#define READ_U16_LE(p) \
    ((uint16_t)(p)[0] | ((uint16_t)(p)[1] << 8))

#define READ_U32_LE(p) \
    ((uint32_t)(p)[0]        | ((uint32_t)(p)[1] << 8) | \
     ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))

#define READ_U64_LE(p) \
    ((uint64_t)READ_U32_LE(p) | ((uint64_t)READ_U32_LE((p) + 4) << 32))

#define WRITE_U16_LE(p, v) do { \
    (p)[0] =  (v)        & 0xFF; \
    (p)[1] = ((v) >>  8) & 0xFF; \
} while (0)

#define WRITE_U32_LE(p, v) do { \
    (p)[0] =  (v)        & 0xFF; \
    (p)[1] = ((v) >>  8) & 0xFF; \
    (p)[2] = ((v) >> 16) & 0xFF; \
    (p)[3] = ((v) >> 24) & 0xFF; \
} while (0)

#define WRITE_U64_LE(p, v) do { \
    WRITE_U32_LE(p,     (uint32_t)((v) & 0xFFFFFFFF)); \
    WRITE_U32_LE((p)+4, (uint32_t)(((v) >> 32) & 0xFFFFFFFF)); \
} while (0)

/* =========================================================================
 * Frame magic / version
 * ======================================================================= */
#define T5_MAGIC_BYTE0        0x54   /* 'T' */
#define T5_MAGIC_BYTE1        0x35   /* '5' */
#define T5_PROTO_VERSION      0x01
#define T5_PROTO_MINOR        0x00

/* Canonical HELLO peer roles. */
#define T5_PEER_ROLE_OPI      0x01
#define T5_PEER_ROLE_PANEL    0x02

/* =========================================================================
 * Command IDs
 * ======================================================================= */
/* System commands */
#define T5_CMD_HELLO              0x0001
#define T5_CMD_HEARTBEAT          0x0002
#define T5_CMD_GET_INFO           0x0003
#define T5_CMD_TIME_SYNC          0x0004
#define T5_CMD_STATE_SYNC_BEGIN   0x0010
#define T5_CMD_STATE_SYNC_END     0x0011

/* State commands */
#define T5_CMD_MODE_SET           0x1001
#define T5_CMD_ATTENTION_SET      0x1002
#define T5_CMD_WORK_STATE_SET     0x1003
#define T5_CMD_DASHBOARD_SET      0x1004
#define T5_CMD_NOTICE_SHOW        0x1005

/* Task list commands */
#define T5_CMD_TASK_LIST_BEGIN    0x1101
#define T5_CMD_TASK_ITEM          0x1102
#define T5_CMD_TASK_LIST_END      0x1103

/* User action commands (T5 → OPi) */
#define T5_CMD_UI_ACTION          0x2001
#define T5_CMD_PAGE_EVENT         0x2002

/* LED / backlight */
#define T5_CMD_LED_OVERRIDE       0x3001
#define T5_CMD_BACKLIGHT_SET      0x3002

/* UI action values */
#define T5_ACTION_CONFIRM          1
#define T5_ACTION_REJECT           2
#define T5_ACTION_RETRY            3
#define T5_ACTION_PAUSE_EXECUTION  4
#define T5_ACTION_RESUME_EXECUTION 5
#define T5_ACTION_OPEN_TASK        6
#define T5_ACTION_CLOSE_TASK       7
#define T5_ACTION_DISMISS_NOTICE   12
#define T5_ACTION_REQUEST_RESYNC   13

/* UI object types used by Nightshift. */
#define T5_OBJECT_NONE             0
#define T5_OBJECT_TASK             1
#define T5_OBJECT_NOTICE           2
#define T5_OBJECT_EXECUTOR         3
#define T5_OBJECT_PANEL            4

/* Notice severities and flags. */
#define T5_NOTICE_INFO             0
#define T5_NOTICE_WARNING          1
#define T5_NOTICE_ERROR            2
#define T5_NOTICE_CRITICAL         3
#define T5_NOTICE_DISMISSIBLE      0x01

/* =========================================================================
 * Flag bits (frame.flags field)
 * ======================================================================= */
#define T5_FLAG_ACK_REQ        0x01
#define T5_FLAG_RESPONSE       0x02
#define T5_FLAG_EVENT          0x04
#define T5_FLAG_MORE           0x08
#define T5_FLAG_HIGH_PRIORITY  0x10
#define T5_FLAG_VALID_MASK     0x1F

/* =========================================================================
 * Status codes (response payload u16LE prefix)
 * ======================================================================= */
#define T5_STATUS_OK                0x0000
#define T5_STATUS_ACCEPTED          0x0001
#define T5_STATUS_UNKNOWN_CMD       0x0002
#define T5_STATUS_INVALID_LEN       0x0003
#define T5_STATUS_INVALID_ARG       0x0004
#define T5_STATUS_BUSY              0x0005
#define T5_STATUS_NOT_READY         0x0006
#define T5_STATUS_NOT_FOUND         0x0007
#define T5_STATUS_INTERNAL_ERR      0x0008
#define T5_STATUS_UNSUPPORTED_VER   0x0009
#define T5_STATUS_NO_SPACE          0x000A
#define T5_STATUS_STATE_CONFLICT    0x000B

/* =========================================================================
 * Mode enum
 * ======================================================================= */
#define T5_MODE_IDLE         0
#define T5_MODE_DAY_WORK     1
#define T5_MODE_NIGHT_EXEC   2

/* =========================================================================
 * Work-state enum
 * ======================================================================= */
#define T5_WORK_STOPPED      0
#define T5_WORK_STARTING     1
#define T5_WORK_RUNNING      2
#define T5_WORK_PAUSED       3
#define T5_WORK_COMPLETED    4
#define T5_WORK_FAILED       5

/* =========================================================================
 * Attention flag bits
 * ======================================================================= */
#define T5_ATTN_NONE            0x00000000U
#define T5_ATTN_NEED_CONFIRM    0x00000001U
#define T5_ATTN_SENSOR_ERROR    0x00000002U
#define T5_ATTN_PANEL_OFFLINE   0x00000004U
#define T5_ATTN_BACKEND_ERROR   0x00000008U
#define T5_ATTN_STORAGE_WARNING 0x00000010U
#define T5_ATTN_AGENT_FAILED    0x00000020U
#define T5_ATTN_NETWORK_OFFLINE 0x00000040U

/* =========================================================================
 * Frame structure (host representation)
 * ======================================================================= */
typedef struct {
    uint8_t  flags;
    uint16_t seq;
    uint16_t cmd;
    uint16_t payload_len;
    uint8_t  payload[NIGHTSHIFT_MAX_PAYLOAD];
} t5_frame_t;

/* =========================================================================
 * Codec function declarations
 * ======================================================================= */

/**
 * @brief Compute CRC-16/CCITT-FALSE over [data, data+len).
 *        Polynomial 0x1021, init 0xFFFF, no final XOR.
 */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

/**
 * @brief COBS-encode [src, src+len) into dst.
 * @return Encoded length (excl. 0x00 delimiter), 0 on error.
 */
size_t cobs_encode(const uint8_t *src, size_t len,
                   uint8_t *dst, size_t dst_max);

/**
 * @brief COBS-decode [src, src+len) into dst.
 * @return Decoded length, 0 on error.
 */
size_t cobs_decode(const uint8_t *src, size_t len,
                   uint8_t *dst, size_t dst_max);

/**
 * @brief Encode a t5_frame_t into wire bytes (COBS + 0x00 delimiter).
 * @return Total wire length including delimiter, 0 on error.
 */
size_t t5_frame_encode(const t5_frame_t *frame,
                       uint8_t *out, size_t out_max);

/**
 * @brief Decode wire bytes (everything before the 0x00 delimiter) into a
 *        t5_frame_t.
 * @return 0 on success, negative on error.
 */
int t5_frame_decode(const uint8_t *cobs_data, size_t len,
                    t5_frame_t *frame);

/** Return non-zero when a flag combination is structurally valid. */
int t5_frame_flags_valid(uint8_t flags);

#endif /* T5_PROTOCOL_H */
