/**
 * @file command_handler.c
 * @brief Command dispatch table and per-command handlers.
 *
 * Each handle_*() function:
 *   1. Validates minimum payload length.
 *   2. Parses LE fields from the incoming payload.
 *   3. Applies the change via app_state (with revision guard where required).
 *   4. Writes any extra response bytes to resp_buf / resp_len.
 *   5. Returns a T5_STATUS_* code.
 *
 * The caller (uart_handler) prepends the u16LE status before sending.
 */

#include "command_handler.h"
#include "app_state.h"
#include "nightshift_config.h"

#include "tal_api.h"
#include <string.h>

/* =========================================================================
 * Forward declarations — individual handlers
 * ======================================================================= */
static uint16_t handle_hello(const uint8_t *p, uint16_t len,
                             uint8_t *resp, uint16_t *rlen);
static uint16_t handle_heartbeat(const uint8_t *p, uint16_t len,
                                 uint8_t *resp, uint16_t *rlen);
static uint16_t handle_state_sync_begin(const uint8_t *p, uint16_t len,
                                        uint8_t *resp, uint16_t *rlen);
static uint16_t handle_state_sync_end(const uint8_t *p, uint16_t len,
                                       uint8_t *resp, uint16_t *rlen);
static uint16_t handle_mode_set(const uint8_t *p, uint16_t len,
                                uint8_t *resp, uint16_t *rlen);
static uint16_t handle_attention_set(const uint8_t *p, uint16_t len,
                                     uint8_t *resp, uint16_t *rlen);
static uint16_t handle_work_state_set(const uint8_t *p, uint16_t len,
                                      uint8_t *resp, uint16_t *rlen);
static uint16_t handle_dashboard_set(const uint8_t *p, uint16_t len,
                                     uint8_t *resp, uint16_t *rlen);

/* =========================================================================
 * command_dispatch  — top-level router
 * ======================================================================= */
uint16_t command_dispatch(uint16_t cmd,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t *resp_buf, uint16_t *resp_len)
{
    *resp_len = 0;

    switch (cmd) {
    case T5_CMD_HELLO:
        return handle_hello(payload, payload_len, resp_buf, resp_len);
    case T5_CMD_HEARTBEAT:
        return handle_heartbeat(payload, payload_len, resp_buf, resp_len);
    case T5_CMD_STATE_SYNC_BEGIN:
        return handle_state_sync_begin(payload, payload_len,
                                       resp_buf, resp_len);
    case T5_CMD_STATE_SYNC_END:
        return handle_state_sync_end(payload, payload_len,
                                     resp_buf, resp_len);
    case T5_CMD_MODE_SET:
        return handle_mode_set(payload, payload_len, resp_buf, resp_len);
    case T5_CMD_ATTENTION_SET:
        return handle_attention_set(payload, payload_len,
                                    resp_buf, resp_len);
    case T5_CMD_WORK_STATE_SET:
        return handle_work_state_set(payload, payload_len,
                                     resp_buf, resp_len);
    case T5_CMD_DASHBOARD_SET:
        return handle_dashboard_set(payload, payload_len,
                                    resp_buf, resp_len);
    default:
        return T5_STATUS_UNKNOWN_CMD;
    }
}

/* =========================================================================
 * Helper: read a u16LE-prefixed UTF-8 string into a fixed buffer.
 * Returns the number of bytes consumed from p (incl. the u16 length prefix),
 * or 0 on error.  The output string is always null-terminated.
 * ======================================================================= */
static uint16_t read_string(const uint8_t *p, uint16_t avail,
                            char *out, uint16_t out_max)
{
    if (avail < 2) return 0;
    uint16_t slen = READ_U16_LE(p);
    if ((uint32_t)(2 + slen) > avail) return 0;
    uint16_t copy = (slen < (uint16_t)(out_max - 1))
                        ? slen : (uint16_t)(out_max - 1);
    memcpy(out, p + 2, copy);
    out[copy] = '\0';
    return (uint16_t)(2 + slen);
}

/* =========================================================================
 * HELLO   (OPi → T5)
 *
 * peer_role      u8
 * proto_major    u8
 * proto_minor    u8
 * boot_id        u32 LE
 * max_payload    u16 LE
 * capabilities   u16 LE   (OPi capability bitmask)
 * sw_version     string   (u16 len + UTF-8)
 *
 * Min: 1+1+1+4+2+2+2 = 13 bytes (empty string)
 * ======================================================================= */
