#include "env_manager.h"
#include <env.h>
#include <rtthread.h>
#include <string.h>

#define HMI_ENV_NAME_MAX 64U
#define HMI_ENV_VALUE_MAX 4096U

static struct rt_mutex env_lock;
static int env_ready;

int env_manager_init(void)
{
    if (env_ready)
        return 0;
    if (rt_mutex_init(&env_lock, "hmienv", RT_IPC_FLAG_PRIO) != RT_EOK)
        return -1;
    env_ready = 1;
    return 0;
}

int env_manager_get_many(const char *const *names, char *const *values,
                         const size_t *capacities, size_t count)
{
    size_t i;
    int result = 0;

    if (!env_ready || !names || !values || !capacities || !count)
        return -1;
    for (i = 0; i < count; i++) {
        if (!names[i] || !values[i] || !capacities[i])
            return -1;
        values[i][0] = '\0';
    }

    rt_mutex_take(&env_lock, RT_WAITING_FOREVER);
    if (fw_env_open() != 0) {
        rt_mutex_release(&env_lock);
        return -2;
    }
    for (i = 0; i < count; i++) {
        char env_name[HMI_ENV_NAME_MAX];
        char *value;
        size_t length;

        length = strlen(names[i]);
        if (!length || length >= sizeof(env_name)) {
            result = -1;
            break;
        }
        memcpy(env_name, names[i], length + 1U);
        value = fw_getenv(env_name);
        if (!value)
            continue;
        length = strlen(value);
        if (length >= capacities[i]) {
            result = -3;
            break;
        }
        memcpy(values[i], value, length + 1U);
    }
    fw_env_close();
    rt_mutex_release(&env_lock);
    return result;
}

int env_manager_get(const char *name, char *value, size_t capacity)
{
    const char *names[1] = {name};
    char *values[1] = {value};
    size_t capacities[1] = {capacity};

    return env_manager_get_many(names, values, capacities, 1U);
}

int env_manager_set_many(const char *const *names, const char *const *values,
                         size_t count)
{
    size_t i;
    int result = 0;

    if (!env_ready || !names || !values || !count)
        return -1;
    for (i = 0; i < count; i++)
        if (!names[i] || !values[i] || !strlen(names[i]) ||
            strlen(names[i]) >= HMI_ENV_NAME_MAX ||
            strlen(values[i]) >= HMI_ENV_VALUE_MAX)
            return -1;

    rt_mutex_take(&env_lock, RT_WAITING_FOREVER);
    if (fw_env_open() != 0) {
        rt_mutex_release(&env_lock);
        return -2;
    }
    for (i = 0; i < count; i++) {
        /* fw_env_write 只读取字符串，避免在 WLAN 回调的小线程栈复制大配置串。 */
        if (fw_env_write((char *)names[i], (char *)values[i]) != 0) {
            result = -3;
            break;
        }
    }
    if (!result && fw_env_flush() != 0)
        result = -4;
    fw_env_close();
    rt_mutex_release(&env_lock);
    return result;
}

int env_manager_set(const char *name, const char *value)
{
    const char *names[1] = {name};
    const char *values[1] = {value};

    return env_manager_set_many(names, values, 1U);
}
