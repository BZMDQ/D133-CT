#include "screen_test.h"
#include "image_preview.h"
#include <rtdevice.h>
#include <stdint.h>
#include <string.h>
#include <hal_adcim.h>

#define SCREEN_TEST_PAGE_COUNT       9U
#define SCREEN_TEST_GRAY_LEVELS      32U
#define SCREEN_TEST_LINE_POINTS      16U
#define SCREEN_TEST_LINE_SEGMENTS    64U
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

static lv_obj_t *test_overlay;
typedef struct {
    lv_obj_t *object;
    lv_point_precise_t absolute[SCREEN_TEST_LINE_POINTS];
    lv_point_precise_t relative[SCREEN_TEST_LINE_POINTS];
    uint32_t count;
} screen_test_segment_t;
static screen_test_segment_t segments[SCREEN_TEST_LINE_SEGMENTS];
static screen_test_segment_t *active_segment;
static uint32_t next_segment;
static lv_obj_t *test_image_status;
static lv_obj_t *test_back_button;
static lv_timer_t *image_poll_timer;
static lv_timer_t *key_dispatch_timer;
static int32_t screen_width;
static int32_t screen_height;
static int32_t last_x;
static int32_t last_y;
static uint8_t current_test_page;
static uint8_t touch_down;
static uint8_t image_request_pending;
static uint8_t exit_started;
static volatile uint8_t key_worker_starting;
static volatile uint8_t key_worker_ready;
static volatile uint8_t key_worker_enabled;
static volatile uint8_t pending_key;
static const lv_font_t *test_font;
static screen_test_exit_cb_t exit_callback;
static void *exit_context;

static void screen_test_exit(void);

/* 将触摸坐标裁剪到当前活动显示器的有效像素范围。 */
static void screen_test_get_point(lv_indev_t *indev, int32_t *x, int32_t *y)
{
    lv_point_t point = {0};

    if (indev)
        lv_indev_get_point(indev, &point);
    if (point.x < 0)
        point.x = 0;
    if (point.y < 0)
        point.y = 0;
    if (point.x >= screen_width)
        point.x = screen_width - 1;
    if (point.y >= screen_height)
        point.y = screen_height - 1;
    *x = point.x;
    *y = point.y;
}

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

