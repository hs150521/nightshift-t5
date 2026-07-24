/**
 * @file state_store.c
 * @brief Atomic state mirror. Callbacks are always invoked after unlock.
 */

#include "state_store.h"

#include "t5_protocol.h"
#include "tal_api.h"

#include <string.h>

#define SYNC_HAVE_MODE       (1U << 0)
#define SYNC_HAVE_ATTENTION  (1U << 1)
#define SYNC_HAVE_WORK       (1U << 2)
#define SYNC_REQUIRED        (SYNC_HAVE_MODE | SYNC_HAVE_ATTENTION | \
                              SYNC_HAVE_WORK)

typedef struct {
    bool active;
    uint32_t revision;
    uint8_t list_type;
    uint16_t expected;
    uint16_t count;
    uint32_t crc32;
    display_task_t items[NIGHTSHIFT_MAX_TASKS];
} task_transaction_t;

static display_state_t g_committed;
static display_state_t g_staging;
static task_transaction_t g_task_txn;
static MUTEX_HANDLE g_mutex;
static state_store_change_cb_t g_change_cb;
static bool g_sync_active;
static uint32_t g_sync_target;
static uint32_t g_sync_started_ms;
static uint32_t g_sync_components;
static uint32_t g_sync_crc32;
static bool g_time_valid;
static uint64_t g_time_unix_ms;
static uint32_t g_time_monotonic_ms;
static int16_t g_time_utc_offset_minutes;

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static state_store_result_t revision_result_locked(uint32_t revision)
{
    if (g_sync_active) {
        return (revision == g_sync_target) ? STATE_STORE_OK
                                           : STATE_STORE_CONFLICT;
    }
    return (revision >= g_committed.revision) ? STATE_STORE_OK
                                               : STATE_STORE_STALE;
}

static display_state_t *write_state_locked(void)
{
    return g_sync_active ? &g_staging : &g_committed;
}

static void preserve_local_fields_locked(display_state_t *dst)
{
    dst->opi_online = g_committed.opi_online;
    dst->action_pending = g_committed.action_pending;
    dst->pending_action = g_committed.pending_action;
    dst->last_action_status = g_committed.last_action_status;
    dst->backlight_percent = g_committed.backlight_percent;
    dst->led_override_active = g_committed.led_override_active;
    dst->led_override_mode = g_committed.led_override_mode;
    dst->led_override_period_ms = g_committed.led_override_period_ms;
}

static void notify(const display_state_t *snapshot,
                   state_store_change_cb_t cb)
{
    if (cb) cb(snapshot);
}

static void prepare_notify_locked(bool changed, display_state_t *snapshot,
                                  state_store_change_cb_t *cb)
{
    if (changed && !g_sync_active) {
        *snapshot = g_committed;
        *cb = g_change_cb;
    } else {
        *cb = NULL;
    }
}

void state_store_init(void)
{
    memset(&g_committed, 0, sizeof(g_committed));
    memset(&g_staging, 0, sizeof(g_staging));
    memset(&g_task_txn, 0, sizeof(g_task_txn));
    g_committed.mode = T5_MODE_IDLE;
    g_committed.work_state = T5_WORK_STOPPED;
    g_committed.backlight_percent = 100;
    g_committed.last_action_status = T5_STATUS_OK;
    g_change_cb = NULL;
    g_sync_active = false;
    g_time_valid = false;
    tal_mutex_create_init(&g_mutex);
}

void state_store_snapshot(display_state_t *out)
{
    if (!out) return;
    tal_mutex_lock(g_mutex);
    *out = g_committed;
    out->sync_in_progress = g_sync_active;
    tal_mutex_unlock(g_mutex);
}

uint32_t state_store_revision(void)
{
    uint32_t revision;
    tal_mutex_lock(g_mutex);
    revision = g_committed.revision;
    tal_mutex_unlock(g_mutex);
    return revision;
}

bool state_store_sync_active(void)
{
    bool active;
    tal_mutex_lock(g_mutex);
    active = g_sync_active;
    tal_mutex_unlock(g_mutex);
    return active;
}

