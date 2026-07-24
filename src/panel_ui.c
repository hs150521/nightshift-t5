/**
 * @file panel_ui.c
 * @brief 480x320 status, dashboard, notice, task list, and touch controls.
 */

#include "panel_ui.h"

#include "t5_protocol.h"
#include "uart_transport.h"

#include "lvgl.h"
#include "tal_api.h"
#include "tdl_display_manage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(LED_NAME) && defined(CONFIG_ENABLE_LED) && CONFIG_ENABLE_LED
#include "tdl_led_manage.h"
#endif

#define COLOR_IDLE_BG       lv_color_hex(0xFFF3E0)
#define COLOR_IDLE_FG       lv_color_hex(0x4E342E)
#define COLOR_DAY_BG        lv_color_hex(0xE3F2FD)
#define COLOR_DAY_FG        lv_color_hex(0x0D47A1)
#define COLOR_NIGHT_BG      lv_color_hex(0x081B33)
#define COLOR_NIGHT_FG      lv_color_hex(0xE3F2FD)
#define COLOR_CARD          lv_color_hex(0xFFFFFF)
#define COLOR_ATTENTION     lv_color_hex(0xB3261E)
#define COLOR_WARNING       lv_color_hex(0xE65100)
#define COLOR_SUCCESS       lv_color_hex(0x1B5E20)
#define COLOR_OFFLINE       lv_color_hex(0x202124)

static MUTEX_HANDLE g_pending_mutex;
static display_state_t g_pending_state;
static display_state_t g_visible_state;
static bool g_pending_dirty;
static uint16_t g_task_offset;
static bool g_tasks_visible;

static lv_obj_t *g_screen;
static lv_obj_t *g_main_page;
static lv_obj_t *g_task_page;
static lv_obj_t *g_mode_label;
static lv_obj_t *g_status_label;
static lv_obj_t *g_task_label;
static lv_obj_t *g_progress;
static lv_obj_t *g_attention_label;
static lv_obj_t *g_dashboard_label;
static lv_obj_t *g_notice_card;
static lv_obj_t *g_notice_title;
static lv_obj_t *g_notice_body;
static lv_obj_t *g_action_status;
static lv_obj_t *g_confirm_button;
static lv_obj_t *g_reject_button;
static lv_obj_t *g_work_button;
static lv_obj_t *g_dismiss_button;
static lv_obj_t *g_tasks_button;
static lv_obj_t *g_task_rows[4];
static lv_obj_t *g_task_row_labels[4];
static lv_obj_t *g_task_page_label;
static lv_obj_t *g_prev_button;
static lv_obj_t *g_next_button;
static lv_obj_t *g_offline_overlay;
static lv_obj_t *g_offline_label;
static TDL_DISP_HANDLE_T g_display_handle;
static uint8_t g_applied_backlight = 0xFF;

#if defined(LED_NAME) && defined(CONFIG_ENABLE_LED) && CONFIG_ENABLE_LED
static TDL_LED_HANDLE_T g_led_handle;
#endif

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            int x, int y, int width,
                            lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int x, int y, int width, int height,
                             lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    return button;
}

