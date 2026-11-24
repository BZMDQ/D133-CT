#ifndef HMI_SCREEN_TEST_H
#define HMI_SCREEN_TEST_H

#include "lvgl.h"

typedef void (*screen_test_exit_cb_t)(void *context);

/* 在活动显示器上打开全屏测试序列；所有控件只在 LVGL 线程创建。 */
int screen_test_start(lv_obj_t *parent,
                      screen_test_exit_cb_t on_exit, void *context);
/* 由 LVGL 线程调用，尽快处理图片工作线程的状态变化。 */
void screen_test_image_ready(void);
/* 启动全局按键采样；回调在 LVGL 线程执行。 */
void screen_test_keys_start(void (*on_enter)(void));

int screen_test_is_active(void);
/* 只允许 LVGL 线程执行页码与 HMI_TEST_* 操作。 */
void screen_test_remote_select(unsigned target);
/* USB 状态查询可跨线程读取模式，不访问 LVGL 控件。 */
int screen_test_auto_enabled(void);
#endif