state_store_result_t state_store_sync_begin(uint32_t target_revision,
                                             uint32_t now_ms)
{
    tal_mutex_lock(g_mutex);
    if (g_sync_active || g_task_txn.active) {
        tal_mutex_unlock(g_mutex);
        return STATE_STORE_BUSY;
    }
    g_staging = g_committed;
    g_staging.revision = target_revision;
    g_staging.sync_in_progress = true;
    g_sync_active = true;
    g_sync_target = target_revision;
    g_sync_started_ms = now_ms;
    g_sync_components = 0;
    g_sync_crc32 = 0;
    tal_mutex_unlock(g_mutex);
    return STATE_STORE_OK;
}

state_store_result_t state_store_sync_end(uint32_t target_revision,
                                           uint32_t snapshot_crc32)
{
    display_state_t snapshot;
    state_store_change_cb_t cb = NULL;
    state_store_result_t result = STATE_STORE_OK;

    tal_mutex_lock(g_mutex);
    if (!g_sync_active || target_revision != g_sync_target) {
        result = STATE_STORE_CONFLICT;
    } else if (g_task_txn.active ||
               (g_sync_components & SYNC_REQUIRED) != SYNC_REQUIRED) {
        result = STATE_STORE_INCOMPLETE;
    } else if (snapshot_crc32 != 0 && snapshot_crc32 != g_sync_crc32) {
        result = STATE_STORE_CONFLICT;
    }

    if (result == STATE_STORE_OK) {
        preserve_local_fields_locked(&g_staging);
        g_staging.revision = target_revision;
        g_staging.sync_in_progress = false;
        g_committed = g_staging;
        snapshot = g_committed;
        cb = g_change_cb;
    } else {
        memset(&g_task_txn, 0, sizeof(g_task_txn));
    }
    g_sync_active = false;
    g_sync_components = 0;
    g_sync_crc32 = 0;
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
    return result;
}

void state_store_sync_abort(void)
{
    tal_mutex_lock(g_mutex);
    g_sync_active = false;
    g_sync_components = 0;
    g_sync_crc32 = 0;
    memset(&g_task_txn, 0, sizeof(g_task_txn));
    tal_mutex_unlock(g_mutex);
}

void state_store_sync_abort_if_expired(uint32_t now_ms)
{
    tal_mutex_lock(g_mutex);
    if (g_sync_active &&
        (uint32_t)(now_ms - g_sync_started_ms) >= NIGHTSHIFT_SYNC_TIMEOUT_MS) {
        g_sync_active = false;
        g_sync_components = 0;
        g_sync_crc32 = 0;
        memset(&g_task_txn, 0, sizeof(g_task_txn));
    }
    tal_mutex_unlock(g_mutex);
}

void state_store_sync_record(uint16_t command,
                             const uint8_t *payload, uint16_t payload_len)
{
    uint8_t command_le[2];
    if (!payload && payload_len != 0) return;
    WRITE_U16_LE(command_le, command);
    tal_mutex_lock(g_mutex);
    if (g_sync_active) {
        g_sync_crc32 = crc32_update(g_sync_crc32, command_le, 2);
        g_sync_crc32 = crc32_update(g_sync_crc32, payload, payload_len);
    }
    tal_mutex_unlock(g_mutex);
}

state_store_result_t state_store_set_mode(uint32_t revision, uint8_t mode,
                                           uint32_t reason,
                                           uint64_t changed_at_ms)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    state_store_result_t result = revision_result_locked(revision);
    if (result == STATE_STORE_OK) {
        display_state_t *state = write_state_locked();
        state->revision = revision;
        state->mode = mode;
        state->mode_reason = reason;
        state->mode_changed_at_ms = changed_at_ms;
        if (g_sync_active) g_sync_components |= SYNC_HAVE_MODE;
    }
    prepare_notify_locked(result == STATE_STORE_OK, &snapshot, &cb);
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
    return result;
}

state_store_result_t state_store_set_attention(uint32_t revision,
                                                uint32_t flags,
                                                uint16_t count,
                                                const char *message)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    state_store_result_t result = revision_result_locked(revision);
    if (result == STATE_STORE_OK) {
        display_state_t *state = write_state_locked();
        state->revision = revision;
        state->attention_flags = flags;
        state->confirmation_count = count;
        copy_text(state->attention_text, sizeof(state->attention_text),
                  message);
        if (g_sync_active) g_sync_components |= SYNC_HAVE_ATTENTION;
    }
    prepare_notify_locked(result == STATE_STORE_OK, &snapshot, &cb);
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
    return result;
}

