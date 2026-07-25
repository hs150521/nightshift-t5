/**
 * @file link_session.h
 * @brief Boot-session identity and canonical HELLO retry state.
 *
 * This module deliberately has no TAL/LVGL dependency so the production
 * session logic can also be compiled and executed by the host test harness.
 */

#ifndef LINK_SESSION_H
#define LINK_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "request_cache.h"
#include "t5_protocol.h"

typedef struct {
    uint8_t peer_role;
    uint8_t protocol_major;
    uint8_t protocol_minor;
    uint32_t boot_id;
    uint16_t max_payload;
    uint16_t capabilities;
    const uint8_t *software_version;
    uint16_t software_version_length;
} link_hello_info_t;

typedef struct {
    uint32_t t5_boot_id;
    uint32_t opi_boot_id;
    bool opi_boot_known;

    t5_frame_t panel_hello;
    bool panel_hello_pending;
    uint8_t panel_hello_retries;
    uint32_t panel_hello_sent_ms;
    uint32_t panel_hello_next_ms;
} link_session_t;

/** Convert one platform-random sample into a nonzero boot identity. */
uint32_t link_session_nonzero_boot_id(uint32_t random_sample);

/** Strictly decode and validate one canonical OPI HELLO payload. */
uint16_t link_session_parse_opi_hello(const uint8_t *payload,
                                      uint16_t payload_len,
                                      link_hello_info_t *info);

/** Build the immutable panel HELLO frame for this T5 boot. */
int link_session_init(link_session_t *session, uint32_t t5_boot_id,
                      uint16_t sequence);

/**
 * Record a valid OPI session.
 *
 * A first/different OPI boot ID clears the production request cache and
 * returns true. The same boot ID preserves the cache and returns false.
 */
bool link_session_accept_opi_hello(link_session_t *session,
                                   const link_hello_info_t *info,
                                   request_cache_t *request_cache);

/** Start an immediate HELLO attempt using the immutable frame. */
void link_session_start_panel_hello(link_session_t *session,
                                    uint32_t now_ms);

/**
 * Match a HELLO response. Returns true only for the current HELLO request.
 * The status prefix is returned through status when non-NULL.
 */
bool link_session_handle_panel_hello_response(link_session_t *session,
                                              const t5_frame_t *frame,
                                              uint16_t *status);

/**
 * Advance bounded HELLO retry/offline discovery state.
 *
 * When true is returned, out_frame is an exact copy of the immutable HELLO
 * frame and must be transmitted once by the caller.
 */
bool link_session_poll(link_session_t *session, uint32_t now_ms,
                       bool opi_online, t5_frame_t *out_frame);

/** Make the next offline poll eligible to rediscover OPI promptly. */
void link_session_note_offline(link_session_t *session, uint32_t now_ms);

#endif /* LINK_SESSION_H */
