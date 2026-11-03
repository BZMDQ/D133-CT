#include "image_preview.h"
#include "hmi_serial_log.h"
#include "file_manager.h"
#include "display_profile_manager.h"
#include <lv_mpp_dec.h>
#include <rtthread.h>
#include <aic_core.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define IMAGE_PREVIEW_FILE_MAX (4U * 1024U * 1024U)
#define IMAGE_PREVIEW_PNG_MAX_PIXELS (8ULL * 1024ULL * 1024ULL)
#define IMAGE_PREVIEW_LVFS_PREFIX "L:"
#define IMAGE_BMP_BATCH_ROWS 16U
#define IMAGE_BMP_BATCH_MAX_BYTES (128U * 1024U)

typedef struct {
    char path[FM_PATH_MAX];
    uint16_t match_width;
    uint16_t match_height;
    uint8_t match_resolution;
} image_preview_job_t;

typedef enum {
    IMAGE_KIND_UNSUPPORTED = 0,
    IMAGE_KIND_JPEG,
    IMAGE_KIND_PNG,
    IMAGE_KIND_BMP
} image_kind_t;

static struct rt_mutex state_lock;
static rt_mq_t job_queue;
static int manager_ready;
static image_preview_state_t current_state = IMAGE_PREVIEW_IDLE;
static int last_error;
static int cancel_requested;
static int decoder_opened;
static lv_image_decoder_dsc_t decoder_dsc;
static lv_image_dsc_t preview_image;
static void *preview_owned_buffer;
static char preview_source_path[FM_PATH_MAX + sizeof(IMAGE_PREVIEW_LVFS_PREFIX)];

enum {
    IMAGE_PREVIEW_ERR_NONE = 0,
    IMAGE_PREVIEW_ERR_PATH = -1,
    IMAGE_PREVIEW_ERR_SIZE = -2,
    IMAGE_PREVIEW_ERR_FORMAT = -3,
    IMAGE_PREVIEW_ERR_DECODE = -4,
    IMAGE_PREVIEW_ERR_CANCELLED = -5,
    IMAGE_PREVIEW_ERR_NOT_FOUND = -6
};

static int extension_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return 0;
        left++;
        right++;
    }
    return *left == 0 && *right == 0;
}

static image_kind_t image_kind(const char *path)
{
    const char *extension;

    if (!path)
        return IMAGE_KIND_UNSUPPORTED;
    extension = strrchr(path, '.');
    if (!extension)
        return IMAGE_KIND_UNSUPPORTED;
    if (extension_equal(extension, ".jpg") || extension_equal(extension, ".jpeg"))
        return IMAGE_KIND_JPEG;
    if (extension_equal(extension, ".png"))
        return IMAGE_KIND_PNG;
    if (extension_equal(extension, ".bmp"))
        return IMAGE_KIND_BMP;
    return IMAGE_KIND_UNSUPPORTED;
}

int image_preview_supported(const char *path)
{
    return image_kind(path) != IMAGE_KIND_UNSUPPORTED;
}

static int preview_cancelled(void)
{
    int cancelled;

    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    cancelled = cancel_requested;
    rt_mutex_release(&state_lock);
    return cancelled;
}

static void release_decode_resources(void)
{
    if (decoder_opened) {
        lv_image_decoder_close(&decoder_dsc);
        decoder_opened = 0;
        memset(&decoder_dsc, 0, sizeof(decoder_dsc));
    }
    if (preview_owned_buffer) {
        aicos_free(MEM_CMA, preview_owned_buffer);
        preview_owned_buffer = NULL;
    }
    memset(&preview_image, 0, sizeof(preview_image));
}

