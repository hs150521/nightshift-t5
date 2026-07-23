/**
 * @file ui_manager.h
 * @brief LVGL-based UI manager for Nightshift T5 firmware.
 *
 * Provides a three-mode display (IDLE / DAY_WORK / NIGHT_EXEC) with
 * offline overlay and LED feedback.  Call ui_manager_init() after
 * lv_vendor_init(), and ui_manager_update() on every state change
 * (inside a lv_vendor_disp_lock/unlock bracket).
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "app_state.h"

/**
 * @brief Create all LVGL objects (pages, labels, progress bar, overlay).
 * @note  Must be called after lv_vendor_init() and within a
 *        lv_vendor_disp_lock() / lv_vendor_disp_unlock() bracket.
 */
void ui_manager_init(void);

/**
 * @brief Refresh the UI to reflect the current application state.
 * @param state  Read-only pointer to the authoritative state snapshot.
 * @note  Must be called within a lv_vendor_disp_lock() / lv_vendor_disp_unlock() bracket.
 */
void ui_manager_update(const app_state_t *state);

#endif /* UI_MANAGER_H */
