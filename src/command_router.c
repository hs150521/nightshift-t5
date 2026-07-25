/**
 * @file command_router.c
 * @brief Strict parsers for the frozen Nightshift T5-Link v1 layouts.
 */

#include "command_router.h"

#include "link_session.h"
#include "nightshift_config.h"
#include "state_store.h"
#include "tal_api.h"

#include <string.h>

static uint16_t map_result(state_store_result_t result)
{
    switch (result) {
    case STATE_STORE_OK:         return T5_STATUS_OK;
    case STATE_STORE_STALE:
    case STATE_STORE_CONFLICT:   return T5_STATUS_STATE_CONFLICT;
    case STATE_STORE_BUSY:       return T5_STATUS_BUSY;
    case STATE_STORE_INCOMPLETE: return T5_STATUS_NOT_READY;
    case STATE_STORE_NO_SPACE:   return T5_STATUS_NO_SPACE;
    default:                     return T5_STATUS_INVALID_ARG;
    }
}

static uint16_t read_string(const uint8_t *payload, uint16_t available,
                            char *out, uint16_t out_size)
{
    if (!payload || !out || out_size == 0 || available < 2) return 0;
    uint16_t wire_length = READ_U16_LE(payload);
    if ((uint32_t)wire_length + 2U > available) return 0;
    uint16_t copy_length = wire_length;
    if (copy_length >= out_size) copy_length = (uint16_t)(out_size - 1);
    if (copy_length != 0) memcpy(out, payload + 2, copy_length);
    out[copy_length] = '\0';
    return (uint16_t)(wire_length + 2U);
}

static bool exact_one_string(const uint8_t *payload, uint16_t length,
                             uint16_t offset, char *out, uint16_t out_size)
{
    if (length < offset) return false;
    uint16_t used = read_string(payload + offset,
                                (uint16_t)(length - offset),
                                out, out_size);
    return used != 0 && (uint32_t)offset + used == length;
}

static uint16_t handle_hello(const uint8_t *p, uint16_t len)
{
    link_hello_info_t info;
    return link_session_parse_opi_hello(p, len, &info);
}

static uint16_t handle_get_info(const uint8_t *p, uint16_t len,
                                uint8_t *response, uint16_t *response_len)
{
    static const char firmware[] = NIGHTSHIFT_FW_VERSION;
    static const char board[] = "TUYA_T5AI_BOARD";
    uint16_t offset = 0;
    (void)p;
    if (len != 0) return T5_STATUS_INVALID_LEN;

    response[offset++] = T5_PROTO_VERSION;
    response[offset++] = T5_PROTO_MINOR;
    WRITE_U16_LE(response + offset, NIGHTSHIFT_MAX_PAYLOAD);
    offset += 2;
    WRITE_U32_LE(response + offset, NIGHTSHIFT_CAPABILITIES);
    offset += 4;
    WRITE_U16_LE(response + offset, 480);
    offset += 2;
    WRITE_U16_LE(response + offset, 320);
    offset += 2;
    response[offset++] = 16; /* RGB565 */
    response[offset++] = NIGHTSHIFT_MAX_TASKS;
    WRITE_U16_LE(response + offset, sizeof(firmware) - 1);
    offset += 2;
    memcpy(response + offset, firmware, sizeof(firmware) - 1);
    offset += sizeof(firmware) - 1;
    WRITE_U16_LE(response + offset, sizeof(board) - 1);
    offset += 2;
    memcpy(response + offset, board, sizeof(board) - 1);
    offset += sizeof(board) - 1;
    *response_len = offset;
    return T5_STATUS_OK;
}

static uint16_t handle_time_sync(const uint8_t *p, uint16_t len)
{
    if (len != 10) return T5_STATUS_INVALID_LEN;
    state_store_time_sync(READ_U64_LE(p), (int16_t)READ_U16_LE(p + 8),
                          tal_system_get_millisecond());
    return T5_STATUS_OK;
}

static uint16_t handle_sync_begin(const uint8_t *p, uint16_t len)
{
    if (len != 5) return T5_STATUS_INVALID_LEN;
    return map_result(state_store_sync_begin(
        READ_U32_LE(p), tal_system_get_millisecond()));
}

static uint16_t handle_sync_end(const uint8_t *p, uint16_t len)
{
    if (len != 8) return T5_STATUS_INVALID_LEN;
    return map_result(state_store_sync_end(READ_U32_LE(p),
                                            READ_U32_LE(p + 4)));
}