state_store_result_t state_store_set_work(uint8_t work_state,
                                          uint16_t progress,
                                          uint32_t token_input,
                                          uint32_t token_output,
                                          uint32_t elapsed_seconds,
                                          uint32_t task_id,
                                          const char *title)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    display_state_t *state = write_state_locked();
    state->work_state = work_state;
    state->progress_permille = progress;
    state->token_input = token_input;
    state->token_output = token_output;
    state->elapsed_seconds = elapsed_seconds;
    state->current_task_id = task_id;
    copy_text(state->current_task_title,
              sizeof(state->current_task_title), title);
    if (g_sync_active) g_sync_components |= SYNC_HAVE_WORK;
    prepare_notify_locked(true, &snapshot, &cb);
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
    return STATE_STORE_OK;
}

state_store_result_t state_store_set_dashboard(uint32_t revision,
                                                uint16_t urgent_auto,
                                                uint16_t normal_auto,
                                                uint16_t urgent_confirm,
                                                uint16_t normal_confirm,
                                                uint16_t completed,
                                                uint16_t failed)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    state_store_result_t result = revision_result_locked(revision);
    if (result == STATE_STORE_OK) {
        display_state_t *state = write_state_locked();
        state->revision = revision;
        state->urgent_auto = urgent_auto;
        state->normal_auto = normal_auto;
        state->urgent_confirm = urgent_confirm;
        state->normal_confirm = normal_confirm;
        state->completed_today = completed;
        state->failed_today = failed;
    }
    prepare_notify_locked(result == STATE_STORE_OK, &snapshot, &cb);
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
    return result;
}

state_store_result_t state_store_show_notice(uint32_t revision,
                                              uint32_t notice_id,
                                              uint8_t severity,
                                              uint8_t flags,
                                              uint64_t expires_at_ms,
                                              const char *title,
                                              const char *body)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    state_store_result_t result = revision_result_locked(revision);
    if (result == STATE_STORE_OK) {
        display_state_t *state = write_state_locked();
        state->revision = revision;
        state->notice.active = true;
        state->notice.notice_id = notice_id;
        state->notice.severity = severity;
        state->notice.flags = flags;
        state->notice.expires_at_ms = expires_at_ms;
        copy_text(state->notice.title, sizeof(state->notice.title), title);
        copy_text(state->notice.body, sizeof(state->notice.body), body);
    }
    prepare_notify_locked(result == STATE_STORE_OK, &snapshot, &cb);
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
    return result;
}

state_store_result_t state_store_task_begin(uint32_t revision,
                                             uint8_t list_type,
                                             uint16_t expected_count)
{
    tal_mutex_lock(g_mutex);
    state_store_result_t result = revision_result_locked(revision);
    if (result == STATE_STORE_OK && g_task_txn.active) {
        result = STATE_STORE_BUSY;
    }
    if (result == STATE_STORE_OK &&
        expected_count > NIGHTSHIFT_MAX_TASKS) {
        result = STATE_STORE_NO_SPACE;
    }
    if (result == STATE_STORE_OK) {
        memset(&g_task_txn, 0, sizeof(g_task_txn));
        g_task_txn.active = true;
        g_task_txn.revision = revision;
        g_task_txn.list_type = list_type;
        g_task_txn.expected = expected_count;
    }
    tal_mutex_unlock(g_mutex);
    return result;
}

state_store_result_t state_store_task_item(uint32_t revision,
                                            const display_task_t *item,
                                            const uint8_t *raw_payload,
                                            uint16_t raw_payload_len)
{
    if (!item || (!raw_payload && raw_payload_len != 0)) {
        return STATE_STORE_INVALID;
    }
    tal_mutex_lock(g_mutex);
    state_store_result_t result = STATE_STORE_OK;
    if (!g_task_txn.active || revision != g_task_txn.revision) {
        result = STATE_STORE_CONFLICT;
    } else if (g_task_txn.count >= g_task_txn.expected ||
               g_task_txn.count >= NIGHTSHIFT_MAX_TASKS) {
        result = STATE_STORE_NO_SPACE;
    } else {
        g_task_txn.items[g_task_txn.count++] = *item;
        g_task_txn.crc32 = crc32_update(g_task_txn.crc32, raw_payload,
                                        raw_payload_len);
    }
    tal_mutex_unlock(g_mutex);
    return result;
}

