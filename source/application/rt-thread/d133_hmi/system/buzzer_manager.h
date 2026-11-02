#ifndef D133_BUZZER_MANAGER_H
#define D133_BUZZER_MANAGER_H

#include <rtthread.h>

/* 蜂鸣器服务返回码，0 表示请求已经进入异步队列。 */
enum
{
    BUZZER_MANAGER_OK = 0,
    BUZZER_MANAGER_ERR_NOT_READY = -1,
    BUZZER_MANAGER_ERR_BAD_DURATION = -2,
    BUZZER_MANAGER_ERR_QUEUE_FULL = -3,
};

int buzzer_manager_init(void);
int buzzer_manager_beep(rt_uint32_t duration_ms);

#endif

