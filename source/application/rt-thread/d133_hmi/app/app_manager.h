#ifndef HMI_APP_MANAGER_H
#define HMI_APP_MANAGER_H
int app_manager_init(void);
int app_manager_ready(void);
/* 返回 app_manager 初始化后的毫秒数，用于对照 LVGL 首帧启动标记。 */
unsigned int app_manager_boot_elapsed_ms(void);
#endif