static uint16_t handle_mode(const uint8_t *p, uint16_t len)
{
    /* revision:u32, mode:u8, reason:u8, changed_at_ms:u64 */
    if (len != 14) return T5_STATUS_INVALID_LEN;
    if (p[4] > T5_MODE_NIGHT_EXEC) return T5_STATUS_INVALID_ARG;
    return map_result(state_store_set_mode(READ_U32_LE(p), p[4],
                                            p[5],
                                            READ_U64_LE(p + 6)));
}

static uint16_t handle_attention(const uint8_t *p, uint16_t len)
{
    char message[NIGHTSHIFT_ATTENTION_TEXT_SIZE];
    if (len < 12 ||
        !exact_one_string(p, len, 10, message, sizeof(message))) {
        return T5_STATUS_INVALID_LEN;
    }
    return map_result(state_store_set_attention(
        READ_U32_LE(p), READ_U32_LE(p + 4), READ_U16_LE(p + 8), message));
}

static uint16_t handle_work(const uint8_t *p, uint16_t len)
{
    char title[NIGHTSHIFT_CURRENT_TITLE_SIZE];
    /* revision:u32 + <BHHIIII> + string */
    if (len < 27 ||
        !exact_one_string(p, len, 25, title, sizeof(title))) {
        return T5_STATUS_INVALID_LEN;
    }
    if (p[4] > T5_WORK_FAILED || READ_U16_LE(p + 5) > 1000) {
        return T5_STATUS_INVALID_ARG;
    }
    return map_result(state_store_set_work(
        READ_U32_LE(p), p[4], READ_U16_LE(p + 5),
        READ_U32_LE(p + 9), READ_U32_LE(p + 13),
        READ_U32_LE(p + 17), READ_U32_LE(p + 21), title));
}

static uint16_t handle_dashboard(const uint8_t *p, uint16_t len)
{
    if (len != 16) return T5_STATUS_INVALID_LEN;
    return map_result(state_store_set_dashboard(
        READ_U32_LE(p), READ_U16_LE(p + 4), READ_U16_LE(p + 6),
        READ_U16_LE(p + 8), READ_U16_LE(p + 10),
        READ_U16_LE(p + 12), READ_U16_LE(p + 14)));
}

static uint16_t handle_notice(const uint8_t *p, uint16_t len)
{
    char title[NIGHTSHIFT_NOTICE_TITLE_SIZE];
    char body[NIGHTSHIFT_NOTICE_BODY_SIZE];
    if (len < 22 || p[8] > T5_NOTICE_CRITICAL) {
        return len < 22 ? T5_STATUS_INVALID_LEN : T5_STATUS_INVALID_ARG;
    }
    uint16_t title_used = read_string(p + 18, (uint16_t)(len - 18),
                                      title, sizeof(title));
    if (title_used == 0 || (uint32_t)18 + title_used > len) {
        return T5_STATUS_INVALID_LEN;
    }
    uint16_t body_offset = (uint16_t)(18 + title_used);
    uint16_t body_used = read_string(p + body_offset,
                                     (uint16_t)(len - body_offset),
                                     body, sizeof(body));
    if (body_used == 0 || (uint32_t)body_offset + body_used != len) {
        return T5_STATUS_INVALID_LEN;
    }
    return map_result(state_store_show_notice(
        READ_U32_LE(p), READ_U32_LE(p + 4), p[8], p[9],
        READ_U64_LE(p + 10), title, body));
}

static uint16_t handle_task_begin(const uint8_t *p, uint16_t len)
{
    if (len != 7) return T5_STATUS_INVALID_LEN;
    return map_result(state_store_task_begin(
        READ_U32_LE(p), p[4], READ_U16_LE(p + 5)));
}

static uint16_t handle_task_item(const uint8_t *p, uint16_t len)
{
    display_task_t item;
    memset(&item, 0, sizeof(item));
    if (len < 15) return T5_STATUS_INVALID_LEN;
    item.task_id = READ_U32_LE(p + 4);
    item.quadrant = p[8];
    item.task_state = p[9];
    item.flags = p[10];

    uint16_t title_used = read_string(p + 11, (uint16_t)(len - 11),
                                      item.title, sizeof(item.title));
    if (title_used == 0 || (uint32_t)11 + title_used > len) {
        return T5_STATUS_INVALID_LEN;
    }
    uint16_t source_offset = (uint16_t)(11 + title_used);
    uint16_t source_used = read_string(p + source_offset,
                                       (uint16_t)(len - source_offset),
                                       item.source, sizeof(item.source));
    if (source_used == 0 || (uint32_t)source_offset + source_used != len) {
        return T5_STATUS_INVALID_LEN;
    }
    return map_result(state_store_task_item(
        READ_U32_LE(p), &item, p, len));
}

