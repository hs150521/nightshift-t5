/**
 * @file app_state.c
 * @brief Global app-state mirror: revision-guarded setters under a mutex.
 */

#include "app_state.h"
#include "tal_api.h"
#include <string.h>

/* =========================================================================
 * Statics
 * ======================================================================= */
static app_state_t       g_state;
static MUTEX_HANDLE      g_mutex;
static state_change_cb_t g_change_cb;

/* =========================================================================
 * Helpers
 * ======================================================================= */
static void notify_change(void)
{
    if (g_change_cb) {
        g_change_cb(&g_state);
    }
}

/* =========================================================================
 * Public API
 * ======================================================================= */

void app_state_init(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.mode       = 0;   /* IDLE */
    g_state.opi_online = false;
    g_state.in_sync    = false;
    g_change_cb        = NULL;

    tal_mutex_create_init(&g_mutex);
}

const app_state_t *app_state_get(void)
{
    return &g_state;
}

/* -------------------------------------------------------------------------
 * Revision guard
 *
 * incoming >= current  → accept  (equal = idempotent re-apply)
 * incoming <  current  → reject  (stale)
 * During STATE_SYNC  → always accept (full re-sync overrides everything)
 * ---------------------------------------------------------------------- */
bool app_state_check_revision(uint32_t incoming_rev)
{
    bool ok;
    tal_mutex_lock(g_mutex);
    if (g_state.in_sync) {
        ok = true;
    } else {
        ok = (incoming_rev >= g_state.revision);
    }
    tal_mutex_unlock(g_mutex);
    return ok;
}

/* -------------------------------------------------------------------------
 * Setters
 * ---------------------------------------------------------------------- */

void app_state_set_mode(uint32_t rev, uint8_t mode)
{
    tal_mutex_lock(g_mutex);
    if (g_state.in_sync || rev >= g_state.revision) {
        g_state.revision = rev;
        g_state.mode     = mode;
        notify_change();
    }
    tal_mutex_unlock(g_mutex);
}

void app_state_set_attention(uint32_t rev, uint32_t flags, uint16_t count)
{
    tal_mutex_lock(g_mutex);
    if (g_state.in_sync || rev >= g_state.revision) {
        g_state.revision          = rev;
        g_state.attention_flags   = flags;
        g_state.confirmation_count = count;
        notify_change();
    }
    tal_mutex_unlock(g_mutex);
}

/*
 * WORK_STATE_SET — no revision prefix per spec.
 * Always accepted unconditionally.
 */
void app_state_set_work_state(uint8_t ws, uint16_t progress,
                              uint32_t task_id, const char *title)
{
    tal_mutex_lock(g_mutex);
    g_state.work_state       = ws;
    g_state.progress_permille = progress;
    g_state.current_task_id  = task_id;

    if (title) {
        strncpy(g_state.current_task_title, title,
                sizeof(g_state.current_task_title) - 1);
        g_state.current_task_title[sizeof(g_state.current_task_title) - 1] = '\0';
    } else {
        g_state.current_task_title[0] = '\0';
    }
    notify_change();
    tal_mutex_unlock(g_mutex);
}

void app_state_set_dashboard(uint32_t rev,
                             uint16_t ua, uint16_t na,
                             uint16_t uc, uint16_t nc,
                             uint16_t comp, uint16_t fail)
{
    tal_mutex_lock(g_mutex);
    if (g_state.in_sync || rev >= g_state.revision) {
        g_state.revision        = rev;
        g_state.urgent_auto     = ua;
        g_state.normal_auto     = na;
        g_state.urgent_confirm  = uc;
        g_state.normal_confirm  = nc;
        g_state.completed_today = comp;
        g_state.failed_today    = fail;
        notify_change();
    }
    tal_mutex_unlock(g_mutex);
}

void app_state_set_opi_online(bool online)
{
    tal_mutex_lock(g_mutex);
    g_state.opi_online = online;
    notify_change();
    tal_mutex_unlock(g_mutex);
}

void app_state_set_in_sync(bool syncing, uint32_t target_rev)
{
    tal_mutex_lock(g_mutex);
    g_state.in_sync = syncing;
    if (!syncing) {
        /* End of sync — commit target revision and notify UI */
        g_state.revision = target_rev;
        notify_change();
    }
    tal_mutex_unlock(g_mutex);
}

void app_state_register_change_cb(state_change_cb_t cb)
{
    tal_mutex_lock(g_mutex);
    g_change_cb = cb;
    tal_mutex_unlock(g_mutex);
}
