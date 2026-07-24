/**
 * @file main.c
 * @brief Nightshift T5 firmware — application entry point.
 *
 * Initialises logging, board hardware, application state, UART transport,
 * the LVGL display stack, and the UI manager.  Follows the same lifecycle
 * pattern as the SDK's lvgl_demo example.
 */

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"

#include "nightshift_config.h"
#include "app_state.h"
#include "uart_handler.h"
#include "ui_manager.h"

/* =========================================================================
 * State-change callback — bridges app_state → UI refresh
 * ======================================================================= */
static void on_state_change(const app_state_t *state)
{
    lv_vendor_disp_lock();
    ui_manager_update(state);
    lv_vendor_disp_unlock();
}

/* =========================================================================
 * user_main — called from the RTOS thread wrapper (or directly on Linux)
 * ======================================================================= */
void user_main(void)
{
    /* 1. Logging */
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("===== Nightshift T5 =====");
    PR_NOTICE("Firmware:   %s", NIGHTSHIFT_FW_VERSION);
    PR_NOTICE("Platform:   %s", PLATFORM_BOARD);
    PR_NOTICE("Compile:    %s %s", __DATE__, __TIME__);

    /* 2. Register board hardware (LED, button, audio, LCD, touch) */
    PR_NOTICE("board_register_hardware...");
    OPERATE_RET hw_ret = board_register_hardware();
    if (hw_ret != OPRT_OK) {
        PR_ERR("board_register_hardware failed: %d", hw_ret);
    }

    /* 3. Application state mirror */
    app_state_init();
    app_state_register_change_cb(on_state_change);

    /* 4. UART transport — starts the RX task */
    int uart_ret = uart_handler_init();
    uart_debug_puts("\r\n[NS] UART0 init: ");
    uart_debug_puts(uart_ret == 0 ? "OK\r\n" : "FAIL\r\n");

    /* 5. Display driver */
#if defined(DISPLAY_NAME)
    uart_debug_puts("[NS] Display init: ");
    lv_vendor_init(DISPLAY_NAME);
    uart_debug_puts("OK\r\n");
#else
    PR_WARN("DISPLAY_NAME not defined — skipping display init");
    uart_debug_puts("[NS] DISPLAY_NAME not defined!\r\n");
#endif

    /* 6. Create UI widgets (must hold the display lock) */
    uart_debug_puts("[NS] UI init: ");
    lv_vendor_disp_lock();
    ui_manager_init();
    lv_vendor_disp_unlock();
    uart_debug_puts("OK\r\n");

    /* 7. Start LVGL rendering task (priority=3, stack=8KB) */
#if defined(DISPLAY_NAME)
    uart_debug_puts("[NS] LVGL render start: ");
    lv_vendor_start(3, 1024 * 8);
    uart_debug_puts("OK\r\n");
#endif

    PR_NOTICE("Nightshift T5 started, waiting for OPi connection...");
    uart_debug_puts("[NS] user_main complete\r\n");

    /* Keep main thread alive — deleting it orphans LVGL/UART tasks.
     * Also send a periodic beacon on UART0 so the host can confirm
     * the firmware is running. */
    for (;;) {
        tal_system_sleep(5000);
        uart_debug_puts("[NS] alive\r\n");
    }
}

/* =========================================================================
 * Platform entry points
 * ======================================================================= */
#if OPERATING_SYSTEM != SYSTEM_LINUX

/* RTOS: wrap user_main in a dedicated thread so the SDK scheduler is free. */
static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    (void)arg;
    uart_debug_puts("[NS] tuya_app_thread started\r\n");
    user_main();
    uart_debug_puts("[NS] tuya_app_thread: user_main returned\r\n");
    /* Don't delete ourselves — stay alive so LVGL/UART tasks aren't orphaned */
}

void tuya_app_main(void)
{
    THREAD_CFG_T cfg = {0};
    cfg.stackDepth = 1024 * 4;
    cfg.priority   = THREAD_PRIO_1;
    cfg.thrdname   = "nightshift_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL,
                                tuya_app_thread, NULL, &cfg);
}

#else

/* Linux simulation build */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    user_main();
    while (1) {
        tal_system_sleep(500);
    }
    return 0;
}

#endif
