#include "buzzer_manager.h"
#include "system_manager.h"

#include <drivers/rt_drv_pwm.h>
#include <rtdevice.h>

#define BUZZER_PWM_DEVICE       "pwm"
#define BUZZER_PWM_CHANNEL      1
#define BUZZER_FREQUENCY_HZ     4000U
#define BUZZER_PERIOD_NS        (1000000000U / BUZZER_FREQUENCY_HZ)
#define BUZZER_DUTY_NS          (BUZZER_PERIOD_NS / 2U)
#define BUZZER_QUEUE_DEPTH      4
#define BUZZER_MAX_DURATION_MS  5000U

static struct rt_device_pwm *pwm_device;
static rt_mq_t buzzer_queue;
static rt_mutex_t buzzer_lock;
static rt_bool_t buzzer_ready;

/* 蜂鸣工作放在线程中执行，网络线程和 LVGL 线程只提交短消息。 */
static void buzzer_worker(void *parameter)
{
    rt_uint32_t duration_ms;
    rt_err_t result;

    (void)parameter;

    while (1)
    {
        if (rt_mq_recv(buzzer_queue, &duration_ms, sizeof(duration_ms),
                       RT_WAITING_FOREVER) != RT_EOK)
            continue;

        rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
        result = rt_pwm_set(pwm_device, BUZZER_PWM_CHANNEL,
                            BUZZER_PERIOD_NS, BUZZER_DUTY_NS);
        if (result == RT_EOK)
            result = rt_pwm_enable(pwm_device, BUZZER_PWM_CHANNEL);
        rt_mutex_release(buzzer_lock);

        if (result != RT_EOK)
        {
            system_manager_log("蜂鸣器 PWM1 参数设置或启动失败");
            continue;
        }

        /* 新事件在当前鸣叫期间到达时，从最后一次事件重新计时。 */
        while (rt_mq_recv(buzzer_queue, &duration_ms, sizeof(duration_ms),
                          rt_tick_from_millisecond((rt_int32_t)duration_ms)) == RT_EOK)
        {
            /* 持续时间由最新的事件重新设定；本工程提示音为 1 秒。 */
        }

        rt_mutex_take(buzzer_lock, RT_WAITING_FOREVER);
        if (rt_pwm_disable(pwm_device, BUZZER_PWM_CHANNEL) != RT_EOK)
            system_manager_log("蜂鸣器 PWM1 停止失败");
        rt_mutex_release(buzzer_lock);
    }
}

int buzzer_manager_init(void)
{
    rt_thread_t thread;

    if (buzzer_ready)
        return BUZZER_MANAGER_OK;

    pwm_device = (struct rt_device_pwm *)rt_device_find(BUZZER_PWM_DEVICE);
    if (pwm_device == RT_NULL)
        return BUZZER_MANAGER_ERR_NOT_READY;

    buzzer_lock = rt_mutex_create("hmibuzz", RT_IPC_FLAG_PRIO);
    buzzer_queue = rt_mq_create("hmibuzzq", sizeof(rt_uint32_t),
                                BUZZER_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    if (buzzer_lock == RT_NULL || buzzer_queue == RT_NULL)
        return BUZZER_MANAGER_ERR_NOT_READY;

    thread = rt_thread_create("hmi_buzzer", buzzer_worker, RT_NULL,
                              2048, 24, 10);
    if (thread == RT_NULL || rt_thread_startup(thread) != RT_EOK)
        return BUZZER_MANAGER_ERR_NOT_READY;

    buzzer_ready = RT_TRUE;
    return BUZZER_MANAGER_OK;
}

int buzzer_manager_beep(rt_uint32_t duration_ms)
{
    if (!buzzer_ready)
        return BUZZER_MANAGER_ERR_NOT_READY;
    if (duration_ms == 0 || duration_ms > BUZZER_MAX_DURATION_MS)
        return BUZZER_MANAGER_ERR_BAD_DURATION;

    if (rt_mq_send(buzzer_queue, &duration_ms, sizeof(duration_ms)) != RT_EOK)
        return BUZZER_MANAGER_ERR_QUEUE_FULL;

    return BUZZER_MANAGER_OK;
}

