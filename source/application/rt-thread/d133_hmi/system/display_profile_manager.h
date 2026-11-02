#ifndef D133_DISPLAY_PROFILE_MANAGER_H
#define D133_DISPLAY_PROFILE_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#define DISPLAY_PROFILE_CODE_LENGTH 6
#define DISPLAY_PROFILE_PANEL_LENGTH 24
#define DISPLAY_PROFILE_TOUCH_LENGTH 16

typedef enum {
    DISPLAY_PROFILE_OK = 0,
    DISPLAY_PROFILE_ERR_ARGUMENT = -1,
    DISPLAY_PROFILE_ERR_NOT_CONFIGURED = -2,
    DISPLAY_PROFILE_ERR_SERIAL = -3
} display_profile_result_t;

typedef struct {
    char code[DISPLAY_PROFILE_CODE_LENGTH];
    uint16_t width;
    uint16_t height;
    char panel[DISPLAY_PROFILE_PANEL_LENGTH];
    char touch[DISPLAY_PROFILE_TOUCH_LENGTH];
} display_profile_info_t;

/* 启动 UART0 屏幕代号服务；串口只发送协议应答，不承载控制台日志。 */
int display_profile_manager_init(void);
/* 当前固件只固化 CS000；未配置的代号会返回明确错误，不会假切换。 */
display_profile_result_t display_profile_apply(const char *code);
display_profile_result_t display_profile_get_active(display_profile_info_t *profile);

#endif
