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

    switch (event)
    {
    case DEVICE_EVENT_TOOLBOX_TCP_CONNECTED:
        name = "toolbox_tcp_connected";
        message = "工具箱 TCP 连接已建立，D133 OTA 服务就绪；已请求蜂鸣器鸣响 1 秒";
        break;
    case DEVICE_EVENT_TOOLBOX_OTA_STARTED:
        name = "toolbox_ota_started";
        message = "工具箱 OTA 升级已开始，设备正在接收 ota.cpio；已请求蜂鸣器鸣响 1 秒";
        break;
    default:
        return;
    }

    if (!event_ready)
        return;

    rt_mutex_take(&event_lock, RT_WAITING_FOREVER);
    rt_strncpy(last_event_name, name, sizeof(last_event_name) - 1);
    last_event_name[sizeof(last_event_name) - 1] = '\0';
    rt_strncpy(last_event_message, message, sizeof(last_event_message) - 1);
    last_event_message[sizeof(last_event_message) - 1] = '\0';
    event_sequence++;
    rt_mutex_release(&event_lock);

    /* 日志可在设备“系统日志”页查看；PWM 操作由蜂鸣工作线程执行。 */
    system_manager_log(message);
    if (buzzer_manager_beep(1000) != BUZZER_MANAGER_OK)
        system_manager_log("蜂鸣器提示音请求未能排队");
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