state_store_result_t state_store_task_end(uint32_t revision,
                                           uint32_t list_crc32)
{
    display_state_t snapshot;
    state_store_change_cb_t cb = NULL;
    tal_mutex_lock(g_mutex);
    state_store_result_t result = STATE_STORE_OK;
    if (!g_task_txn.active || revision != g_task_txn.revision) {
        result = STATE_STORE_CONFLICT;
    } else if (g_task_txn.count != g_task_txn.expected) {
        result = STATE_STORE_INCOMPLETE;
    } else if (list_crc32 != 0 && list_crc32 != g_task_txn.crc32) {
        result = STATE_STORE_CONFLICT;
    } else {
        display_state_t *state = write_state_locked();
        state->revision = revision;
        state->task_list_type = g_task_txn.list_type;
        state->task_count = g_task_txn.count;
        memset(state->tasks, 0, sizeof(state->tasks));
        memcpy(state->tasks, g_task_txn.items,
               (size_t)g_task_txn.count * sizeof(g_task_txn.items[0]));
        prepare_notify_locked(true, &snapshot, &cb);
    }
    memset(&g_task_txn, 0, sizeof(g_task_txn));
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
    return result;
}

void state_store_task_abort(void)
{
    tal_mutex_lock(g_mutex);
    memset(&g_task_txn, 0, sizeof(g_task_txn));
    tal_mutex_unlock(g_mutex);
}

void state_store_set_online(bool online)
{
    display_state_t snapshot;
    state_store_change_cb_t cb = NULL;
    tal_mutex_lock(g_mutex);
    if (g_committed.opi_online != online) {
        g_committed.opi_online = online;
        snapshot = g_committed;
        snapshot.sync_in_progress = g_sync_active;
        cb = g_change_cb;
    }
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
}

void state_store_set_action(uint16_t action, bool pending, uint16_t status)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    g_committed.action_pending = pending;
    g_committed.pending_action = pending ? action : 0;
    g_committed.last_action_status = status;
    snapshot = g_committed;
    snapshot.sync_in_progress = g_sync_active;
    cb = g_change_cb;
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
}

void state_store_set_backlight(uint8_t percent)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    g_committed.backlight_percent = percent;
    snapshot = g_committed;
    snapshot.sync_in_progress = g_sync_active;
    cb = g_change_cb;
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
}

void state_store_set_led_override(bool active, uint8_t mode,
                                  uint16_t period_ms)
{
    display_state_t snapshot;
    state_store_change_cb_t cb;
    tal_mutex_lock(g_mutex);
    g_committed.led_override_active = active;
    g_committed.led_override_mode = mode;
    g_committed.led_override_period_ms = period_ms;
    snapshot = g_committed;
    snapshot.sync_in_progress = g_sync_active;
    cb = g_change_cb;
    tal_mutex_unlock(g_mutex);
    notify(&snapshot, cb);
}

void state_store_time_sync(uint64_t unix_time_ms,
                           int16_t utc_offset_minutes,
                           uint32_t monotonic_ms)
{
    tal_mutex_lock(g_mutex);
    g_time_valid = true;
    g_time_unix_ms = unix_time_ms;
    g_time_monotonic_ms = monotonic_ms;
    g_time_utc_offset_minutes = utc_offset_minutes;
    tal_mutex_unlock(g_mutex);
}

bool state_store_time_now(uint32_t monotonic_ms, uint64_t *unix_time_ms,
                          int16_t *utc_offset_minutes)
{
    tal_mutex_lock(g_mutex);
    bool valid = g_time_valid;
    if (valid && unix_time_ms) {
        *unix_time_ms = g_time_unix_ms +
                        (uint32_t)(monotonic_ms - g_time_monotonic_ms);
    }
    if (valid && utc_offset_minutes) {
        *utc_offset_minutes = g_time_utc_offset_minutes;
    }
    tal_mutex_unlock(g_mutex);
    return valid;
}

void state_store_register_change_cb(state_store_change_cb_t cb)
{
    tal_mutex_lock(g_mutex);
    g_change_cb = cb;
    tal_mutex_unlock(g_mutex);
}