static void set_enabled(lv_obj_t *object, bool enabled)
{
    if (!object) return;
    if (enabled) lv_obj_clear_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static uint32_t confirmation_object_id(void)
{
    return g_visible_state.current_task_id;
}

static void send_action(uint16_t action, uint8_t object_type,
                        uint32_t object_id)
{
    uart_transport_send_ui_action(action, object_type, object_id, 0, "");
}

static void on_confirm(lv_event_t *event)
{
    (void)event;
    uint32_t task_id = confirmation_object_id();
    if (task_id != 0) {
        send_action(T5_ACTION_CONFIRM, T5_OBJECT_TASK, task_id);
    }
}

static void on_reject(lv_event_t *event)
{
    (void)event;
    uint32_t task_id = confirmation_object_id();
    if (task_id != 0) {
        send_action(T5_ACTION_REJECT, T5_OBJECT_TASK, task_id);
    }
}

static void on_work(lv_event_t *event)
{
    (void)event;
    if (g_visible_state.work_state == T5_WORK_RUNNING) {
        send_action(T5_ACTION_PAUSE_EXECUTION, T5_OBJECT_EXECUTOR,
                    g_visible_state.current_task_id);
    } else if (g_visible_state.work_state == T5_WORK_PAUSED) {
        send_action(T5_ACTION_RESUME_EXECUTION, T5_OBJECT_EXECUTOR,
                    g_visible_state.current_task_id);
    } else if (g_visible_state.work_state == T5_WORK_FAILED &&
               g_visible_state.current_task_id != 0) {
        send_action(T5_ACTION_RETRY, T5_OBJECT_TASK,
                    g_visible_state.current_task_id);
    }
}

static void on_dismiss(lv_event_t *event)
{
    (void)event;
    send_action(T5_ACTION_DISMISS_NOTICE, T5_OBJECT_NOTICE,
                g_visible_state.notice.notice_id);
}

static void show_tasks(bool visible)
{
    g_tasks_visible = visible;
    if (visible) {
        lv_obj_add_flag(g_main_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_task_page, LV_OBJ_FLAG_HIDDEN);
        uart_transport_send_page_event(1, 1, 0);
    } else {
        lv_obj_add_flag(g_task_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_main_page, LV_OBJ_FLAG_HIDDEN);
        uart_transport_send_page_event(0, 1, 0);
    }
}

static void on_tasks(lv_event_t *event)
{
    (void)event;
    show_tasks(true);
}

static void on_main(lv_event_t *event)
{
    (void)event;
    show_tasks(false);
}

static void on_previous(lv_event_t *event)
{
    (void)event;
    if (g_task_offset >= 4) g_task_offset -= 4;
    uart_transport_send_page_event(1, 3, g_task_offset);
    panel_ui_post_state(&g_visible_state);
}

static void on_next(lv_event_t *event)
{
    (void)event;
    if ((uint32_t)g_task_offset + 4U < g_visible_state.task_count) {
        g_task_offset += 4;
    }
    uart_transport_send_page_event(1, 3, g_task_offset);
    panel_ui_post_state(&g_visible_state);
}

static void on_task_row(lv_event_t *event)
{
    uintptr_t row = (uintptr_t)lv_event_get_user_data(event);
    uint16_t index = (uint16_t)(g_task_offset + row);
    if (index < g_visible_state.task_count) {
        send_action(T5_ACTION_OPEN_TASK, T5_OBJECT_TASK,
                    g_visible_state.tasks[index].task_id);
    }
}

static void apply_palette(const display_state_t *state)
{
    lv_color_t background;
    lv_color_t foreground;
    const char *mode_text;
    switch (state->mode) {
    case T5_MODE_DAY_WORK:
        background = COLOR_DAY_BG;
        foreground = COLOR_DAY_FG;
        mode_text = "DAY WORK";
        break;
    case T5_MODE_NIGHT_EXEC:
        background = COLOR_NIGHT_BG;
        foreground = COLOR_NIGHT_FG;
        mode_text = "NIGHT EXEC";
        break;
    default:
        background = COLOR_IDLE_BG;
        foreground = COLOR_IDLE_FG;
        mode_text = "IDLE / WAITING";
        break;
    }
    lv_obj_set_style_bg_color(g_screen, background, 0);
    lv_obj_set_style_text_color(g_mode_label, foreground, 0);
    lv_obj_set_style_text_color(g_status_label, foreground, 0);
    lv_obj_set_style_text_color(g_task_label, foreground, 0);
    lv_label_set_text(g_mode_label, mode_text);
}

static void update_work(const display_state_t *state)
{
    static const char *const names[] = {
        "Stopped", "Starting", "Running", "Paused", "Completed", "Failed"
    };
    char buffer[96];
    const char *name = state->work_state <= T5_WORK_FAILED
                           ? names[state->work_state] : "Unknown";
    if (state->work_state == T5_WORK_RUNNING ||
        state->work_state == T5_WORK_PAUSED) {
        snprintf(buffer, sizeof(buffer), "%s  %u.%u%%  %lus",
                 name, state->progress_permille / 10,
                 state->progress_permille % 10,
                 (unsigned long)state->elapsed_seconds);
        lv_bar_set_value(g_progress,
                         state->progress_permille > 1000
                             ? 1000 : state->progress_permille,
                         LV_ANIM_ON);
        lv_obj_clear_flag(g_progress, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", name);
        lv_obj_add_flag(g_progress, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(g_status_label, buffer);
    lv_label_set_text(g_task_label,
                      state->current_task_title[0]
                          ? state->current_task_title : "No active task");
}

static void update_attention(const display_state_t *state)
{
    char buffer[160];
    if (state->attention_flags == 0) {
        lv_obj_add_flag(g_attention_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (state->attention_text[0]) {
        snprintf(buffer, sizeof(buffer), "ATTENTION: %s",
                 state->attention_text);
    } else if (state->attention_flags & T5_ATTN_SENSOR_ERROR) {
        snprintf(buffer, sizeof(buffer),
                 "ATTENTION: sensor/input unavailable");
    } else if (state->attention_flags & T5_ATTN_NEED_CONFIRM) {
        snprintf(buffer, sizeof(buffer),
                 "ATTENTION: %u item(s) need confirmation",
                 state->confirmation_count);
    } else {
        snprintf(buffer, sizeof(buffer), "ATTENTION: 0x%08lX",
                 (unsigned long)state->attention_flags);
    }
    lv_label_set_text(g_attention_label, buffer);
    lv_obj_clear_flag(g_attention_label, LV_OBJ_FLAG_HIDDEN);
}

static void update_dashboard(const display_state_t *state)
{
    char buffer[128];
    snprintf(buffer, sizeof(buffer),
             "DASHBOARD\nAuto  U:%u  N:%u\nConfirm  U:%u  N:%u\nToday  OK:%u  Fail:%u",
             state->urgent_auto, state->normal_auto,
             state->urgent_confirm, state->normal_confirm,
             state->completed_today, state->failed_today);
    lv_label_set_text(g_dashboard_label, buffer);
}

static void update_notice(const display_state_t *state)
{
    if (!state->notice.active) {
        lv_obj_add_flag(g_notice_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_color_t color = COLOR_DAY_FG;
    if (state->notice.severity == T5_NOTICE_WARNING) color = COLOR_WARNING;
    if (state->notice.severity >= T5_NOTICE_ERROR) color = COLOR_ATTENTION;
    lv_obj_set_style_border_color(g_notice_card, color, 0);
    lv_label_set_text(g_notice_title,
                      state->notice.title[0] ? state->notice.title : "Notice");
    lv_label_set_text(g_notice_body, state->notice.body);
    lv_obj_clear_flag(g_notice_card, LV_OBJ_FLAG_HIDDEN);
}

static void update_action_status(const display_state_t *state)
{
    if (state->action_pending) {
        lv_label_set_text(g_action_status, "Sending action...");
        return;
    }
    switch (state->last_action_status) {
    case T5_STATUS_OK:
        lv_label_set_text(g_action_status, "");
        break;
    case T5_STATUS_NOT_READY:
        lv_label_set_text(g_action_status, "Host did not acknowledge");
        break;
    case T5_STATUS_INTERNAL_ERR:
        lv_label_set_text(g_action_status, "Action send failed");
        break;
    default:
        lv_label_set_text(g_action_status, "Action rejected by host");
        break;
    }
}

static void update_controls(const display_state_t *state)
{
    bool ready = state->opi_online && !state->action_pending;
    bool confirmation = (state->attention_flags & T5_ATTN_NEED_CONFIRM) != 0;
    bool work_control = state->work_state == T5_WORK_RUNNING ||
                        state->work_state == T5_WORK_PAUSED ||
                        (state->work_state == T5_WORK_FAILED &&
                         state->current_task_id != 0);
    bool has_confirmation_task = confirmation_object_id() != 0;

    set_enabled(g_confirm_button,
                ready && confirmation && has_confirmation_task);
    set_enabled(g_reject_button,
                ready && confirmation && has_confirmation_task);
    set_enabled(g_work_button, ready && work_control);
    set_enabled(g_dismiss_button, ready && state->notice.active &&
                (state->notice.flags & T5_NOTICE_DISMISSIBLE));
    set_enabled(g_tasks_button, ready);

    lv_obj_t *work_label = lv_obj_get_child(g_work_button, 0);
    if (state->work_state == T5_WORK_RUNNING) {
        lv_label_set_text(work_label, "Pause");
    } else if (state->work_state == T5_WORK_PAUSED) {
        lv_label_set_text(work_label, "Resume");
    } else {
        lv_label_set_text(work_label, "Retry");
    }
}

static void update_task_page(const display_state_t *state)
{
    if (g_task_offset >= state->task_count) g_task_offset = 0;
    for (uint16_t row = 0; row < 4; ++row) {
        uint16_t index = (uint16_t)(g_task_offset + row);
        if (index < state->task_count) {
            char buffer[112];
            const display_task_t *task = &state->tasks[index];
            snprintf(buffer, sizeof(buffer), "Q%u  #%lu  %s",
                     task->quadrant, (unsigned long)task->task_id,
                     task->title[0] ? task->title : "(untitled)");
            lv_label_set_text(g_task_row_labels[row], buffer);
            lv_obj_clear_flag(g_task_rows[row], LV_OBJ_FLAG_HIDDEN);
            set_enabled(g_task_rows[row],
                        state->opi_online && !state->action_pending);
        } else {
            lv_obj_add_flag(g_task_rows[row], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char page[64];
    if (state->task_count == 0) {
        snprintf(page, sizeof(page), "No tasks");
    } else {
        snprintf(page, sizeof(page), "Tasks %u-%u of %u",
                 g_task_offset + 1,
                 (uint16_t)((g_task_offset + 4 < state->task_count)
                                ? g_task_offset + 4 : state->task_count),
                 state->task_count);
    }
    lv_label_set_text(g_task_page_label, page);
    set_enabled(g_prev_button, g_task_offset >= 4);
    set_enabled(g_next_button,
                (uint32_t)g_task_offset + 4U < state->task_count);
}

static void update_local_hardware(const display_state_t *state)
{
    if (g_display_handle &&
        g_applied_backlight != state->backlight_percent) {
        tdl_disp_set_brightness(g_display_handle,
                                state->backlight_percent);
        g_applied_backlight = state->backlight_percent;
    }
#if defined(LED_NAME) && defined(CONFIG_ENABLE_LED) && CONFIG_ENABLE_LED
    if (g_led_handle) {
        if (state->led_override_active) {
            tdl_led_set_status(g_led_handle,
                state->led_override_mode == 0 ? TDL_LED_OFF : TDL_LED_ON);
        } else if (state->attention_flags != 0) {
            TDL_LED_BLINK_CFG_T blink = {
                .cnt = TDL_BLINK_FOREVER,
                .start_stat = TDL_LED_ON,
                .first_half_cycle_time = 300,
                .latter_half_cycle_time = 300,
            };
            tdl_led_blink(g_led_handle, &blink);
        } else {
            tdl_led_set_status(g_led_handle,
                state->mode == T5_MODE_DAY_WORK ? TDL_LED_ON : TDL_LED_OFF);
        }
    }
#endif
}

static void apply_state(const display_state_t *state)
{
    g_visible_state = *state;
    apply_palette(state);
    update_work(state);
    update_attention(state);
    update_dashboard(state);
    update_notice(state);
    update_action_status(state);
    update_controls(state);
    update_task_page(state);
    update_local_hardware(state);

    if (state->opi_online) {
        lv_obj_add_flag(g_offline_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(g_offline_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_offline_overlay);
    }
}

static void pending_timer(lv_timer_t *timer)
{
    display_state_t state;
    bool dirty;
    (void)timer;
    tal_mutex_lock(g_pending_mutex);
    dirty = g_pending_dirty;
    if (dirty) {
        state = g_pending_state;
        g_pending_dirty = false;
    }
    tal_mutex_unlock(g_pending_mutex);
    if (dirty) apply_state(&state);
}

void panel_ui_post_state(const display_state_t *state)
{
    if (!state) return;
    tal_mutex_lock(g_pending_mutex);
    g_pending_state = *state;
    g_pending_dirty = true;
    tal_mutex_unlock(g_pending_mutex);
}

void panel_ui_init(void)
{
    tal_mutex_create_init(&g_pending_mutex);
    memset(&g_pending_state, 0, sizeof(g_pending_state));
    memset(&g_visible_state, 0, sizeof(g_visible_state));
    g_pending_dirty = false;
    g_task_offset = 0;
    g_tasks_visible = false;

    g_screen = lv_screen_active();
    lv_obj_remove_style_all(g_screen);
    lv_obj_set_size(g_screen, 480, 320);
    lv_obj_set_style_bg_color(g_screen, COLOR_IDLE_BG, 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);

    g_main_page = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_main_page);
    lv_obj_set_size(g_main_page, 480, 320);

    g_mode_label = make_label(g_main_page, &lv_font_montserrat_28,
                              12, 5, 300, LV_TEXT_ALIGN_LEFT);
    g_status_label = make_label(g_main_page, &lv_font_montserrat_16,
                                12, 42, 300, LV_TEXT_ALIGN_LEFT);
    g_task_label = make_label(g_main_page, &lv_font_montserrat_14,
                              12, 67, 300, LV_TEXT_ALIGN_LEFT);

    g_progress = lv_bar_create(g_main_page);
    lv_obj_set_pos(g_progress, 12, 91);
    lv_obj_set_size(g_progress, 300, 12);
    lv_bar_set_range(g_progress, 0, 1000);
    lv_obj_add_flag(g_progress, LV_OBJ_FLAG_HIDDEN);

    g_attention_label = make_label(g_main_page, &lv_font_montserrat_14,
                                    12, 111, 300, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_color(g_attention_label, COLOR_ATTENTION, 0);
    lv_label_set_long_mode(g_attention_label, LV_LABEL_LONG_WRAP);

    g_dashboard_label = make_label(g_main_page, &lv_font_montserrat_12,
                                    326, 12, 146, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_bg_color(g_dashboard_label, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(g_dashboard_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(g_dashboard_label, 7, 0);
    lv_obj_set_height(g_dashboard_label, 90);

    g_notice_card = lv_obj_create(g_main_page);
    lv_obj_set_pos(g_notice_card, 12, 151);
    lv_obj_set_size(g_notice_card, 460, 76);
    lv_obj_set_style_bg_color(g_notice_card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(g_notice_card, LV_OPA_80, 0);
    lv_obj_set_style_border_width(g_notice_card, 2, 0);
    lv_obj_set_style_pad_all(g_notice_card, 7, 0);
    g_notice_title = make_label(g_notice_card, &lv_font_montserrat_14,
                                0, 0, 438, LV_TEXT_ALIGN_LEFT);
    g_notice_body = make_label(g_notice_card, &lv_font_montserrat_12,
                               0, 23, 438, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(g_notice_body, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(g_notice_card, LV_OBJ_FLAG_HIDDEN);

    g_action_status = make_label(g_main_page, &lv_font_montserrat_12,
                                 12, 235, 460, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_color(g_action_status, COLOR_ATTENTION, 0);

    g_confirm_button = make_button(g_main_page, "Confirm",
                                   8, 267, 88, 44, on_confirm);
    g_reject_button = make_button(g_main_page, "Reject",
                                  102, 267, 88, 44, on_reject);
    g_work_button = make_button(g_main_page, "Retry",
                                196, 267, 88, 44, on_work);
    g_dismiss_button = make_button(g_main_page, "Dismiss",
                                   290, 267, 88, 44, on_dismiss);
    g_tasks_button = make_button(g_main_page, "Tasks",
                                 384, 267, 88, 44, on_tasks);

    g_task_page = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_task_page);
    lv_obj_set_size(g_task_page, 480, 320);
    lv_obj_add_flag(g_task_page, LV_OBJ_FLAG_HIDDEN);
    make_label(g_task_page, &lv_font_montserrat_28,
               12, 7, 220, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(lv_obj_get_child(g_task_page, 0), "TASK LIST");
    g_task_page_label = make_label(g_task_page, &lv_font_montserrat_14,
                                   240, 16, 228, LV_TEXT_ALIGN_RIGHT);

    for (uintptr_t row = 0; row < 4; ++row) {
        g_task_rows[row] = lv_button_create(g_task_page);
        lv_obj_set_pos(g_task_rows[row], 12, 51 + (int)row * 51);
        lv_obj_set_size(g_task_rows[row], 456, 43);
        lv_obj_set_style_bg_color(g_task_rows[row], COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(g_task_rows[row], LV_OPA_80, 0);
        g_task_row_labels[row] = make_label(
            g_task_rows[row], &lv_font_montserrat_14,
            7, 7, 424, LV_TEXT_ALIGN_LEFT);
        lv_obj_add_event_cb(g_task_rows[row], on_task_row,
                            LV_EVENT_CLICKED, (void *)row);
    }
    make_button(g_task_page, "Main", 12, 267, 120, 44, on_main);
    g_prev_button = make_button(g_task_page, "Previous",
                                180, 267, 120, 44, on_previous);
    g_next_button = make_button(g_task_page, "Next",
                                348, 267, 120, 44, on_next);

    g_offline_overlay = lv_obj_create(g_screen);
    lv_obj_set_pos(g_offline_overlay, 0, 0);
    lv_obj_set_size(g_offline_overlay, 480, 320);
    lv_obj_set_style_bg_color(g_offline_overlay, COLOR_OFFLINE, 0);
    lv_obj_set_style_bg_opa(g_offline_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g_offline_overlay, 0, 0);
    g_offline_label = make_label(g_offline_overlay,
                                 &lv_font_montserrat_28,
                                 20, 112, 440, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(g_offline_label, "HOST OFFLINE");
    lv_obj_t *detail = make_label(g_offline_overlay,
                                  &lv_font_montserrat_14,
                                  30, 162, 420, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(detail,
        "Controls are disabled.\nWaiting for UART heartbeat and resync.");

#if defined(DISPLAY_NAME)
    g_display_handle = tdl_disp_find_dev(DISPLAY_NAME);
#endif
#if defined(LED_NAME) && defined(CONFIG_ENABLE_LED) && CONFIG_ENABLE_LED
    g_led_handle = tdl_led_find_dev(LED_NAME);
    if (g_led_handle) tdl_led_open(g_led_handle);
#endif
    lv_timer_create(pending_timer, 50, NULL);
}
