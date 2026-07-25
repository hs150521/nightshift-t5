/**
 * @file link_session.c
 * @brief Production T5/OPI boot-session and HELLO state machine.
 */

#include "link_session.h"

#include "nightshift_config.h"

#include <string.h>

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t hello_retry_delay_ms(uint8_t retry_count)
{
    uint32_t delay = NIGHTSHIFT_HELLO_ACK_TIMEOUT_MS;
    if (retry_count < 3U) delay <<= retry_count;
    return delay;
}

uint32_t link_session_nonzero_boot_id(uint32_t random_sample)
{
    /* Preserve all random bits except forcing the identity away from zero. */
    return random_sample | 1U;
}

uint16_t link_session_parse_opi_hello(const uint8_t *payload,
                                      uint16_t payload_len,
                                      link_hello_info_t *info)
{
    if (!payload || !info || payload_len < 13U) {
        return T5_STATUS_INVALID_LEN;
    }

    uint16_t version_length = READ_U16_LE(payload + 11);
    if ((uint32_t)version_length + 13U != payload_len) {
        return T5_STATUS_INVALID_LEN;
    }
    if (payload[0] != T5_PEER_ROLE_OPI ||
        payload[2] != T5_PROTO_MINOR ||
        READ_U32_LE(payload + 3) == 0U ||
        READ_U16_LE(payload + 7) != NIGHTSHIFT_MAX_PAYLOAD) {
        return T5_STATUS_INVALID_ARG;
    }
    if (payload[1] != T5_PROTO_VERSION) {
        return T5_STATUS_UNSUPPORTED_VER;
    }

    info->peer_role = payload[0];
    info->protocol_major = payload[1];
    info->protocol_minor = payload[2];
    info->boot_id = READ_U32_LE(payload + 3);
    info->max_payload = READ_U16_LE(payload + 7);
    info->capabilities = READ_U16_LE(payload + 9);
    info->software_version = payload + 13;
    info->software_version_length = version_length;
    return T5_STATUS_OK;
}

int link_session_init(link_session_t *session, uint32_t t5_boot_id,
                      uint16_t sequence)
{
    static const char software_version[] = NIGHTSHIFT_FW_VERSION;
    const uint16_t version_length =
        (uint16_t)(sizeof(software_version) - 1U);

    if (!session || t5_boot_id == 0U || sequence == 0U ||
        (uint32_t)version_length + 13U > NIGHTSHIFT_MAX_PAYLOAD) {
        return -1;
    }

    memset(session, 0, sizeof(*session));
    session->t5_boot_id = t5_boot_id;
    session->panel_hello.flags = T5_FLAG_ACK_REQ;
    session->panel_hello.seq = sequence;
    session->panel_hello.cmd = T5_CMD_HELLO;
    session->panel_hello.payload[0] = T5_PEER_ROLE_PANEL;
    session->panel_hello.payload[1] = T5_PROTO_VERSION;
    session->panel_hello.payload[2] = T5_PROTO_MINOR;
    WRITE_U32_LE(session->panel_hello.payload + 3, t5_boot_id);
    WRITE_U16_LE(session->panel_hello.payload + 7,
                 NIGHTSHIFT_MAX_PAYLOAD);
    WRITE_U16_LE(session->panel_hello.payload + 9,
                 (uint16_t)NIGHTSHIFT_CAPABILITIES);
    WRITE_U16_LE(session->panel_hello.payload + 11, version_length);
    memcpy(session->panel_hello.payload + 13, software_version,
           version_length);
    session->panel_hello.payload_len =
        (uint16_t)(13U + version_length);
    return 0;
}

bool link_session_accept_opi_hello(link_session_t *session,
                                   const link_hello_info_t *info,
                                   request_cache_t *request_cache)
{
    if (!session || !info || !request_cache || info->boot_id == 0U) {
        return false;
    }
    if (session->opi_boot_known &&
        session->opi_boot_id == info->boot_id) {
        return false;
    }

    request_cache_init(request_cache);
    session->opi_boot_id = info->boot_id;
    session->opi_boot_known = true;
    return true;
}

void link_session_start_panel_hello(link_session_t *session,
                                    uint32_t now_ms)
{
    if (!session) return;
    session->panel_hello_pending = true;
    session->panel_hello_retries = 0;
    session->panel_hello_sent_ms = now_ms;
    session->panel_hello_next_ms = 0;
}

bool link_session_handle_panel_hello_response(link_session_t *session,
                                              const t5_frame_t *frame,
                                              uint16_t *status)
{
    if (!session || !frame || !session->panel_hello_pending ||
        frame->flags != T5_FLAG_RESPONSE ||
        frame->seq != session->panel_hello.seq ||
        frame->cmd != T5_CMD_HELLO ||
        frame->payload_len != 2U) {
        return false;
    }

    if (status) *status = READ_U16_LE(frame->payload);
    session->panel_hello_pending = false;
    session->panel_hello_next_ms =
        session->panel_hello_sent_ms + NIGHTSHIFT_HELLO_OFFLINE_RETRY_MS;
    return true;
}

bool link_session_poll(link_session_t *session, uint32_t now_ms,
                       bool opi_online, t5_frame_t *out_frame)
{
    if (!session || !out_frame) return false;

    if (session->panel_hello_pending) {
        const uint32_t delay =
            hello_retry_delay_ms(session->panel_hello_retries);
        if (!deadline_reached(
                now_ms, session->panel_hello_sent_ms + delay)) {
            return false;
        }
        if (session->panel_hello_retries <
            NIGHTSHIFT_HELLO_MAX_RETRIES) {
            session->panel_hello_retries++;
            session->panel_hello_sent_ms = now_ms;
            *out_frame = session->panel_hello;
            return true;
        }
        session->panel_hello_pending = false;
        session->panel_hello_next_ms =
            now_ms + NIGHTSHIFT_HELLO_OFFLINE_RETRY_MS;
        return false;
    }

    if (!opi_online &&
        deadline_reached(now_ms, session->panel_hello_next_ms)) {
        link_session_start_panel_hello(session, now_ms);
        *out_frame = session->panel_hello;
        return true;
    }
    return false;
}

void link_session_note_offline(link_session_t *session, uint32_t now_ms)
{
    if (!session || session->panel_hello_pending) return;
    session->panel_hello_next_ms = now_ms;
}
