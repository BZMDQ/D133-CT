#ifndef D133_BUZZER_MANAGER_H
#define D133_BUZZER_MANAGER_H

#include <rtthread.h>

/* 蜂鸣器服务返回码，鸣响请求静音时会被主动忽略。 */
enum
{
    BUZZER_MANAGER_OK = 0,
    BUZZER_MANAGER_MUTED = 1,
    BUZZER_MANAGER_ERR_NOT_READY = -1,
    BUZZER_MANAGER_ERR_BAD_DURATION = -2,
    BUZZER_MANAGER_ERR_QUEUE_FULL = -3,
    BUZZER_MANAGER_ERR_BAD_SETTINGS = -4,
    BUZZER_MANAGER_ERR_PWM = -5,
};

enum
{
    BUZZER_MANAGER_SAVE_IDLE = 0,
    BUZZER_MANAGER_SAVE_PENDING,
    BUZZER_MANAGER_SAVE_OK,
    BUZZER_MANAGER_SAVE_ERROR,
};

int buzzer_manager_init(void);
int buzzer_manager_beep(rt_uint32_t duration_ms);

/* 返回当前设置，两个输出指针均可为空。 */
void buzzer_manager_get_settings(unsigned *enabled, unsigned *volume);
/* 开关取 0 或 1，音量取 0–100。返回成功表示立即生效，保存状态另查。 */
int buzzer_manager_set_settings(unsigned enabled, unsigned volume);
int buzzer_manager_settings_save_state(void);

#endif

