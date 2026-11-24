#ifndef D133_DISPLAY_PROFILE_MANAGER_H
#define D133_DISPLAY_PROFILE_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#define DISPLAY_PROFILE_SLOT_LENGTH 6U
#define DISPLAY_PROFILE_CODE_LENGTH 25U
#define DISPLAY_PROFILE_PANEL_LENGTH 32U
#define DISPLAY_PROFILE_TOUCH_LENGTH 16
#define DISPLAY_PROFILE_MAX_CHOICES 14U

typedef enum {
    DISPLAY_PROFILE_OK = 0,
    DISPLAY_PROFILE_REBOOT_REQUIRED = 1,
    DISPLAY_PROFILE_ERR_ARGUMENT = -1,
    DISPLAY_PROFILE_ERR_NOT_CONFIGURED = -2,
    DISPLAY_PROFILE_ERR_SERIAL = -3,
    DISPLAY_PROFILE_ERR_ENV = -4,
    DISPLAY_PROFILE_ERR_LIMIT = -5,
    DISPLAY_PROFILE_ERR_CONFLICT = -6
} display_profile_result_t;

typedef struct {
    char code[DISPLAY_PROFILE_CODE_LENGTH];
    uint16_t width;
    uint16_t height;
    char panel[DISPLAY_PROFILE_PANEL_LENGTH];
    char touch[DISPLAY_PROFILE_TOUCH_LENGTH];
    char touch_device[DISPLAY_PROFILE_TOUCH_LENGTH];
    uint8_t touch_enabled;
} display_profile_info_t;

typedef struct {
    char code[DISPLAY_PROFILE_CODE_LENGTH];
    char slot[DISPLAY_PROFILE_SLOT_LENGTH];
} display_profile_choice_t;

/* 列出可选择的内置与已登记屏幕，供本机界面和 USB 只读核对。 */
size_t display_profile_choices(display_profile_choice_t *choices, size_t capacity);

/* 启动 UART0 屏幕代号服务；串口只发送协议应答，不承载控制台日志。 */
int display_profile_manager_init(void);
/* 更新官方 ENV 选择项；切换显示模式需要重启后由 panel probe 读取。 */
display_profile_result_t display_profile_apply(const char *code);
int display_profile_code_valid(const char *code);
int display_profile_custom_code(const char *code);
const char *display_profile_ic(const char *slot);
const char *display_profile_active_slot(void);
/* 仅在 USB 配置包验签和配对校验后登记。 */
display_profile_result_t display_profile_register(const char *code, const char *slot);
display_profile_result_t display_profile_get_active(display_profile_info_t *profile);
/* 查找当前固件包含的稳定硬件槽及其配套驱动。 */
display_profile_result_t display_profile_get_configured(const char *code, display_profile_info_t *profile);
/* 健康检查和 LVGL 输入适配使用实际触摸探测状态。 */
int display_profile_touch_enabled(void);

#endif
