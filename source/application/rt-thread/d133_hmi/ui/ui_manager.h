#ifndef HMI_UI_MANAGER_H
#define HMI_UI_MANAGER_H
/* 唯一入口由官方 LVGL 线程的 lv_user_gui_init 调用。 */
void hmi_ui_init(void);
/* 工作线程仅发布标志；LVGL 线程负责触发定时器。 */
void hmi_ui_image_signal(void);
void hmi_ui_image_dispatch(void);
/* 通信线程交接到 LVGL。0..8 为直接选择画面，其余是测试操作。 */
enum {
    HMI_TEST_EXIT = 9,
    HMI_TEST_PREVIOUS,
    HMI_TEST_NEXT,
    HMI_TEST_TOGGLE_MODE,
    HMI_TEST_INTERVAL_MIN_MS = 500,
    HMI_TEST_INTERVAL_MAX_MS = 10000,
    HMI_TEST_INTERVAL_STEP_MS = 500
};
/* 轮播间隔以毫秒提交，与画面测试操作共用一个交接槽。 */
int hmi_ui_remote_test(int target);
#endif
