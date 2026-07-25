#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "link_session.h"
#include "nightshift_config.h"
#include "request_cache.h"
#include "t5_protocol.h"

static uint16_t make_opi_hello(uint8_t *payload, uint32_t boot_id,
                               const char *version)
{
    size_t version_length = strlen(version);
    assert(version_length <= 64U);
    payload[0] = T5_PEER_ROLE_OPI;
    payload[1] = T5_PROTO_VERSION;
    payload[2] = T5_PROTO_MINOR;
    WRITE_U32_LE(payload + 3, boot_id);
    WRITE_U16_LE(payload + 7, NIGHTSHIFT_MAX_PAYLOAD);
    WRITE_U16_LE(payload + 9, 0);
    WRITE_U16_LE(payload + 11, (uint16_t)version_length);
    memcpy(payload + 13, version, version_length);
    return (uint16_t)(13U + version_length);
}

static size_t encode_frame(const t5_frame_t *frame, uint8_t *wire)
{
    size_t length = t5_frame_encode(
        frame, wire, NIGHTSHIFT_MAX_WIRE_FRAME);
    assert(length != 0U);
    return length;
}

static void test_startup_hello_and_exact_retry(void)
{
    link_session_t session;
    const uint32_t boot_id = 0xA1B2C3D5U;
    assert(link_session_init(&session, boot_id, 17) == 0);
    assert(session.panel_hello.flags == T5_FLAG_ACK_REQ);
    assert(session.panel_hello.seq == 17);
    assert(session.panel_hello.cmd == T5_CMD_HELLO);
    assert(session.panel_hello.payload[0] == T5_PEER_ROLE_PANEL);
    assert(session.panel_hello.payload[1] == 1);
    assert(session.panel_hello.payload[2] == 0);
    assert(READ_U32_LE(session.panel_hello.payload + 3) == boot_id);
    assert(READ_U16_LE(session.panel_hello.payload + 7) == 1024);
    assert(READ_U16_LE(session.panel_hello.payload + 9) ==
           (uint16_t)NIGHTSHIFT_CAPABILITIES);

    link_session_start_panel_hello(&session, 0);
    uint8_t original[NIGHTSHIFT_MAX_WIRE_FRAME];
    size_t original_length =
        encode_frame(&session.panel_hello, original);
    t5_frame_t retry;
    uint8_t retry_wire[NIGHTSHIFT_MAX_WIRE_FRAME];

    assert(!link_session_poll(&session, 999, false, &retry));
    assert(link_session_poll(&session, 1000, false, &retry));
    size_t retry_length = encode_frame(&retry, retry_wire);
    assert(retry_length == original_length);
    assert(memcmp(retry_wire, original, original_length) == 0);
    assert(retry.seq == 17);

    assert(!link_session_poll(&session, 2999, false, &retry));
    assert(link_session_poll(&session, 3000, false, &retry));
    retry_length = encode_frame(&retry, retry_wire);
    assert(retry_length == original_length);
    assert(memcmp(retry_wire, original, original_length) == 0);

    /* The bounded batch expires after the 4-second final ACK window. */
    assert(!link_session_poll(&session, 7000, false, &retry));
    assert(!session.panel_hello_pending);

    /*
     * OPI can start after T5. The low-rate offline probe sends the exact
     * boot HELLO again without allocating a new sequence.
     */
    assert(!link_session_poll(&session, 16999, false, &retry));
    assert(link_session_poll(&session, 17000, false, &retry));
    retry_length = encode_frame(&retry, retry_wire);
    assert(retry_length == original_length);
    assert(memcmp(retry_wire, original, original_length) == 0);

    t5_frame_t wrong_ack = {0};
    wrong_ack.flags = T5_FLAG_RESPONSE;
    wrong_ack.seq = 18;
    wrong_ack.cmd = T5_CMD_HELLO;
    wrong_ack.payload_len = 2;
    assert(!link_session_handle_panel_hello_response(
        &session, &wrong_ack, NULL));
    t5_frame_t ack = wrong_ack;
    ack.seq = 17;
    uint16_t ack_status = 0xFFFF;
    assert(link_session_handle_panel_hello_response(
        &session, &ack, &ack_status));
    assert(ack_status == T5_STATUS_OK);
    assert(!session.panel_hello_pending);
}

