#include "buzzer_manager.h"
#include "env_manager.h"
#include "system_manager.h"

#include <drivers/rt_drv_pwm.h>
#include <rtdevice.h>
#include <stdlib.h>

#define BUZZER_PWM_DEVICE       "pwm"
#define BUZZER_PWM_CHANNEL      1
#define BUZZER_FREQUENCY_HZ     4000U
#define BUZZER_PERIOD_NS        (1000000000U / BUZZER_FREQUENCY_HZ)
#define BUZZER_MAX_DUTY_NS      (BUZZER_PERIOD_NS / 2U)
#define BUZZER_QUEUE_DEPTH      4
#define BUZZER_MAX_DURATION_MS  5000U
#define BUZZER_MAX_VOLUME       100U
#define BUZZER_SETTINGS_ENV     "hmi_buzzer"

typedef struct
{
    rt_uint32_t duration_ms;
    rt_uint32_t generation;
} buzzer_request_t;

static struct rt_device_pwm *pwm_device;
static rt_mq_t buzzer_queue;
static rt_mutex_t buzzer_lock;
static rt_sem_t settings_signal;
static rt_bool_t buzzer_ready;
static rt_bool_t pwm_active;
static rt_uint32_t tone_generation;
static unsigned buzzer_enabled = 1U;
static unsigned buzzer_volume = BUZZER_MAX_VOLUME;
static int settings_save_state = BUZZER_MANAGER_SAVE_IDLE;

static rt_err_t buzzer_set_duty(unsigned volume)
{
    /* 原有声响在 50% 占空比下工作，作为音量 100 的上限。 */
    rt_uint32_t pulse_ns = BUZZER_MAX_DUTY_NS * volume / BUZZER_MAX_VOLUME;

    return rt_pwm_set(pwm_device, BUZZER_PWM_CHANNEL, BUZZER_PERIOD_NS, pulse_ns);
}

/* 蜂鸣线程单独负责开始和计时，保存设置期间仍可按时停止。 */
static void buzzer_worker(void *parameter)
{
    buzzer_request_t request;
    rt_int32_t wait_ticks = RT_WAITING_FOREVER;

    (void)parameter;

    while (1)
    {
        rt_err_t received = rt_mq_recv(buzzer_queue, &request, sizeof(request), wait_ticks);
        const char *error = RT_NULL;

        rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
        wait_ticks = RT_WAITING_FOREVER;
        if (received != RT_EOK)
        {
            if (pwm_active)
            {
                if (rt_pwm_disable(pwm_device, BUZZER_PWM_CHANNEL) == RT_EOK)
                    pwm_active = RT_FALSE;
                else
                    error = "蜂鸣器 PWM1 停止失败";
            }
        }
        else if (request.generation == tone_generation && buzzer_enabled && buzzer_volume)
        {
            if (!pwm_active)
            {
                if (buzzer_set_duty(buzzer_volume) == RT_EOK &&
                    rt_pwm_enable(pwm_device, BUZZER_PWM_CHANNEL) == RT_EOK)
                    pwm_active = RT_TRUE;
                else
                    error = "蜂鸣器 PWM1 参数设置或启动失败";
            }
            /* 有效的新请求从到达时重新计时，保持设备事件提示音行为。 */
            if (pwm_active)
                wait_ticks = rt_tick_from_millisecond((rt_int32_t)request.duration_ms);
        }
        rt_mutex_release(buzzer_lock);

        if (error)
            system_manager_log(error);
    }
}

static void buzzer_load_settings(void)
{
    char value[6];
    char *end;
    unsigned long volume;

    /* 两个字段一起保存。旧固件没有该项时沿用原有的响铃和占空比。 */
    if (env_manager_get(BUZZER_SETTINGS_ENV, value, sizeof(value)) != 0 ||
        (value[0] != '0' && value[0] != '1') || value[1] != ',' ||
        value[2] < '0' || value[2] > '9')
        return;

    volume = strtoul(value + 2, &end, 10);
    if (*end == '\0' && volume <= BUZZER_MAX_VOLUME)
    {
        buzzer_enabled = (unsigned)(value[0] - '0');
        buzzer_volume = (unsigned)volume;
    }
}

static void buzzer_settings_worker(void *parameter)
{
    char value[6];

    (void)parameter;

    while (rt_sem_take(settings_signal, RT_WAITING_FOREVER) == RT_EOK)
    {
        rt_bool_t current;

        do
        {
            unsigned enabled, volume;
            int result;

            rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
            enabled = buzzer_enabled;
            volume = buzzer_volume;
            rt_mutex_release(buzzer_lock);

            rt_snprintf(value, sizeof(value), "%u,%u", enabled, volume);
            result = env_manager_set(BUZZER_SETTINGS_ENV, value);

            rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
            current = enabled == buzzer_enabled && volume == buzzer_volume;
            if (current)
                settings_save_state = result ? BUZZER_MANAGER_SAVE_ERROR : BUZZER_MANAGER_SAVE_OK;
            rt_mutex_release(buzzer_lock);

            if (result && current)
                system_manager_log("蜂鸣器设置写入 ENV 失败");
            /* 写入期间如果又收到设置，只处理最新的一组值。 */
        } while (!current);
    }
}

