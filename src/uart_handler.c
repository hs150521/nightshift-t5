/**
 * @file uart_handler.c
 * @brief UART transport layer: init, RX task, dedup cache, send helpers.
 */

#include "uart_handler.h"
#include "command_handler.h"
#include "app_state.h"
#include "nightshift_config.h"

#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_pinmux.h"

#include <string.h>

/* =========================================================================
 * Dedup cache entry
 * ======================================================================= */
typedef struct {
    uint16_t seq;
    uint16_t cmd;
    uint16_t status;
    uint8_t  resp_data[32];
    uint16_t resp_len;
    bool     valid;
} dedup_entry_t;

/* =========================================================================
 * Module state
 * ======================================================================= */
static dedup_entry_t  g_dedup[NIGHTSHIFT_DEDUP_CACHE_SIZE];
static uint16_t       g_dedup_head;          /* next write index */

static uint32_t       g_last_heartbeat_ms;   /* timestamp of last HEARTBEAT */
static uint16_t       g_out_seq;             /* outgoing event sequence 1-65535 */

static THREAD_HANDLE  g_rx_thread;
static bool           g_initialised;

/* =========================================================================
 * Dedup helpers
 * ======================================================================= */

/**
 * @brief Look up (seq,cmd) in the cache.
 * @return Pointer to entry on hit, NULL on miss.
 */
static dedup_entry_t *dedup_find(uint16_t seq, uint16_t cmd)
{
    for (uint16_t i = 0; i < NIGHTSHIFT_DEDUP_CACHE_SIZE; i++) {
        if (g_dedup[i].valid &&
            g_dedup[i].seq == seq &&
            g_dedup[i].cmd == cmd) {
            return &g_dedup[i];
        }
    }
    return NULL;
}

/**
 * @brief Store a new result in the circular cache.
 */
static void dedup_store(uint16_t seq, uint16_t cmd,
                        uint16_t status,
                        const uint8_t *resp_data, uint16_t resp_len)
{
    dedup_entry_t *e = &g_dedup[g_dedup_head];
    e->seq    = seq;
    e->cmd    = cmd;
    e->status = status;
    e->resp_len = (resp_len > 32) ? 32 : resp_len;
    if (e->resp_len > 0 && resp_data) {
        memcpy(e->resp_data, resp_data, e->resp_len);
    }
    e->valid  = true;

    g_dedup_head = (uint16_t)((g_dedup_head + 1) %
                              NIGHTSHIFT_DEDUP_CACHE_SIZE);
}

/* =========================================================================
 * Outgoing sequence counter
 * ======================================================================= */
static uint16_t next_out_seq(void)
{
    g_out_seq++;
    if (g_out_seq == 0) g_out_seq = 1;   /* 0 is reserved */
    return g_out_seq;
}

/* =========================================================================
 * uart_send_frame  — encode + transmit
 * ======================================================================= */
int uart_send_frame(const t5_frame_t *frame)
{
    uint8_t wire[NIGHTSHIFT_MAX_WIRE_FRAME];

    size_t wire_len = t5_frame_encode(frame, wire, sizeof(wire));
    if (wire_len == 0) return -1;

    int ret = tal_uart_write(NIGHTSHIFT_UART_PORT, wire, (uint32_t)wire_len);
    return (ret >= 0) ? 0 : -2;
}

/* =========================================================================
 * uart_send_response  — build a RESPONSE frame and send it
 * ======================================================================= */
int uart_send_response(uint16_t seq, uint16_t cmd, uint16_t status,
                       const uint8_t *data, uint16_t data_len)
{
    t5_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    frame.flags = T5_FLAG_RESPONSE;
    frame.seq   = seq;
    frame.cmd   = cmd;

    /* Payload = u16LE status + optional extra data */
    WRITE_U16_LE(frame.payload, status);
    frame.payload_len = 2;

    if (data && data_len > 0) {
        uint16_t copy = data_len;
        if (copy > (uint16_t)(NIGHTSHIFT_MAX_PAYLOAD - 2)) {
            copy = (uint16_t)(NIGHTSHIFT_MAX_PAYLOAD - 2);
        }
        memcpy(frame.payload + 2, data, copy);
        frame.payload_len = (uint16_t)(2 + copy);
    } else {
        WRITE_U16_LE(frame.payload + 2, 0);
    }

    return uart_send_frame(&frame);
}

/* =========================================================================
 * uart_send_event  — build an EVENT frame and send it
 * ======================================================================= */
int uart_send_event(uint16_t cmd,
                    const uint8_t *payload, uint16_t payload_len)
{
    t5_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    frame.flags = T5_FLAG_EVENT;
    frame.seq   = next_out_seq();
    frame.cmd   = cmd;

    if (payload && payload_len > 0) {
        uint16_t copy = payload_len;
        if (copy > NIGHTSHIFT_MAX_PAYLOAD) copy = NIGHTSHIFT_MAX_PAYLOAD;
        memcpy(frame.payload, payload, copy);
        frame.payload_len = copy;
    }

    return uart_send_frame(&frame);
}

