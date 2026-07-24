/**
 * @file firmware_main.c
 * @brief Nightshift panel startup. Protocol UART0 never carries text logs.
 */

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"

#include "nightshift_config.h"
#include "panel_ui.h"
#include "state_store.h"
#include "uart_transport.h"

static void on_state_change(const display_state_t *snapshot)
{
    /* This only copies into a one-slot queue. LVGL runs in its own context. */
    panel_ui_post_state(snapshot);
}

void user_main(void)
{
    /*
     * SDK output is configured for UART1/mailbox in this T5AI build.
     * UART0 (P10/P11) is exclusively owned by uart_transport.
     */
    tal_log_init(TAL_LOG_LEVEL_INFO, 2048,
                 (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("Nightshift panel %s", NIGHTSHIFT_FW_VERSION);

    OPERATE_RET hardware_result = board_register_hardware();
    if (hardware_result != OPRT_OK) {
        PR_ERR("board_register_hardware failed: %d", hardware_result);
    }

    state_store_init();

#if defined(DISPLAY_NAME)
    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_disp_lock();

    /*
     * The ILI9488 driver registers its native memory geometry as 320x480.
     * Nightshift is a 480x320 landscape UI, so rotate the LVGL display before
     * creating any objects. LVGL applies the same transform to pointer input,
     * keeping GT1151 touch coordinates aligned with the rendered controls.
     */
    lv_display_t *display = lv_display_get_default();
    if (display != NULL) {
        lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    }

    panel_ui_init();
    lv_vendor_disp_unlock();
    lv_vendor_start(3, 1024 * 8);
#else
    PR_ERR("DISPLAY_NAME is not configured");
#endif

    state_store_register_change_cb(on_state_change);
    display_state_t initial_state;
    state_store_snapshot(&initial_state);
    panel_ui_post_state(&initial_state);

    int uart_result = uart_transport_init();
    if (uart_result != 0) {
        PR_ERR("protocol UART0 initialization failed: %d", uart_result);
    } else {
        PR_NOTICE("T5-Link UART0 ready at %d baud",
                  NIGHTSHIFT_UART_BAUDRATE);
    }

    for (;;) {
        tal_system_sleep(1000);
    }
}

#if OPERATING_SYSTEM != SYSTEM_LINUX

static THREAD_HANDLE g_app_thread;

static void app_thread(void *arg)
{
    (void)arg;
    user_main();
}

void tuya_app_main(void)
{
    THREAD_CFG_T config = {0};
    config.stackDepth = 1024 * 5;
    config.priority = THREAD_PRIO_1;
    config.thrdname = "nightshift_main";
    tal_thread_create_and_start(&g_app_thread, NULL, NULL,
                                app_thread, NULL, &config);
}

#else

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    user_main();
    return 0;
}

#endif
