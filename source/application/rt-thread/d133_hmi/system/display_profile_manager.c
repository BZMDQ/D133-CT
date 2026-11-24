#include "display_profile_manager.h"
#include "display_profile_codes.h"
#include "d133_wf101_profile.h"
#include "display_custom_codes.h"
#include "gt9271.h"
#include "env_manager.h"
#include "system_manager.h"
#include "reboot_manager.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_PROFILE_UART_NAME "uart0"
#define DISPLAY_PROFILE_UART_BAUD 115200
#define DISPLAY_PROFILE_RX_CHUNK 32
#define DISPLAY_PROFILE_THREAD_STACK 3072

static struct rt_mutex profile_lock;

static rt_device_t profile_uart;
static int profile_lock_ready;
static int manager_ready;
static display_profile_info_t active_profile = {
    D133_PUBLIC_CS000, 1024, 600, "LVDS-1024x600", "GT911", "gt911", 1
};

static int profile_info_from_code(const char *code, display_profile_info_t *profile)
{
    if (!code || !profile)
        return -1;
    memset(profile, 0, sizeof(*profile));
    if (strcmp(code, "CS000") == 0) {
        rt_snprintf(profile->code, sizeof(profile->code), "%s", D133_PUBLIC_CS000);
        profile->width = 1024;
        profile->height = 600;
        rt_snprintf(profile->panel, sizeof(profile->panel), "LVDS-1024x600");
        rt_snprintf(profile->touch, sizeof(profile->touch), "GT911");
        /* 协议显示名保留 GT911；健康检查使用驱动注册的小写设备名。 */
        rt_snprintf(profile->touch_device, sizeof(profile->touch_device), "gt911");
        profile->touch_enabled = 1;
        return 0;
    }
    if (strcmp(code, "CS001") == 0) {
        /* WJ101E5 使用 800x1280、4 通道 MIPI ILI9881C，配套触摸为 GT9271。 */
        rt_snprintf(profile->code, sizeof(profile->code), "%s", D133_PUBLIC_CS001);
        profile->width = 800;
        profile->height = 1280;
        rt_snprintf(profile->panel, sizeof(profile->panel), "ILI9881C-04T00GA MIPI-4L");
        rt_snprintf(profile->touch, sizeof(profile->touch), "GT9271");
        rt_snprintf(profile->touch_device, sizeof(profile->touch_device), GT9271_DEVICE_NAME);
        profile->touch_enabled = 1;
        return 0;
    }
    return -1;
}

display_profile_result_t display_profile_get_configured(const char *code, display_profile_info_t *profile)
{
    if (!code || !profile)
        return DISPLAY_PROFILE_ERR_ARGUMENT;
    /* 配置包只接受稳定槽号，界面仍可展示工程自定义识别码。 */
#if defined(AIC_DISP_LVDS) && defined(AIC_SIMPLE_PANEL) && defined(AIC_USING_CTP) && defined(AIC_TOUCH_PANEL_GT911)
    if (strcmp(code, "CS000") == 0 && profile_info_from_code(code, profile) == 0)
        return DISPLAY_PROFILE_OK;
#endif
#if defined(AIC_DISP_MIPI_DSI) && defined(AIC_PANEL_DSI_ILI9881C) && defined(AIC_USING_CTP)
    if (strcmp(code, "CS001") == 0 && profile_info_from_code(code, profile) == 0)
        return DISPLAY_PROFILE_OK;
#endif
    return DISPLAY_PROFILE_ERR_NOT_CONFIGURED;
}

/* 冗余 ENV 一次性保存选屏、活动代号和已签名登记表。早期面板驱动仍读取原生槽。 */
#define SCREEN_ENV "hmi_screen"
#define CODE_ENV "hmi_screen_code"
#define REGISTRY_ENV "hmi_screen_registry"
#define REGISTRY_MAX 12U
#define REGISTRY_SIZE (REGISTRY_MAX * (DISPLAY_PROFILE_CODE_LENGTH + DISPLAY_PROFILE_SLOT_LENGTH) + 1U)

static char active_slot[DISPLAY_PROFILE_SLOT_LENGTH] = "CS000";