/* =========================================================================
 * Heartbeat / online status
 * ======================================================================= */
bool uart_is_opi_online(void)
{
    uint32_t now = tal_system_get_millisecond();
    return (now - g_last_heartbeat_ms) < NIGHTSHIFT_HEARTBEAT_TIMEOUT_MS;
}

uint32_t uart_get_last_heartbeat_ms(void)
{
    return g_last_heartbeat_ms;
}

/* =========================================================================
 * Process a decoded inbound frame
 * ======================================================================= */
static void process_frame(const t5_frame_t *frame)
{
    /* ---- HEARTBEAT bookkeeping (always update timestamp) ---- */
    if (frame->cmd == T5_CMD_HEARTBEAT) {
        g_last_heartbeat_ms = tal_system_get_millisecond();
        app_state_set_opi_online(true);
    }

    /* ---- Dedup check (only for ACK_REQ requests) ---- */
    if (frame->flags & T5_FLAG_ACK_REQ) {
        dedup_entry_t *hit = dedup_find(frame->seq, frame->cmd);
        if (hit) {
            /* Replay cached response without re-executing */
            uart_send_response(frame->seq, frame->cmd,
                               hit->status, hit->resp_data, hit->resp_len);
            return;
        }
    }

    /* ---- Dispatch ---- */
    uint8_t  resp_buf[64];
    uint16_t resp_len = 0;

    uint16_t status = command_dispatch(frame->cmd,
                                       frame->payload, frame->payload_len,
                                       resp_buf, &resp_len);

    /* ---- Send response if requested ---- */
    if (frame->flags & T5_FLAG_ACK_REQ) {
        uart_send_response(frame->seq, frame->cmd, status,
                           resp_buf, resp_len);

        /* Store in dedup cache */
        dedup_store(frame->seq, frame->cmd, status, resp_buf, resp_len);
    }
}

/* =========================================================================
 * RX task thread
 * ======================================================================= */
