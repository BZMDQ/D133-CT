#include "screen_test.h"
#include "ui_manager.h"
#include "media_orientation.h"
#include "image_preview.h"
#include "lv_port_indev.h"
#include "lv_port_disp.h"
#include "lvgl.h"
#include <rtdevice.h>
#include <stdint.h>
#include <string.h>
#include <hal_adcim.h>

#define SCREEN_TEST_PAGE_COUNT       9U
#define SCREEN_TEST_AUTO_INTERVAL_MS 1000U
#define SCREEN_TEST_GRAY_LEVELS      32U
#define SCREEN_TEST_LINE_POINTS      8U
#define SCREEN_TEST_LINE_SEGMENTS    128U
#define SCREEN_TEST_IMAGE_DIR        "/sdcard/测试图像使用"
#define SCREEN_TEST_KEYADC_CHANNEL   2U
#define SCREEN_TEST_KEY_SAMPLE_MS    20U
#define SCREEN_TEST_KEY_DEBOUNCE_MS  30U
#define SCREEN_TEST_ADC_SCALE        10000
#define SCREEN_TEST_ADC_REFERENCE    3.0f
#define SCREEN_TEST_UP_MIN_VOLTAGE   2000
#define SCREEN_TEST_UP_MAX_VOLTAGE   4200
#define SCREEN_TEST_DOWN_MIN_VOLTAGE 4700
#define SCREEN_TEST_DOWN_MAX_VOLTAGE 8000

#define SCREEN_TEST_KEY_NONE         0U
#define SCREEN_TEST_KEY_UP           1U
#define SCREEN_TEST_KEY_DOWN         2U
#define SCREEN_TEST_KEY_LEFT         3U
#define SCREEN_TEST_KEY_RIGHT        4U

enum {
    SCREEN_TEST_WHITE = 0,
    SCREEN_TEST_COLOR_GRAY,
    SCREEN_TEST_BORDER,
    SCREEN_TEST_IMAGE,
    SCREEN_TEST_FULL_GRAY,
    SCREEN_TEST_RED,
    SCREEN_TEST_GREEN,
    SCREEN_TEST_BLUE,
    SCREEN_TEST_BLACK
};

static const uint32_t screen_test_bar_colors[8] = {
    0x000000U, 0x0000FFU, 0x00FFFFU, 0x00FF00U,
    0xFFFF00U, 0xFF0000U, 0xFF00FFU, 0xFFFFFFU
};

/* 按 GT911 触点 ID 固定分配高对比颜色，最多支持五指。 */
static const uint32_t screen_test_touch_colors[AIC_LV_TOUCH_MAX_POINTS] = {
    0x0000FFU, 0xFF0000U, 0x00A000U, 0xFF8000U, 0xA000D0U
};

static lv_obj_t *test_overlay;
static lv_obj_t *previous_screen;
static uint8_t top_was_hidden, sys_was_hidden;
typedef struct {
    lv_obj_t *object;
    lv_point_precise_t absolute[SCREEN_TEST_LINE_POINTS];
    lv_point_precise_t relative[SCREEN_TEST_LINE_POINTS];
    uint32_t count;
} screen_test_segment_t;
static screen_test_segment_t segments[SCREEN_TEST_LINE_SEGMENTS];
static screen_test_segment_t *active_segments[AIC_LV_TOUCH_MAX_POINTS];
static uint32_t next_segment;
static lv_timer_t *image_poll_timer;
void screen_test_image_ready(void)
{
    if (image_poll_timer)
        lv_timer_ready(image_poll_timer);
}
static lv_timer_t *key_dispatch_timer;
static lv_timer_t *touch_timer;
static lv_timer_t *auto_page_timer;
static unsigned auto_mode;
/* 本次开机期间保存轮播间隔，退出画面测试后仍然有效。 */
static unsigned auto_interval_ms = SCREEN_TEST_AUTO_INTERVAL_MS;
static int32_t screen_width;
static int32_t screen_height;
static uint8_t current_test_page;
static uint8_t touch_was_pressed[AIC_LV_TOUCH_MAX_POINTS];
static uint8_t image_request_pending;
static uint8_t exit_started;
static volatile uint8_t key_worker_starting;
static volatile uint8_t key_worker_ready;
static volatile uint8_t key_worker_enabled;
static volatile uint8_t pending_key;
static screen_test_exit_cb_t exit_callback;
static void *exit_context;
static void (*enter_callback)(void);

