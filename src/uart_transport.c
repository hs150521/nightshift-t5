/**
 * @file uart_transport.c
 * @brief Clean binary UART0 transport, deduplication, and watchdog.
 */

#include "uart_transport.h"

#include "command_router.h"
#include "frame_stream.h"
#include "link_session.h"
#include "nightshift_config.h"
#include "request_cache.h"
#include "state_store.h"

#include "tal_api.h"
#include "tkl_pinmux.h"

#include <string.h>

typedef struct {
    bool active;
    uint16_t action;
    uint32_t sent_ms;
    uint8_t retries;
    t5_frame_t frame;
} pending_event_t;

static request_cache_t g_request_cache;
static frame_stream_t g_stream;
static link_session_t g_session;
static pending_event_t g_pending_event;
static MUTEX_HANDLE g_tx_mutex;
static MUTEX_HANDLE g_link_mutex;
static THREAD_HANDLE g_rx_thread;
static THREAD_HANDLE g_watchdog_thread;
static uint32_t g_last_heartbeat_ms;
static uint32_t g_hello_grace_started_ms;
static bool g_heartbeat_watchdog_active;
static bool g_waiting_first_heartbeat;
static uint16_t g_out_sequence;
static bool g_initialized;

static uint16_t next_sequence_locked(void)
{
    g_out_sequence++;
    if (g_out_sequence == 0) g_out_sequence = 1;
    return g_out_sequence;
}

int uart_transport_send_frame(const t5_frame_t *frame)
{
    uint8_t wire[NIGHTSHIFT_MAX_WIRE_FRAME];
    size_t wire_len = t5_frame_encode(frame, wire, sizeof(wire));
    if (wire_len == 0 || !g_initialized) return -1;

    tal_mutex_lock(g_tx_mutex);
    int written = tal_uart_write(NIGHTSHIFT_UART_PORT, wire,
                                 (uint32_t)wire_len);
    tal_mutex_unlock(g_tx_mutex);
    return (written == (int)wire_len) ? 0 : -2;
}

static int send_response(uint16_t sequence, uint16_t command,
                         uint16_t status, const uint8_t *data,
                         uint16_t data_len)
{
    if (data_len > NIGHTSHIFT_MAX_PAYLOAD - 2) return -1;
    t5_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.flags = T5_FLAG_RESPONSE;
    frame.seq = sequence;
    frame.cmd = command;
    WRITE_U16_LE(frame.payload, status);
    if (data_len != 0 && data) memcpy(frame.payload + 2, data, data_len);
    frame.payload_len = (uint16_t)(data_len + 2);
    return uart_transport_send_frame(&frame);
}

static void mark_valid_liveness(uint16_t command)
{
    uint32_t now = tal_system_get_millisecond();
    bool mark_online = false;
    tal_mutex_lock(g_link_mutex);
    if (command == T5_CMD_HEARTBEAT) {
        g_last_heartbeat_ms = now;
        g_heartbeat_watchdog_active = true;
        g_waiting_first_heartbeat = false;
        mark_online = true;
    } else if (command == T5_CMD_HELLO &&
               !g_heartbeat_watchdog_active) {
        /*
         * Start one grace window for the first heartbeat. Repeated HELLO
         * requests during the same window cannot extend the deadline.
         */
        g_hello_grace_started_ms = now;
        g_heartbeat_watchdog_active = true;
        g_waiting_first_heartbeat = true;
        mark_online = true;
    }
    tal_mutex_unlock(g_link_mutex);
    if (mark_online) state_store_set_online(true);
}

static void handle_response(const t5_frame_t *frame)
{
    uint16_t status;
    uint16_t action = 0;
    bool matched_action = false;
    bool matched_hello = false;
    if (frame->payload_len < 2U) return;
    status = READ_U16_LE(frame->payload);

    tal_mutex_lock(g_link_mutex);
    matched_hello = link_session_handle_panel_hello_response(
        &g_session, frame, &status);
    if (!matched_hello && g_pending_event.active &&
        frame->seq == g_pending_event.frame.seq &&
        frame->cmd == g_pending_event.frame.cmd) {
        action = g_pending_event.action;
        memset(&g_pending_event, 0, sizeof(g_pending_event));
        matched_action = true;
    }
    tal_mutex_unlock(g_link_mutex);
    if (matched_action) state_store_set_action(action, false, status);
}

