/**
 * @file nightshift_config.h
 * @brief Project-wide configuration for Nightshift T5 firmware.
 *
 * All tunable constants live here so they can be adjusted in one place.
 */

#ifndef NIGHTSHIFT_CONFIG_H
#define NIGHTSHIFT_CONFIG_H

/* -------------------------------------------------------------------------
 * UART configuration
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_UART_PORT        TUYA_UART_NUM_0
#define NIGHTSHIFT_UART_BAUDRATE    460800
#define NIGHTSHIFT_UART_RX_BUF_SIZE 2048

/* -------------------------------------------------------------------------
 * Protocol limits
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_MAX_PAYLOAD      1024
/* Raw frame: magic(2)+ver(1)+flags(1)+seq(2)+cmd(2)+plen(2)+payload+crc(2) */
#define NIGHTSHIFT_MAX_RAW_FRAME    (12 + NIGHTSHIFT_MAX_PAYLOAD)
/* COBS worst-case: ceil(N/254)*255 ≈ N + N/254 + 1; add 2 for safety */
#define NIGHTSHIFT_MAX_WIRE_FRAME   (NIGHTSHIFT_MAX_RAW_FRAME + \
                                     NIGHTSHIFT_MAX_RAW_FRAME / 254 + 3)

/* -------------------------------------------------------------------------
 * Heartbeat
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_HEARTBEAT_INTERVAL_MS  2000
#define NIGHTSHIFT_HEARTBEAT_TIMEOUT_MS   6000   /* 3 missed beats */

/* -------------------------------------------------------------------------
 * Dedup cache
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_DEDUP_CACHE_SIZE 32
#define NIGHTSHIFT_DEDUP_DATA_SIZE  160

/* -------------------------------------------------------------------------
 * Display-state bounds
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_MAX_TASKS             12
#define NIGHTSHIFT_TASK_TITLE_SIZE       64
#define NIGHTSHIFT_TASK_SOURCE_SIZE      32
#define NIGHTSHIFT_CURRENT_TITLE_SIZE    128
#define NIGHTSHIFT_ATTENTION_TEXT_SIZE   128
#define NIGHTSHIFT_NOTICE_TITLE_SIZE     64
#define NIGHTSHIFT_NOTICE_BODY_SIZE      192

/* A partial full-state transaction is abandoned after this interval. */
#define NIGHTSHIFT_SYNC_TIMEOUT_MS       10000

/* UI events wait this long for an Orange Pi acknowledgement. */
#define NIGHTSHIFT_ACTION_ACK_TIMEOUT_MS 1500

/* -------------------------------------------------------------------------
 * Firmware identification
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_FW_VERSION       "nightshift-t5/1.0.0"
#define NIGHTSHIFT_FW_MAJOR         1
#define NIGHTSHIFT_FW_MINOR         0

/* -------------------------------------------------------------------------
 * T5 capability flags (mirrors HELLO capabilities bitmask)
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_CAP_LCD          (1U << 0)
#define NIGHTSHIFT_CAP_TOUCH        (1U << 1)
#define NIGHTSHIFT_CAP_LED          (1U << 2)
#if defined(LED_NAME) && defined(CONFIG_ENABLE_LED) && CONFIG_ENABLE_LED
#define NIGHTSHIFT_CAPABILITIES     (NIGHTSHIFT_CAP_LCD | \
                                     NIGHTSHIFT_CAP_TOUCH | \
                                     NIGHTSHIFT_CAP_LED)
#else
#define NIGHTSHIFT_CAPABILITIES     (NIGHTSHIFT_CAP_LCD | \
                                     NIGHTSHIFT_CAP_TOUCH)
#endif

/* -------------------------------------------------------------------------
 * RX task configuration
 * ---------------------------------------------------------------------- */
#define NIGHTSHIFT_RX_TASK_STACK    8192
#define NIGHTSHIFT_RX_TASK_PRIO     THREAD_PRIO_2
#define NIGHTSHIFT_RX_TIMEOUT_MS    10
#define NIGHTSHIFT_WATCHDOG_STACK   3072
#define NIGHTSHIFT_WATCHDOG_PRIO    THREAD_PRIO_1
#define NIGHTSHIFT_WATCHDOG_POLL_MS 250

#endif /* NIGHTSHIFT_CONFIG_H */