static int dimensions_supported(const lv_image_header_t *header)
{
    /* LVGL 的图像描述符使用 16 位宽高；不再人为限制为 1024×768。 */
    return header && header->w > 0 && header->h > 0;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint8_t bmp_mask_to_u8(uint32_t pixel, uint32_t mask)
{
    uint32_t shift = 0;
    uint32_t maximum;
    uint32_t value;

    while (shift < 32U && ((mask >> shift) & 1U) == 0U)
        shift++;
    if (shift == 32U)
        return 0;
    maximum = mask >> shift;
    value = (pixel & mask) >> shift;
    if (maximum == 0U)
        return 0;
    return (uint8_t)((value * 255U + maximum / 2U) / maximum);
}

static void preview_viewport(uint16_t *width, uint16_t *height)
{
    display_profile_info_t profile;

    if (!width || !height)
        return;
    if (display_profile_get_active(&profile) == DISPLAY_PROFILE_OK &&
        profile.width > 0 && profile.height > 0) {
        *width = profile.width;
        *height = profile.height;
    } else {
        /* 仅在串口配置服务尚未启动时回退到当前 CS000 屏幕。 */
        *width = 1024;
        *height = 600;
    }
}

static int read_png_dimensions(const char *path, uint32_t *width, uint32_t *height)
{
    uint8_t header[24];
    FILE *file;
    int result = IMAGE_PREVIEW_ERR_DECODE;

    file = fopen(path, "rb");
    if (!file)
        return IMAGE_PREVIEW_ERR_PATH;
    if (fread(header, 1, sizeof(header), file) == sizeof(header) &&
        memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        memcmp(header + 12, "IHDR", 4) == 0) {
        uint32_t image_width = read_be32(header + 16);
        uint32_t image_height = read_be32(header + 20);
        uint64_t pixels = (uint64_t)image_width * image_height;

        if (image_width && image_height && image_width <= UINT16_MAX &&
            image_height <= UINT16_MAX && pixels <= IMAGE_PREVIEW_PNG_MAX_PIXELS) {
            *width = image_width;
            *height = image_height;
            result = IMAGE_PREVIEW_ERR_NONE;
        } else {
            result = IMAGE_PREVIEW_ERR_SIZE;
        }
    }
    fclose(file);
    return result;
}

/* BMP 只读屏幕中央可见窗口，源图片分辨率不会决定输出缓冲区大小。 */
static int decode_bmp_preview(const char *path, uint64_t file_size)
{
    uint8_t header[54];
    uint8_t *source_row = NULL;
    uint8_t *source_buffer = NULL;
    uint8_t *raw = NULL;
    uint8_t *output;
    FILE *file = NULL;
    int32_t signed_width;
    int32_t signed_height;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_offset;
    uint32_t dib_size;
    uint32_t compression;
    uint32_t red_mask = 0;
    uint32_t green_mask = 0;
    uint32_t blue_mask = 0;
    uint32_t source_stride;
    uint64_t source_stride_wide;
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t output_stride;
    uint32_t bytes_per_pixel;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint16_t viewport_width;
    uint16_t viewport_height;
    uint64_t row_bits;
    uint64_t image_end;
    uint64_t output_size;
    uint64_t source_bytes;
    uint64_t next_offset = UINT64_MAX;
    uint32_t row_batch;
    uint32_t y;
    int top_down;
    int result = IMAGE_PREVIEW_ERR_DECODE;

    file = fopen(path, "rb");
    if (!file)
        return IMAGE_PREVIEW_ERR_PATH;
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 'B' || header[1] != 'M')
        goto done;

    pixel_offset = read_le32(header + 10);
    dib_size = read_le32(header + 14);
    signed_width = (int32_t)read_le32(header + 18);
    signed_height = (int32_t)read_le32(header + 22);
    planes = read_le16(header + 26);
    bits_per_pixel = read_le16(header + 28);
    compression = read_le32(header + 30);
    if (dib_size < 40 || (uint64_t)pixel_offset < 14ULL + dib_size ||
        14ULL + dib_size > file_size || signed_width <= 0 ||
        signed_height == 0 || signed_height == INT32_MIN || planes != 1 ||
        (compression != 0 && compression != 3) ||
        (bits_per_pixel != 16 && bits_per_pixel != 24 && bits_per_pixel != 32) ||
        (compression == 3 && bits_per_pixel == 24)) {
        result = IMAGE_PREVIEW_ERR_FORMAT;
        goto done;
    }

    if (compression == 3) {
        uint8_t masks[12];

        if (pixel_offset < 66U || fseek(file, 54, SEEK_SET) != 0 ||
            fread(masks, 1, sizeof(masks), file) != sizeof(masks))
            goto done;
        red_mask = read_le32(masks);
        green_mask = read_le32(masks + 4);
        blue_mask = read_le32(masks + 8);
        if (!red_mask || !green_mask || !blue_mask) {
            result = IMAGE_PREVIEW_ERR_FORMAT;
            goto done;
        }
    } else if (bits_per_pixel == 16) {
        /* BI_RGB 的 16 位 BMP 按标准 5-5-5 解码，再转换成屏幕 RGB565。 */
        red_mask = 0x7C00U;
        green_mask = 0x03E0U;
        blue_mask = 0x001FU;
    }

    width = (uint32_t)signed_width;
    height = (uint32_t)(signed_height < 0 ? -signed_height : signed_height);
    top_down = signed_height < 0;
    row_bits = (uint64_t)width * bits_per_pixel;
    source_stride_wide = ((row_bits + 31U) / 32U) * 4U;
    if (source_stride_wide == 0 || source_stride_wide > UINT32_MAX) {
        result = IMAGE_PREVIEW_ERR_SIZE;
        goto done;
    }
    source_stride = (uint32_t)source_stride_wide;
    image_end = (uint64_t)pixel_offset + (uint64_t)source_stride * height;
    if (source_stride == 0 || image_end > file_size || image_end > LONG_MAX) {
        result = IMAGE_PREVIEW_ERR_SIZE;
        goto done;
    }

    preview_viewport(&viewport_width, &viewport_height);
    output_width = width < viewport_width ? width : viewport_width;
    output_height = height < viewport_height ? height : viewport_height;
    crop_x = (width - output_width) / 2U;
    crop_y = (height - output_height) / 2U;
    bytes_per_pixel = bits_per_pixel / 8U;
    source_bytes = (uint64_t)output_width * bytes_per_pixel;
    output_stride = lv_draw_buf_width_to_stride(output_width, LV_COLOR_FORMAT_RGB565);
    output_size = (uint64_t)output_stride * output_height;
    if (source_bytes == 0 || source_bytes > UINT32_MAX || output_size == 0 ||
        output_size > INT_MAX - CACHE_LINE_SIZE) {
        result = IMAGE_PREVIEW_ERR_SIZE;
        goto done;
    }

    /* 连续完整行最多批读 16 行，缓冲控制在 128 KiB 内。 */
    row_batch = 1U;
    if (source_bytes == source_stride && source_bytes <= IMAGE_BMP_BATCH_MAX_BYTES) {
        row_batch = IMAGE_BMP_BATCH_MAX_BYTES / (uint32_t)source_bytes;
        if (row_batch > IMAGE_BMP_BATCH_ROWS)
            row_batch = IMAGE_BMP_BATCH_ROWS;
    }
    source_buffer = (uint8_t *)rt_malloc((size_t)source_bytes * row_batch);
    if (!source_buffer && row_batch > 1U) {
        /* 内存紧张时逐步减小批次，最后回退逐行读取。 */
        while (!source_buffer && row_batch > 1U) {
            row_batch /= 2U;
            source_buffer = (uint8_t *)rt_malloc((size_t)source_bytes * row_batch);
        }
    }
    if (!source_buffer) {
        source_buffer = (uint8_t *)rt_malloc((size_t)source_bytes);
    }
    raw = (uint8_t *)aicos_malloc_try_cma((int)output_size + CACHE_LINE_SIZE - 1);
    if (!source_buffer || !raw) {
        result = IMAGE_PREVIEW_ERR_SIZE;
        goto done;
    }
    output = (uint8_t *)ALIGN_UP((ulong)raw, CACHE_LINE_SIZE);

    for (y = 0; y < output_height; y++) {
        /* 总是按文件偏移递增读取，倒序 BMP 在内存中翻转显示行。 */
        uint32_t source_y = top_down ? crop_y + y : height - crop_y - output_height + y;
        uint32_t output_y = top_down ? y : output_height - 1U - y;
        uint64_t source_offset = (uint64_t)pixel_offset +
            (uint64_t)source_y * source_stride + (uint64_t)crop_x * bytes_per_pixel;
        uint8_t *output_row = output + (size_t)output_y * output_stride;
        uint32_t x;

        if ((y % row_batch) == 0U) {
            uint32_t rows = output_height - y;
            size_t read_bytes;
            if (rows > row_batch)
                rows = row_batch;
            read_bytes = (size_t)source_bytes * rows;
            if (source_offset + read_bytes > image_end || source_offset > LONG_MAX ||
                (source_offset != next_offset && fseek(file, (long)source_offset, SEEK_SET) != 0) ||
                fread(source_buffer, 1, read_bytes, file) != read_bytes) {
                result = IMAGE_PREVIEW_ERR_DECODE;
                goto done;
            }
            next_offset = source_offset + read_bytes;
        }
        source_row = source_buffer + (size_t)(y % row_batch) * (size_t)source_bytes;

        /* 常见 BGR24/BGRX32 不走通用格式分派与逐像素 memcpy。 */
        if (bits_per_pixel == 24 ||
            (bits_per_pixel == 32 &&
             (compression == 0 ||
              (red_mask == 0x00FF0000U && green_mask == 0x0000FF00U &&
               blue_mask == 0x000000FFU)))) {
            const uint8_t *src = source_row;
            uint16_t *dst = (uint16_t *)output_row;

            for (x = 0; x < output_width; x++) {
                uint8_t blue = src[0];
                uint8_t green = src[1];
                uint8_t red = src[2];
                dst[x] = (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                                    ((uint16_t)(green & 0xFCU) << 3) |
                                    ((uint16_t)blue >> 3));
                src += bytes_per_pixel;
            }
            if (output_stride > output_width * 2U)
                memset(output_row + output_width * 2U, 0,
                       output_stride - output_width * 2U);
            continue;
        }

        /* 标准 RGB565 无需逐像素转换，直接复制到目标行。 */
        if (bits_per_pixel == 16 && red_mask == 0xF800U &&
            green_mask == 0x07E0U && blue_mask == 0x001FU) {
            memcpy(output_row, source_row, (size_t)source_bytes);
            if (output_stride > output_width * 2U)
                memset(output_row + output_width * 2U, 0, output_stride - output_width * 2U);
            continue;
        }

        for (x = 0; x < output_width; x++) {
            uint16_t rgb565;
            if (bits_per_pixel == 16 && red_mask == 0x7C00U &&
                green_mask == 0x03E0U && blue_mask == 0x001FU) {
                uint16_t pixel = read_le16(source_row + (size_t)x * 2U);
                uint16_t green = (pixel >> 5) & 0x1FU;
                /* 标准 RGB555 的 5 位绿色扩展到 6 位。 */
                rgb565 = (uint16_t)(((pixel & 0x7C00U) << 1) |
                                    (((green << 1) | (green >> 4)) << 5) |
                                    (pixel & 0x001FU));
            } else if (bits_per_pixel == 16) {
                uint32_t pixel = read_le16(source_row + (size_t)x * 2U);
                uint8_t red = bmp_mask_to_u8(pixel, red_mask);
                uint8_t green = bmp_mask_to_u8(pixel, green_mask);
                uint8_t blue = bmp_mask_to_u8(pixel, blue_mask);
                rgb565 = (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                                    ((uint16_t)(green & 0xFCU) << 3) |
                                    ((uint16_t)blue >> 3));
            } else if (bits_per_pixel == 32 && compression == 3 &&
                       !(red_mask == 0x00FF0000U && green_mask == 0x0000FF00U &&
                         blue_mask == 0x000000FFU)) {
                const uint8_t *pixel = source_row + (size_t)x * 4U;
                uint32_t value = read_le32(pixel);
                uint8_t red = bmp_mask_to_u8(value, red_mask);
                uint8_t green = bmp_mask_to_u8(value, green_mask);
                uint8_t blue = bmp_mask_to_u8(value, blue_mask);
                rgb565 = (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                                    ((uint16_t)(green & 0xFCU) << 3) |
                                    ((uint16_t)blue >> 3));
            } else {
                const uint8_t *pixel = source_row + (size_t)x * bytes_per_pixel;
                uint8_t blue = pixel[0];
                uint8_t green = pixel[1];
                uint8_t red = pixel[2];
                rgb565 = (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                                    ((uint16_t)(green & 0xFCU) << 3) |
                                    ((uint16_t)blue >> 3));
            }
            memcpy(output_row + (size_t)x * sizeof(rgb565), &rgb565, sizeof(rgb565));
        }
        if (output_stride > output_width * 2U)
            memset(output_row + output_width * 2U, 0, output_stride - output_width * 2U);
    }

    aicos_dcache_clean_range((unsigned long *)output, (int64_t)output_size);
    preview_owned_buffer = raw;
    raw = NULL;
    memset(&preview_image, 0, sizeof(preview_image));
    preview_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_image.header.cf = LV_COLOR_FORMAT_RGB565;
    preview_image.header.w = output_width;
    preview_image.header.h = output_height;
    preview_image.header.stride = output_stride;
    preview_image.data_size = (uint32_t)output_size;
    preview_image.data = output;
    result = IMAGE_PREVIEW_ERR_NONE;

done:
    if (source_buffer)
        rt_free(source_buffer);
    if (raw)
        aicos_free(MEM_CMA, raw);
    if (file)
        fclose(file);
    return result;
}