static uint16_t handle_hello(const uint8_t *p, uint16_t len,
                             uint8_t *resp, uint16_t *rlen)
{
    if (len < 13) return T5_STATUS_INVALID_LEN;

    /* uint8_t  peer_role   = p[0]; */
    uint8_t  proto_major = p[1];
    /* uint8_t  proto_minor = p[2]; */
    /* uint32_t boot_id     = READ_U32_LE(p + 3); */
    /* uint16_t max_pay     = READ_U16_LE(p + 7); */
    /* uint16_t caps        = READ_U16_LE(p + 9); */
    /* string   sw_ver      = at p+11 */

    if (proto_major != T5_PROTO_VERSION) {
        return T5_STATUS_UNSUPPORTED_VER;
    }

    /* Mark OPi online on successful HELLO */
    app_state_set_opi_online(true);

    (void)resp;
    *rlen = 0;
    return T5_STATUS_OK;
}

/* =========================================================================
 * HEARTBEAT   (OPi → T5)
 *
 * Request:
 *   uptime_ms        u32 LE
 *   state_revision   u32 LE
 *
 * Response extra:
 *   t5_uptime_ms     u32 LE
 *   applied_revision u32 LE
 *   error_flags      u32 LE
 * ======================================================================= */
static uint16_t handle_heartbeat(const uint8_t *p, uint16_t len,
                                 uint8_t *resp, uint16_t *rlen)
{
    if (len < 8) return T5_STATUS_INVALID_LEN;

    /* Parse OPi heartbeat fields (reserved for future use) */
    /* uint32_t opi_uptime = READ_U32_LE(p); */
    /* uint32_t state_rev  = READ_U32_LE(p + 4); */
    (void)p;

    /* Update heartbeat timestamp (handled externally by uart_handler, but
       we also keep it in app_state for completeness). */
    const app_state_t *st = app_state_get();

    WRITE_U32_LE(resp + 0,  tal_system_get_millisecond());  /* t5 uptime   */
    WRITE_U32_LE(resp + 4,  st->revision);                  /* applied rev */
    WRITE_U32_LE(resp + 8,  0);                             /* error flags */
    *rlen = 12;

    return T5_STATUS_OK;
}

/* =========================================================================
 * STATE_SYNC_BEGIN   (OPi → T5)
 *   revision      u32 LE
 *   sync_reason   u8
 * ======================================================================= */
static uint16_t handle_state_sync_begin(const uint8_t *p, uint16_t len,
                                        uint8_t *resp, uint16_t *rlen)
{
    if (len < 5) return T5_STATUS_INVALID_LEN;

    uint32_t revision = READ_U32_LE(p);
    /* uint8_t  reason   = p[4]; */

    /* Enter sync mode — bypass revision guard for all subsequent setters */
    app_state_set_in_sync(true, revision);

    (void)resp;
    *rlen = 0;
    return T5_STATUS_OK;
}

/* =========================================================================
 * STATE_SYNC_END   (OPi → T5)
 *   revision        u32 LE
 *   snapshot_crc32  u32 LE
 * ======================================================================= */
static uint16_t handle_state_sync_end(const uint8_t *p, uint16_t len,
                                       uint8_t *resp, uint16_t *rlen)
{
    if (len < 8) return T5_STATUS_INVALID_LEN;

    uint32_t revision = READ_U32_LE(p);
    /* uint32_t snap_crc = READ_U32_LE(p + 4); */

    /* Leave sync mode — commit target revision and trigger UI refresh */
    app_state_set_in_sync(false, revision);

    (void)resp;
    *rlen = 0;
    return T5_STATUS_OK;
}

/* =========================================================================
 * MODE_SET   (OPi → T5)
 *   revision       u32 LE
 *   mode           u8
 *   reason         u8
 *   changed_at_ms  u64 LE
 * ======================================================================= */
static uint16_t handle_mode_set(const uint8_t *p, uint16_t len,
                                uint8_t *resp, uint16_t *rlen)
{
    if (len < 14) return T5_STATUS_INVALID_LEN;

    uint32_t rev  = READ_U32_LE(p);
    uint8_t  mode = p[4];
    /* uint8_t  reason = p[5]; */
    /* uint64_t changed_at = READ_U64_LE(p + 6); */

    if (mode > T5_MODE_NIGHT_EXEC) {
        return T5_STATUS_INVALID_ARG;
    }
    if (!app_state_check_revision(rev)) {
        return T5_STATUS_STATE_CONFLICT;
    }

    app_state_set_mode(rev, mode);
    (void)resp;
    *rlen = 0;
    return T5_STATUS_OK;
}

