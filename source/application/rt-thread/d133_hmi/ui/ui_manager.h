#ifndef HMI_UI_MANAGER_H
#define HMI_UI_MANAGER_H
/* 唯一入口由官方 LVGL 线程的 lv_user_gui_init 调用。 */
void hmi_ui_init(void);
/* 工作线程仅发布标志；LVGL 线程负责触发定时器。 */
void hmi_ui_image_signal(void);
void hmi_ui_image_dispatch(void);
#endif
