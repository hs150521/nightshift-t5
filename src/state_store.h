/**
 * @file state_store.h
 * @brief Thread-safe, revision-guarded display state with atomic snapshots.
 */

#ifndef STATE_STORE_H
#define STATE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nightshift_config.h"

typedef struct {
    uint32_t task_id;
    uint8_t quadrant;
    uint8_t task_state;
    uint8_t flags;
    char title[NIGHTSHIFT_TASK_TITLE_SIZE];
    char source[NIGHTSHIFT_TASK_SOURCE_SIZE];
} display_task_t;

typedef struct {
    bool active;
    uint32_t notice_id;
    uint8_t severity;
    uint8_t flags;
    uint64_t expires_at_ms;
    char title[NIGHTSHIFT_NOTICE_TITLE_SIZE];
    char body[NIGHTSHIFT_NOTICE_BODY_SIZE];
} display_notice_t;

typedef struct {
    uint32_t revision;
    uint8_t mode;
    uint32_t mode_reason;
    uint64_t mode_changed_at_ms;

    uint32_t attention_flags;
    uint16_t confirmation_count;
    char attention_text[NIGHTSHIFT_ATTENTION_TEXT_SIZE];

    uint8_t work_state;
    uint16_t progress_permille;
    uint32_t token_input;
    uint32_t token_output;
    uint32_t elapsed_seconds;
    uint32_t current_task_id;
    char current_task_title[NIGHTSHIFT_CURRENT_TITLE_SIZE];

    uint16_t urgent_auto;
    uint16_t normal_auto;
    uint16_t urgent_confirm;
    uint16_t normal_confirm;
    uint16_t completed_today;
    uint16_t failed_today;

    uint8_t task_list_type;
    uint16_t task_count;
    display_task_t tasks[NIGHTSHIFT_MAX_TASKS];
    display_notice_t notice;

    bool opi_online;
    bool sync_in_progress;
    bool action_pending;
    uint16_t pending_action;
    uint16_t last_action_status;
    uint8_t backlight_percent;
    bool led_override_active;
    uint8_t led_override_mode;
    uint16_t led_override_period_ms;
} display_state_t;

typedef void (*state_store_change_cb_t)(const display_state_t *snapshot);

typedef enum {
    STATE_STORE_OK = 0,
    STATE_STORE_STALE,
    STATE_STORE_CONFLICT,
    STATE_STORE_BUSY,
    STATE_STORE_INCOMPLETE,
    STATE_STORE_NO_SPACE,
    STATE_STORE_INVALID,
} state_store_result_t;

void state_store_init(void);
void state_store_snapshot(display_state_t *out);
uint32_t state_store_revision(void);
bool state_store_sync_active(void);

state_store_result_t state_store_sync_begin(uint32_t target_revision,
                                             uint32_t now_ms);
state_store_result_t state_store_sync_end(uint32_t target_revision,
                                           uint32_t snapshot_crc32);
void state_store_sync_abort(void);
void state_store_sync_abort_if_expired(uint32_t now_ms);
void state_store_sync_record(uint16_t command,
                             const uint8_t *payload, uint16_t payload_len);

state_store_result_t state_store_set_mode(uint32_t revision, uint8_t mode,
                                           uint32_t reason,
                                           uint64_t changed_at_ms);
state_store_result_t state_store_set_attention(uint32_t revision,
                                                uint32_t flags,
                                                uint16_t count,
                                                const char *message);
state_store_result_t state_store_set_work(uint8_t work_state,
                                          uint16_t progress,
                                          uint32_t token_input,
                                          uint32_t token_output,
                                          uint32_t elapsed_seconds,
                                          uint32_t task_id,
                                          const char *title);
state_store_result_t state_store_set_dashboard(uint32_t revision,
                                                uint16_t urgent_auto,
                                                uint16_t normal_auto,
                                                uint16_t urgent_confirm,
                                                uint16_t normal_confirm,
                                                uint16_t completed,
                                                uint16_t failed);
state_store_result_t state_store_show_notice(uint32_t revision,
                                              uint32_t notice_id,
                                              uint8_t severity,
                                              uint8_t flags,
                                              uint64_t expires_at_ms,
                                              const char *title,
                                              const char *body);

state_store_result_t state_store_task_begin(uint32_t revision,
                                             uint8_t list_type,
                                             uint16_t expected_count);
state_store_result_t state_store_task_item(uint32_t revision,
                                            const display_task_t *item,
                                            const uint8_t *raw_payload,
                                            uint16_t raw_payload_len);
state_store_result_t state_store_task_end(uint32_t revision,
                                           uint32_t list_crc32);
void state_store_task_abort(void);

void state_store_set_online(bool online);
void state_store_set_action(uint16_t action, bool pending, uint16_t status);
void state_store_set_backlight(uint8_t percent);
void state_store_set_led_override(bool active, uint8_t mode,
                                  uint16_t period_ms);

void state_store_time_sync(uint64_t unix_time_ms,
                           int16_t utc_offset_minutes,
                           uint32_t monotonic_ms);
bool state_store_time_now(uint32_t monotonic_ms, uint64_t *unix_time_ms,
                          int16_t *utc_offset_minutes);

void state_store_register_change_cb(state_store_change_cb_t cb);

#endif /* STATE_STORE_H */