int buzzer_manager_init(void)
{
    rt_thread_t thread = RT_NULL;
    rt_thread_t settings_thread = RT_NULL;

    if (buzzer_ready)
        return BUZZER_MANAGER_OK;

    pwm_device = (struct rt_device_pwm *)rt_device_find(BUZZER_PWM_DEVICE);
    if (pwm_device == RT_NULL)
        return BUZZER_MANAGER_ERR_NOT_READY;

    buzzer_lock = rt_mutex_create("hmibuzz", RT_IPC_FLAG_PRIO);
    buzzer_queue = rt_mq_create("hmibuzzq", sizeof(buzzer_request_t),
                                BUZZER_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    settings_signal = rt_sem_create("hmibuzzcfg", 0, RT_IPC_FLAG_FIFO);
    if (buzzer_lock == RT_NULL || buzzer_queue == RT_NULL || settings_signal == RT_NULL)
        goto failed;

    thread = rt_thread_create("hmi_buzzer", buzzer_worker, RT_NULL,
                              2048, 24, 10);
    settings_thread = rt_thread_create("hmi_buzzer_cfg", buzzer_settings_worker, RT_NULL,
                                      2048, 25, 10);
    if (thread == RT_NULL || settings_thread == RT_NULL)
        goto failed;

    buzzer_load_settings();
    rt_thread_startup(thread);
    rt_thread_startup(settings_thread);
    buzzer_ready = RT_TRUE;
    return BUZZER_MANAGER_OK;

failed:
    if (thread != RT_NULL)
        rt_thread_delete(thread);
    if (settings_thread != RT_NULL)
        rt_thread_delete(settings_thread);
    if (settings_signal != RT_NULL)
        rt_sem_delete(settings_signal);
    if (buzzer_queue != RT_NULL)
        rt_mq_delete(buzzer_queue);
    if (buzzer_lock != RT_NULL)
        rt_mutex_delete(buzzer_lock);
    return BUZZER_MANAGER_ERR_NOT_READY;
}

int buzzer_manager_beep(rt_uint32_t duration_ms)
{
    int result = BUZZER_MANAGER_OK;
    buzzer_request_t request;

    if (!buzzer_ready)
        return BUZZER_MANAGER_ERR_NOT_READY;
    if (duration_ms == 0 || duration_ms > BUZZER_MAX_DURATION_MS)
        return BUZZER_MANAGER_ERR_BAD_DURATION;

    rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
    request.duration_ms = duration_ms;
    request.generation = tone_generation;
    if (!buzzer_enabled || !buzzer_volume)
        result = BUZZER_MANAGER_MUTED;
    else if (rt_mq_send(buzzer_queue, &request, sizeof(request)) != RT_EOK)
        result = BUZZER_MANAGER_ERR_QUEUE_FULL;
    rt_mutex_release(buzzer_lock);
    return result;
}

void buzzer_manager_get_settings(unsigned *enabled, unsigned *volume)
{
    if (!buzzer_ready)
    {
        if (enabled)
            *enabled = 1U;
        if (volume)
            *volume = BUZZER_MAX_VOLUME;
        return;
    }

    rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
    if (enabled)
        *enabled = buzzer_enabled;
    if (volume)
        *volume = buzzer_volume;
    rt_mutex_release(buzzer_lock);
}

int buzzer_manager_set_settings(unsigned enabled, unsigned volume)
{
    rt_bool_t muted = !enabled || !volume;
    rt_err_t result = RT_EOK;

    if (!buzzer_ready)
        return BUZZER_MANAGER_ERR_NOT_READY;
    if (enabled > 1U || volume > BUZZER_MAX_VOLUME)
        return BUZZER_MANAGER_ERR_BAD_SETTINGS;

    rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
    if (pwm_active)
    {
        if (muted)
            result = rt_pwm_disable(pwm_device, BUZZER_PWM_CHANNEL);
        else if (volume != buzzer_volume)
            result = buzzer_set_duty(volume);
    }

    if (result != RT_EOK)
    {
        rt_mutex_release(buzzer_lock);
        return BUZZER_MANAGER_ERR_PWM;
    }

    if (muted && buzzer_enabled && buzzer_volume)
    {
        /* 清除未处理的旧事件，防止重新开启时补响。 */
        pwm_active = RT_FALSE;
        tone_generation++;
        rt_mq_control(buzzer_queue, RT_IPC_CMD_RESET, RT_NULL);
    }

    if (enabled != buzzer_enabled || volume != buzzer_volume ||
        settings_save_state == BUZZER_MANAGER_SAVE_ERROR)
    {
        buzzer_enabled = enabled;
        buzzer_volume = volume;
        if (settings_save_state != BUZZER_MANAGER_SAVE_PENDING)
        {
            settings_save_state = BUZZER_MANAGER_SAVE_PENDING;
            rt_sem_release(settings_signal);
        }
    }
    rt_mutex_release(buzzer_lock);

    return BUZZER_MANAGER_OK;
}

int buzzer_manager_settings_save_state(void)
{
    int result;

    if (!buzzer_ready)
        return BUZZER_MANAGER_SAVE_ERROR;

    rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
    result = settings_save_state;
    rt_mutex_release(buzzer_lock);
    return result;
}
