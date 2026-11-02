#include "display_profile_manager.h"
#include "system_manager.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_PROFILE_UART_NAME "uart0"
#define DISPLAY_PROFILE_UART_BAUD 115200
#define DISPLAY_PROFILE_RX_CHUNK 32
#define DISPLAY_PROFILE_THREAD_STACK 3072

static struct rt_mutex profile_lock;
static rt_sem_t rx_sem;
static rt_device_t profile_uart;
static int profile_lock_ready;
static int manager_ready;
static display_profile_info_t active_profile = {
    "CS000", 1024, 600, "LVDS-1024x600", "GT911"
};

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
    display_profile_result_t result;

    result = display_profile_apply(command);
    display_profile_get_active(&active);
    if (result == DISPLAY_PROFILE_OK) {
        rt_snprintf(response, sizeof(response),
                    "%s,OK,ACTIVE=%s,RES=%ux%u,TOUCH=%s\r\n",
                    command, active.code, active.width, active.height, active.touch);
    } else if (result == DISPLAY_PROFILE_ERR_NOT_CONFIGURED) {
        rt_snprintf(response, sizeof(response),
                    "%s,ERR,NOT_CONFIGURED,ACTIVE=%s\r\n",
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
    RT_UNUSED(device);
    RT_UNUSED(size);
    rt_sem_release(rx_sem);
    return RT_EOK;
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
    rt_device_set_rx_indicate(profile_uart, profile_uart_rx_indicate);
    system_manager_log("屏幕配置串口已启动：UART0 115200 8N1");

    while (1) {
        rt_size_t received;

        rt_sem_take(rx_sem, RT_WAITING_FOREVER);
        do {
            rt_size_t index;
            received = rt_device_read(profile_uart, 0, bytes, sizeof(bytes));
            for (index = 0; index < received; index++)
                profile_uart_consume_byte((char)bytes[index]);
        } while (received == sizeof(bytes));
    }
}

display_profile_result_t display_profile_apply(const char *code)
{
    if (!code)
        return DISPLAY_PROFILE_ERR_ARGUMENT;
    if (strcmp(code, "CS000") == 0)
        return DISPLAY_PROFILE_OK;
    if (strcmp(code, "CS001") == 0)
        return DISPLAY_PROFILE_ERR_NOT_CONFIGURED;
    return DISPLAY_PROFILE_ERR_ARGUMENT;
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
    rx_sem = rt_sem_create("dsp_rx", 0, RT_IPC_FLAG_FIFO);
    if (!rx_sem)
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
