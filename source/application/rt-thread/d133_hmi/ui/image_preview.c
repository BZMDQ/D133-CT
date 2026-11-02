#include "image_preview.h"
#include "hmi_serial_log.h"
#include "file_manager.h"
#include <rtthread.h>
#include <aic_core.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define IMAGE_PREVIEW_FILE_MAX (4U * 1024U * 1024U)
#define IMAGE_PREVIEW_WIDTH_MAX 1024U
#define IMAGE_PREVIEW_HEIGHT_MAX 768U
#define IMAGE_PREVIEW_LVFS_PREFIX "L:"

typedef struct {
    char path[FM_PATH_MAX];
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
static char preview_source_path[FM_PATH_MAX + sizeof(IMAGE_PREVIEW_LVFS_PREFIX)];

enum {
    IMAGE_PREVIEW_ERR_NONE = 0,
    IMAGE_PREVIEW_ERR_PATH = -1,
    IMAGE_PREVIEW_ERR_SIZE = -2,
    IMAGE_PREVIEW_ERR_FORMAT = -3,
    IMAGE_PREVIEW_ERR_DECODE = -4,
    IMAGE_PREVIEW_ERR_CANCELLED = -5
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
    memset(&preview_image, 0, sizeof(preview_image));
}

static int dimensions_supported(const lv_image_header_t *header)
{
    return header && header->w > 0 && header->h > 0 &&
           header->w <= IMAGE_PREVIEW_WIDTH_MAX &&
           header->h <= IMAGE_PREVIEW_HEIGHT_MAX;
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
    rt_uint32_t decode_started;
    rt_uint32_t decode_ms;
    rt_uint32_t normalize_started;
    rt_uint32_t normalize_ms = 0;
    int result;

    if (!dimensions_supported(header))
        return IMAGE_PREVIEW_ERR_SIZE;

    args.no_cache = true;
    memset(&decoder_dsc, 0, sizeof(decoder_dsc));
    decode_started = rt_tick_get_millisecond();
    if (lv_image_decoder_open(&decoder_dsc, preview_source_path, &args) != LV_RESULT_OK) {
        hmi_serial_log("[image_preview] SDK MPP 打开/解码失败，耗时 %u ms：%s\n",
                   (unsigned)(rt_tick_get_millisecond() - decode_started),
                   preview_source_path);
        return IMAGE_PREVIEW_ERR_DECODE;
    }
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

static int load_image(const char *path)
{
    struct stat st;
    lv_image_header_t header = {0};
    image_kind_t kind = image_kind(path);
    size_t path_len;
    rt_uint32_t started = rt_tick_get_millisecond();
    rt_uint32_t info_ms;
    rt_uint32_t info_started;
    rt_uint32_t decode_started;
    int result;

    if (kind == IMAGE_KIND_UNSUPPORTED)
        return IMAGE_PREVIEW_ERR_FORMAT;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > IMAGE_PREVIEW_FILE_MAX) {
        hmi_serial_log("[image_preview] 文件不可读或大小无效：%s\n", path);
        return IMAGE_PREVIEW_ERR_SIZE;
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
    if (lv_image_decoder_get_info(preview_source_path, &header) != LV_RESULT_OK) {
        hmi_serial_log("[image_preview] SDK 解码器无法识别文件头：%s\n", path);
        return IMAGE_PREVIEW_ERR_DECODE;
    }
    info_ms = rt_tick_get_millisecond() - info_started;
    if (!dimensions_supported(&header)) {
        hmi_serial_log("[image_preview] 图片尺寸超限：%s (%u x %u)\n",
                   path, header.w, header.h);
        return IMAGE_PREVIEW_ERR_SIZE;
    }
    if (preview_cancelled())
        return IMAGE_PREVIEW_ERR_CANCELLED;

    /* MPP 的 BMP 路径也返回整张图像缓冲区，不提供逐行 get_area 回调。 */
    decode_started = rt_tick_get_millisecond();
    result = decode_mpp_image(&header);
    hmi_serial_log("[image_preview] 总耗时：%u ms，文件头=%u ms，解码/归一化=%u ms，结果=%d\n",
               (unsigned)(rt_tick_get_millisecond() - started), info_ms,
               (unsigned)(rt_tick_get_millisecond() - decode_started), result);
    return result;
}

static const char *error_text(int error)
{
    switch (error) {
    case IMAGE_PREVIEW_ERR_PATH: return "图片路径无效。";
    case IMAGE_PREVIEW_ERR_SIZE: return "图片文件超过 4 MiB，或分辨率超出 1024×768 解码限制。";
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
        result = load_image(job.path);
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
        current_state = result == IMAGE_PREVIEW_ERR_NONE ? IMAGE_PREVIEW_READY : IMAGE_PREVIEW_FAILED;
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
    } else if (current_state == IMAGE_PREVIEW_READY || current_state == IMAGE_PREVIEW_FAILED) {
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