/* 背景色填满整个物理分辨率，页面切换时删除前页所有图形对象。 */
static void screen_test_begin_page(uint32_t rgb)
{
    lv_obj_clean(test_overlay);
    lv_obj_set_style_bg_color(test_overlay, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(test_overlay, LV_OPA_COVER, LV_PART_MAIN);
    memset(segments, 0, sizeof(segments));
    active_segment = NULL;
    next_segment = 0;
    test_image_status = NULL;
    test_back_button = NULL;
    touch_down = 0;
}

/* 绘制白屏触摸测试专用的两像素蓝线。 */
static void screen_test_add_touch_point(int32_t x, int32_t y)
{
    int32_t min_x, min_y, max_x, max_y;
    uint32_t i;
    lv_point_precise_t previous = {0};
    int carry = active_segment && active_segment->count == SCREEN_TEST_LINE_POINTS;

    if (carry)
        previous = active_segment->absolute[active_segment->count - 1U];
    if (!active_segment || carry) {
        active_segment = &segments[next_segment++ % SCREEN_TEST_LINE_SEGMENTS];
        if (active_segment->object)
            lv_obj_delete(active_segment->object);
        memset(active_segment, 0, sizeof(*active_segment));
        active_segment->object = lv_line_create(test_overlay);
        if (!active_segment->object) {
            active_segment = NULL;
            return;
        }
        lv_obj_set_style_line_width(active_segment->object, 2, LV_PART_MAIN);
        lv_obj_set_style_line_color(active_segment->object, lv_color_hex(0x0000FFU), LV_PART_MAIN);
        lv_obj_set_style_line_rounded(active_segment->object, true, LV_PART_MAIN);
        lv_obj_clear_flag(active_segment->object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        if (carry)
            active_segment->absolute[active_segment->count++] = previous;
        if (test_back_button)
            lv_obj_move_foreground(test_back_button);
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

/* 白到黑的灰阶按照 round(255 * (31 - i) / 31) 计算。 */
static uint8_t screen_test_gray_value(uint32_t index)
{
    return (uint8_t)((255U * (SCREEN_TEST_GRAY_LEVELS - 1U - index) + 15U) /
                     (SCREEN_TEST_GRAY_LEVELS - 1U));
}

/* 上半屏输出八色条，下半屏输出 32 级灰阶。 */
static void screen_test_draw_color_gray(void)
{
    int32_t split_y = screen_height / 2;
    uint32_t i;

    screen_test_begin_page(0x000000U);
    for (i = 0; i < 8U; i++) {
        int32_t x0 = (int32_t)(((int64_t)i * screen_width) / 8);
        int32_t x1 = (int32_t)(((int64_t)(i + 1U) * screen_width) / 8);
        screen_test_add_rect(x0, 0, x1 - x0, split_y, screen_test_bar_colors[i]);
    }
    for (i = 0; i < SCREEN_TEST_GRAY_LEVELS; i++) {
        int32_t x0 = (int32_t)(((int64_t)i * screen_width) /
                               SCREEN_TEST_GRAY_LEVELS);
        int32_t x1 = (int32_t)(((int64_t)(i + 1U) * screen_width) /
                               SCREEN_TEST_GRAY_LEVELS);
        uint8_t value = screen_test_gray_value(i);
        uint32_t gray = ((uint32_t)value << 16) |
                        ((uint32_t)value << 8) | value;
        screen_test_add_rect(x0, split_y, x1 - x0,
                             screen_height - split_y, gray);
    }
}

/* 32 级全屏灰阶带从左侧白色逐步过渡到右侧黑色。 */
static void screen_test_draw_full_gray(void)
{
    uint32_t i;

    screen_test_begin_page(0x000000U);
    for (i = 0; i < SCREEN_TEST_GRAY_LEVELS; i++) {
        int32_t x0 = (int32_t)(((int64_t)i * screen_width) /
                               SCREEN_TEST_GRAY_LEVELS);
        int32_t x1 = (int32_t)(((int64_t)(i + 1U) * screen_width) /
                               SCREEN_TEST_GRAY_LEVELS);
        uint8_t value = screen_test_gray_value(i);
        uint32_t gray = ((uint32_t)value << 16) |
                        ((uint32_t)value << 8) | value;
        screen_test_add_rect(x0, 0, x1 - x0, screen_height, gray);
    }
}

/* 图片测试只显示与活动屏幕分辨率完全匹配的 SD 卡图片。 */
static void screen_test_poll_image(lv_timer_t *timer)
{
    char status[96];
    image_preview_state_t state;

    state = image_preview_status(status, sizeof(status));
    if (image_request_pending && state == IMAGE_PREVIEW_IDLE) {
        if (image_preview_request_matching(SCREEN_TEST_IMAGE_DIR,
                                          (uint16_t)screen_width,
                                          (uint16_t)screen_height) == 0) {
            image_request_pending = 0;
            return;
        }
        lv_label_set_text(test_image_status, "SD 图片读取服务不可用");
        image_poll_timer = NULL;
        lv_timer_delete(timer);
        return;
    }

    if (state == IMAGE_PREVIEW_LOADING)
        return;

    image_poll_timer = NULL;
    lv_timer_delete(timer);
    if (state == IMAGE_PREVIEW_READY) {
        const lv_image_dsc_t *source = image_preview_get_image();
        lv_obj_t *image;

        if (source && source->header.w == screen_width &&
            source->header.h == screen_height) {
            image = lv_image_create(test_overlay);
            if (image) {
                lv_image_set_src(image, source);
                lv_obj_center(image);
                lv_obj_clear_flag(image,
                                  LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
                if (test_back_button)
                    lv_obj_move_foreground(test_back_button);
                return;
            }
        }
        lv_label_set_text(test_image_status, "图片显示失败");
        return;
    }
    lv_label_set_text(test_image_status, status);
}

/* 进入图片页后提交异步目录扫描，SD 操作不占用 LVGL 线程。 */
static void screen_test_start_image_search(void)
{
    if (image_preview_init() != 0) {
        lv_label_set_text(test_image_status, "SD 图片读取服务不可用");
        return;
    }

    image_preview_cancel();
    if (image_preview_request_matching(SCREEN_TEST_IMAGE_DIR,
                                       (uint16_t)screen_width,
                                       (uint16_t)screen_height) != 0)
        image_request_pending = 1;
    image_poll_timer = lv_timer_create(screen_test_poll_image, 80, NULL);
    if (!image_poll_timer)
        lv_label_set_text(test_image_status, "SD 图片读取服务不可用");
}

/* 左上角返回键是全屏测试画面唯一的常驻控件。 */
static void screen_test_back_clicked(lv_event_t *event)
{
    LV_UNUSED(event);
    screen_test_exit();
}

static void screen_test_add_back_button(void)
{
    lv_obj_t *text;

    test_back_button = lv_button_create(test_overlay);
    if (!test_back_button)
        return;
    lv_obj_set_pos(test_back_button, 8, 8);
    lv_obj_set_size(test_back_button, 76, 44);
    lv_obj_set_style_bg_color(test_back_button, lv_color_hex(0x2778D4U),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(test_back_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(test_back_button, lv_color_hex(0xFFFFFFU),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(test_back_button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(test_back_button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(test_back_button, 0, LV_PART_MAIN);
    text = lv_label_create(test_back_button);
    if (text) {
        lv_label_set_text(text, "返回");
        lv_obj_set_style_text_color(text, lv_color_hex(0xFFFFFFU), LV_PART_MAIN);
        if (test_font)
            lv_obj_set_style_text_font(text, test_font, LV_PART_MAIN);
        lv_obj_center(text);
    }
    lv_obj_add_event_cb(test_back_button, screen_test_back_clicked,
                        LV_EVENT_CLICKED, NULL);
}

/* 根据测试顺序重画页面，并在离开图片页时释放解码缓存。 */
static void screen_test_show_page(uint8_t page)
{
    if (current_test_page == SCREEN_TEST_IMAGE) {
        if (image_poll_timer) {
            lv_timer_delete(image_poll_timer);
            image_poll_timer = NULL;
        }
        image_request_pending = 0;
        image_preview_cancel();
    }
    current_test_page = page;

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
        test_image_status = lv_label_create(test_overlay);
        if (!test_image_status) {
            screen_test_add_back_button();
            return;
        }
        lv_label_set_text(test_image_status, "正在查找 SD 本机测试图片……");
        lv_obj_set_style_text_color(test_image_status, lv_color_hex(0xFFFFFFU),
                                    LV_PART_MAIN);
        if (test_font)
            lv_obj_set_style_text_font(test_image_status, test_font, LV_PART_MAIN);
        lv_obj_center(test_image_status);
        lv_obj_clear_flag(test_image_status,
                          LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
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
    screen_test_add_back_button();
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
    if (key == SCREEN_TEST_KEY_UP) {
        screen_test_show_page((uint8_t)((current_test_page + SCREEN_TEST_PAGE_COUNT - 1U) %
                                        SCREEN_TEST_PAGE_COUNT));
    } else if (key == SCREEN_TEST_KEY_DOWN) {
        screen_test_show_page((uint8_t)((current_test_page + 1U) %
                                        SCREEN_TEST_PAGE_COUNT));
    }
}

/* 首次进入检测页时启动单一采样线程；离开检测页后线程休眠。 */
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

/* UP/DOWN 仅切换测试画面，返回操作由左上角控件处理。 */
static void screen_test_exit(void)
{
    screen_test_exit_cb_t callback;
    void *context;

    if (exit_started)
        return;
    exit_started = 1;
    key_worker_enabled = 0;
    pending_key = SCREEN_TEST_KEY_NONE;
    if (key_dispatch_timer) {
        lv_timer_delete(key_dispatch_timer);
        key_dispatch_timer = NULL;
    }
    if (image_poll_timer) {
        lv_timer_delete(image_poll_timer);
        image_poll_timer = NULL;
    }
    image_request_pending = 0;
    image_preview_cancel();
    if (test_overlay) {
        lv_obj_delete(test_overlay);
        test_overlay = NULL;
    }
    callback = exit_callback;
    context = exit_context;
    exit_callback = NULL;
    exit_context = NULL;
    if (callback)
        callback(context);
}

/* 测试页不响应触摸切换；白屏页只记录并绘制蓝色触摸轨迹。 */
static void screen_test_input(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    int32_t x;
    int32_t y;

    if (exit_started)
        return;
    screen_test_get_point(lv_event_get_indev(event), &x, &y);

    if (code == LV_EVENT_PRESSED) {
        active_segment = NULL;
        last_x = x;
        last_y = y;
        touch_down = 1;
        if (current_test_page == SCREEN_TEST_WHITE)
            screen_test_add_touch_point(x, y);
    } else if (code == LV_EVENT_PRESSING) {
        if (touch_down && current_test_page == SCREEN_TEST_WHITE &&
            (x != last_x || y != last_y))
            screen_test_add_touch_point(x, y);
        last_x = x;
        last_y = y;
    } else if (code == LV_EVENT_RELEASED) {
        if (!touch_down)
            return;
        touch_down = 0;
        active_segment = NULL;
    }
}

int screen_test_start(lv_obj_t *parent, const lv_font_t *font,
                      screen_test_exit_cb_t on_exit, void *context)
{
    lv_display_t *display = lv_display_get_default();

    if (!parent || !display || test_overlay)
        return -1;
    screen_width = lv_display_get_horizontal_resolution(display);
    screen_height = lv_display_get_vertical_resolution(display);
    if (screen_width <= 0 || screen_height <= 0 ||
        screen_width > UINT16_MAX || screen_height > UINT16_MAX)
        return -2;

    test_font = font;
    exit_callback = on_exit;
    exit_context = context;
    exit_started = 0;
    image_poll_timer = NULL;
    key_dispatch_timer = NULL;
    image_request_pending = 0;
    pending_key = SCREEN_TEST_KEY_NONE;
    key_worker_enabled = 1;
    current_test_page = SCREEN_TEST_BLACK;
    test_overlay = lv_obj_create(parent);
    if (!test_overlay) {
        exit_callback = NULL;
        exit_context = NULL;
        return -3;
    }
    lv_obj_set_pos(test_overlay, 0, 0);
    lv_obj_set_size(test_overlay, (lv_coord_t)screen_width,
                    (lv_coord_t)screen_height);
    lv_obj_set_style_border_width(test_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(test_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(test_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(test_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(test_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(test_overlay, screen_test_input, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(test_overlay, screen_test_input, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(test_overlay, screen_test_input, LV_EVENT_RELEASED, NULL);
    lv_obj_move_foreground(test_overlay);
    screen_test_start_key_worker();
    key_dispatch_timer = lv_timer_create(screen_test_dispatch_key, 10, NULL);
    screen_test_show_page(SCREEN_TEST_WHITE);
    return 0;
}
