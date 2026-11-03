/*
 * Copyright (c) 2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Ning Fang <ning.fang@artinchip.com>
 */

#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>

#define AIC_LV_TOUCH_MAX_POINTS 5
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t pressed;
} aic_lv_touch_point_t;

void lv_port_indev_init(void);
void aic_touch_inputevent_cb(rt_int16_t x, rt_int16_t y, rt_uint8_t state);
void aic_touch_inputevent_cb_ex(rt_int16_t x, rt_int16_t y,
                                rt_uint8_t track_id, rt_uint8_t state);
void aic_touch_snapshot(aic_lv_touch_point_t *points, uint8_t capacity);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