/* 各解码器输出统一归一化到屏幕原生 RGB565，避免格式映射造成通道偏色。 */
static int normalize_decoded_frame_to_rgb565(const lv_draw_buf_t *decoded)
{
    lv_image_header_t source_header = decoded->header;
    rt_uint8_t *pixels;
    rt_uint32_t source_stride = source_header.stride;
    rt_uint32_t destination_stride;
    rt_uint32_t source_pixel_size;
    rt_uint32_t y;
    rt_uint32_t x;
    size_t destination_size;

    if (source_header.cf == LV_COLOR_FORMAT_RGB565) {
        preview_image.header = source_header;
        preview_image.header.magic = LV_IMAGE_HEADER_MAGIC;
        preview_image.data_size = decoded->data_size;
        preview_image.data = decoded->unaligned_data;
        preview_image.reserved = decoded;
        return IMAGE_PREVIEW_ERR_NONE;
    }

    if (source_header.cf == LV_COLOR_FORMAT_RGB888)
        source_pixel_size = 3U;
    else if (source_header.cf == LV_COLOR_FORMAT_ARGB8888 ||
             source_header.cf == LV_COLOR_FORMAT_XRGB8888)
        source_pixel_size = 4U;
    else
        return IMAGE_PREVIEW_ERR_FORMAT;

    if (source_stride < (rt_uint32_t)source_header.w * source_pixel_size)
        return IMAGE_PREVIEW_ERR_SIZE;

    destination_stride = lv_draw_buf_width_to_stride(source_header.w,
                                                      LV_COLOR_FORMAT_RGB565);
    destination_size = (size_t)destination_stride * source_header.h;
    if (!destination_stride || destination_stride > source_stride ||
        destination_size > decoded->data_size)
        return IMAGE_PREVIEW_ERR_SIZE;

    pixels = (rt_uint8_t *)lv_draw_buf_align(decoded->unaligned_data,
                                             LV_COLOR_FORMAT_RGB888);
    if (!pixels)
        return IMAGE_PREVIEW_ERR_DECODE;

    for (y = 0; y < source_header.h; y++) {
        rt_uint8_t *source_row = pixels + (size_t)y * source_stride;
        rt_uint8_t *destination_row = pixels + (size_t)y * destination_stride;

        for (x = 0; x < source_header.w; x++) {
            const rt_uint8_t *source_pixel = source_row + x * source_pixel_size;
            rt_uint8_t blue = source_pixel[0];
            rt_uint8_t green = source_pixel[1];
            rt_uint8_t red = source_pixel[2];
            rt_uint8_t alpha = source_header.cf == LV_COLOR_FORMAT_ARGB8888 ?
                               source_pixel[3] : 0xFF;
            rt_uint32_t inverse_alpha = 0xFFU - alpha;
            uint32_t red32;
            uint32_t green32;
            uint32_t blue32;
            uint16_t rgb565 = (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                                         ((uint16_t)(green & 0xFCU) << 3) |
                                         ((uint16_t)blue >> 3));

            if (alpha != 0xFFU) {
                if (source_header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED) {
                    red32 = red + (0x11U * inverse_alpha + 127U) / 255U;
                    green32 = green + (0x18U * inverse_alpha + 127U) / 255U;
                    blue32 = blue + (0x27U * inverse_alpha + 127U) / 255U;
                } else {
                    red32 = (red * alpha + 0x11U * inverse_alpha + 127U) / 255U;
                    green32 = (green * alpha + 0x18U * inverse_alpha + 127U) / 255U;
                    blue32 = (blue * alpha + 0x27U * inverse_alpha + 127U) / 255U;
                }
                red = (rt_uint8_t)(red32 > 0xFFU ? 0xFFU : red32);
                green = (rt_uint8_t)(green32 > 0xFFU ? 0xFFU : green32);
                blue = (rt_uint8_t)(blue32 > 0xFFU ? 0xFFU : blue32);
                rgb565 = (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                                    ((uint16_t)(green & 0xFCU) << 3) |
                                    ((uint16_t)blue >> 3));
            }

            memcpy(destination_row + x * 2U, &rgb565, sizeof(rgb565));
        }
    }

    preview_image.header = source_header;
    preview_image.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_image.header.cf = LV_COLOR_FORMAT_RGB565;
    preview_image.header.stride = destination_stride;
    preview_image.data_size = (uint32_t)destination_size;
    preview_image.data = pixels;
    preview_image.reserved = decoded;
    aicos_dcache_clean_range((unsigned long *)pixels, (int64_t)destination_size);
    hmi_serial_log("[image_preview] MPP color format %u normalized to RGB565: %u x %u\n",
               (unsigned)source_header.cf, source_header.w, source_header.h);
    return IMAGE_PREVIEW_ERR_NONE;
}