static void uart_rx_task(void *arg)
{
    (void)arg;

    uint8_t  buf[256];
    uint8_t  frame_buf[NIGHTSHIFT_MAX_WIRE_FRAME];
    uint16_t frame_len = 0;
    uint32_t loop_count = 0;

    uart_debug_puts("[NS] RX task started\r\n");

    for (;;) {
        int n = tal_uart_read(NIGHTSHIFT_UART_PORT,
                              buf, (uint32_t)sizeof(buf));
        loop_count++;

        if (n <= 0) {
            /* Log every 100 empty loops so we know the task is alive */
            if ((loop_count % 100) == 1) {
                char msg[48];
                int pos = 0;
                const char *pfx = "[NS] RX loop ";
                while (*pfx && pos < 40) msg[pos++] = *pfx++;
                /* simple decimal of loop_count */
                uint32_t v = loop_count;
                char tmp[10];
                int ti = 0;
                if (v == 0) { tmp[ti++] = '0'; }
                else { while (v > 0 && ti < 10) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
                for (int k = ti - 1; k >= 0; k--) msg[pos++] = tmp[k];
                const char *sfx = ", n=";
                while (*sfx && pos < 40) msg[pos++] = *sfx++;
                if (n < 0) { msg[pos++] = '-'; }
                else       { msg[pos++] = '0' + n; }
                msg[pos++] = '\r'; msg[pos++] = '\n'; msg[pos] = 0;
                uart_debug_puts(msg);
            }
            tal_system_sleep(NIGHTSHIFT_RX_TIMEOUT_MS);
            continue;
        }

        /* Log received byte count and hex dump */
        {
            char hdr[64];
            int pos = 0;
            const char *pfx = "[NS] RX ";
            while (*pfx && pos < 50) hdr[pos++] = *pfx++;
            uint32_t v = (uint32_t)n;
            char tmp[10]; int ti = 0;
            if (v == 0) { tmp[ti++] = '0'; }
            else { while (v > 0 && ti < 10) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
            for (int k = ti - 1; k >= 0; k--) hdr[pos++] = tmp[k];
            const char *sfx = " bytes: ";
            while (*sfx && pos < 55) hdr[pos++] = *sfx++;
            hdr[pos] = 0;
            uart_debug_puts(hdr);

            /* hex dump (up to 32 bytes) */
            char hex[128];
            int hi = 0;
            int dump = (n > 32) ? 32 : n;
            for (int i = 0; i < dump; i++) {
                uint8_t hi_nib = (buf[i] >> 4) & 0x0F;
                uint8_t lo_nib = buf[i] & 0x0F;
                hex[hi++] = (hi_nib < 10) ? ('0' + hi_nib) : ('A' + hi_nib - 10);
                hex[hi++] = (lo_nib < 10) ? ('0' + lo_nib) : ('A' + lo_nib - 10);
                hex[hi++] = ' ';
            }
            hex[hi++] = '\r'; hex[hi++] = '\n'; hex[hi] = 0;
            uart_debug_puts(hex);
        }

        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];

            if (b == 0x00) {
                /* End-of-frame delimiter */
                if (frame_len > 0) {
                    char fmsg[48];
                    int fp = 0;
                    const char *fpfx = "[NS] frame ";
                    while (*fpfx && fp < 40) fmsg[fp++] = *fpfx++;
                    uint32_t v = (uint32_t)frame_len;
                    char tmp[10]; int ti = 0;
                    if (v == 0) { tmp[ti++] = '0'; }
                    else { while (v > 0 && ti < 10) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
                    for (int k = ti - 1; k >= 0; k--) fmsg[fp++] = tmp[k];
                    fmsg[fp++] = '\r'; fmsg[fp++] = '\n'; fmsg[fp] = 0;
                    uart_debug_puts(fmsg);

                    t5_frame_t decoded;
                    int rc = t5_frame_decode(frame_buf, frame_len, &decoded);
                    if (rc == 0) {
                        char ok[48];
                        int op = 0;
                        const char *opfx = "[NS] cmd=0x";
                        while (*opfx && op < 40) ok[op++] = *opfx++;
                        /* cmd hex */
                        uint16_t cv = decoded.cmd;
                        char ch[5];
                        for (int ci = 3; ci >= 0; ci--) {
                            uint8_t nib = (cv >> (ci * 4)) & 0x0F;
                            ch[3 - ci] = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
                        }
                        ch[4] = 0;
                        for (int ci = 0; ci < 4; ci++) ok[op++] = ch[ci];
                        ok[op++] = '\r'; ok[op++] = '\n'; ok[op] = 0;
                        uart_debug_puts(ok);
                        process_frame(&decoded);
                    } else {
                        char err[32];
                        int ep = 0;
                        const char *epfx = "[NS] decode err ";
                        while (*epfx && ep < 24) err[ep++] = *epfx++;
                        if (rc < 0) { err[ep++] = '-'; rc = -rc; }
                        err[ep++] = '0' + rc;
                        err[ep++] = '\r'; err[ep++] = '\n'; err[ep] = 0;
                        uart_debug_puts(err);
                    }
                    frame_len = 0;
                }
            } else {
                if (frame_len < (uint16_t)sizeof(frame_buf)) {
                    frame_buf[frame_len++] = b;
                } else {
                    /* Overflow → discard partial frame and reset */
                    uart_debug_puts("[NS] frame overflow!\r\n");
                    frame_len = 0;
                }
            }
        }
    }
}

/* =========================================================================
 * uart_handler_init
 * ======================================================================= */
int uart_handler_init(void)
{
    if (g_initialised) return 0;

    /* ---- Pinmux for UART0 (P11 header: P10=RX, P11=TX) ---- */
    if (NIGHTSHIFT_UART_PORT == TUYA_UART_NUM_0) {
        tkl_io_pinmux_config(TUYA_IO_PIN_10, TUYA_UART0_RX);
        tkl_io_pinmux_config(TUYA_IO_PIN_11, TUYA_UART0_TX);
    }

    /* ---- UART peripheral init ---- */
    TAL_UART_CFG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.base_cfg.baudrate = NIGHTSHIFT_UART_BAUDRATE;
    cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    cfg.base_cfg.parity   = TUYA_UART_PARITY_TYPE_NONE;
    cfg.rx_buffer_size    = NIGHTSHIFT_UART_RX_BUF_SIZE;
    cfg.open_mode         = 0;  /* non-blocking: tal_uart_read returns 0 when no data */

    OPERATE_RET ret = tal_uart_init(NIGHTSHIFT_UART_PORT, &cfg);
    if (ret != OPRT_OK) return -1;

    /* ---- Zero dedup cache ---- */
    memset(g_dedup, 0, sizeof(g_dedup));
    g_dedup_head        = 0;
    g_last_heartbeat_ms = 0;
    g_out_seq           = 0;

    /* Enable debug output before RX task starts */
    g_initialised = true;

    /* ---- Spawn RX task ---- */
    THREAD_CFG_T thrd;
    memset(&thrd, 0, sizeof(thrd));
    thrd.stackDepth = NIGHTSHIFT_RX_TASK_STACK;
    thrd.priority   = NIGHTSHIFT_RX_TASK_PRIO;
    thrd.thrdname   = "ns_uart_rx";

    ret = tal_thread_create_and_start(&g_rx_thread, NULL, NULL,
                                      uart_rx_task, NULL, &thrd);
    if (ret != OPRT_OK) return -2;

    return 0;
}

/* =========================================================================
 * uart_debug_puts  — write plain text to the protocol UART for diagnostics
 * ======================================================================= */
void uart_debug_puts(const char *str)
{
    (void)str;
}
