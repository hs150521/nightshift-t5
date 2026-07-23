/**
 * @file ui_manager.c
 * @brief LVGL UI: three-mode pages, offline overlay, and LED feedback.
 */

#include "ui_manager.h"
#include "app_state.h"
#include "nightshift_config.h"
#include "t5_protocol.h"

#include "lvgl.h"
#include "lv_vendor.h"
#include "tal_api.h"

#include <stdio.h>
#include <string.h>

/* =========================================================================
 * LED support (optional — single-GPIO on TUYA_T5AI_BOARD)
 * ======================================================================= */
#if defined(LED_NAME)
#include "tdl_led_manage.h"
#endif

/* =========================================================================
 * Colour palette (LVGL lv_color_hex)
 * ======================================================================= */
/* IDLE — warm orange-ish */
#define CLR_IDLE_BG     lv_color_hex(0xFFF3E0)
#define CLR_IDLE_FG     lv_color_hex(0x5D4037)

/* DAY_WORK — cool blue-white */
#define CLR_DAY_BG      lv_color_hex(0xE3F2FD)
#define CLR_DAY_FG      lv_color_hex(0x1565C0)

/* NIGHT_EXEC — deep blue */
#define CLR_NIGHT_BG    lv_color_hex(0x0D47A1)
#define CLR_NIGHT_FG    lv_color_hex(0xFFFFFF)

/* Offline overlay */
#define CLR_OVERLAY_BG  lv_color_hex(0x424242)

/* =========================================================================
 * Static LVGL objects
 * ======================================================================= */
static lv_obj_t *scr_main;
static lv_obj_t *lbl_mode;
static lv_obj_t *lbl_status;
static lv_obj_t *lbl_task;
static lv_obj_t *bar_progress;
static lv_obj_t *overlay_offline;
static lv_obj_t *lbl_offline;

#if defined(LED_NAME)
static TDL_LED_HANDLE_T sg_led_hdl = NULL;
#endif

/* =========================================================================
 * Work-state labels (indexed by T5_WORK_* values)
 * ======================================================================= */
static const char *const ws_labels[] = {
    [T5_WORK_STOPPED]   = "已停止",
    [T5_WORK_STARTING]  = "启动中…",
    [T5_WORK_RUNNING]   = "执行中",
    [T5_WORK_PAUSED]    = "已暂停",
    [T5_WORK_COMPLETED] = "已完成",
    [T5_WORK_FAILED]    = "失败",
};
#define WS_LABEL_COUNT (sizeof(ws_labels) / sizeof(ws_labels[0]))

/* =========================================================================
 * Helpers — apply mode colours
 * ======================================================================= */
static void apply_mode_colours(uint8_t mode)
{
    lv_color_t bg, fg;
    switch (mode) {
    case T5_MODE_DAY_WORK:
        bg = CLR_DAY_BG;   fg = CLR_DAY_FG;   break;
    case T5_MODE_NIGHT_EXEC:
        bg = CLR_NIGHT_BG; fg = CLR_NIGHT_FG;  break;
    default: /* T5_MODE_IDLE */
        bg = CLR_IDLE_BG;  fg = CLR_IDLE_FG;   break;
    }

    lv_obj_set_style_bg_color(scr_main, bg, 0);
    lv_obj_set_style_text_color(lbl_mode,   fg, 0);
    lv_obj_set_style_text_color(lbl_status, fg, 0);
    lv_obj_set_style_text_color(lbl_task,   fg, 0);
}

/* =========================================================================
 * Helpers — LED update
 *
 * The TUYA_T5AI_BOARD LED is single-colour GPIO (pin 1, active HIGH).
 * Breathing requires a PWM-capable driver and will gracefully fall back
 * to plain on/off if unsupported.
 * ======================================================================= */
static void update_led(const app_state_t *state)
{
#if defined(LED_NAME)
    if (!sg_led_hdl) return;

    /* Attention active → try breathing pattern, else just ON */
    if (state->attention_flags != 0) {
        TDL_LED_BREATH_CFG_T breath = {
            .period_ms = 1500,
            .min_level = 0,
            .max_level = 100,
            .cnt       = TDL_BLINK_FOREVER,
        };
        OPERATE_RET rt = tdl_led_breath(sg_led_hdl, &breath);
        if (rt == OPRT_NOT_SUPPORTED) {
            /* GPIO LED — fall back to fast blink */
            TDL_LED_BLINK_CFG_T blink = {
                .cnt                    = TDL_BLINK_FOREVER,
                .start_stat             = TDL_LED_ON,
                .first_half_cycle_time  = 300,
                .latter_half_cycle_time = 300,
            };
            tdl_led_blink(sg_led_hdl, &blink);
        }
        return;
    }

    /* No attention — steady state */
    switch (state->mode) {
    case T5_MODE_NIGHT_EXEC:
        /* Night: LED off (minimise light pollution) */
        tdl_led_set_status(sg_led_hdl, TDL_LED_OFF);
        break;
    case T5_MODE_DAY_WORK:
        /* Day: LED on full */
        tdl_led_set_status(sg_led_hdl, TDL_LED_ON);
        break;
    default: /* IDLE */
        /* Idle: slow blink to indicate standby */
        {
            TDL_LED_BLINK_CFG_T blink = {
                .cnt                    = TDL_BLINK_FOREVER,
                .start_stat             = TDL_LED_ON,
                .first_half_cycle_time  = 1000,
                .latter_half_cycle_time = 1000,
            };
            tdl_led_blink(sg_led_hdl, &blink);
        }
        break;
    }
#else
    (void)state;
#endif
}