static int decode_mpp_image(const lv_image_header_t *header)
{
    lv_image_decoder_args_t args = {0};
    struct mpp_dec_crop_info crop = {0};
    uint16_t viewport_width;
    uint16_t viewport_height;
    uint32_t crop_width;
    uint32_t crop_height;
    int crop_requested = 0;
    rt_uint32_t decode_started;
    rt_uint32_t decode_ms;
    rt_uint32_t normalize_started;
    rt_uint32_t normalize_ms = 0;
    int result;

    if (!dimensions_supported(header))
        return IMAGE_PREVIEW_ERR_SIZE;

    preview_viewport(&viewport_width, &viewport_height);
    crop_width = header->w < viewport_width ? header->w : viewport_width;
    crop_height = header->h < viewport_height ? header->h : viewport_height;
    if (crop_width != header->w || crop_height != header->h) {
        crop.crop_x = ((int)header->w - (int)crop_width) / 2;
        crop.crop_y = ((int)header->h - (int)crop_height) / 2;
        crop.crop_width = (int)crop_width;
        crop.crop_height = (int)crop_height;
        crop.crop_out_x = 0;
        crop.crop_out_y = 0;
        if (lv_mpp_dec_set_preview_crop(preview_source_path, &crop) != 0)
            return IMAGE_PREVIEW_ERR_DECODE;
        crop_requested = 1;
    }

    args.no_cache = true;
    memset(&decoder_dsc, 0, sizeof(decoder_dsc));
    decode_started = rt_tick_get_millisecond();
    if (lv_image_decoder_open(&decoder_dsc, preview_source_path, &args) != LV_RESULT_OK) {
        if (crop_requested)
            lv_mpp_dec_clear_preview_crop(preview_source_path);
        hmi_serial_log("[image_preview] SDK MPP 打开/解码失败，耗时 %u ms：%s\n",
                   (unsigned)(rt_tick_get_millisecond() - decode_started),
                   preview_source_path);
        return IMAGE_PREVIEW_ERR_DECODE;
    }
    if (crop_requested)
        lv_mpp_dec_clear_preview_crop(preview_source_path);
    decode_ms = rt_tick_get_millisecond() - decode_started;
    decoder_opened = 1;
    if (!decoder_dsc.decoded || !decoder_dsc.decoded->unaligned_data ||
        (decoder_dsc.header.cf != LV_COLOR_FORMAT_RGB565 &&
         decoder_dsc.header.cf != LV_COLOR_FORMAT_RGB888 &&
         decoder_dsc.header.cf != LV_COLOR_FORMAT_ARGB8888 &&
         decoder_dsc.header.cf != LV_COLOR_FORMAT_XRGB8888)) {
        hmi_serial_log("[image_preview] 解码器输出格式无效：%s\n", preview_source_path);
        lv_image_decoder_close(&decoder_dsc);
        decoder_opened = 0;
        memset(&decoder_dsc, 0, sizeof(decoder_dsc));
        return IMAGE_PREVIEW_ERR_FORMAT;
    }

    /* 各种源格式在工作线程归一为 RGB565，避免 LVGL/GE2D 对格式解释不一致。 */
    normalize_started = rt_tick_get_millisecond();
    result = normalize_decoded_frame_to_rgb565(decoder_dsc.decoded);
    normalize_ms = rt_tick_get_millisecond() - normalize_started;
    hmi_serial_log("[image_preview] 阶段耗时：MPP=%u ms，RGB565=%u ms，尺寸=%u x %u，CF=%u\n",
               decode_ms, normalize_ms, decoder_dsc.header.w, decoder_dsc.header.h,
               (unsigned)decoder_dsc.header.cf);
    if (result != IMAGE_PREVIEW_ERR_NONE) {
        lv_image_decoder_close(&decoder_dsc);
        decoder_opened = 0;
        memset(&decoder_dsc, 0, sizeof(decoder_dsc));
    }
    return result;
}

