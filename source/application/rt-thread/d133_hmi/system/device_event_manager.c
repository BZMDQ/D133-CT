#include "device_event_manager.h"

#include "buzzer_manager.h"
#include "system_manager.h"

#include <string.h>

static struct rt_mutex event_lock;
static rt_bool_t event_ready;
static rt_uint32_t event_sequence;
static char last_event_name[32];
static char last_event_message[192];

int device_event_manager_init(void)
{
    int buzzer_result;

    if (event_ready)
        return RT_EOK;

    rt_mutex_init(&event_lock, "dev_evt", RT_IPC_FLAG_PRIO);
    event_ready = RT_TRUE;

    /* 蜂鸣器不可用时保留设备消息和 OTA/TCP 服务，不阻断 HMI 启动。 */
    buzzer_result = buzzer_manager_init();
    if (buzzer_result != BUZZER_MANAGER_OK)
        system_manager_log("蜂鸣器 PWM1 初始化失败；设备消息服务仍可用");

    return RT_EOK;
}

void device_event_manager_notify(device_event_type_t event)
{
    const char *name;
    const char *message;
    const char *buzzer_message;
    char event_message[sizeof(last_event_message)];
    int buzzer_result;

    switch (event)
    {
    case DEVICE_EVENT_TOOLBOX_TCP_CONNECTED:
        name = "toolbox_tcp_connected";
        message = "工具箱 TCP 连接已建立，D133 OTA 服务就绪";
        break;
    case DEVICE_EVENT_TOOLBOX_OTA_STARTED:
        name = "toolbox_ota_started";
        message = "工具箱 OTA 升级已开始，设备正在接收 ota.cpio";
        break;
    default:
        return;
    }

    if (!event_ready)
        return;

    /* 对外事件仍按原顺序发布，蜂鸣器状态与本次提交结果一致。 */
    buzzer_result = buzzer_manager_beep(1000);
    if (buzzer_result == BUZZER_MANAGER_OK)
        buzzer_message = "已请求蜂鸣器鸣响 1 秒";
    else if (buzzer_result == BUZZER_MANAGER_MUTED)
        buzzer_message = "蜂鸣器已静音";
    else
        buzzer_message = "蜂鸣器提示音请求未能排队";
    rt_snprintf(event_message, sizeof(event_message), "%s；%s", message, buzzer_message);

    rt_mutex_take(&event_lock, RT_WAITING_FOREVER);
    rt_strncpy(last_event_name, name, sizeof(last_event_name) - 1);
    last_event_name[sizeof(last_event_name) - 1] = '\0';
    rt_strncpy(last_event_message, event_message, sizeof(last_event_message) - 1);
    last_event_message[sizeof(last_event_message) - 1] = '\0';
    event_sequence++;
    rt_mutex_release(&event_lock);

    system_manager_log(event_message);
}

void device_event_manager_snapshot(char *event_name, rt_size_t event_name_size,
                                   char *message, rt_size_t message_size,
                                   rt_uint32_t *sequence)
{
    if (event_name_size)
        event_name[0] = '\0';
    if (message_size)
        message[0] = '\0';
    if (sequence)
        *sequence = 0;
    if (!event_ready)
        return;

    rt_mutex_take(&event_lock, RT_WAITING_FOREVER);
    if (event_name_size)
    {
        rt_strncpy(event_name, last_event_name, event_name_size - 1);
        event_name[event_name_size - 1] = '\0';
    }
    if (message_size)
    {
        rt_strncpy(message, last_event_message, message_size - 1);
        message[message_size - 1] = '\0';
    }
    if (sequence)
        *sequence = event_sequence;
    rt_mutex_release(&event_lock);
}