/* =========================================================================
 * ATTENTION_SET   (OPi → T5)
 *   revision            u32 LE
 *   attention_flags     u32 LE
 *   confirmation_count  u16 LE
 *   short_message       string (u16 len + UTF-8)
 * ======================================================================= */
static uint16_t handle_attention_set(const uint8_t *p, uint16_t len,
                                     uint8_t *resp, uint16_t *rlen)
{
    if (len < 10) return T5_STATUS_INVALID_LEN;

    uint32_t rev   = READ_U32_LE(p);
    uint32_t flags = READ_U32_LE(p + 4);
    uint16_t count = READ_U16_LE(p + 8);
    /* string short_msg at p+10 — consumed but not stored in MVP */

    if (!app_state_check_revision(rev)) {
        return T5_STATUS_STATE_CONFLICT;
    }

    app_state_set_attention(rev, flags, count);
    (void)resp;
    *rlen = 0;
    return T5_STATUS_OK;
}

/* =========================================================================
 * WORK_STATE_SET   (OPi → T5)
 *
 * NOTE: No revision prefix per implementation spec.
 *
 *   work_state          u8
 *   progress_permille   u16 LE
 *   reserved            u16 LE
 *   token_input         u32 LE
 *   token_output        u32 LE
 *   elapsed_seconds     u32 LE
 *   current_task_id     u32 LE
 *   current_task_title  string (u16 len + UTF-8)
 *
 * Min: 1+2+2+4+4+4+4+2 = 23 bytes (empty string)
 * ======================================================================= */
static uint16_t handle_work_state_set(const uint8_t *p, uint16_t len,
                                      uint8_t *resp, uint16_t *rlen)
{
    if (len < 23) return T5_STATUS_INVALID_LEN;

    uint8_t  ws       = p[0];
    uint16_t progress = READ_U16_LE(p + 1);
    /* uint16_t reserved = READ_U16_LE(p + 3); */
    /* uint32_t tok_in   = READ_U32_LE(p + 5);  */
    /* uint32_t tok_out  = READ_U32_LE(p + 9);  */
    /* uint32_t elapsed  = READ_U32_LE(p + 13); */
    uint32_t task_id  = READ_U32_LE(p + 17);

    char title[128];
    uint16_t consumed = read_string(p + 21, (uint16_t)(len - 21),
                                   title, sizeof(title));
    if (consumed == 0) {
        title[0] = '\0';   /* graceful fallback */
    }

    if (ws > T5_WORK_FAILED) {
        return T5_STATUS_INVALID_ARG;
    }

    app_state_set_work_state(ws, progress, task_id, title);
    (void)resp;
    *rlen = 0;
    return T5_STATUS_OK;
}

/* =========================================================================
 * DASHBOARD_SET   (OPi → T5)
 *   revision         u32 LE
 *   urgent_auto      u16 LE
 *   normal_auto      u16 LE
 *   urgent_confirm   u16 LE
 *   normal_confirm   u16 LE
 *   completed_today  u16 LE
 *   failed_today     u16 LE
 * ======================================================================= */
static uint16_t handle_dashboard_set(const uint8_t *p, uint16_t len,
                                     uint8_t *resp, uint16_t *rlen)
{
    if (len < 16) return T5_STATUS_INVALID_LEN;

    uint32_t rev   = READ_U32_LE(p);
    uint16_t ua    = READ_U16_LE(p + 4);
    uint16_t na    = READ_U16_LE(p + 6);
    uint16_t uc    = READ_U16_LE(p + 8);
    uint16_t nc    = READ_U16_LE(p + 10);
    uint16_t comp  = READ_U16_LE(p + 12);
    uint16_t fail  = READ_U16_LE(p + 14);

    if (!app_state_check_revision(rev)) {
        return T5_STATUS_STATE_CONFLICT;
    }

    app_state_set_dashboard(rev, ua, na, uc, nc, comp, fail);
    (void)resp;
    *rlen = 0;
    return T5_STATUS_OK;
}