static uint16_t process_cached_request(const t5_frame_t *frame)
{
    const request_cache_entry_t *cached =
        request_cache_find(&g_request_cache, frame->seq, frame->cmd);
    if (cached) {
        if (cached->status == T5_STATUS_OK) {
            mark_valid_liveness(frame->cmd);
        }
        if (frame->flags & T5_FLAG_ACK_REQ) {
            send_response(frame->seq, frame->cmd, cached->status,
                          cached->data, cached->data_len);
        }
        return cached->status;
    }

    uint8_t response[COMMAND_RESPONSE_MAX];
    uint16_t response_len = 0;
    uint16_t status = command_router_dispatch(
        frame->cmd, frame->payload, frame->payload_len,
        response, &response_len);

    if (status == T5_STATUS_OK) {
        mark_valid_liveness(frame->cmd);
    }
    request_cache_store(&g_request_cache, frame->seq, frame->cmd,
                        status, response, response_len);
    if (frame->flags & T5_FLAG_ACK_REQ) {
        send_response(frame->seq, frame->cmd, status,
                      response, response_len);
    }
    return status;
}

static void process_hello_request(const t5_frame_t *frame)
{
    link_hello_info_t info;
    uint16_t validation = link_session_parse_opi_hello(
        frame->payload, frame->payload_len, &info);
    if (validation != T5_STATUS_OK) {
        if (frame->flags & T5_FLAG_ACK_REQ) {
            send_response(frame->seq, frame->cmd, validation, NULL, 0);
        }
        return;
    }

    uint32_t now = tal_system_get_millisecond();
    bool new_opi_session;
    bool cancelled_action = false;
    uint16_t cancelled_action_id = 0;

    /*
     * HELLO is decoded before any cache lookup. A new OPI boot ID therefore
     * cannot hit an old cached HELLO response or any old request result.
     */
    tal_mutex_lock(g_link_mutex);
    new_opi_session = link_session_accept_opi_hello(
        &g_session, &info, &g_request_cache);
    if (new_opi_session) {
        if (g_pending_event.active) {
            cancelled_action = true;
            cancelled_action_id = g_pending_event.action;
            memset(&g_pending_event, 0, sizeof(g_pending_event));
        }
        g_last_heartbeat_ms = 0;
        g_hello_grace_started_ms = 0;
        g_heartbeat_watchdog_active = false;
        g_waiting_first_heartbeat = false;
        link_session_start_panel_hello(&g_session, now);
    }
    tal_mutex_unlock(g_link_mutex);

    if (new_opi_session) {
        state_store_sync_abort();
        if (cancelled_action) {
            state_store_set_action(cancelled_action_id, false,
                                   T5_STATUS_NOT_READY);
        }
    }

    uint16_t status = process_cached_request(frame);
    if (new_opi_session && status == T5_STATUS_OK) {
        /* ACK the OPI HELLO first, then announce the unchanged T5 boot ID. */
        (void)uart_transport_send_frame(&g_session.panel_hello);
    }
}

static void process_request(const t5_frame_t *frame)
{
    if (frame->cmd == T5_CMD_HELLO) {
        process_hello_request(frame);
        return;
    }
    (void)process_cached_request(frame);
}

static void process_frame(const t5_frame_t *frame)
{
    if (!frame || frame->seq == 0) return;
    if (frame->flags & T5_FLAG_RESPONSE) {
        handle_response(frame);
        return;
    }
    /* Orange Pi commands are requests, never peer events. */
    if (frame->flags & T5_FLAG_EVENT) return;
    process_request(frame);
}

static void uart_rx_task(void *arg)
{
    uint8_t bytes[256];
    (void)arg;
    for (;;) {
        int count = tal_uart_read(NIGHTSHIFT_UART_PORT, bytes,
                                  (uint32_t)sizeof(bytes));
        if (count <= 0) {
            tal_system_sleep(NIGHTSHIFT_RX_TIMEOUT_MS);
            continue;
        }
        for (int i = 0; i < count; ++i) {
            frame_stream_result_t stream_result =
                frame_stream_push(&g_stream, bytes[i]);
            if (stream_result == FRAME_STREAM_READY) {
                t5_frame_t frame;
                if (t5_frame_decode(frame_stream_data(&g_stream),
                                    frame_stream_length(&g_stream),
                                    &frame) == 0) {
                    process_frame(&frame);
                }
                frame_stream_consume(&g_stream);
            }
        }
    }
}

