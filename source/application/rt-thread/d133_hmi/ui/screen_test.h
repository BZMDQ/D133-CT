#ifndef HMI_SCREEN_TEST_H
#define HMI_SCREEN_TEST_H

#include "lvgl.h"

typedef void (*screen_test_exit_cb_t)(void *context);

/* 在活动显示器上打开全屏测试序列；所有控件只在 LVGL 线程创建。 */
int screen_test_start(lv_obj_t *parent, const lv_font_t *font,
                      screen_test_exit_cb_t on_exit, void *context);
/* 由 LVGL 线程调用，尽快处理图片工作线程的状态变化。 */
void screen_test_image_ready(void);

#endif
