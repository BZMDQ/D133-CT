/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-01-13     RiceChen     the first version
 */

#include <rtconfig.h>
#if defined(KERNEL_RTTHREAD) && defined(AIC_USING_TOUCH)
#include <rtdevice.h>
#include <rtthread.h>
#include "lv_tpc_run.h"
#include "lv_port_indev.h"
#include "aic_osal.h"
#include <string.h>
#include "../components/drivers/include/drivers/touch.h"

#define THREAD_PRIORITY   25
#define THREAD_STACK_SIZE 4096
#define THREAD_TIMESLICE  5
#define TPC_MAX_POINTS 16U
#define CS001_TOUCH_POLL_MS 12U

static rt_thread_t  touch_thread = RT_NULL;
static rt_sem_t     touch_sem = RT_NULL;
static rt_device_t  dev = RT_NULL;
/* 固定缓冲覆盖 4 位触点 ID，保留 GT9271 的 10 点能力。 */
static struct rt_touch_data read_data[TPC_MAX_POINTS];
static struct rt_touch_info info;
static rt_uint8_t cs001_poll;

static void touch_entry(void *parameter) /* touch panel control entry */
{
    while (1)
    {
#ifndef AIC_CTP_POLLING_MODE
        /* GT9271 仍优先响应中断，未收到边沿时限频读取状态寄存器。 */
        rt_sem_take(touch_sem, cs001_poll ?
                    rt_tick_from_millisecond(CS001_TOUCH_POLL_MS) : RT_WAITING_FOREVER);
#endif
        int num = rt_device_read(dev, 0, read_data, info.point_num);
        if (num > 0)
        {
            for (rt_uint8_t i = 0; i < num && i < info.point_num; i++)
            {
                if (read_data[i].event == RT_TOUCH_EVENT_DOWN
                        || read_data[i].event == RT_TOUCH_EVENT_UP
                        || read_data[i].event == RT_TOUCH_EVENT_MOVE)
                {
                    rt_uint16_t  u16X, u16Y;
#ifdef AIC_USING_RTP
#if AIC_RTP_RECALIBRATE_ENABLE
                    if (rtp_is_recalibrate_started()) {
                        rtp_store_recalibrate_data(dev, read_data);
                        if (rtp_is_recalibrate_all_data_stored()) {
                            rtp_update_recalibrate();
                        }
                    }
#endif
                    rtp_check_event_type(read_data[i].event, read_data[i].pressure);
                    extern void lv_convert_adc_to_coord(rt_device_t rtp_dev, struct rt_touch_data *data);
                    lv_convert_adc_to_coord(dev, &read_data[i]);
#endif
                    u16X = read_data[i].x_coordinate;
                    u16Y = read_data[i].y_coordinate;

                    // rt_kprintf("[%d] %d %d\n", read_data[i].event, u16X, u16Y);
                    aic_touch_inputevent_cb_ex(u16X, u16Y,
                                               read_data[i].track_id,
                                               read_data[i].event);
                }
            }
        }
#ifdef AIC_CTP_POLLING_MODE
        aicos_msleep(10);
#else
        rt_device_control(dev, RT_TOUCH_CTRL_ENABLE_INT, RT_NULL);
#endif
    }
}

static rt_err_t rx_callback(rt_device_t dev, rt_size_t size)
{
#ifndef AIC_CTP_POLLING_MODE
    rt_device_control(dev, RT_TOUCH_CTRL_DISABLE_INT, RT_NULL);
    if (touch_sem)
        rt_sem_release(touch_sem);
#endif
#ifdef AIC_PM_DEMO
    extern struct rt_event pm_event;
    rt_event_send(&pm_event, 2);
#endif
    return 0;
}

int tpc_run(const char *name, rt_uint16_t x, rt_uint16_t y)
{
    if (touch_thread)
        return -RT_EBUSY;
    cs001_poll = strcmp(name, "gt9271") == 0;
    dev = rt_device_find(name);
    if (dev == RT_NULL)
    {
        rt_kprintf("can't find device:%s\n", name);
        return -1;
    }

#ifdef AIC_LVGL_USB_OSD_DEMO
    rt_device_close(dev);
#endif
    if (rt_device_open(dev, RT_DEVICE_FLAG_INT_RX) != RT_EOK)
    {
        rt_kprintf("Failed to open dev %s\n", name);
        return -1;
    }

#ifdef AIC_USING_RTP
#ifdef RT_TOUCH_PIN_IRQ
    rt_kprintf("Using RTP requires turning off RT_TOUCH_PIN_IRQ\n");
    return -1;
#endif
    extern void lv_rtp_calibrate(rt_device_t rtp_dev, int fb_width, int fb_height);
    lv_rtp_calibrate(dev, x, y);
#endif

    /* GET_INFO 写入完整结构体，不能与 8 字节芯片 ID 缓冲混用。 */
#ifdef AIC_USING_RTP
    info.point_num = 1;
#else
    if (rt_device_control(dev, RT_TOUCH_CTRL_GET_INFO, &info) != RT_EOK ||
        !info.point_num || info.point_num > TPC_MAX_POINTS)
        goto fail;
#endif
    rt_kprintf("Touch %s: %ld x %ld, %u points\n", name,
               (long)info.range_x, (long)info.range_y, (unsigned)info.point_num);

    touch_sem = rt_sem_create("touch_sem", 0, RT_IPC_FLAG_FIFO);
    if (touch_sem == RT_NULL)
    {
        rt_kprintf("create touch semaphore failed.\n");
        goto fail;
    }

#ifndef AIC_LVGL_USB_OSD_DEMO
    rt_device_set_rx_indicate(dev, rx_callback);
#endif
    touch_thread = rt_thread_create("touch",
                                     touch_entry,
                                     RT_NULL,
                                     THREAD_STACK_SIZE,
                                     THREAD_PRIORITY,
                                     THREAD_TIMESLICE);

    if (!touch_thread || rt_thread_startup(touch_thread) != RT_EOK)
        goto fail;

    return 0;

fail:
    /* 打开失败时撤销回调和资源，不向 LVGL 报告触摸已启动。 */
    rt_device_control(dev, RT_TOUCH_CTRL_DISABLE_INT, RT_NULL);
    rt_device_set_rx_indicate(dev, RT_NULL);
    if (touch_thread) {
        rt_thread_delete(touch_thread);
        touch_thread = RT_NULL;
    }
    if (touch_sem) {
        rt_sem_delete(touch_sem);
        touch_sem = RT_NULL;
    }
    rt_device_close(dev);
    dev = RT_NULL;
    return -RT_ERROR;
}

#ifdef AIC_USING_TOUCH
static rt_err_t (*bak_callback)(rt_device_t, rt_size_t);

void lvgl_get_tp(void)
{
    if (bak_callback == NULL) {
        bak_callback = dev->rx_indicate;
#ifdef LV_DISPLAY_ROTATE_EN
        rt_uint8_t flag = 1;
        rt_device_control(dev, RT_TOUCH_CTRL_SET_OSD_FLAG, &flag);
#endif
        rt_device_set_rx_indicate(dev, rx_callback);
    }
}

void lvgl_put_tp(void)
{
    rt_uint8_t flag = 0;
    if (bak_callback != NULL) {
        rt_device_control(dev, RT_TOUCH_CTRL_SET_OSD_FLAG, &flag);
        rt_device_set_rx_indicate(dev, bak_callback);
        bak_callback = NULL;
    }
}
#endif /* AIC_USING_TOUCH */
#endif