int display_profile_code_valid(const char *code)
{
    size_t length = 0;
    if (!code || !((code[0] >= 'A' && code[0] <= 'Z') ||
                   (code[0] >= '0' && code[0] <= '9')))
        return 0;
    for (; code[length]; ++length) {
        char ch = code[length];
        if (length >= DISPLAY_PROFILE_CODE_LENGTH - 1U ||
            !((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.' || ch == '#'))
            return 0;
    }
    return 1;
}

int display_profile_custom_code(const char *code)
{
    display_profile_info_t profile;
    if (!display_profile_code_valid(code) || !strcmp(code, "CS000") || !strcmp(code, "CS001"))
        return 0;
    /* 自定义代号不得覆盖工程中已经改名的内置型号。 */
    return (display_profile_get_configured("CS000", &profile) != DISPLAY_PROFILE_OK ||
            strcmp(code, profile.code)) &&
           (display_profile_get_configured("CS001", &profile) != DISPLAY_PROFILE_OK ||
            strcmp(code, profile.code));
}

const char *display_profile_ic(const char *slot)
{
    if (!strcmp(slot, "CS000"))
        return "UNSPECIFIED";
    if (!strcmp(slot, "CS001"))
        return "ILI9881C";
    return "";
}

static int decode_registry(const char *text, display_profile_choice_t *records)
{
    int count = 0;
    while (*text) {
        const char *equal = strchr(text, '=');
        display_profile_choice_t *entry;
        size_t size;
        if (count >= (int)REGISTRY_MAX || !equal)
            return -1;
        size = (size_t)(equal - text);
        if (!size || size >= DISPLAY_PROFILE_CODE_LENGTH || strlen(equal + 1) < DISPLAY_PROFILE_SLOT_LENGTH ||
            equal[DISPLAY_PROFILE_SLOT_LENGTH] != ';')
            return -1;
        entry = &records[count];
        memcpy(entry->code, text, size);
        entry->code[size] = '\0';
        memcpy(entry->slot, equal + 1, DISPLAY_PROFILE_SLOT_LENGTH - 1U);
        entry->slot[DISPLAY_PROFILE_SLOT_LENGTH - 1U] = '\0';
        if (!display_profile_code_valid(entry->code) ||
            (strcmp(entry->slot, "CS000") && strcmp(entry->slot, "CS001")))
            return -1;
        ++count;
        text = equal + DISPLAY_PROFILE_SLOT_LENGTH + 1U;
    }
    return count;
}

static int read_registry(display_profile_choice_t *records)
{
    char text[REGISTRY_SIZE] = {0};
    if (env_manager_get(REGISTRY_ENV, text, sizeof(text)) != 0)
        return -1;
    return decode_registry(text, records);
}

static const char *registry_slot(const display_profile_choice_t *records, int count, const char *code)
{
    for (int i = 0; i < count; ++i)
        if (!strcmp(records[i].code, code))
            return records[i].slot;
    return NULL;
}

/* 工程管理器同步的识别码表。待接入型号只有代号声明，无法选屏。 */
#define CUSTOM_ENTRY(code_text, slot_text) {code_text, slot_text},
static const struct { const char *code; const char *slot; } source_profiles[] = {
    D133_CUSTOM_SCREEN_CODES(CUSTOM_ENTRY)
    {NULL, NULL}
};
#undef CUSTOM_ENTRY

static const char *custom_source_slot(const char *code)
{
    for (size_t i = 0; source_profiles[i].code; ++i)
        if (!strcmp(code, source_profiles[i].code))
            return source_profiles[i].slot;
    return NULL;
}

static const char *registered_slot(const display_profile_choice_t *records, int count, const char *code)
{
    const char *slot;
    if (!display_profile_custom_code(code))
        return NULL;
    slot = registry_slot(records, count, code);
    if (!strcmp(code, D133_WF101_CODE))
        return slot && !strcmp(slot, D133_WF101_SLOT) ? slot : NULL;
    return slot ? slot : custom_source_slot(code);
}

static void append_choice(display_profile_choice_t *choices, size_t capacity, size_t *count,
                          const char *code, const char *slot)
{
    display_profile_info_t profile;
    if (*count >= capacity || !display_profile_code_valid(code) ||
        display_profile_get_configured(slot, &profile) != DISPLAY_PROFILE_OK)
        return;
    for (size_t i = 0; i < *count; ++i)
        if (!strcmp(choices[i].code, code))
            return;
    strcpy(choices[*count].code, code);
    strcpy(choices[*count].slot, slot);
    ++*count;
}

size_t display_profile_choices(display_profile_choice_t *choices, size_t capacity)
{
    display_profile_choice_t records[REGISTRY_MAX];
    size_t count = 0;
    int stored;
    if (!choices)
        return 0;
    append_choice(choices, capacity, &count, D133_PUBLIC_CS000, "CS000");
    append_choice(choices, capacity, &count, D133_PUBLIC_CS001, "CS001");
    stored = read_registry(records);
    for (int i = 0; i < stored; ++i)
        if (display_profile_custom_code(records[i].code) &&
            (strcmp(records[i].code, D133_WF101_CODE) || !strcmp(records[i].slot, D133_WF101_SLOT)))
            append_choice(choices, capacity, &count, records[i].code, records[i].slot);
    for (size_t i = 0; source_profiles[i].code && count < capacity; ++i)
        if (display_profile_custom_code(source_profiles[i].code) &&
            strcmp(source_profiles[i].code, D133_WF101_CODE))
            append_choice(choices, capacity, &count, source_profiles[i].code, source_profiles[i].slot);
    return count;
}

static void profile_load_from_env(void)
{
    const char *const names[] = {SCREEN_ENV, CODE_ENV, REGISTRY_ENV};
    char slot[DISPLAY_PROFILE_SLOT_LENGTH] = {0}, code[DISPLAY_PROFILE_CODE_LENGTH] = {0};
    char registry[REGISTRY_SIZE] = {0};
    display_profile_choice_t records[REGISTRY_MAX];
    char *const values[] = {slot, code, registry};
    const size_t sizes[] = {sizeof(slot), sizeof(code), sizeof(registry)};
    display_profile_info_t profile;
    const char *registered;

    if (env_manager_get_many(names, values, sizes, 3U) != 0 ||
        display_profile_get_configured(slot, &profile) != DISPLAY_PROFILE_OK)
        return;
    memcpy(active_slot, slot, sizeof(active_slot));
    registered = registered_slot(records, decode_registry(registry, records), code);
    if (registered && !strcmp(registered, slot)) {
        memcpy(profile.code, code, sizeof(profile.code));
        if (!strcmp(code, D133_WF101_CODE)) {
            /* 保持 LVDS 时序，只切换到 WF101 的无触摸运行状态。 */
            snprintf(profile.touch, sizeof(profile.touch), "%s", D133_WF101_TOUCH);
            profile.touch_device[0] = '\0';
            profile.touch_enabled = 0;
        }
    }
    active_profile = profile;
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
    } else if (result == DISPLAY_PROFILE_REBOOT_REQUIRED) {
        char slot[DISPLAY_PROFILE_SLOT_LENGTH] = {0};
        if (env_manager_get(SCREEN_ENV, slot, sizeof(slot)) == 0 &&
            profile_info_from_code(slot, &target) == 0)
            rt_snprintf(response, sizeof(response),
                        "%s,OK,REBOOTING,RES=%ux%u,TOUCH=%s\r\n",
                        command, target.width, target.height, target.touch);
        else
            rt_snprintf(response, sizeof(response), "%s,OK,REBOOTING\r\n", command);
        profile_uart_send(response);
        /* 给 UART 发完应答的时间，再重启让早期 panel probe 采用新 timing。 */
        rt_thread_mdelay(100);
        if (reboot_manager_request() != 0) {
            profile_uart_send("ERR,REBOOT_FAILED\r\n");
            return;
        }
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

/* 代号为固定 5 字节 ASCII，因此既支持直接发送，也支持 CR/LF 结尾。 */
static void profile_uart_consume_byte(char value)
{
    static char command[DISPLAY_PROFILE_SLOT_LENGTH];
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
    if (command_length >= DISPLAY_PROFILE_SLOT_LENGTH - 1U)
        command_length = 0;
    command[command_length++] = (char)toupper(byte);
    if (command_length == DISPLAY_PROFILE_SLOT_LENGTH - 1U) {
        command[command_length] = '\0';
        profile_command_reply(command);
        command_length = 0;
    }
}

static int display_profile_uart_open(void)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    rt_device_t device = rt_device_find(DISPLAY_PROFILE_UART_NAME);

    if (!device) {
        system_manager_log("屏幕配置串口未找到 uart0");
        return -1;
    }
    /* 保留 SDK 扩展端口号、时钟及工作模式，默认模板的端口号是无效值 15。 */
    if (rt_device_control(device, RT_SERIAL_GET_CONFIG, &config) != RT_EOK)
        return -3;
    config.baud_rate = DISPLAY_PROFILE_UART_BAUD;
    if (rt_device_control(device, RT_DEVICE_CTRL_CONFIG, &config) != RT_EOK ||
        rt_device_open(device, RT_DEVICE_OFLAG_RDWR) != RT_EOK) {
        system_manager_log("屏幕配置串口 uart0 初始化失败");
        return -2;
    }

    profile_uart = device;
    system_manager_log("屏幕配置串口已启动：UART0 115200 8N1");
    return 0;
}

static void display_profile_uart_send_ready(void)
{
    display_profile_info_t active;
    char response[128];

    display_profile_get_active(&active);
    rt_snprintf(response, sizeof(response),
                "%s,READY,RES=%ux%u,TOUCH=%s\r\n",
                active.code, active.width, active.height, active.touch);
    profile_uart_send(response);
}

/* 使用专用线程轮询 RT-Thread 串口设备，避免依赖 UART 中断接收配置。 */
static void display_profile_uart_thread(void *parameter)
{
    uint8_t bytes[DISPLAY_PROFILE_RX_CHUNK];

    RT_UNUSED(parameter);
    while (1) {
        rt_size_t received = rt_device_read(profile_uart, 0, bytes, sizeof(bytes));
        if (received) {
            rt_size_t index;
            for (index = 0; index < received; index++)
                profile_uart_consume_byte((char)bytes[index]);
        } else {
            rt_thread_mdelay(1);
        }
    }
}

static const char *native_for_code(const char *code)
{
    if (!strcmp(code, D133_PUBLIC_CS000))
        return "CS000";
    if (!strcmp(code, D133_PUBLIC_CS001))
        return "CS001";
    return code;
}

static display_profile_result_t stage_profile(const char *code)
{
    const char *const names[] = {SCREEN_ENV, CODE_ENV};
    display_profile_choice_t records[REGISTRY_MAX];
    char slot[DISPLAY_PROFILE_SLOT_LENGTH] = {0};
    const char *values[] = {slot, ""};
    int count;
    display_profile_info_t base;
    display_profile_result_t result = DISPLAY_PROFILE_ERR_NOT_CONFIGURED;
    const char *native;

    if (!display_profile_code_valid(code))
        return DISPLAY_PROFILE_ERR_ARGUMENT;
    if (rt_mutex_take(&profile_lock, RT_WAITING_FOREVER) != RT_EOK)
        return DISPLAY_PROFILE_ERR_ENV;
    count = read_registry(records);
    if (count < 0) {
        result = DISPLAY_PROFILE_ERR_ENV;
        goto done;
    }
    native = native_for_code(code);
    if (display_profile_get_configured(native, &base) == DISPLAY_PROFILE_OK) {
        memcpy(slot, native, sizeof(slot));
    } else {
        const char *registered = registered_slot(records, count, code);
        if (!registered)
            goto done;
        strcpy(slot, registered);
        if (display_profile_get_configured(slot, &base) != DISPLAY_PROFILE_OK)
            goto done;
        values[1] = code;
    }
    if (!values[1][0] && !strcmp(slot, "CS000") && !strcmp(active_profile.code, base.code)) {
        char stored_slot[DISPLAY_PROFILE_SLOT_LENGTH] = {0}, stored_code[DISPLAY_PROFILE_CODE_LENGTH] = {0};
        char *const current_values[] = {stored_slot, stored_code};
        const size_t current_sizes[] = {sizeof(stored_slot), sizeof(stored_code)};
        if (env_manager_get_many(names, current_values, current_sizes, 2U) == 0 &&
            !stored_code[0] && (!stored_slot[0] || !strcmp(stored_slot, slot))) {
            result = DISPLAY_PROFILE_OK;
            goto done;
        }
    }
    result = env_manager_set_many(names, values, 2U) == 0 ?
             DISPLAY_PROFILE_REBOOT_REQUIRED : DISPLAY_PROFILE_ERR_ENV;

done:
    rt_mutex_release(&profile_lock);
    return result;
}

display_profile_result_t display_profile_apply(const char *code)
{
    return stage_profile(code);
}

display_profile_result_t display_profile_register(const char *code, const char *slot)
{
    display_profile_choice_t records[REGISTRY_MAX];
    char text[REGISTRY_SIZE] = {0};
    display_profile_info_t profile;
    display_profile_result_t result = DISPLAY_PROFILE_ERR_ENV;
    const char *existing;
    int count;
    size_t used = 0;

    if (!display_profile_custom_code(code) ||
        display_profile_get_configured(slot, &profile) != DISPLAY_PROFILE_OK ||
        (!strcmp(code, D133_WF101_CODE) && strcmp(slot, D133_WF101_SLOT)))
        return DISPLAY_PROFILE_ERR_NOT_CONFIGURED;
    if (rt_mutex_take(&profile_lock, RT_WAITING_FOREVER) != RT_EOK)
        return DISPLAY_PROFILE_ERR_ENV;
    count = read_registry(records);
    if (count < 0)
        goto done;
    /* 即使旧版已有同料号的错误绑定，也继续按旧冲突规则拒绝覆盖。 */
    existing = registry_slot(records, count, code);
    if (!existing && strcmp(code, D133_WF101_CODE))
        existing = custom_source_slot(code);
    if (existing && strcmp(existing, slot)) {
        result = DISPLAY_PROFILE_ERR_CONFLICT;
        goto done;
    }
    if (registry_slot(records, count, code)) {
        result = DISPLAY_PROFILE_OK;
        goto done;
    }
    if (count >= (int)REGISTRY_MAX) {
        result = DISPLAY_PROFILE_ERR_LIMIT;
        goto done;
    }
    strcpy(records[count].code, code);
    strcpy(records[count++].slot, slot);
    for (int i = 0; i < count; ++i) {
        int length = snprintf(text + used, sizeof(text) - used, "%s=%s;", records[i].code, records[i].slot);
        if (length < 0 || (size_t)length >= sizeof(text) - used)
            goto done;
        used += (size_t)length;
    }

    /* 兼容旧格式，安装只写登记表，保持当前和下次开机的屏幕选择。 */
    if (env_manager_set(REGISTRY_ENV, text) == 0)
        result = DISPLAY_PROFILE_OK;

done:
    rt_mutex_release(&profile_lock);
    return result;
}

const char *display_profile_active_slot(void)
{
    return active_slot;
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
    if (strcmp(active_slot, "CS001") == 0 && gt9271_init() != RT_EOK) {
        active_profile.touch_enabled = 0;
        rt_snprintf(active_profile.touch, sizeof(active_profile.touch), "GT9271_OFFLINE");
        system_manager_log("CS001 GT9271 探测失败");
    }
    if (display_profile_uart_open() != 0)
        return -2;

    thread = rt_thread_create("dsp_cfg", display_profile_uart_thread, NULL,
                              DISPLAY_PROFILE_THREAD_STACK, 20, 10);
    if (!thread) {
        rt_device_close(profile_uart);
        profile_uart = RT_NULL;
        return -3;
    }
    if (rt_thread_startup(thread) != RT_EOK) {
        rt_device_close(profile_uart);
        profile_uart = RT_NULL;
        return -4;
    }
    manager_ready = 1;
    display_profile_uart_send_ready();
    return 0;
}
int display_profile_touch_enabled(void)
{
    display_profile_info_t active;

    if (display_profile_get_active(&active) != DISPLAY_PROFILE_OK)
        return 0;
    return active.touch_enabled;
}


const char *display_profile_touch_device(void)
{
    return active_profile.touch_device;
}