/* =========================================================================
 * ui_manager_init
 * ======================================================================= */
void ui_manager_init(void)
{
#if defined(LED_NAME)
    sg_led_hdl = tdl_led_find_dev(LED_NAME);
    if (sg_led_hdl) {
        tdl_led_open(sg_led_hdl);
    }
#endif

    /* --- Main screen (full-size container) --- */
    scr_main = lv_scr_act();
    lv_obj_set_style_bg_color(scr_main, CLR_IDLE_BG, 0);
    lv_obj_set_style_bg_opa(scr_main, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_main, 12, 0);
    lv_obj_set_flex_flow(scr_main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr_main,
                          LV_FLEX_ALIGN_START,   /* main axis  */
                          LV_FLEX_ALIGN_CENTER,  /* cross axis */
                          LV_FLEX_ALIGN_CENTER); /* track      */

    /* --- Mode label (large, centred top) --- */
    lbl_mode = lv_label_create(scr_main);
    lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_mode, "正在连接主机...");
    lv_obj_set_width(lbl_mode, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_mode, LV_TEXT_ALIGN_CENTER, 0);

    /* --- Status label (smaller, below mode) --- */
    lbl_status = lv_label_create(scr_main);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_status, "");
    lv_obj_set_width(lbl_status, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);

    /* --- Task label (small) --- */
    lbl_task = lv_label_create(scr_main);
    lv_obj_set_style_text_font(lbl_task, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl_task, "");
    lv_obj_set_width(lbl_task, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_task, LV_TEXT_ALIGN_CENTER, 0);

    /* --- Progress bar (hidden initially) --- */
    bar_progress = lv_bar_create(scr_main);
    lv_bar_set_range(bar_progress, 0, 1000);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_width(bar_progress, LV_PCT(80));
    lv_obj_set_height(bar_progress, 16);
    lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);

    /* --- Offline overlay (full-screen, hidden) --- */
    overlay_offline = lv_obj_create(scr_main);
    lv_obj_set_size(overlay_offline, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay_offline, CLR_OVERLAY_BG, 0);
    lv_obj_set_style_bg_opa(overlay_offline, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay_offline, 0, 0);
    lv_obj_set_style_pad_all(overlay_offline, 0, 0);
    lv_obj_center(overlay_offline);
    lv_obj_add_flag(overlay_offline, LV_OBJ_FLAG_HIDDEN);

    lbl_offline = lv_label_create(overlay_offline);
    lv_obj_set_style_text_font(lbl_offline, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_offline, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_offline, "主机离线");
    lv_obj_center(lbl_offline);
}

/* =========================================================================
 * ui_manager_update
 * ======================================================================= */
void ui_manager_update(const app_state_t *state)
{
    if (!state) return;

    /* ---- Offline overlay ---- */
    if (!state->opi_online) {
        lv_obj_clear_flag(overlay_offline, LV_OBJ_FLAG_HIDDEN);
        update_led(state);
        return;
    }
    lv_obj_add_flag(overlay_offline, LV_OBJ_FLAG_HIDDEN);

    /* ---- Mode colours & label ---- */
    apply_mode_colours(state->mode);

    switch (state->mode) {
    case T5_MODE_DAY_WORK:
        lv_label_set_text(lbl_mode, "工作中");      break;
    case T5_MODE_NIGHT_EXEC:
        lv_label_set_text(lbl_mode, "夜间执行");    break;
    default:
        lv_label_set_text(lbl_mode, "待命");         break;
    }

    /* ---- Status label & progress bar ---- */
    if (state->attention_flags & T5_ATTN_NEED_CONFIRM) {
        char buf[64];
        snprintf(buf, sizeof(buf), "待确认: %u项", state->confirmation_count);
        lv_label_set_text(lbl_status, buf);
        lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
    } else if (state->work_state == T5_WORK_RUNNING) {
        char buf[64];
        /* progress_permille is 0–1000; display as XX.X% */
        unsigned pct_x10 = state->progress_permille;
        snprintf(buf, sizeof(buf), "执行中 %u.%u%%", pct_x10 / 10, pct_x10 % 10);
        lv_label_set_text(lbl_status, buf);
        lv_bar_set_value(bar_progress, state->progress_permille, LV_ANIM_ON);
        lv_obj_clear_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *txt = "—";
        if (state->work_state < WS_LABEL_COUNT) {
            txt = ws_labels[state->work_state];
        }
        lv_label_set_text(lbl_status, txt);
        lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- Task title ---- */
    if (state->current_task_title[0] != '\0') {
        lv_label_set_text(lbl_task, state->current_task_title);
    } else {
        lv_label_set_text(lbl_task, "");
    }

    /* ---- LED ---- */
    update_led(state);
}
