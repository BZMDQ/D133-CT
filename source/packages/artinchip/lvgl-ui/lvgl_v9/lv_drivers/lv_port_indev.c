/*
 * Copyright (c) 2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Ning Fang <ning.fang@artinchip.com>
 */

#include <rtconfig.h>
#ifdef KERNEL_RTTHREAD
#include <lvgl.h>
#include <stdbool.h>
#include <rtthread.h>
#include <../components/drivers/include/drivers/touch.h>
#include "lv_port_indev.h"

static lv_indev_state_t last_state = LV_INDEV_STATE_REL;
static rt_int16_t last_x = 0;
static rt_int16_t last_y = 0;
static lv_indev_t *indev_touchpad;
static aic_lv_touch_point_t touch_points[AIC_LV_TOUCH_MAX_POINTS];
static rt_mutex_t touch_points_mutex;

static void input_read(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = last_state;
}

void aic_touch_inputevent_cb(rt_int16_t x, rt_int16_t y, rt_uint8_t state)
{
    aic_touch_inputevent_cb_ex(x, y, 0, state);
}

void aic_touch_inputevent_cb_ex(rt_int16_t x, rt_int16_t y,
                                rt_uint8_t track_id, rt_uint8_t state)
{
    uint8_t i;

#ifdef AIC_BT_BT8858A
	extern int bt_hid_set_touch_event(int, unsigned short, unsigned short);
	bt_hid_set_touch_event(state, y, x);
#endif
    if (touch_points_mutex)
        rt_mutex_take(touch_points_mutex, RT_WAITING_FOREVER);
    if (track_id < AIC_LV_TOUCH_MAX_POINTS) {
        touch_points[track_id].x = x;
        touch_points[track_id].y = y;
        touch_points[track_id].pressed = state != RT_TOUCH_EVENT_UP;
    }
    switch (state)
    {
    case RT_TOUCH_EVENT_UP:
        last_state = LV_INDEV_STATE_RELEASED;
        for (i = 0; i < AIC_LV_TOUCH_MAX_POINTS; i++) {
            if (touch_points[i].pressed) {
                last_x = touch_points[i].x;
                last_y = touch_points[i].y;
                last_state = LV_INDEV_STATE_PRESSED;
                break;
            }
        }
        break;
    case RT_TOUCH_EVENT_MOVE:
    case RT_TOUCH_EVENT_DOWN:
        last_x = x;
        last_y = y;
        last_state = LV_INDEV_STATE_PRESSED;
        break;
#ifdef AIC_MONKEY_TEST
    case RT_TOUCH_MONKEY_TEST:
        last_x = x;
        last_y = y;
        last_state = LV_INDEV_STATE_PRESSED;
        break;
#endif
    }
    if (touch_points_mutex)
        rt_mutex_release(touch_points_mutex);
}

void aic_touch_snapshot(aic_lv_touch_point_t *points, uint8_t capacity)
{
    uint8_t count = capacity < AIC_LV_TOUCH_MAX_POINTS ? capacity : AIC_LV_TOUCH_MAX_POINTS;

    if (!points)
        return;
    if (touch_points_mutex)
        rt_mutex_take(touch_points_mutex, RT_WAITING_FOREVER);
    rt_memcpy(points, touch_points, count * sizeof(points[0]));
    if (touch_points_mutex)
        rt_mutex_release(touch_points_mutex);
}

void lv_port_indev_init(void)
{
    touch_points_mutex = rt_mutex_create("lvmtp", RT_IPC_FLAG_PRIO);
    indev_touchpad = lv_indev_create();
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_touchpad, input_read);
#ifdef AIC_USING_ENCODER
    extern lv_indev_t *lv_aic_encoder_create(void);
    lv_indev_t * encoder = lv_aic_encoder_create();
    if (encoder == NULL)
        return;

    lv_group_set_default(lv_group_create());
    lv_indev_set_group(encoder, lv_group_get_default());
#endif
#ifdef AIC_USE_LV_USB_MOUSE
    extern void lv_aic_mouse_hotplug_init(void);
    lv_aic_mouse_hotplug_init();
#endif
}
#endif