static void watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now = tal_system_get_millisecond();
        uint32_t heartbeat_reference = 0;
        bool watchdog_active;
        bool host_online;
        bool retry_action = false;
        t5_frame_t retry_frame;
        bool action_expired = false;
        uint16_t expired_action = 0;
        bool send_panel_hello = false;

        host_online = state_store_opi_online();

        tal_mutex_lock(g_link_mutex);
        watchdog_active = g_heartbeat_watchdog_active;
        if (watchdog_active) {
            heartbeat_reference = g_waiting_first_heartbeat
                                      ? g_hello_grace_started_ms
                                      : g_last_heartbeat_ms;
        }
        if (g_pending_event.active &&
            (uint32_t)(now - g_pending_event.sent_ms) >=
                NIGHTSHIFT_ACTION_ACK_TIMEOUT_MS) {
            if (g_pending_event.retries <
                NIGHTSHIFT_ACTION_MAX_RETRIES) {
                g_pending_event.retries++;
                g_pending_event.sent_ms = now;
                retry_frame = g_pending_event.frame;
                retry_action = true;
            } else {
                expired_action = g_pending_event.action;
                memset(&g_pending_event, 0, sizeof(g_pending_event));
                action_expired = true;
            }
        }
        if (watchdog_active && heartbeat_reference != 0 &&
            (uint32_t)(now - heartbeat_reference) >=
                NIGHTSHIFT_HEARTBEAT_TIMEOUT_MS) {
            g_heartbeat_watchdog_active = false;
            g_waiting_first_heartbeat = false;
            link_session_note_offline(&g_session, now);
        }
        send_panel_hello = link_session_poll(
            &g_session, now, host_online, &g_session.panel_hello);
        tal_mutex_unlock(g_link_mutex);

        if (watchdog_active && heartbeat_reference != 0 &&
            (uint32_t)(now - heartbeat_reference) >=
                NIGHTSHIFT_HEARTBEAT_TIMEOUT_MS) {
            state_store_set_online(false);
            state_store_sync_abort();
        }
        if (retry_action) {
            (void)uart_transport_send_frame(&retry_frame);
        }
        if (send_panel_hello) {
            /* Immutable for this boot; no watchdog-stack frame copy needed. */
            (void)uart_transport_send_frame(&g_session.panel_hello);
        }
        if (action_expired) {
            state_store_set_action(expired_action, false,
                                   T5_STATUS_NOT_READY);
        }
        state_store_sync_abort_if_expired(now);
        tal_system_sleep(NIGHTSHIFT_WATCHDOG_POLL_MS);
    }
}

int uart_transport_send_ui_action(uint16_t action, uint8_t object_type,
                                  uint32_t object_id, int32_t value,
                                  const char *text)
{
    display_state_t state;
    uint8_t text_length = 0;
    if (!text) text = "";
    size_t source_length = strlen(text);
    if (source_length > 96) source_length = 96;
    text_length = (uint8_t)source_length;

    state_store_snapshot(&state);
    if (!state.opi_online || state.action_pending) return -1;
    if ((action == T5_ACTION_CONFIRM ||
         action == T5_ACTION_REJECT ||
         action == T5_ACTION_RETRY) &&
        (object_type != T5_OBJECT_TASK || object_id == 0)) {
        return -3;
    }
    if ((action == T5_ACTION_PAUSE_EXECUTION ||
         action == T5_ACTION_RESUME_EXECUTION) &&
        object_type != T5_OBJECT_EXECUTOR) {
        return -3;
    }

    t5_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.flags = T5_FLAG_EVENT | T5_FLAG_ACK_REQ;
    frame.cmd = T5_CMD_UI_ACTION;
    WRITE_U16_LE(frame.payload, action);
    frame.payload[2] = object_type;
    WRITE_U32_LE(frame.payload + 3, object_id);
    WRITE_U32_LE(frame.payload + 7, (uint32_t)value);
    WRITE_U16_LE(frame.payload + 11, text_length);
    if (text_length != 0) memcpy(frame.payload + 13, text, text_length);
    frame.payload_len = (uint16_t)(13 + text_length);

    tal_mutex_lock(g_link_mutex);
    if (g_pending_event.active) {
        tal_mutex_unlock(g_link_mutex);
        return -2;
    }
    frame.seq = next_sequence_locked();
    g_pending_event.active = true;
    g_pending_event.action = action;
    g_pending_event.sent_ms = tal_system_get_millisecond();
    g_pending_event.retries = 0;
    g_pending_event.frame = frame;
    tal_mutex_unlock(g_link_mutex);

    state_store_set_action(action, true, T5_STATUS_ACCEPTED);
    int result = uart_transport_send_frame(&frame);
    if (result != 0) {
        tal_mutex_lock(g_link_mutex);
        memset(&g_pending_event, 0, sizeof(g_pending_event));
        tal_mutex_unlock(g_link_mutex);
        state_store_set_action(action, false, T5_STATUS_INTERNAL_ERR);
    }
    return result;
}