static int load_image(const char *path, const lv_image_header_t *known_header)
{
    struct stat st;
    lv_image_header_t header = {0};
    image_kind_t kind = image_kind(path);
    uint32_t png_width = 0;
    uint32_t png_height = 0;
    size_t path_len;
    rt_uint32_t started = rt_tick_get_millisecond();
    rt_uint32_t info_ms;
    rt_uint32_t info_started;
    rt_uint32_t decode_started;
    int result;

    if (kind == IMAGE_KIND_UNSUPPORTED)
        return IMAGE_PREVIEW_ERR_FORMAT;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        hmi_serial_log("[image_preview] 文件不可读或大小无效：%s\n", path);
        return IMAGE_PREVIEW_ERR_SIZE;
    }

    if (kind == IMAGE_KIND_BMP) {
        decode_started = rt_tick_get_millisecond();
        result = decode_bmp_preview(path, (uint64_t)st.st_size);
        hmi_serial_log("[image_preview] BMP 可视区域读取耗时：%u ms，结果=%d\n",
                       (unsigned)(rt_tick_get_millisecond() - decode_started), result);
        return result;
    }

    /* JPEG/PNG 的官方 MPP 解码器会缓存压缩输入，保留有界文件大小保护。 */
    if ((uint64_t)st.st_size > IMAGE_PREVIEW_FILE_MAX) {
        hmi_serial_log("[image_preview] 压缩图片超过 MPP 安全输入上限：%s\n", path);
        return IMAGE_PREVIEW_ERR_SIZE;
    }

    if (kind == IMAGE_KIND_PNG) {
        result = read_png_dimensions(path, &png_width, &png_height);
        if (result != IMAGE_PREVIEW_ERR_NONE) {
            hmi_serial_log("[image_preview] PNG 尺寸无效或超过 SDK 解码范围：%s\n", path);
            return result;
        }
    }

    path_len = strlen(path);
    if (path_len + 2 >= sizeof(preview_source_path))
        return IMAGE_PREVIEW_ERR_PATH;
    preview_source_path[0] = IMAGE_PREVIEW_LVFS_PREFIX[0];
    preview_source_path[1] = IMAGE_PREVIEW_LVFS_PREFIX[1];
    memcpy(preview_source_path + 2, path, path_len + 1);
    /* FAT32 不区分大小写；统一扩展名小写以匹配 SDK 解码器后缀判断。 */
    {
        char *extension = strrchr(preview_source_path, '.');
        if (extension) {
            while (*extension) {
                *extension = (char)tolower((unsigned char)*extension);
                extension++;
            }
        }
    }

    info_started = rt_tick_get_millisecond();
    /* 屏幕测试目录扫描已读取文件头时，避免对同一图片重复读取。 */
    if (known_header) {
        header = *known_header;
    } else if (lv_image_decoder_get_info(preview_source_path, &header) != LV_RESULT_OK) {
        hmi_serial_log("[image_preview] SDK 解码器无法识别文件头：%s\n", path);
        return IMAGE_PREVIEW_ERR_DECODE;
    }
    info_ms = rt_tick_get_millisecond() - info_started;
    if (!dimensions_supported(&header) ||
        (kind == IMAGE_KIND_PNG &&
         (header.w != png_width || header.h != png_height))) {
        hmi_serial_log("[image_preview] 图片尺寸超出 LVGL 描述符或 SDK 范围：%s (%u x %u)\n",
                       path, header.w, header.h);
        return IMAGE_PREVIEW_ERR_SIZE;
    }
    if (preview_cancelled())
        return IMAGE_PREVIEW_ERR_CANCELLED;

    decode_started = rt_tick_get_millisecond();
    result = decode_mpp_image(&header);
    hmi_serial_log("[image_preview] 总耗时：%u ms，文件头=%u ms，解码/归一化=%u ms，结果=%d\n",
               (unsigned)(rt_tick_get_millisecond() - started), info_ms,
               (unsigned)(rt_tick_get_millisecond() - decode_started), result);
    return result;
}