static uint16_t handle_task_end(const uint8_t *p, uint16_t len)
{
    if (len != 8) return T5_STATUS_INVALID_LEN;
    return map_result(state_store_task_end(
        READ_U32_LE(p), READ_U32_LE(p + 4)));
}

static uint16_t handle_led_override(const uint8_t *p, uint16_t len)
{
    if (len != 4) return T5_STATUS_INVALID_LEN;
    if (p[0] > 1 || p[1] > 2) return T5_STATUS_INVALID_ARG;
#if defined(LED_NAME) && defined(CONFIG_ENABLE_LED) && CONFIG_ENABLE_LED
    state_store_set_led_override(p[0] != 0, p[1], READ_U16_LE(p + 2));
    return T5_STATUS_OK;
#else
    (void)p;
    return T5_STATUS_NOT_READY;
#endif
}

static uint16_t handle_backlight(const uint8_t *p, uint16_t len)
{
    if (len != 1) return T5_STATUS_INVALID_LEN;
    if (p[0] > 100) return T5_STATUS_INVALID_ARG;
    state_store_set_backlight(p[0]);
    return T5_STATUS_OK;
}

uint16_t command_router_dispatch(uint16_t command,
                                 const uint8_t *payload,
                                 uint16_t payload_len,
                                 uint8_t *response_data,
                                 uint16_t *response_len)
{
    if (!response_data || !response_len ||
        (!payload && payload_len != 0)) {
        return T5_STATUS_INTERNAL_ERR;
    }
    *response_len = 0;

    uint16_t status;
    switch (command) {
    case T5_CMD_HELLO:
        status = handle_hello(payload, payload_len);
        break;
    case T5_CMD_HEARTBEAT:
        if (payload_len != 8) return T5_STATUS_INVALID_LEN;
        WRITE_U32_LE(response_data, tal_system_get_millisecond());
        WRITE_U32_LE(response_data + 4, state_store_revision());
        WRITE_U32_LE(response_data + 8, 0);
        *response_len = 12;
        return T5_STATUS_OK;
    case T5_CMD_GET_INFO:
        return handle_get_info(payload, payload_len,
                               response_data, response_len);
    case T5_CMD_TIME_SYNC:
        return handle_time_sync(payload, payload_len);
    case T5_CMD_STATE_SYNC_BEGIN:
        return handle_sync_begin(payload, payload_len);
    case T5_CMD_STATE_SYNC_END:
        return handle_sync_end(payload, payload_len);
    case T5_CMD_MODE_SET:
        status = handle_mode(payload, payload_len);
        break;
    case T5_CMD_ATTENTION_SET:
        status = handle_attention(payload, payload_len);
        break;
    case T5_CMD_WORK_STATE_SET:
        status = handle_work(payload, payload_len);
        break;
    case T5_CMD_DASHBOARD_SET:
        status = handle_dashboard(payload, payload_len);
        break;
    case T5_CMD_NOTICE_SHOW:
        status = handle_notice(payload, payload_len);
        break;
    case T5_CMD_TASK_LIST_BEGIN:
        status = handle_task_begin(payload, payload_len);
        break;
    case T5_CMD_TASK_ITEM:
        status = handle_task_item(payload, payload_len);
        break;
    case T5_CMD_TASK_LIST_END:
        status = handle_task_end(payload, payload_len);
        break;
    case T5_CMD_LED_OVERRIDE:
        return handle_led_override(payload, payload_len);
    case T5_CMD_BACKLIGHT_SET:
        return handle_backlight(payload, payload_len);
    default:
        return T5_STATUS_UNKNOWN_CMD;
    }

    if (status == T5_STATUS_OK && state_store_sync_active() &&
        command != T5_CMD_STATE_SYNC_BEGIN &&
        command != T5_CMD_STATE_SYNC_END) {
        state_store_sync_record(command, payload, payload_len);
    }
    return status;
}