static void test_session_cache_transition_and_sequence_reuse(void)
{
    link_session_t session;
    request_cache_t cache;
    link_hello_info_t hello;
    uint8_t payload[96];
    uint8_t old_data[] = {0x11, 0x22};
    uint8_t new_data[] = {0x33, 0x44};

    request_cache_init(&cache);
    assert(link_session_init(&session, 0x12345679U, 3) == 0);

    uint16_t length = make_opi_hello(payload, 0x11111111U, "opi/a");
    assert(link_session_parse_opi_hello(payload, length, &hello) ==
           T5_STATUS_OK);
    assert(link_session_accept_opi_hello(&session, &hello, &cache));

    request_cache_store(&cache, 91, T5_CMD_MODE_SET,
                        T5_STATUS_OK, old_data, sizeof(old_data));
    assert(request_cache_find(&cache, 91, T5_CMD_MODE_SET) != NULL);

    /* Same OPI boot session retains duplicate-response replay state. */
    assert(!link_session_accept_opi_hello(&session, &hello, &cache));
    const request_cache_entry_t *same =
        request_cache_find(&cache, 91, T5_CMD_MODE_SET);
    assert(same != NULL);
    assert(same->data[0] == old_data[0]);

    /*
     * A restarted OPI may reuse sequence 91. A new boot ID clears the old
     * entry, so the new command result is executed/stored instead of replayed.
     */
    length = make_opi_hello(payload, 0x22222223U, "opi/b");
    assert(link_session_parse_opi_hello(payload, length, &hello) ==
           T5_STATUS_OK);
    assert(link_session_accept_opi_hello(&session, &hello, &cache));
    assert(request_cache_find(&cache, 91, T5_CMD_MODE_SET) == NULL);
    request_cache_store(&cache, 91, T5_CMD_MODE_SET,
                        T5_STATUS_ACCEPTED, new_data, sizeof(new_data));
    const request_cache_entry_t *fresh =
        request_cache_find(&cache, 91, T5_CMD_MODE_SET);
    assert(fresh != NULL);
    assert(fresh->status == T5_STATUS_ACCEPTED);
    assert(fresh->data[0] == new_data[0]);
}

static void test_malformed_hello_has_no_transition(void)
{
    link_session_t session;
    request_cache_t cache;
    link_hello_info_t hello;
    uint8_t payload[96];
    uint8_t cached[] = {0xAB};

    request_cache_init(&cache);
    assert(link_session_init(&session, 0xABCDEF01U, 5) == 0);
    uint16_t length = make_opi_hello(payload, 0x77777777U, "opi/valid");
    assert(link_session_parse_opi_hello(payload, length, &hello) ==
           T5_STATUS_OK);
    assert(link_session_accept_opi_hello(&session, &hello, &cache));
    request_cache_store(&cache, 8, T5_CMD_DASHBOARD_SET,
                        T5_STATUS_OK, cached, sizeof(cached));

    const uint32_t previous_boot_id = session.opi_boot_id;
    payload[11]++; /* Declared string now extends beyond the payload. */
    assert(link_session_parse_opi_hello(payload, length, &hello) ==
           T5_STATUS_INVALID_LEN);
    assert(session.opi_boot_id == previous_boot_id);
    assert(request_cache_find(&cache, 8, T5_CMD_DASHBOARD_SET) != NULL);

    length = make_opi_hello(payload, 0, "opi/zero");
    assert(link_session_parse_opi_hello(payload, length, &hello) ==
           T5_STATUS_INVALID_ARG);
    assert(session.opi_boot_id == previous_boot_id);
    assert(request_cache_find(&cache, 8, T5_CMD_DASHBOARD_SET) != NULL);
}

static void test_reboot_boot_id_samples(void)
{
    uint32_t first = link_session_nonzero_boot_id(0x10203040U);
    uint32_t second = link_session_nonzero_boot_id(0x50607080U);
    assert(first != 0U);
    assert(second != 0U);
    assert(first != second);
    assert(link_session_nonzero_boot_id(0U) != 0U);
}

int main(void)
{
    test_startup_hello_and_exact_retry();
    test_session_cache_transition_and_sequence_reuse();
    test_malformed_hello_has_no_transition();
    test_reboot_boot_id_samples();
    puts("production link_session C tests: PASS");
    return 0;
}
