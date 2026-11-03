#ifndef HMI_IMAGE_PREVIEW_H
#define HMI_IMAGE_PREVIEW_H

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    IMAGE_PREVIEW_IDLE = 0,
    IMAGE_PREVIEW_LOADING,
    IMAGE_PREVIEW_READY,
    IMAGE_PREVIEW_FAILED,
    IMAGE_PREVIEW_NOT_FOUND,
    IMAGE_PREVIEW_RELEASING
} image_preview_state_t;

/* 图片文件读取和解码在线程内完成；返回给界面的只是只读图像描述符。 */
int image_preview_init(void);
int image_preview_supported(const char *path);
int image_preview_request(const char *path);
/* 在后台扫描指定目录，只解码宽高与当前屏幕完全相同的图片。 */
int image_preview_request_matching(const char *directory,
                                  uint16_t width, uint16_t height);
image_preview_state_t image_preview_status(char *text, size_t text_size);
const lv_image_dsc_t *image_preview_get_image(void);
void image_preview_cancel(void);

#endif
