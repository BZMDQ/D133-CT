#include "media_orientation.h"

int hmi_media_needs_rotation(unsigned width, unsigned height,
                             unsigned screen_width, unsigned screen_height)
{
    return (width > height && screen_width < screen_height) ||
           (width < height && screen_width > screen_height);
}

/* 图片保持原始像素。视频沿源坐标轴缩放，旋转后覆盖活动屏幕。 */
void hmi_media_place(lv_obj_t *image, unsigned width, unsigned height,
                     unsigned screen_width, unsigned screen_height, int fill)
{
    int rotate = hmi_media_needs_rotation(width, height, screen_width, screen_height);

    lv_image_set_pivot(image, width / 2, height / 2);
    lv_image_set_rotation(image, rotate ? 900 : 0);
    if (fill) {
        unsigned target_width = rotate ? screen_height : screen_width;
        unsigned target_height = rotate ? screen_width : screen_height;
        lv_image_set_scale_x(image, LV_MAX(1U, (target_width * 256U + width - 1U) / width));
        lv_image_set_scale_y(image, LV_MAX(1U, (target_height * 256U + height - 1U) / height));
    }
    lv_obj_center(image);
}