static void screen_test_exit(void);
static uint8_t screen_test_gray_value(uint32_t index);

/* 将触摸坐标裁剪到当前活动显示器的有效像素范围。 */
/* 新建无边框矩形，测试色值由 LVGL 按当前面板像素格式输出。 */
static void screen_test_add_rect(int32_t x, int32_t y,
                                 int32_t width, int32_t height,
                                 uint32_t rgb)
{
    lv_obj_t *rect;

    if (width <= 0 || height <= 0)
        return;
    rect = lv_obj_create(test_overlay);
    if (!rect)
        return;
    lv_obj_set_pos(rect, (lv_coord_t)x, (lv_coord_t)y);
    lv_obj_set_size(rect, (lv_coord_t)width, (lv_coord_t)height);
    lv_obj_set_style_bg_color(rect, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(rect, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rect, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* 八色条和 32 级灰阶在一个全屏对象内绘制，降低分区刷新造成的闪动。 */
static void screen_test_draw_pattern(lv_event_t *event)
{
    lv_layer_t *layer;
    lv_draw_rect_dsc_t descriptor;
    lv_area_t area;
    uint32_t count, index;
    int32_t y_end;

    if (current_test_page != SCREEN_TEST_COLOR_GRAY && current_test_page != SCREEN_TEST_FULL_GRAY)
        return;
    if (lv_event_get_code(event) == LV_EVENT_COVER_CHECK) {
        /* 两种灰阶测试都逐像素覆盖整个画布，省掉额外的黑底和下层绘制。 */
        lv_event_set_cover_res(event, LV_COVER_RES_COVER);
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN)
        return;

    layer = lv_event_get_layer(event);
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.bg_opa = LV_OPA_COVER;
    descriptor.border_width = 0;
    descriptor.radius = 0;
    count = current_test_page == SCREEN_TEST_COLOR_GRAY ? 8U : SCREEN_TEST_GRAY_LEVELS;
    y_end = current_test_page == SCREEN_TEST_COLOR_GRAY ? screen_height / 2 : screen_height;
    for (index = 0; index < count; ++index) {
        uint8_t value;
        uint32_t color;
        area.x1 = (int32_t)(((int64_t)index * screen_width) / count);
        area.x2 = (int32_t)(((int64_t)(index + 1U) * screen_width) / count) - 1;
        area.y1 = 0;
        area.y2 = y_end - 1;
        if (current_test_page == SCREEN_TEST_COLOR_GRAY) {
            descriptor.bg_color = lv_color_hex(screen_test_bar_colors[index]);
        } else {
            value = screen_test_gray_value(index);
            color = ((uint32_t)value << 16) | ((uint32_t)value << 8) | value;
            descriptor.bg_color = lv_color_hex(color);
        }
        lv_draw_rect(layer, &descriptor, &area);
    }
    if (current_test_page == SCREEN_TEST_COLOR_GRAY) {
        for (index = 0; index < SCREEN_TEST_GRAY_LEVELS; ++index) {
            uint8_t value = screen_test_gray_value(index);
            uint32_t color = ((uint32_t)value << 16) |
                             ((uint32_t)value << 8) | value;
            area.x1 = (int32_t)(((int64_t)index * screen_width) /
                                SCREEN_TEST_GRAY_LEVELS);
            area.x2 = (int32_t)(((int64_t)(index + 1U) * screen_width) /
                                SCREEN_TEST_GRAY_LEVELS) - 1;
            area.y1 = screen_height / 2;
            area.y2 = screen_height - 1;
            descriptor.bg_color = lv_color_hex(color);
            lv_draw_rect(layer, &descriptor, &area);
        }
    }
}

/* 背景色填满整个物理分辨率，页面切换时删除前页所有图形对象。 */
static void screen_test_begin_page(uint32_t rgb)
{
    lv_obj_clean(test_overlay);
    lv_obj_set_style_bg_color(test_overlay, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(test_overlay,
                           current_test_page == SCREEN_TEST_COLOR_GRAY || current_test_page == SCREEN_TEST_FULL_GRAY ?
                           LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    memset(segments, 0, sizeof(segments));
    memset(active_segments, 0, sizeof(active_segments));
    memset(touch_was_pressed, 0, sizeof(touch_was_pressed));
    next_segment = 0;
}

/* 灰阶像素从左侧白色按 32 级递减到右侧黑色。 */
static uint8_t screen_test_gray_value(uint32_t index)
{
    return (uint8_t)((255U * (SCREEN_TEST_GRAY_LEVELS - 1U - index) + 15U) /
                     (SCREEN_TEST_GRAY_LEVELS - 1U));
}

/* 绘制白屏触摸测试专用的两像素蓝线。 */
static void screen_test_add_touch_point(uint8_t finger_id, int32_t x, int32_t y)
{
    int32_t min_x, min_y, max_x, max_y;
    uint32_t i;
    lv_point_precise_t previous = {0};
    screen_test_segment_t *active_segment;
    int carry;

    if (finger_id >= AIC_LV_TOUCH_MAX_POINTS)
        return;
    active_segment = active_segments[finger_id];
    /* 多次读到同一坐标时不追加点，也不触发重绘。 */
    if (active_segment && active_segment->count) {
        lv_point_precise_t last = active_segment->absolute[active_segment->count - 1U];
        if (last.x == x && last.y == y)
            return;
    }
    carry = active_segment && active_segment->count == SCREEN_TEST_LINE_POINTS;

    if (carry)
        previous = active_segment->absolute[active_segment->count - 1U];
    if (!active_segment || carry) {
        /* 线段池用满时保留全部旧笔迹，切页后再复用。 */
        if (next_segment >= SCREEN_TEST_LINE_SEGMENTS)
            return;
        active_segment = &segments[next_segment++];
        for (i = 0; i < AIC_LV_TOUCH_MAX_POINTS; i++) {
            if (active_segments[i] == active_segment)
                active_segments[i] = NULL;
        }
        if (active_segment->object)
            lv_obj_delete(active_segment->object);
        memset(active_segment, 0, sizeof(*active_segment));
        active_segment->object = lv_line_create(test_overlay);
        if (!active_segment->object) {
            active_segment = NULL;
            active_segments[finger_id] = NULL;
            return;
        }
        lv_obj_set_style_line_width(active_segment->object, 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(active_segment->object,
                                    lv_color_hex(screen_test_touch_colors[finger_id]),
                                    LV_PART_MAIN);
        lv_obj_set_style_line_rounded(active_segment->object, true, LV_PART_MAIN);
        lv_obj_clear_flag(active_segment->object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        active_segments[finger_id] = active_segment;
        if (carry)
            active_segment->absolute[active_segment->count++] = previous;
    }
    active_segment->absolute[active_segment->count].x = x;
    active_segment->absolute[active_segment->count++].y = y;
    min_x = max_x = active_segment->absolute[0].x;
    min_y = max_y = active_segment->absolute[0].y;
    for (i = 1; i < active_segment->count; i++) {
        int32_t px = active_segment->absolute[i].x;
        int32_t py = active_segment->absolute[i].y;
        if (px < min_x) min_x = px;
        if (px > max_x) max_x = px;
        if (py < min_y) min_y = py;
        if (py > max_y) max_y = py;
    }
    /* 每段只重绘自己的包围区域，旧笔迹保持独立。 */
    for (i = 0; i < active_segment->count; i++) {
        active_segment->relative[i].x = active_segment->absolute[i].x - min_x + 2;
        active_segment->relative[i].y = active_segment->absolute[i].y - min_y + 2;
    }
    lv_obj_set_pos(active_segment->object, min_x - 2, min_y - 2);
    lv_obj_set_size(active_segment->object, max_x - min_x + 5, max_y - min_y + 5);
    if (active_segment->count == 1U) {
        active_segment->relative[1] = active_segment->relative[0];
        lv_line_set_points(active_segment->object, active_segment->relative, 2);
    } else {
        lv_line_set_points(active_segment->object, active_segment->relative, active_segment->count);
    }
}

/* 在 LVGL 线程读取触摸适配层提供的五点快照，各指针保留自己的轨迹。 */
static void screen_test_multitouch_timer(lv_timer_t *timer)
{
    aic_lv_touch_point_t points[AIC_LV_TOUCH_MAX_POINTS];
    uint8_t i;

    LV_UNUSED(timer);
    /* 换页时已清理轨迹，静态测试画面无需读取触摸快照。 */
    if (current_test_page != SCREEN_TEST_WHITE)
        return;
    aic_touch_snapshot(points, AIC_LV_TOUCH_MAX_POINTS);
    for (i = 0; i < AIC_LV_TOUCH_MAX_POINTS; i++) {
        if (!points[i].pressed) {
            active_segments[i] = NULL;
            touch_was_pressed[i] = 0;
            continue;
        }
        if (!touch_was_pressed[i])
            active_segments[i] = NULL;
        touch_was_pressed[i] = 1;
        /* 白屏全区域支持划线。 */
        screen_test_add_touch_point(i, points[i].x, points[i].y);
    }
}

/* 上半屏输出八色条，下半屏只创建一个水平灰阶渐变控件。 */
static void screen_test_draw_color_gray(void)
{
    screen_test_begin_page(0x000000U);
}

/* 全屏灰阶从左侧白色逐步过渡到右侧黑色。 */
static void screen_test_draw_full_gray(void)
{
    screen_test_begin_page(0x000000U);
}

/* 先显示内置校准图，匹配的 SD 图片解码完成后替换。全程无文字或提示框。 */
static void screen_test_draw_image_fallback(void)
{
    int32_t half = screen_height / 2;
    unsigned i;

    for (i = 0; i < 8U; ++i) {
        int32_t x = (int32_t)i * screen_width / 8;
        int32_t right = (int32_t)(i + 1U) * screen_width / 8;
        screen_test_add_rect(x, 0, right - x, half, screen_test_bar_colors[i]);
    }
    for (i = 0; i < 8U; ++i) {
        int32_t column = (int32_t)(i % 4U);
        int32_t row = (int32_t)(i / 4U);
        int32_t x = column * screen_width / 4;
        int32_t right = (column + 1) * screen_width / 4;
        int32_t y = half + row * (screen_height - half) / 2;
        int32_t bottom = half + (row + 1) * (screen_height - half) / 2;
        screen_test_add_rect(x, y, right - x, bottom - y,
                             (column + row) % 2 ? 0xD0D0D0U : 0x202020U);
    }
}

/* 图片测试接受原方向或旋转后与活动屏幕等尺寸的 SD 卡图片。 */
static void screen_test_poll_image(lv_timer_t *timer)
{
    image_preview_state_t state = image_preview_status(NULL, 0);

    if (image_request_pending && state == IMAGE_PREVIEW_IDLE) {
        image_request_pending = 0;
        if (image_preview_request_matching(SCREEN_TEST_IMAGE_DIR,
                                           (uint16_t)screen_width,
                                           (uint16_t)screen_height) == 0)
            return;
    }
    if (state == IMAGE_PREVIEW_LOADING)
        return;

    image_poll_timer = NULL;
    lv_timer_delete(timer);
    if (state == IMAGE_PREVIEW_READY) {
        const lv_image_dsc_t *source = image_preview_get_image();
        if (source) {
            lv_obj_t *image;

            screen_test_begin_page(0x000000U);
            image = lv_image_create(test_overlay);
            if (image) {
                lv_image_set_src(image, source);
                lv_image_set_scale(image, image_preview_get_scale());
                hmi_media_place(image, source->header.w, source->header.h,
                                screen_width, screen_height, 0);
                lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
                lv_port_disp_sync_next_refresh();
            } else {
                screen_test_draw_image_fallback();
            }
        }
    }
}

/* 文件系统读取及旋转继续由原有预读工作线程处理。 */
static void screen_test_start_image_search(void)
{
    image_preview_state_t state;

    if (image_preview_init() != 0)
        return;
    state = image_preview_status(NULL, 0);
    if (state != IMAGE_PREVIEW_READY && state != IMAGE_PREVIEW_LOADING) {
        image_preview_cancel();
        if (image_preview_request_matching(SCREEN_TEST_IMAGE_DIR,
                                           (uint16_t)screen_width,
                                           (uint16_t)screen_height) != 0)
            image_request_pending = 1;
    }
    image_poll_timer = lv_timer_create(screen_test_poll_image, 20, NULL);
    if (image_poll_timer && image_preview_status(NULL, 0) == IMAGE_PREVIEW_READY)
        screen_test_poll_image(image_poll_timer);
}

/* 测试期间保留图片解码结果，退出测试时统一释放。 */
static void screen_test_show_page(uint8_t page)
{
    if (current_test_page == SCREEN_TEST_IMAGE) {
        if (image_poll_timer) {
            lv_timer_delete(image_poll_timer);
            image_poll_timer = NULL;
        }
        image_request_pending = 0;
        /* 先移除图片控件和缓存引用，再归还 PSRAM 像素缓冲。 */
        lv_obj_clean(test_overlay);
        lv_image_cache_drop(NULL);
    }
    current_test_page = page;
    lv_port_disp_sync_next_refresh();
    if (auto_page_timer)
        lv_timer_reset(auto_page_timer);

    if (page == SCREEN_TEST_WHITE) {
        screen_test_begin_page(0xFFFFFFU);
    } else if (page == SCREEN_TEST_COLOR_GRAY) {
        screen_test_draw_color_gray();
    } else if (page == SCREEN_TEST_BORDER) {
        screen_test_begin_page(0x000000U);
        screen_test_add_rect(0, 0, screen_width, 1, 0xFFFFFFU);
        screen_test_add_rect(0, screen_height - 1, screen_width, 1, 0xFFFFFFU);
        screen_test_add_rect(0, 0, 1, screen_height, 0xFFFFFFU);
        screen_test_add_rect(screen_width - 1, 0, 1, screen_height, 0xFFFFFFU);
    } else if (page == SCREEN_TEST_IMAGE) {
        screen_test_begin_page(0x000000U);
        screen_test_draw_image_fallback();
        screen_test_start_image_search();
    } else if (page == SCREEN_TEST_FULL_GRAY) {
        screen_test_draw_full_gray();
    } else if (page == SCREEN_TEST_RED) {
        screen_test_begin_page(0xFF0000U);
    } else if (page == SCREEN_TEST_GREEN) {
        screen_test_begin_page(0x00FF00U);
    } else if (page == SCREEN_TEST_BLUE) {
        screen_test_begin_page(0x0000FFU);
    } else {
        screen_test_begin_page(0x000000U);
    }
    /* 测试画面无触摸返回按钮，使用 RIGHT 返回。 */
}

/* 实体按键、USB 和自动轮播共用当前页码与首尾循环。 */
static void screen_test_step(int direction)
{
    int page = (int)current_test_page + (int)SCREEN_TEST_PAGE_COUNT + direction;
    screen_test_show_page((uint8_t)(page % (int)SCREEN_TEST_PAGE_COUNT));
}

static void screen_test_autoplay_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    screen_test_step(1);
}

static void screen_test_stop_autoplay(void)
{
    if (auto_page_timer) {
        lv_timer_delete(auto_page_timer);
        auto_page_timer = NULL;
    }
    __atomic_store_n(&auto_mode, 0U, __ATOMIC_RELAXED);
}

static void screen_test_toggle_autoplay(void)
{
    if (auto_page_timer) {
        screen_test_stop_autoplay();
        return;
    }
    auto_page_timer = lv_timer_create(screen_test_autoplay_tick, screen_test_interval_ms(), NULL);
    __atomic_store_n(&auto_mode, auto_page_timer != NULL, __ATOMIC_RELAXED);
}

int screen_test_auto_enabled(void)
{
    return (int)__atomic_load_n(&auto_mode, __ATOMIC_RELAXED);
}

unsigned screen_test_interval_ms(void)
{
    return __atomic_load_n(&auto_interval_ms, __ATOMIC_RELAXED);
}

void screen_test_set_interval_ms(unsigned interval_ms)
{
    if (screen_test_interval_ms() == interval_ms)
        return;
    if (auto_page_timer) {
        lv_timer_set_period(auto_page_timer, interval_ms);
        lv_timer_reset(auto_page_timer);
    }
    __atomic_store_n(&auto_interval_ms, interval_ms, __ATOMIC_RELAXED);
}

/* 通过 SDK 的 RT-Thread ADC 设备接口读取板载 GPAI2 电阻按键。 */
static uint8_t screen_test_read_key(rt_adc_device_t adc, uint32_t calibration)
{
    rt_uint32_t raw = rt_adc_read(adc, SCREEN_TEST_KEYADC_CHANNEL);
    uint16_t sample;
    int voltage;

    if (raw > UINT16_MAX)
        return SCREEN_TEST_KEY_NONE;
    sample = (uint16_t)raw;
    voltage = hal_adcim_adc2voltage(&sample, calibration,
                                    SCREEN_TEST_ADC_SCALE,
                                    SCREEN_TEST_ADC_REFERENCE);
    if (voltage >= SCREEN_TEST_UP_MIN_VOLTAGE &&
        voltage <= SCREEN_TEST_UP_MAX_VOLTAGE)
        return SCREEN_TEST_KEY_UP;
    if (voltage >= SCREEN_TEST_DOWN_MIN_VOLTAGE &&
        voltage <= SCREEN_TEST_DOWN_MAX_VOLTAGE)
        return SCREEN_TEST_KEY_DOWN;
    /* 原理图标称 LEFT 1.0 V、RIGHT 1.3 V，电压单位为 0.1 mV。 */
    if (voltage >= 8500 && voltage <= 11400)
        return SCREEN_TEST_KEY_LEFT;
    if (voltage >= 11600 && voltage <= 14500)
        return SCREEN_TEST_KEY_RIGHT;
    return SCREEN_TEST_KEY_NONE;
}

/* 按键线程只采样、消抖并投递事件，不调用任何 LVGL 控件接口。 */
static void screen_test_key_worker(void *parameter)
{
    rt_adc_device_t adc;
    uint32_t calibration;
    uint8_t candidate = SCREEN_TEST_KEY_NONE;
    uint8_t stable = SCREEN_TEST_KEY_NONE;
    uint32_t candidate_since = rt_tick_get_millisecond();

    LV_UNUSED(parameter);
    adc = (rt_adc_device_t)rt_device_find("gpai");
    if (!adc || hal_adcim_probe() < 0) {
        key_worker_starting = 0;
        return;
    }
    calibration = hal_adcim_auto_calibration();
    if (rt_adc_enable(adc, SCREEN_TEST_KEYADC_CHANNEL) != RT_EOK) {
        key_worker_starting = 0;
        return;
    }
    key_worker_ready = 1;
    key_worker_starting = 0;

    while (1) {
        uint8_t sampled;
        uint32_t now;

        if (!key_worker_enabled) {
            rt_thread_mdelay(100);
            continue;
        }
        sampled = screen_test_read_key(adc, calibration);
        now = rt_tick_get_millisecond();
        if (sampled != candidate) {
            candidate = sampled;
            candidate_since = now;
        } else if (candidate != stable &&
                   (uint32_t)(now - candidate_since) >= SCREEN_TEST_KEY_DEBOUNCE_MS) {
            stable = candidate;
            if (stable != SCREEN_TEST_KEY_NONE)
                pending_key = stable;
        }
        rt_thread_mdelay(SCREEN_TEST_KEY_SAMPLE_MS);
    }
}

/* 延时器运行在 LVGL 线程中，将按键线程提交的页码事件安全应用到界面。 */
static void screen_test_dispatch_key(lv_timer_t *timer)
{
    uint8_t key = pending_key;

    LV_UNUSED(timer);
    if (!key)
        return;
    pending_key = SCREEN_TEST_KEY_NONE;
    if (key == SCREEN_TEST_KEY_LEFT && !test_overlay) {
        if (enter_callback) enter_callback();
        return;
    }
    if (!test_overlay) return;
    if (key == SCREEN_TEST_KEY_RIGHT) {
        screen_test_exit();
        return;
    }
    if (key == SCREEN_TEST_KEY_UP)
        screen_test_step(-1);
    else if (key == SCREEN_TEST_KEY_DOWN)
        screen_test_step(1);
}

/* 单一线程持续采样，以便从首页响应 LEFT。 */
static void screen_test_start_key_worker(void)
{
    rt_thread_t thread;

    if (key_worker_ready || key_worker_starting)
        return;
    key_worker_starting = 1;
    thread = rt_thread_create("scrkey", screen_test_key_worker, NULL,
                              2048, 24, 10);
    if (!thread) {
        key_worker_starting = 0;
        return;
    }
    if (rt_thread_startup(thread) != RT_EOK) {
        key_worker_starting = 0;
    }
}

void screen_test_keys_start(void (*on_enter)(void))
{
    enter_callback = on_enter;
    key_worker_enabled = 1;
    screen_test_start_key_worker();
    if (!key_dispatch_timer)
        key_dispatch_timer = lv_timer_create(screen_test_dispatch_key, 10, NULL);
}

/* RIGHT 退出测试，并释放覆盖层和图片缓存。 */
static void screen_test_exit(void)
{
    screen_test_exit_cb_t callback;
    void *context;

    if (exit_started)
        return;
    exit_started = 1;
    lv_port_disp_sync_next_refresh();
    screen_test_stop_autoplay();
    pending_key = SCREEN_TEST_KEY_NONE;
    if (touch_timer) {
        lv_timer_delete(touch_timer);
        touch_timer = NULL;
    }
    if (image_poll_timer) {
        lv_timer_delete(image_poll_timer);
        image_poll_timer = NULL;
    }
    image_request_pending = 0;
    if (test_overlay) {
        lv_display_t *display = lv_obj_get_display(test_overlay);

        lv_screen_load(previous_screen);
        lv_obj_delete(test_overlay);
        test_overlay = NULL;
        previous_screen = NULL;
        if (!top_was_hidden)
            lv_obj_clear_flag(lv_display_get_layer_top(display), LV_OBJ_FLAG_HIDDEN);
        if (!sys_was_hidden)
            lv_obj_clear_flag(lv_display_get_layer_sys(display), LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_cache_drop(NULL);
    image_preview_cancel();
    callback = exit_callback;
    context = exit_context;
    exit_callback = NULL;
    exit_context = NULL;
    if (callback)
        callback(context);
}

int screen_test_start(lv_obj_t *parent,
                      screen_test_exit_cb_t on_exit, void *context)
{
    lv_display_t *display = lv_display_get_default();

    if (!parent || !display || lv_display_get_screen_active(display) != parent || test_overlay)
        return -1;
    screen_width = lv_display_get_horizontal_resolution(display);
    screen_height = lv_display_get_vertical_resolution(display);
    if (screen_width <= 0 || screen_height <= 0 ||
        screen_width > UINT16_MAX || screen_height > UINT16_MAX)
        return -2;

    exit_callback = on_exit;
    exit_context = context;
    exit_started = 0;
    image_poll_timer = NULL;
    touch_timer = NULL;
    image_request_pending = 0;
    pending_key = SCREEN_TEST_KEY_NONE;
    key_worker_enabled = 1;
    current_test_page = SCREEN_TEST_BLACK;
    test_overlay = lv_obj_create(NULL);
    if (!test_overlay) {
        exit_callback = NULL;
        exit_context = NULL;
        return -3;
    }
    /* 单独的 LVGL 屏幕占满物理分辨率，并暂停全局弹窗层与系统层。 */
    previous_screen = parent;
    lv_obj_remove_style_all(test_overlay);
    lv_obj_set_pos(test_overlay, 0, 0);
    lv_obj_set_size(test_overlay, (lv_coord_t)screen_width,
                    (lv_coord_t)screen_height);
    lv_obj_set_scrollbar_mode(test_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(test_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(test_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(test_overlay, screen_test_draw_pattern, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(test_overlay, screen_test_draw_pattern, LV_EVENT_COVER_CHECK, NULL);
    top_was_hidden = lv_obj_has_flag(lv_display_get_layer_top(display), LV_OBJ_FLAG_HIDDEN);
    sys_was_hidden = lv_obj_has_flag(lv_display_get_layer_sys(display), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_display_get_layer_top(display), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_display_get_layer_sys(display), LV_OBJ_FLAG_HIDDEN);
    screen_test_show_page(SCREEN_TEST_WHITE);
    lv_screen_load(test_overlay);
    screen_test_start_key_worker();
    touch_timer = lv_timer_create(screen_test_multitouch_timer, 8, NULL);
    /* 进入测试即后台预读，在用户切到图片页之前完成读卡和旋转。 */
    if (image_preview_init() == 0) {
        image_preview_cancel();
        image_preview_request_matching(SCREEN_TEST_IMAGE_DIR,
                                       (uint16_t)screen_width,
                                       (uint16_t)screen_height);
    }
    return 0;
}

void screen_test_remote_select(unsigned target)
{
    if (!test_overlay) return;
    if (target == HMI_TEST_EXIT)
        screen_test_exit();
    else if (target == HMI_TEST_PREVIOUS)
        screen_test_step(-1);
    else if (target == HMI_TEST_NEXT)
        screen_test_step(1);
    else if (target == HMI_TEST_TOGGLE_MODE)
        screen_test_toggle_autoplay();
    else if (target < SCREEN_TEST_PAGE_COUNT)
        screen_test_show_page((uint8_t)target);
}

int screen_test_is_active(void)
{
    return test_overlay != NULL;
}
