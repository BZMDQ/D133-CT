#include "display_profile_manager.h"
#include "system_manager.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <env.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_PROFILE_UART_NAME "uart0"
#define DISPLAY_PROFILE_UART_BAUD 115200
#define DISPLAY_PROFILE_RX_CHUNK 32
#define DISPLAY_PROFILE_RX_QUEUE 8
#define DISPLAY_PROFILE_THREAD_STACK 3072

typedef struct {
    rt_device_t device;
    rt_size_t size;
} profile_rx_message_t;

static struct rt_mutex profile_lock;
static rt_mq_t rx_messages;
static rt_device_t profile_uart;
static int profile_lock_ready;
static int manager_ready;
static display_profile_info_t active_profile = {
    "CS000", 1024, 600, "LVDS-1024x600", "GT911", 1
};

static int profile_info_from_code(const char *code, display_profile_info_t *profile)
{
    if (!code || !profile)
        return -1;
    memset(profile, 0, sizeof(*profile));
    if (strcmp(code, "CS000") == 0) {
        rt_snprintf(profile->code, sizeof(profile->code), "CS000");
        profile->width = 1024;
        profile->height = 600;
        rt_snprintf(profile->panel, sizeof(profile->panel), "LVDS-1024x600");
        rt_snprintf(profile->touch, sizeof(profile->touch), "GT911");
        profile->touch_enabled = 1;
        return 0;
    }
    if (strcmp(code, "CS001") == 0) {
        /* 1280x800 为 EK79202DA 芯片资料给出的候选显示配置；触摸暂未接入。 */
        rt_snprintf(profile->code, sizeof(profile->code), "CS001");
        profile->width = 1280;
        profile->height = 800;
        rt_snprintf(profile->panel, sizeof(profile->panel), "EK79202DA-MIPI");
        rt_snprintf(profile->touch, sizeof(profile->touch), "DISABLED");
        profile->touch_enabled = 0;
        return 0;
    }
    return -1;
}

static void profile_load_from_env(void)
{
    char *code;
    display_profile_info_t profile;

    if (fw_env_open() != 0)
        return;
    code = fw_getenv("hmi_screen");
    if (code && profile_info_from_code(code, &profile) == 0)
        active_profile = profile;
    fw_env_close();
}

static void profile_uart_send(const char *text)
{
    size_t length;

    if (!profile_uart || !text)
        return;
    length = strlen(text);
    if (rt_device_write(profile_uart, 0, text, length) != length)
        system_manager_log("屏幕配置串口应答发送失败");
}

static void profile_command_reply(const char *command)
{
    char response[128];
    display_profile_info_t active;
    display_profile_info_t target;
    display_profile_result_t result;

    result = display_profile_apply(command);
    display_profile_get_active(&active);
    if (result == DISPLAY_PROFILE_OK) {
        rt_snprintf(response, sizeof(response),
                    "%s,OK,ACTIVE=%s,RES=%ux%u,TOUCH=%s\r\n",
                    command, active.code, active.width, active.height, active.touch);
    } else if (result == DISPLAY_PROFILE_REBOOT_REQUIRED &&
               profile_info_from_code(command, &target) == 0) {
        rt_snprintf(response, sizeof(response),
                    "%s,OK,REBOOTING,RES=%ux%u,TOUCH=%s\r\n",
                    command, target.width, target.height, target.touch);
        profile_uart_send(response);
        /* 给 UART 发完应答的时间，再重启让早期 panel probe 采用新 timing。 */
        rt_thread_mdelay(100);
        rt_hw_cpu_reset();
        while (1)
            rt_thread_mdelay(1000);
    } else if (result == DISPLAY_PROFILE_ERR_NOT_CONFIGURED) {
        rt_snprintf(response, sizeof(response),
                    "%s,ERR,NOT_CONFIGURED,ACTIVE=%s\r\n",
                    command, active.code);
    } else if (result == DISPLAY_PROFILE_ERR_ENV) {
        rt_snprintf(response, sizeof(response),
                    "%s,ERR,ENV_WRITE_FAILED,ACTIVE=%s\r\n",
                    command, active.code);
    } else {
        rt_snprintf(response, sizeof(response),
                    "%s,ERR,UNSUPPORTED_CODE,ACTIVE=%s\r\n",
                    command, active.code);
    }
    profile_uart_send(response);
}

static rt_err_t profile_uart_rx_indicate(rt_device_t device, rt_size_t size)
{
    profile_rx_message_t message;

    if (!rx_messages || !size)
        return RT_EOK;
    message.device = device;
    message.size = size;
    return rt_mq_send(rx_messages, &message, sizeof(message));
}