int uart_transport_send_page_event(uint8_t page_id, uint8_t event,
                                   uint32_t object_id)
{
    display_state_t state;
    state_store_snapshot(&state);
    if (!state.opi_online) return -1;

    t5_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.flags = T5_FLAG_EVENT;
    frame.cmd = T5_CMD_PAGE_EVENT;
    frame.payload[0] = page_id;
    frame.payload[1] = event;
    WRITE_U32_LE(frame.payload + 2, object_id);
    frame.payload_len = 6;
    tal_mutex_lock(g_link_mutex);
    frame.seq = next_sequence_locked();
    tal_mutex_unlock(g_link_mutex);
    return uart_transport_send_frame(&frame);
}

bool uart_transport_host_online(void)
{
    return state_store_opi_online();
}

uint32_t uart_transport_last_heartbeat_ms(void)
{
    uint32_t timestamp;
    tal_mutex_lock(g_link_mutex);
    timestamp = g_last_heartbeat_ms;
    tal_mutex_unlock(g_link_mutex);
    return timestamp;
}

int uart_transport_init(void)
{
    if (g_initialized) return 0;
    if (NIGHTSHIFT_UART_PORT == TUYA_UART_NUM_0) {
        tkl_io_pinmux_config(TUYA_IO_PIN_10, TUYA_UART0_RX);
        tkl_io_pinmux_config(TUYA_IO_PIN_11, TUYA_UART0_TX);
    }

    TAL_UART_CFG_T config;
    memset(&config, 0, sizeof(config));
    config.base_cfg.baudrate = NIGHTSHIFT_UART_BAUDRATE;
    config.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    config.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    config.base_cfg.parity = TUYA_UART_PARITY_TYPE_NONE;
    config.rx_buffer_size = NIGHTSHIFT_UART_RX_BUF_SIZE;
    config.open_mode = 0;
    if (tal_uart_init(NIGHTSHIFT_UART_PORT, &config) != OPRT_OK) return -1;

    tal_mutex_create_init(&g_tx_mutex);
    tal_mutex_create_init(&g_link_mutex);
    request_cache_init(&g_request_cache);
    frame_stream_init(&g_stream);
    memset(&g_pending_event, 0, sizeof(g_pending_event));
    g_last_heartbeat_ms = 0;
    g_hello_grace_started_ms = 0;
    g_heartbeat_watchdog_active = false;
    g_waiting_first_heartbeat = false;
    g_out_sequence = 0;
    uint32_t random_sample =
        (uint32_t)tal_system_get_random(0xFFFFFFFFU);
    uint32_t t5_boot_id =
        link_session_nonzero_boot_id(random_sample);
    if (link_session_init(&g_session, t5_boot_id,
                          next_sequence_locked()) != 0) {
        return -4;
    }
    link_session_start_panel_hello(
        &g_session, tal_system_get_millisecond());
    g_initialized = true;

    THREAD_CFG_T rx_config;
    memset(&rx_config, 0, sizeof(rx_config));
    rx_config.stackDepth = NIGHTSHIFT_RX_TASK_STACK;
    rx_config.priority = NIGHTSHIFT_RX_TASK_PRIO;
    rx_config.thrdname = "ns_uart_rx";
    if (tal_thread_create_and_start(&g_rx_thread, NULL, NULL,
                                    uart_rx_task, NULL,
                                    &rx_config) != OPRT_OK) {
        return -2;
    }

    THREAD_CFG_T watchdog_config;
    memset(&watchdog_config, 0, sizeof(watchdog_config));
    watchdog_config.stackDepth = NIGHTSHIFT_WATCHDOG_STACK;
    watchdog_config.priority = NIGHTSHIFT_WATCHDOG_PRIO;
    watchdog_config.thrdname = "ns_watchdog";
    if (tal_thread_create_and_start(&g_watchdog_thread, NULL, NULL,
                                    watchdog_task, NULL,
                                    &watchdog_config) != OPRT_OK) {
        return -3;
    }

    /* Startup discovery is asynchronous; a missing ACK never blocks boot. */
    (void)uart_transport_send_frame(&g_session.panel_hello);
    return 0;
}
