/**
 * @file app_state.h
 * @brief Authoritative local mirror of the OPi SystemState.
 *
 * T5 never invents state — it only stores what the Orange Pi sends.
 * A single change-callback is provided for the UI layer to refresh on
 * every accepted state update.
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * State structure
 * ======================================================================= */
typedef struct {
    uint32_t revision;
    uint8_t  mode;              /* 0=IDLE, 1=DAY_WORK, 2=NIGHT_EXEC */
    uint32_t attention_flags;
    uint8_t  work_state;        /* 0=STOPPED .. 5=FAILED */
    uint16_t progress_permille; /* 0 – 1000 */
    uint32_t current_task_id;
    uint16_t confirmation_count;
    bool     opi_online;
    bool     in_sync;           /* STATE_SYNC transaction active */
    uint32_t last_heartbeat_ms;

    /* Dashboard counters */
    uint16_t urgent_auto;
    uint16_t normal_auto;
    uint16_t urgent_confirm;
    uint16_t normal_confirm;
    uint16_t completed_today;
    uint16_t failed_today;

    /* Current task title (UTF-8, null-terminated locally) */
    char     current_task_title[128];
} app_state_t;

/* =========================================================================
 * Change callback type
 * ======================================================================= */
typedef void (*state_change_cb_t)(const app_state_t *state);

/* =========================================================================
 * API
 * ======================================================================= */

/** Zero-initialise the global state struct and its mutex. */
void app_state_init(void);

/** Return a read-only pointer to the current state snapshot.
 *  The caller must not modify the returned struct directly. */
const app_state_t *app_state_get(void);

/**
 * @brief Revision guard.
 * @return true  if incoming_rev >= current revision (or during STATE_SYNC).
 * @return false if incoming_rev < current revision (stale update).
 */
bool app_state_check_revision(uint32_t incoming_rev);

/** Apply MODE_SET.  Subject to revision guard. */
void app_state_set_mode(uint32_t rev, uint8_t mode);

/** Apply ATTENTION_SET.  Subject to revision guard. */
void app_state_set_attention(uint32_t rev, uint32_t flags, uint16_t count);

/**
 * Apply WORK_STATE_SET.
 * NOTE: no revision prefix per protocol variant — always accepted.
 */
void app_state_set_work_state(uint8_t ws, uint16_t progress,
                              uint32_t task_id, const char *title);

/** Apply DASHBOARD_SET.  Subject to revision guard. */
void app_state_set_dashboard(uint32_t rev,
                             uint16_t ua, uint16_t na,
                             uint16_t uc, uint16_t nc,
                             uint16_t comp, uint16_t fail);

/** Set / clear the OPi-online flag (driven by heartbeat timeout). */
void app_state_set_opi_online(bool online);

/** Begin / end a STATE_SYNC transaction (bypasses revision guard). */
void app_state_set_in_sync(bool syncing, uint32_t target_rev);

/** Register a single callback invoked (under lock) on every state change. */
void app_state_register_change_cb(state_change_cb_t cb);

#endif /* APP_STATE_H */