/* 代号为固定 5 字节 ASCII，因此既支持直接发送，也支持 CR/LF 结尾。 */
static void profile_uart_consume_byte(char value)
{
    static char command[DISPLAY_PROFILE_CODE_LENGTH];
    static size_t command_length;
    unsigned char byte = (unsigned char)value;

    if (value == '\r' || value == '\n' || value == ' ' || value == '\t') {
        if (value == '\r' || value == '\n')
            command_length = 0;
        return;
    }

    if (!isalnum(byte)) {
        command_length = 0;
        return;
    }
    if (command_length >= DISPLAY_PROFILE_CODE_LENGTH - 1)
        command_length = 0;
    command[command_length++] = (char)toupper(byte);
    if (command_length == DISPLAY_PROFILE_CODE_LENGTH - 1) {
        command[command_length] = '\0';
        profile_command_reply(command);
        command_length = 0;
    }
}

static void display_profile_uart_thread(void *parameter)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    uint8_t bytes[DISPLAY_PROFILE_RX_CHUNK];
    profile_rx_message_t message;
    rt_device_t device;

    RT_UNUSED(parameter);
    device = rt_device_find(DISPLAY_PROFILE_UART_NAME);
    if (!device) {
        system_manager_log("屏幕配置串口未找到 uart0");
        return;
    }
    config.baud_rate = DISPLAY_PROFILE_UART_BAUD;
    if (rt_device_control(device, RT_DEVICE_CTRL_CONFIG, &config) != RT_EOK ||
        rt_device_open(device, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX) != RT_EOK) {
        system_manager_log("屏幕配置串口 uart0 初始化失败");
        return;
    }

    profile_uart = device;
    if (rt_device_set_rx_indicate(profile_uart, profile_uart_rx_indicate) != RT_EOK) {
        system_manager_log("屏幕配置串口 UART0 接收回调注册失败");
        return;
    }
    system_manager_log("屏幕配置串口已启动：UART0 115200 8N1");
    {
        display_profile_info_t active;
        char response[128];

        display_profile_get_active(&active);
        rt_snprintf(response, sizeof(response),
                    "%s,READY,RES=%ux%u,TOUCH=%s\r\n",
                    active.code, active.width, active.height, active.touch);
        profile_uart_send(response);
    }

    while (1) {
        if (rt_mq_recv(rx_messages, &message, sizeof(message), RT_WAITING_FOREVER) != RT_EOK)
            continue;
        while (message.size > 0) {
            rt_size_t index;
            rt_size_t request = message.size > sizeof(bytes) ? sizeof(bytes) : message.size;
            rt_size_t received = rt_device_read(message.device, 0, bytes, request);
            if (!received)
                break;
            for (index = 0; index < received; index++)
                profile_uart_consume_byte((char)bytes[index]);
            message.size -= received;
        }
    }
}

display_profile_result_t display_profile_apply(const char *code)
{
    display_profile_info_t target;

    if (!code)
        return DISPLAY_PROFILE_ERR_ARGUMENT;
    if (strcmp(code, active_profile.code) == 0)
        return DISPLAY_PROFILE_OK;
    if (strcmp(code, "CS000") != 0 && strcmp(code, "CS001") != 0)
        return DISPLAY_PROFILE_ERR_ARGUMENT;
    if (profile_info_from_code(code, &target) != 0)
        return DISPLAY_PROFILE_ERR_NOT_CONFIGURED;

    if (fw_env_open() != 0)
        return DISPLAY_PROFILE_ERR_ENV;
    {
        char env_value[DISPLAY_PROFILE_CODE_LENGTH];
        int ret;

        rt_snprintf(env_value, sizeof(env_value), "%s", code);
        ret = fw_env_write("hmi_screen", env_value);
        if (!ret)
            ret = fw_env_flush();
        fw_env_close();
        if (ret)
            return DISPLAY_PROFILE_ERR_ENV;
    }
    return DISPLAY_PROFILE_REBOOT_REQUIRED;
}

display_profile_result_t display_profile_get_active(display_profile_info_t *profile)
{
    if (!profile)
        return DISPLAY_PROFILE_ERR_ARGUMENT;
    if (profile_lock_ready)
        rt_mutex_take(&profile_lock, RT_WAITING_FOREVER);
    *profile = active_profile;
    if (profile_lock_ready)
        rt_mutex_release(&profile_lock);
    return DISPLAY_PROFILE_OK;
}

int display_profile_manager_init(void)
{
    rt_thread_t thread;

    if (manager_ready)
        return 0;
    if (rt_mutex_init(&profile_lock, "dspcfg", RT_IPC_FLAG_PRIO) != RT_EOK)
        return -1;
    profile_lock_ready = 1;
    profile_load_from_env();
    rx_messages = rt_mq_create("dsp_rx", sizeof(profile_rx_message_t),
                               DISPLAY_PROFILE_RX_QUEUE, RT_IPC_FLAG_FIFO);
    if (!rx_messages)
        return -2;
    thread = rt_thread_create("dsp_cfg", display_profile_uart_thread, NULL,
                              DISPLAY_PROFILE_THREAD_STACK, 27, 10);
    if (!thread)
        return -3;
    if (rt_thread_startup(thread) != RT_EOK)
        return -4;
    manager_ready = 1;
    return 0;
}

int display_profile_touch_enabled(void)
{
    display_profile_info_t active;

    if (display_profile_get_active(&active) != DISPLAY_PROFILE_OK)
        return 0;
    return active.touch_enabled;
}
