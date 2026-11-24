#ifndef HMI_MEDIA_ORIENTATION_H
#define HMI_MEDIA_ORIENTATION_H
#include "lvgl.h"

int hmi_media_needs_rotation(unsigned width, unsigned height,
                             unsigned screen_width, unsigned screen_height);
void hmi_media_place(lv_obj_t *image, unsigned width, unsigned height,
                     unsigned screen_width, unsigned screen_height, int fill);
#endif