/* 生成 LVGL 文件系统路径并统一扩展名大小写，目录检查与正式解码共用。 */
static int make_lv_image_path(const char *path, char *lv_path, size_t path_size)
{
    size_t path_len;
    char *extension;

    if (!path || !lv_path)
        return -1;
    path_len = strlen(path);
    if (path_len + 2 >= path_size)
        return -1;
    lv_path[0] = IMAGE_PREVIEW_LVFS_PREFIX[0];
    lv_path[1] = IMAGE_PREVIEW_LVFS_PREFIX[1];
    memcpy(lv_path + 2, path, path_len + 1);
    extension = strrchr(lv_path, '.');
    if (extension) {
        while (*extension) {
            *extension = (char)tolower((unsigned char)*extension);
            extension++;
        }
    }
    return 0;
}

/* 目录扫描和解码都在图片工作线程执行，LVGL 线程不会访问 SD 文件系统。 */
static int load_matching_image(const image_preview_job_t *job)
{
    DIR *directory;
    struct dirent *entry;
    struct stat st;
    char candidate[FM_PATH_MAX];
    char lv_path[FM_PATH_MAX + sizeof(IMAGE_PREVIEW_LVFS_PREFIX)];
    lv_image_header_t header;
    int found_matching_size = 0;
    int last_decode_error = IMAGE_PREVIEW_ERR_DECODE;

    directory = opendir(job->path);
    if (!directory)
        return IMAGE_PREVIEW_ERR_NOT_FOUND;

    while ((entry = readdir(directory)) != NULL) {
        if (preview_cancelled()) {
            closedir(directory);
            return IMAGE_PREVIEW_ERR_CANCELLED;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !image_preview_supported(entry->d_name))
            continue;
        if (snprintf(candidate, sizeof(candidate), "%s/%s",
                     job->path, entry->d_name) >= (int)sizeof(candidate))
            continue;
        if (stat(candidate, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
            continue;
        if (make_lv_image_path(candidate, lv_path, sizeof(lv_path)) ||
            lv_image_decoder_get_info(lv_path, &header) != LV_RESULT_OK ||
            header.w != job->match_width || header.h != job->match_height)
            continue;

        found_matching_size = 1;
        last_decode_error = load_image(candidate, &header);
        if (last_decode_error == IMAGE_PREVIEW_ERR_NONE) {
            closedir(directory);
            return IMAGE_PREVIEW_ERR_NONE;
        }
        release_decode_resources();
    }

    closedir(directory);
    return found_matching_size ? last_decode_error : IMAGE_PREVIEW_ERR_NOT_FOUND;
}

static const char *error_text(int error)
{
    switch (error) {
    case IMAGE_PREVIEW_ERR_NOT_FOUND: return "SD没有本机测试图片";
    case IMAGE_PREVIEW_ERR_PATH: return "图片路径无效。";
    case IMAGE_PREVIEW_ERR_SIZE: return "图片缓冲或尺寸超过当前 SDK 的安全范围；JPEG/PNG 输入上限为 4 MiB，PNG 像素上限为 800 万。";
    case IMAGE_PREVIEW_ERR_FORMAT: return "图片格式不支持，请选择基线 JPG/JPEG、PNG 或 16/24/32 位 BMP。";
    case IMAGE_PREVIEW_ERR_CANCELLED: return "已取消图片加载。";
    case IMAGE_PREVIEW_ERR_DECODE:
    default: return "SDK 解码器无法识别图片；请使用基线 JPG/JPEG、PNG 或 16/24/32 位 BMP。";
    }
}

static void image_worker(void *parameter)
{
    image_preview_job_t job;

    LV_UNUSED(parameter);
    while (1) {
        int result;
        int cancelled;

        if (rt_mq_recv(job_queue, &job, sizeof(job), RT_WAITING_FOREVER) != RT_EOK)
            continue;
        result = job.match_resolution ? load_matching_image(&job) : load_image(job.path, NULL);
        rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
        cancelled = cancel_requested || result == IMAGE_PREVIEW_ERR_CANCELLED;
        rt_mutex_release(&state_lock);

        if (cancelled) {
            release_decode_resources();
            rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
            current_state = IMAGE_PREVIEW_IDLE;
            cancel_requested = 0;
            last_error = IMAGE_PREVIEW_ERR_NONE;
            rt_mutex_release(&state_lock);
            continue;
        }

        rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
        last_error = result;
        if (result == IMAGE_PREVIEW_ERR_NONE)
            current_state = IMAGE_PREVIEW_READY;
        else if (result == IMAGE_PREVIEW_ERR_NOT_FOUND)
            current_state = IMAGE_PREVIEW_NOT_FOUND;
        else
            current_state = IMAGE_PREVIEW_FAILED;
        rt_mutex_release(&state_lock);
    }
}

int image_preview_init(void)
{
    rt_thread_t thread;

    if (manager_ready)
        return 0;
    if (rt_mutex_init(&state_lock, "imgprev", RT_IPC_FLAG_PRIO) != RT_EOK)
        return -1;
    job_queue = rt_mq_create("imgprev", sizeof(image_preview_job_t), 1, RT_IPC_FLAG_FIFO);
    if (!job_queue)
        return -2;
    thread = rt_thread_create("hmi_imgprev", image_worker, NULL, 8192, 26, 10);
    if (!thread)
        return -3;
    if (rt_thread_startup(thread) != RT_EOK)
        return -4;
    manager_ready = 1;
    return 0;
}

int image_preview_request(const char *path)
{
    image_preview_job_t job = {0};

    if (!manager_ready || !path || strlen(path) >= sizeof(job.path))
        return -1;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    if (!job_queue || current_state != IMAGE_PREVIEW_IDLE) {
        rt_mutex_release(&state_lock);
        return -2;
    }
    current_state = IMAGE_PREVIEW_LOADING;
    cancel_requested = 0;
    last_error = IMAGE_PREVIEW_ERR_NONE;
    snprintf(job.path, sizeof(job.path), "%s", path);
    rt_mutex_release(&state_lock);

    if (rt_mq_send(job_queue, &job, sizeof(job)) != RT_EOK) {
        rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
        current_state = IMAGE_PREVIEW_IDLE;
        rt_mutex_release(&state_lock);
        return -3;
    }
    return 0;
}

int image_preview_request_matching(const char *directory,
                                  uint16_t width, uint16_t height)
{
    image_preview_job_t job = {0};

    if (!manager_ready || !directory || !width || !height ||
        strlen(directory) >= sizeof(job.path))
        return -1;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    if (!job_queue || current_state != IMAGE_PREVIEW_IDLE) {
        rt_mutex_release(&state_lock);
        return -2;
    }
    current_state = IMAGE_PREVIEW_LOADING;
    cancel_requested = 0;
    last_error = IMAGE_PREVIEW_ERR_NONE;
    snprintf(job.path, sizeof(job.path), "%s", directory);
    job.match_width = width;
    job.match_height = height;
    job.match_resolution = 1;
    rt_mutex_release(&state_lock);

    if (rt_mq_send(job_queue, &job, sizeof(job)) != RT_EOK) {
        rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
        current_state = IMAGE_PREVIEW_IDLE;
        rt_mutex_release(&state_lock);
        return -3;
    }
    return 0;
}

image_preview_state_t image_preview_status(char *text, size_t text_size)
{
    image_preview_state_t state;
    int error;
    const char *status;

    if (!manager_ready) {
        if (text && text_size)
            snprintf(text, text_size, "图片预览服务未就绪。");
        return IMAGE_PREVIEW_IDLE;
    }
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    state = current_state;
    error = last_error;
    rt_mutex_release(&state_lock);

    if (!text || !text_size)
        return state;
    if (state == IMAGE_PREVIEW_LOADING)
        status = "正在读取并解码图片……";
    else if (state == IMAGE_PREVIEW_READY)
        status = "图片已加载。";
    else if (state == IMAGE_PREVIEW_NOT_FOUND)
        status = "SD没有本机测试图片";
    else if (state == IMAGE_PREVIEW_FAILED)
        status = error_text(error);
    else
        status = "选择 SD 卡图片进行预览。";
    snprintf(text, text_size, "%s", status);
    return state;
}

const lv_image_dsc_t *image_preview_get_image(void)
{
    const lv_image_dsc_t *image = NULL;

    if (!manager_ready)
        return NULL;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    if (current_state == IMAGE_PREVIEW_READY)
        image = &preview_image;
    rt_mutex_release(&state_lock);
    return image;
}

void image_preview_cancel(void)
{
    int release_now = 0;

    if (!manager_ready)
        return;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    if (current_state == IMAGE_PREVIEW_LOADING) {
        cancel_requested = 1;
    } else if (current_state == IMAGE_PREVIEW_READY ||
               current_state == IMAGE_PREVIEW_FAILED ||
               current_state == IMAGE_PREVIEW_NOT_FOUND) {
        current_state = IMAGE_PREVIEW_RELEASING;
        release_now = 1;
    }
    rt_mutex_release(&state_lock);

    if (release_now) {
        release_decode_resources();
        rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
        current_state = IMAGE_PREVIEW_IDLE;
        cancel_requested = 0;
        last_error = IMAGE_PREVIEW_ERR_NONE;
        rt_mutex_release(&state_lock);
    }
}
