#include <time.h>
#include "ui_manager.h"
#include "hmi_serial_log.h"
#include "app_manager.h"
#include "lvgl.h"
#include "lv_draw_ge2d.h"
#include "lv_port_disp.h"
#include "time_sync.h"
#include "buzzer_manager.h"
#include <rtthread.h>
#include "storage_manager.h"
#include "sd_manager.h"
#include "ota_manager.h"
#include "ota_package.h"
#include "boot_manager.h"
#include "network_manager.h"
#include "wifi_manager.h"
#include "system_manager.h"
#include "image_preview.h"
#include "media_orientation.h"
#include "screen_test.h"
#include "display_profile_manager.h"

/* 显示中断只更新计数，界面线程读取诊断快照。 */
extern uint32_t aic_de_underflow_count(void);
#ifdef AIC_DISP_MIPI_DSI_DRV
extern uint32_t aic_dsi_fifo_overflow_count(void);
#endif
#include "hmi_logo.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <aic_core.h>

#define HMI_FONT_PATH "/rodata/hmi/fonts/NotoSansSC-HMI.ttf"

/* F750 风格：白色顶栏、浅绿底色、深绿状态卡与主操作。 */
#define UI_COLOR_WHITE       0xFFFFFFU
#define UI_COLOR_BACKGROUND  0xF0F5F3U
#define UI_COLOR_HEADER      0xFFFFFFU
#define UI_COLOR_SURFACE     0xFFFFFFU
#define UI_COLOR_SURFACE_ALT 0xE8F3EFU
#define UI_COLOR_BORDER      0xD5E4DFU
#define UI_COLOR_ACCENT      0x106752U
#define UI_COLOR_ACCENT_DARK 0x0D5947U
#define UI_COLOR_STATUS      0x153A36U
#define UI_COLOR_MINT_TEXT   0xBDE7DCU
#define UI_COLOR_MINT_TINT   0xD9EFE7U
#define UI_COLOR_BLUE_TINT   0xE4EEFCU
#define UI_COLOR_BLUE        0x2964ABU
#define UI_COLOR_GREEN       0x218369U
#define UI_COLOR_VIOLET      0x6E56CFU
#define UI_COLOR_TEXT        0x102E2EU
#define UI_COLOR_MUTED       0x4E6A69U
#define PAGE_SYSTEM_INFO     3U
#define PAGE_SCREEN_TEST     7U
#define PAGE_ALL_APPS        10U
#define PAGE_SETTINGS        11U
#define UI_HEADER_HEIGHT     73

static lv_obj_t *content;
static lv_obj_t *ui_root, *ui_header, *home_content, *apps_content;
static lv_obj_t *retired_content, *retired_preview;
static lv_timer_t *page_cleanup_timer;
static lv_obj_t *top;
static lv_obj_t *page_title;
static lv_obj_t *back_button;
static lv_obj_t *details;
static lv_obj_t *file_grid;
static lv_obj_t *file_grid_cards[FM_LIST_MAX];
static int file_selected_index = -1;
static lv_obj_t *pathbox;
static lv_obj_t *targetbox;
static lv_obj_t *editor;
static lv_obj_t *progress;
static lv_obj_t *ota_metrics;
static lv_obj_t *keyboard;
static lv_obj_t *remote_dialog;
static lv_obj_t *image_overlay;
static lv_obj_t *image_area;
static lv_obj_t *image_status_label;
static lv_obj_t *image_object;
static lv_timer_t *image_ui_timer;
static volatile int image_ui_pending;

void hmi_ui_image_signal(void)
{
    rt_enter_critical();
    image_ui_pending = 1;
    rt_exit_critical();
}

/* 此函数只由 LVGL 线程调用，不跨线程操作 LVGL 对象。 */
void hmi_ui_image_dispatch(void)
{
    int pending;
    rt_enter_critical();
    pending = image_ui_pending;
    image_ui_pending = 0;
    rt_exit_critical();
    if (pending) {
        if (image_ui_timer)
            lv_timer_ready(image_ui_timer);
        screen_test_image_ready();
    }
}
static lv_obj_t *wifi_ssid_input;
static lv_obj_t *wifi_password_input;
static lv_obj_t *wifi_status_label;
static lv_obj_t *wifi_saved_dropdown;
static lv_obj_t *wifi_scan_dropdown;
static lv_obj_t *wifi_saved_connect_button;
static lv_obj_t *wifi_indicator;
static int wifi_auto_scan_pending;
static lv_obj_t *home_clock;
static lv_obj_t *home_date;
static lv_obj_t *home_ssid;
static lv_obj_t *home_ip;
static lv_obj_t *home_sd;
static uint32_t home_last_refresh;
static int home_wifi_state = -1;
static lv_font_t *hmi_clock_font;
static lv_obj_t *device_name_label;
static lv_obj_t *device_slot;
static uint32_t device_last_refresh, diagnostic_last_refresh;
static unsigned wifi_scan_display_generation = UINT32_MAX;
static unsigned wifi_saved_display_generation = UINT32_MAX;
static lv_obj_t *logo_image;
static lv_font_t *hmi_font;
static lv_font_t *hmi_font_small;
static int image_preview_ready;
static int first_render_logged;
static int first_flush_logged;
static unsigned page;
static unsigned back_page;
static unsigned settings_origin, settings_interval_shown;
static lv_obj_t *settings_buzzer_switch, *settings_volume_slider;
static lv_obj_t *settings_volume_value, *settings_preview_button, *settings_save_error;
static lv_obj_t *settings_interval_value, *settings_interval_down, *settings_interval_up;
static char folder[256] = "/sdcard";
static fm_list_t files;
static storage_request_t pending;

/* 更新目标是非活动系统槽；ENV 的 next 表示实际下次启动位置。 */
static char ui_update_target(const boot_status_t *boot)
{
    if (boot->current != 'A' && boot->current != 'B')
        return '-';
    if ((boot->next == 'A' || boot->next == 'B') && boot->next != boot->current)
        return boot->next; /* 已提交，等待重启。 */
    if (!boot->confirmed || boot->pending)
        return '-'; /* 试运行尚未确认，暂不可写下一槽。 */
    return boot->current == 'A' ? 'B' : 'A';
}
static const char *titles[] = {
    "首页", "文件管理", "固件升级", "系统信息", "网络设置", "系统日志",
    "设备总览", "屏幕检测", "", "应用支持", "全部应用", "系统设置"
};

static const lv_font_t *body_font(void)
{
    return hmi_font ? hmi_font : &lv_font_montserrat_16;
}

static const lv_font_t *small_font(void)
{
    return hmi_font_small ? hmi_font_small : body_font();
}

/* 在 LVGL 首次完成渲染和显示提交时记录 RT-Thread 阶段耗时。 */
static void boot_display_timing(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_RENDER_READY && !first_render_logged) {
        first_render_logged = 1;
        hmi_serial_log("[boot_time] lvgl_render_ready=%u ms\n",
                   app_manager_boot_elapsed_ms());
    } else if (code == LV_EVENT_FLUSH_FINISH && !first_flush_logged) {
        first_flush_logged = 1;
        hmi_serial_log("[boot_time] display_flush_return=%u ms\n",
                   app_manager_boot_elapsed_ms());
    }
}

/* 弹窗创建完成后递归设置字体，保证标题、正文和按钮都能显示中文。 */
static void apply_dialog_font(lv_obj_t *obj)
{
    uint32_t i;

    if (!obj)
        return;
    lv_obj_set_style_text_font(obj, small_font(), LV_PART_MAIN);
    for (i = 0; i < lv_obj_get_child_count(obj); i++)
        apply_dialog_font(lv_obj_get_child(obj, (int32_t)i));
}

/* 弹窗按屏幕宽度换行，正文独立滚动，底部操作保持可见。 */
static void style_dialog(lv_obj_t *box)
{
    lv_obj_t *body = lv_msgbox_get_content(box);
    lv_display_t *display = lv_obj_get_display(box);

    lv_obj_set_width(box, LV_MIN(550, lv_display_get_horizontal_resolution(display) - 32));
    lv_obj_set_style_max_height(body, lv_display_get_vertical_resolution(display) - 150, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    apply_dialog_font(box);
}

/* 页面按可用宽度换算坐标，弹层仍使用物理像素。 */
static int page_x(lv_obj_t *parent, int value)
{
    while (parent && parent != content)
        parent = lv_obj_get_parent(parent);
    return parent ? value * lv_obj_get_width(content) / 992 : value;
}
static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       int x, int y, int width)
{
    lv_obj_t *obj = lv_label_create(parent);

    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, page_x(parent, x), y);
    lv_obj_set_width(obj, page_x(parent, width));
    lv_obj_set_style_text_font(obj, body_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    return obj;
}

/* 中性卡片统一边框和圆角，不拦截其上的触摸控件。 */
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int width, int height,
                       uint32_t color, uint32_t accent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    LV_UNUSED(accent);

    lv_obj_set_pos(obj, page_x(parent, x), y);
    lv_obj_set_size(obj, page_x(parent, width), height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(obj);

    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y,
                        int width, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_t *text_obj;

    lv_obj_set_pos(obj, page_x(parent, x), y);
    lv_obj_set_size(obj, page_x(parent, width), 48);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_SURFACE_ALT),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, small_font(), LV_PART_MAIN);
    text_obj = lv_label_create(obj);
    lv_label_set_text(text_obj, text);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_width(text_obj, lv_pct(92));
    lv_obj_set_height(text_obj, lv_font_get_line_height(small_font()));
    lv_label_set_long_mode(text_obj, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(text_obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(text_obj, small_font(), LV_PART_MAIN);
    lv_obj_center(text_obj);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, user_data);
    return obj;
}

/* 主要操作使用深绿色填充；其余操作保持浅色，减少视觉竞争。 */
static lv_obj_t *primary_button(lv_obj_t *parent, const char *text, int x, int y,
                                int width, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *obj = button(parent, text, x, y, width, callback, user_data);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_ACCENT_DARK), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    if (lv_obj_get_child(obj, 0))
        lv_obj_set_style_text_color(lv_obj_get_child(obj, 0), lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    return obj;
}


/* 内容卡片按实际文字高度排列，长行换行后推开后续控件。 */
static void use_app_stack(void)
{
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_row(content, 12, 0);
}

static lv_obj_t *flow_container(lv_obj_t *parent, lv_flex_flow_t flow, int gap)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, flow);
    lv_obj_set_style_pad_row(obj, gap, 0);
    lv_obj_set_style_pad_column(obj, gap, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *flow_text(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);

    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(obj, body_font(), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(obj, 4, 0);
    lv_label_set_text(obj, text);
    return obj;
}

static lv_obj_t *app_card(const char *title)
{
    lv_obj_t *card = flow_container(content, LV_FLEX_FLOW_COLUMN, 9);

    lv_obj_set_style_pad_all(card, 15, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    if (title && title[0])
        flow_text(card, title, UI_COLOR_ACCENT_DARK);
    return card;
}

static lv_obj_t *flow_button(lv_obj_t *row, const char *text, int width,
                             lv_event_cb_t callback, unsigned action, int primary)
{
    lv_obj_t *obj;
    void *data = (void *)(uintptr_t)action;

    if (primary)
        obj = primary_button(row, text, 0, 0, 160, callback, data);
    else
        obj = button(row, text, 0, 0, 160, callback, data);
    lv_obj_set_width(obj, width);
    return obj;
}

/* 图标按钮使用 LVGL 的符号字库，保留完整触摸区域。 */
static void use_symbol_font(lv_obj_t *obj)
{
    lv_obj_t *text = lv_obj_get_child(obj, 0);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_16, 0);
    lv_obj_set_height(text, lv_font_get_line_height(&lv_font_montserrat_16));
}

static lv_obj_t *icon_action(lv_obj_t *row, const char *symbol, lv_event_cb_t cb, unsigned action)
{
    lv_obj_t *obj = flow_button(row, symbol, 52, cb, action, 0);
    use_symbol_font(obj);
    return obj;
}

/* 标签列保持稳定，值可换行，长屏幕型号和设备名不会压住相邻项目。 */
static lv_obj_t *info_row(lv_obj_t *card, const char *caption, const char *value)
{
    lv_obj_t *row = flow_container(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_t *key = flow_text(row, caption, UI_COLOR_MUTED);
    lv_obj_t *data = flow_text(row, value, UI_COLOR_TEXT);

    lv_obj_set_width(key, 100);
    lv_obj_set_width(data, 1);
    lv_obj_set_flex_grow(data, 1);
    return data;
}

static void toggle_section(lv_event_t *event)
{
    lv_obj_t *section = lv_event_get_user_data(event);

    lv_obj_t *control = lv_event_get_target(event);
    if (lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(control, LV_STATE_CHECKED);
    } else {
        lv_obj_add_flag(section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(control, LV_STATE_CHECKED);
    }
}

static void section_button(lv_obj_t *parent, const char *caption, lv_obj_t *section)
{
    lv_obj_t *obj = button(parent, caption, 0, 0, 160, toggle_section, section);
    lv_obj_set_width(obj, 116);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_MINT_TINT), LV_STATE_CHECKED);
    lv_obj_move_to_index(obj, lv_obj_get_index(section));
}

static void close_dialog(lv_event_t *event)
{
    lv_msgbox_close(lv_event_get_user_data(event));
}

static void message(const char *text)
{
    lv_obj_t *box = lv_msgbox_create(NULL);
    lv_obj_t *button_obj;

    lv_msgbox_add_title(box, "提示");
    lv_msgbox_add_text(box, text);
    button_obj = lv_msgbox_add_footer_button(box, "确定");
    lv_obj_add_event_cb(button_obj, close_dialog, LV_EVENT_CLICKED, box);
    style_dialog(box);
}

/* 键盘占用独立区域，输入框始终滚动到其上方可见位置。 */
static int app_content_height(void)
{
    return lv_obj_get_height(lv_obj_get_parent(content)) - UI_HEADER_HEIGHT - 15;
}

static void close_keyboard(void)
{
    if (!keyboard || lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN))
        return;
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(content, app_content_height());
}

static void keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        close_keyboard();
}

/* 键盘只在首次聚焦输入框时创建。 */
static int keyboard_ensure(void)
{
    lv_obj_t *root = lv_screen_active();

    if (keyboard)
        return 0;
    keyboard = lv_keyboard_create(root);
    if (!keyboard)
        return -1;
    lv_obj_set_size(keyboard, lv_pct(100), LV_MIN(235, lv_obj_get_height(root) / 2));
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_ALL, NULL);
    return 0;
}

static void focus(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);

    if (keyboard_ensure()) {
        message("无法创建输入键盘。");
        return;
    }
    lv_keyboard_set_textarea(keyboard, target);
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
    lv_obj_update_layout(keyboard);
    lv_obj_set_height(content, app_content_height() - lv_obj_get_height(keyboard));
    lv_obj_update_layout(content);
    lv_obj_scroll_to_view_recursive(target, LV_ANIM_OFF);
}

static lv_obj_t *input(lv_obj_t *parent, const char *text,
                       int x, int y, int width, int height)
{
    lv_obj_t *obj = lv_textarea_create(parent);

    lv_obj_set_pos(obj, page_x(parent, x), y);
    lv_obj_set_size(obj, page_x(parent, width), height);
    lv_textarea_set_text(obj, text);
    lv_textarea_set_max_length(obj, 255);
    lv_obj_set_style_text_font(obj, small_font(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN);
    if (height < 60)
        lv_textarea_set_one_line(obj, 1);
    lv_obj_add_event_cb(obj, focus, LV_EVENT_FOCUSED, NULL);
    return obj;
}


static lv_obj_t *flow_input(lv_obj_t *parent, const char *caption, const char *value)
{
    lv_obj_t *obj;

    flow_text(parent, caption, UI_COLOR_MUTED);
    obj = input(parent, value, 0, 0, 240, 48);
    lv_obj_set_width(obj, lv_pct(100));
    lv_obj_set_style_pad_hor(obj, 12, 0);
    lv_obj_set_style_pad_ver(obj, 8, 0);
    return obj;
}

static void submit(storage_request_t *request)
{
    if (storage_manager_submit(request))
        message("文件服务繁忙，请稍后重试。");
}

static void list_folder(void)
{
    storage_request_t request = {0};

    /* 地址栏随当前目录更新；目录读取仍由存储线程执行。 */
    if (pathbox)
        lv_textarea_set_text(pathbox, folder);
    request.op = ST_LIST;
    snprintf(request.path, sizeof(request.path), "%s", folder);
    submit(&request);
}

static void screen(unsigned number);
static void start_screen_test(lv_event_t *event);

static void navigation(lv_event_t *event)
{
    unsigned target = (unsigned)(uintptr_t)lv_event_get_user_data(event);

    if (target == PAGE_SETTINGS) {
        settings_origin = page == PAGE_ALL_APPS ? PAGE_ALL_APPS : 0U;
        back_page = settings_origin;
    } else if (page == PAGE_SETTINGS && target == PAGE_SYSTEM_INFO) {
        back_page = PAGE_SETTINGS;
    } else if (page == PAGE_ALL_APPS && target != PAGE_SCREEN_TEST)
        back_page = PAGE_ALL_APPS;
    else if (page == 0U || target == PAGE_ALL_APPS)
        back_page = 0;
    screen(target);
}

static void back_navigation(lv_event_t *event)
{
    unsigned target = back_page;

    LV_UNUSED(event);
    back_page = target == PAGE_SETTINGS ? settings_origin : 0U;
    screen(target);
}

typedef struct {
    const char *label;
    const char *symbol;
    unsigned page;
    uint8_t blue;
} home_app_t;

static const home_app_t home_apps[] = {
    {"屏幕检测", LV_SYMBOL_IMAGE, PAGE_SCREEN_TEST, 0},
    {"固件升级", LV_SYMBOL_UPLOAD, 2, 0},
    {"网络设置", LV_SYMBOL_WIFI, 4, 1},
    {"设备总览", LV_SYMBOL_HOME, 6, 0},
    {"文件管理", LV_SYMBOL_DIRECTORY, 1, 1},
    {"系统设置", LV_SYMBOL_SETTINGS, PAGE_SETTINGS, 0},
    {"应用支持", LV_SYMBOL_LIST, 9, 0},
    {"系统信息", LV_SYMBOL_HOME, 3, 0},
    {"系统日志", LV_SYMBOL_LIST, 5, 1},
};

/* 宫格只显示图标与名称，整张卡片为触摸目标。 */
static void app_tile(lv_obj_t *parent, const home_app_t *app,
                     int x, int y, int width, int height)
{
    unsigned featured = app->page == PAGE_SCREEN_TEST;
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_t *icon_tile = lv_label_create(tile);
    lv_obj_t *title_label = lv_label_create(tile);
    int tile_width = page_x(parent, width);
    int icon_width = tile_width < 120 ? 42 : 60;
    int icon_height = height < 150 ? 46 : 55;

    lv_obj_set_pos(tile, page_x(parent, x), y);
    lv_obj_set_size(tile, tile_width, height);
    lv_obj_set_style_bg_color(tile, lv_color_hex(featured ? UI_COLOR_ACCENT : UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(featured ? UI_COLOR_ACCENT_DARK : UI_COLOR_SURFACE_ALT),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(tile, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(tile, featured ? 0 : 1, 0);
    lv_obj_set_style_radius(tile, 17, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tile, navigation, LV_EVENT_CLICKED, (void *)(uintptr_t)app->page);

    lv_label_set_text(icon_tile, app->symbol);
    lv_obj_set_size(icon_tile, icon_width, icon_height);
    lv_obj_align(icon_tile, LV_ALIGN_TOP_MID, 0, height < 150 ? 15 : 27);
    lv_obj_set_style_bg_color(icon_tile, lv_color_hex(featured ? 0x247865U :
                              app->blue ? UI_COLOR_BLUE_TINT : UI_COLOR_MINT_TINT), 0);
    lv_obj_set_style_bg_opa(icon_tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_tile, 13, 0);
    lv_obj_set_style_pad_top(icon_tile, (icon_height - 16) / 2, 0);
    lv_obj_set_style_text_font(icon_tile, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(icon_tile, lv_color_hex(featured ? UI_COLOR_MINT_TEXT :
                                app->blue ? 0x215999U : UI_COLOR_ACCENT_DARK), 0);
    lv_obj_set_style_text_align(icon_tile, LV_TEXT_ALIGN_CENTER, 0);

    lv_label_set_text(title_label, app->label);
    lv_obj_set_width(title_label, tile_width - 12);
    lv_obj_set_height(title_label, lv_font_get_line_height(body_font()));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(title_label, body_font(), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(featured ? UI_COLOR_WHITE : UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_label, LV_ALIGN_BOTTOM_MID, 0, -16);
}

static void set_text_if_changed(lv_obj_t *obj, const char *value)
{
    if (strcmp(lv_label_get_text(obj), value))
        lv_label_set_text(obj, value);
}

/* 首页使用板上 RTC、Wi-Fi 与 SD 挂载状态，不显示模拟数据。 */
static void update_home_status(void)
{
    static const char *weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    wifi_manager_indicator_t wifi = wifi_manager_indicator_state();
    char ssid[33], ip[32], date[48], clock[12];
    const char *network;
    struct tm local;
    uint32_t color = wifi == WIFI_INDICATOR_CONNECTED ? UI_COLOR_ACCENT : UI_COLOR_MUTED;

    if (wifi != home_wifi_state) {
        lv_obj_set_style_text_color(lv_obj_get_child(wifi_indicator, 0), lv_color_hex(color), 0);
        home_wifi_state = wifi;
    }
    if (time_sync_local_time(&local) == 0) {
        snprintf(clock, sizeof(clock), "%02u:%02u",
                 (unsigned)local.tm_hour, (unsigned)local.tm_min);
        snprintf(date, sizeof(date), "%u 月 %u 日 周%s",
                 (unsigned)(local.tm_mon + 1), (unsigned)local.tm_mday, weekdays[local.tm_wday]);
    } else {
        strcpy(clock, "--:--");
        date[0] = '\0';
    }
    wifi_manager_link_info(ssid, sizeof(ssid), ip, sizeof(ip));
    network = ssid[0] ? ssid : wifi == WIFI_INDICATOR_CONNECTED ? "Wi-Fi" :
              wifi == WIFI_INDICATOR_CONNECTING ? "连接中" :
              wifi == WIFI_INDICATOR_FAILED ? "连接失败" :
              wifi == WIFI_INDICATOR_IDLE ? "未连接" : "Wi-Fi 未开启";
    set_text_if_changed(home_clock, clock);
    set_text_if_changed(home_date, date);
    set_text_if_changed(home_ssid, network);
    set_text_if_changed(home_ip, ip);
    set_text_if_changed(home_sd, sd_manager_ready() ? "已挂载" : "未插入");
    home_last_refresh = lv_tick_get();
}

/* 首页使用实际字库行高，窄屏将状态卡和应用入口纵向排列。 */
static void build_home(void)
{
    const lv_font_t *clock_font = hmi_clock_font ? hmi_clock_font : body_font();
    int height = lv_obj_get_height(content);
    int landscape = lv_obj_get_width(content) >= 690;
    int side_width = landscape ? 326 : 960;
    int status_y = landscape ? 54 : 8;
    int tile_height = landscape ? (height - 88) / 2 : 126;
    int right_x = landscape ? 362 : 16;
    int right_width = landscape ? 614 : 960;
    int line = lv_font_get_line_height(body_font());
    int clock_y = 28 + line;
    int date_y = clock_y + lv_font_get_line_height(clock_font) + 4;
    int network_height = 2 * line + 16;
    int storage_height, status_height, deck_y, network_y, storage_icon_y;
    int columns = landscape ? 3 : 2;
    int tile_width = (right_width - (columns - 1) * 12) / columns;
    lv_obj_t *status, *label_obj, *storage, *network_btn, *all;

    tile_height = LV_CLAMP(118, tile_height, 193);
    status_height = LV_MAX(landscape ? tile_height + 66 : 0,
                           date_y + line + network_height + 21);
    storage_height = landscape ? LV_MAX(70, 2 * tile_height + 2 - status_height) : 70;
    deck_y = landscape ? 8 : status_y + status_height + storage_height + 29;
    network_y = status_height - network_height - 9;
    storage_icon_y = (storage_height - 43) / 2;

    status = panel(content, 14, status_y, side_width, status_height, UI_COLOR_STATUS, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_radius(status, 17, 0);
    label_obj = label(status, "设备状态", 22, 16, 138);
    lv_obj_set_style_text_color(label_obj, lv_color_hex(UI_COLOR_MINT_TEXT), 0);
    label_obj = panel(status, side_width - 42, 23, 11, 11, UI_COLOR_MINT_TEXT, 0);
    lv_obj_set_style_border_width(label_obj, 0, 0);
    lv_obj_set_style_radius(label_obj, LV_RADIUS_CIRCLE, 0);
    home_clock = label(status, "--:--", 22, clock_y, side_width - 44);
    lv_obj_set_style_text_color(home_clock, lv_color_hex(UI_COLOR_WHITE), 0);
    lv_obj_set_style_text_font(home_clock, clock_font, 0);
    lv_label_set_long_mode(home_clock, LV_LABEL_LONG_CLIP);
    home_date = label(status, "", 24, date_y, side_width - 48);
    lv_obj_set_style_text_color(home_date, lv_color_hex(UI_COLOR_MINT_TEXT), 0);
    lv_label_set_long_mode(home_date, LV_LABEL_LONG_CLIP);
    label_obj = panel(status, 23, network_y - 4, side_width - 46, 1, 0x58857AU, 0);
    lv_obj_set_style_border_width(label_obj, 0, 0);

    /* 网络名称与 IP 各用一行，长名称不会把 IP 挤掉。 */
    network_btn = button(status, "", 16, network_y, side_width - 32, navigation,
                         (void *)(uintptr_t)4);
    lv_obj_clean(network_btn);
    lv_obj_set_height(network_btn, network_height);
    lv_obj_set_style_bg_opa(network_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(network_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(network_btn, 0, 0);
    label_obj = label(network_btn, LV_SYMBOL_WIFI, 9, 9, 26);
    lv_obj_set_style_text_font(label_obj, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label_obj, lv_color_hex(UI_COLOR_MINT_TEXT), 0);
    home_ssid = label(network_btn, "", 42, 5, side_width - 86);
    lv_obj_set_height(home_ssid, line);
    lv_label_set_long_mode(home_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(home_ssid, lv_color_hex(UI_COLOR_WHITE), 0);
    home_ip = label(network_btn, "", 42, 9 + line, side_width - 86);
    lv_obj_set_style_text_color(home_ip, lv_color_hex(UI_COLOR_MINT_TEXT), 0);
    lv_label_set_long_mode(home_ip, LV_LABEL_LONG_CLIP);

    storage = button(content, "", 14, status_y + status_height + 14, side_width,
                     navigation, (void *)(uintptr_t)1);
    lv_obj_clean(storage);
    lv_obj_set_height(storage, storage_height);
    lv_obj_set_style_radius(storage, 17, 0);
    label_obj = label(storage, LV_SYMBOL_SD_CARD, 19, storage_icon_y, 46);
    lv_obj_set_height(label_obj, 43);
    lv_obj_set_style_text_font(label_obj, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(label_obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_obj, lv_color_hex(0x215999U), 0);
    lv_obj_set_style_pad_top(label_obj, 13, 0);
    lv_obj_set_style_radius(label_obj, 11, 0);
    lv_obj_set_style_bg_color(label_obj, lv_color_hex(UI_COLOR_BLUE_TINT), 0);
    lv_obj_set_style_bg_opa(label_obj, LV_OPA_COVER, 0);
    label(storage, "SD 卡", 79, (storage_height - line) / 2, 98);
    home_sd = label(storage, "", 183, (storage_height - line) / 2, side_width - 206);
    lv_obj_set_style_text_color(home_sd, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_align(home_sd, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(home_sd, LV_LABEL_LONG_CLIP);

    label(content, "常用应用", right_x, deck_y + 11, 190);
    all = button(content, "全部应用", right_x + right_width - 148, deck_y,
                 148, navigation, (void *)(uintptr_t)PAGE_ALL_APPS);
    lv_obj_set_height(all, 46);
    for (unsigned i = 0; i < 6U; ++i)
        app_tile(content, &home_apps[i], right_x + (int)(i % columns) * (tile_width + 12),
                 deck_y + 55 + (int)(i / columns) * (tile_height + 12),
                 tile_width, tile_height);
    update_home_status();
}


static void build_all_apps(void)
{
    unsigned columns = lv_obj_get_width(content) >= 690 ? 3U : 2U;
    int width = (960 - (int)(columns - 1U) * 12) / (int)columns;

    for (unsigned i = 0; i < sizeof(home_apps) / sizeof(home_apps[0]); ++i)
        app_tile(content, &home_apps[i], 16 + (int)(i % columns) * (width + 12),
                 12 + (int)(i / columns) * 139, width, 127);
}

/* 旧的单缓冲配置只在真正滚动时对齐垂直消隐。 */
static void scroll_refresh(lv_event_t *event)
{
    LV_UNUSED(event);
    lv_port_disp_sync_next_refresh();
}

/* 下拉列表由 LVGL 挂在根屏幕上，打开后把固定返回栏抬到最前。 */
static void keep_header_above_list(lv_event_t *event)
{
    LV_UNUSED(event);
    lv_obj_move_foreground(ui_header);
}

static void close_network_lists(void)
{
    if (wifi_scan_dropdown && lv_dropdown_is_open(wifi_scan_dropdown))
        lv_dropdown_close(wifi_scan_dropdown);
    if (wifi_saved_dropdown && lv_dropdown_is_open(wifi_saved_dropdown))
        lv_dropdown_close(wifi_saved_dropdown);
}

/* 首页和全部应用常驻，只保留一个活动的普通应用。 */
static lv_obj_t *create_page(void)
{
    lv_obj_t *obj = lv_obj_create(ui_root);

    if (!obj)
        return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 16, UI_HEADER_HEIGHT + 3);
    lv_obj_set_size(obj, lv_obj_get_width(ui_root) - 32,
                    lv_obj_get_height(ui_root) - UI_HEADER_HEIGHT - 15);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_BORDER), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(obj, scroll_refresh, LV_EVENT_SCROLL, NULL);
    return obj;
}

static void finish_preview_close(void);

static void collect_hidden_views(lv_timer_t *timer_obj)
{
    lv_obj_t *old = retired_content;

    if (timer_obj)
        lv_timer_pause(timer_obj);
    retired_content = NULL;
    if (old)
        lv_obj_delete(old);
    finish_preview_close();
}

static void schedule_collection(void)
{
    if (!page_cleanup_timer) {
        collect_hidden_views(NULL);
        return;
    }
    /* 正常在返回画面提交后清理，刷新未触发时由定时器兜底。 */
    lv_timer_reset(page_cleanup_timer);
    lv_timer_resume(page_cleanup_timer);
}

static void collect_after_refresh(lv_event_t *event)
{
    LV_UNUSED(event);
    if (page_cleanup_timer && (retired_content || retired_preview))
        lv_timer_ready(page_cleanup_timer);
}

static void retire_page(lv_obj_t *old)
{
    if (!old)
        return;
    lv_obj_add_flag(old, LV_OBJ_FLAG_HIDDEN);
    if (old == home_content || old == apps_content)
        return;

    /* 最多保留一个等待清理的页面，连续切换时避免堆积。 */
    if (retired_content)
        lv_obj_delete(retired_content);
    retired_content = old;
    schedule_collection();
}

/* 先断开预览控件与像素缓冲，再交给原有解码服务释放。 */
static void finish_preview_close(void)
{
    lv_obj_t *overlay = retired_preview;
    int was_loading;

    if (!overlay)
        return;
    retired_preview = NULL;
    lv_obj_delete(overlay);
    was_loading = image_preview_status(NULL, 0) == IMAGE_PREVIEW_LOADING;
    image_preview_cancel();
    if (!was_loading)
        lv_image_cache_drop(NULL);
}

static void close_image_preview(lv_event_t *event)
{
    lv_obj_t *overlay = image_overlay;

    if (!overlay)
        return;
    if (image_ui_timer) {
        lv_timer_delete(image_ui_timer);
        image_ui_timer = NULL;
    }
    image_overlay = image_area = image_status_label = image_object = NULL;
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    retired_preview = overlay;
    if (event)
        schedule_collection();
    else
        finish_preview_close();
}

/* 由 LVGL 线程快速检查解码结果，避免等待主界面 200 ms 状态轮询。 */
static void image_preview_ui_timer(lv_timer_t *timer_obj)
{
    image_preview_state_t state;
    const lv_image_dsc_t *image_dsc;
    char status[128];

    if (!image_overlay || !image_status_label) {
        image_ui_timer = NULL;
        lv_timer_delete(timer_obj);
        return;
    }

    state = image_preview_status(status, sizeof(status));
    if (state == IMAGE_PREVIEW_LOADING) {
        lv_obj_clear_flag(image_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(image_status_label, status);
        return;
    }

    if (state == IMAGE_PREVIEW_READY && image_area) {
        image_dsc = image_preview_get_image();
        if (image_dsc && !image_object) {
            image_object = lv_image_create(image_area);
            if (image_object) {
                lv_image_set_src(image_object, image_dsc);
                lv_image_set_antialias(image_object, false);
                /* 解码器已按屏幕可视区域裁剪，控件保持 1:1 原始像素显示。 */
                lv_image_set_scale(image_object, image_preview_get_scale());
                hmi_media_place(image_object, image_dsc->header.w, image_dsc->header.h,
                                lv_obj_get_width(image_area), lv_obj_get_height(image_area), 0);
            }
        }

        if (image_object) {
            lv_obj_add_flag(image_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(image_status_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(image_status_label, "图片控件创建失败。");
        }
    } else {
        lv_obj_clear_flag(image_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(image_status_label, status);
    }

    /* 完成或失败后移除短周期轮询，释放 LVGL timer 对象。 */
    image_ui_timer = NULL;
    lv_timer_delete(timer_obj);
}

static void show_image_preview(const char *path)
{
    lv_obj_t *root;
    lv_obj_t *back_button;
    lv_coord_t screen_width;
    lv_coord_t screen_height;

    if (!image_preview_ready) {
        /* 图片服务延迟到用户首次打开图片时启动，不占用首页启动路径。 */
        image_preview_ready = image_preview_init() == 0;
        if (!image_preview_ready) {
            system_manager_log("图片预览服务初始化失败");
            message("图片预览服务未就绪。");
            return;
        }
    }
    finish_preview_close();
    if (image_preview_request(path)) {
        message("图片正在加载，或预览服务繁忙。");
        return;
    }

    close_keyboard();
    root = lv_screen_active();
    screen_width = lv_obj_get_width(root);
    screen_height = lv_obj_get_height(root);
    image_overlay = lv_obj_create(root);
    if (!image_overlay) {
        image_preview_cancel();
        message("无法创建图片预览窗口。");
        return;
    }
    lv_obj_set_pos(image_overlay, 0, 0);
    lv_obj_set_size(image_overlay, screen_width, screen_height);
    lv_obj_set_style_bg_color(image_overlay, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(image_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(image_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(image_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(image_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(image_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(image_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(image_overlay, LV_OBJ_FLAG_SCROLLABLE);

    image_area = lv_obj_create(image_overlay);
    lv_obj_set_pos(image_area, 0, 0);
    lv_obj_set_size(image_area, screen_width, screen_height);
    lv_obj_set_style_bg_color(image_area, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(image_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(image_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(image_area, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(image_area, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(image_area, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(image_area, 0, LV_PART_MAIN);
    lv_obj_clear_flag(image_area, LV_OBJ_FLAG_SCROLLABLE);

    /* 状态栏覆盖在全屏图片上方，保留完整画面区域。 */
    image_status_label = label(image_overlay, "加载中",
                                82, 22, screen_width - 98);
    lv_label_set_long_mode(image_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(image_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(image_status_label, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(image_status_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(image_status_label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(image_status_label, 8, LV_PART_MAIN);

    /* 返回键始终固定在屏幕左上角，覆盖图片且便于触摸。 */
    back_button = button(image_overlay, LV_SYMBOL_LEFT, 16, 16, 52,
                         close_image_preview, NULL);
    lv_obj_set_height(back_button, 52);
    use_symbol_font(back_button);
    lv_obj_move_foreground(image_overlay);

    image_ui_timer = lv_timer_create(image_preview_ui_timer, 20, NULL);
    if (!image_ui_timer) {
        close_image_preview(NULL);
        message("无法创建图片刷新任务。");
    }
}

static uint32_t file_icon_color(const fm_entry_t *entry)
{
    const char *extension = entry ? strrchr(entry->name, '.') : NULL;
    if (!entry || entry->directory)
        return UI_COLOR_ACCENT;
    if (extension && (!strcasecmp(extension, ".bmp") || !strcasecmp(extension, ".jpg") ||
                      !strcasecmp(extension, ".jpeg") || !strcasecmp(extension, ".png")))
        return UI_COLOR_GREEN;
    return UI_COLOR_MUTED;
}

static void file_card_event(lv_event_t *event)
{
    uintptr_t encoded = (uintptr_t)lv_event_get_user_data(event);
    unsigned index;
    char path[FM_PATH_MAX];
    size_t folder_len, name_len;
    unsigned i;

    if (!encoded)
        return;
    index = (unsigned)(encoded - 1U);
    if (index >= files.count)
        return;
    folder_len = strlen(folder);
    name_len = strlen(files.entries[index].name);
    if (folder_len + name_len + 2U > sizeof(path)) {
        message("文件路径过长。");
        return;
    }
    memcpy(path, folder, folder_len);
    path[folder_len] = '/';
    memcpy(path + folder_len + 1U, files.entries[index].name, name_len + 1U);
    lv_textarea_set_text(pathbox, path);
    /* 文件夹单击即进入，普通文件保留选中后打开的操作。 */
    if (files.entries[index].directory) {
        rt_snprintf(folder, sizeof(folder), "%s", path);
        list_folder();
        return;
    }
    file_selected_index = (int)index;
    for (i = 0; i < files.count && i < FM_LIST_MAX; i++) {
        if (!file_grid_cards[i])
            continue;
        lv_obj_set_style_border_color(file_grid_cards[i],
            lv_color_hex(i == index ? UI_COLOR_ACCENT : UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(file_grid_cards[i], i == index ? 2 : 1, LV_PART_MAIN);
    }
}

/* 文件名和大小各占一个真实字库行高，按列表宽度分列。 */
static void file_grid_render(void)
{
    unsigned i;
    int width, columns, card_width, line;
    const int gap = 8;

    if (!file_grid)
        return;
    lv_obj_clean(file_grid);
    lv_obj_scroll_to_y(file_grid, 0, LV_ANIM_OFF);
    lv_obj_update_layout(file_grid);
    width = lv_obj_get_content_width(file_grid);
    if (width < 24)
        return;
    columns = LV_MAX(1, (width + gap) / (148 + gap));
    card_width = (width - (columns - 1) * gap) / columns;
    line = lv_font_get_line_height(small_font());
    memset(file_grid_cards, 0, sizeof(file_grid_cards));
    file_selected_index = -1;
    for (i = 0; i < files.count && i < FM_LIST_MAX; i++) {
        lv_obj_t *card = lv_button_create(file_grid);
        lv_obj_t *icon;
        lv_obj_t *name;
        lv_obj_t *size;
        char size_text[24];
        int column = (int)i % columns;
        int row = (int)i / columns;
        int card_height = 49 + 2 * line;
        uint32_t icon_color = file_icon_color(&files.entries[i]);

        if (!card)
            continue;
        file_grid_cards[i] = card;
        lv_obj_clear_flag(card, LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_SCROLL_ON_FOCUS |
                               LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(card, column * (card_width + gap), row * (card_height + gap));
        lv_obj_set_size(card, card_width, card_height);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_WHITE), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_SURFACE_ALT), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 11, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);

        icon = lv_label_create(card);
        lv_label_set_text(icon, files.entries[i].directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(icon_color), 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);

        name = lv_label_create(card);
        lv_obj_set_pos(name, 5, 38);
        lv_obj_set_size(name, card_width - 12, line);
        lv_obj_set_style_text_font(name, small_font(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_label_set_text(name, files.entries[i].name);

        if (files.entries[i].directory) {
            strcpy(size_text, "文件夹");
        } else {
            unsigned long bytes = (unsigned long)files.entries[i].size;
            if (bytes >= 1024UL * 1024UL)
                snprintf(size_text, sizeof(size_text), "%lu.%lu MiB",
                         bytes / (1024UL * 1024UL),
                         (bytes % (1024UL * 1024UL)) * 10UL / (1024UL * 1024UL));
            else if (bytes >= 1024UL)
                snprintf(size_text, sizeof(size_text), "%lu.%lu KiB",
                         bytes / 1024UL, (bytes % 1024UL) * 10UL / 1024UL);
            else
                snprintf(size_text, sizeof(size_text), "%lu B", bytes);
        }
        size = lv_label_create(card);
        lv_obj_set_pos(size, 5, 42 + line);
        lv_obj_set_size(size, card_width - 12, line);
        lv_obj_set_style_text_font(size, small_font(), 0);
        lv_obj_set_style_text_color(size, lv_color_hex(UI_COLOR_MUTED), 0);
        lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(size, LV_LABEL_LONG_DOT);
        lv_label_set_text(size, size_text);
        lv_obj_add_event_cb(card, file_card_event, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)(i + 1U));
    }
    if (!files.count)
        flow_text(file_grid, "此文件夹为空", UI_COLOR_MUTED);
}

static void confirmed_file(lv_event_t *event)
{
    lv_obj_t *box = lv_event_get_user_data(event);

    submit(&pending);
    lv_msgbox_close(box);
}

static void file_action(lv_event_t *event)
{
    unsigned operation = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    storage_request_t request = {0};

    if (operation == 100) {
        list_folder();
        return;
    }
    if (operation == 101) {
        char *slash = strrchr(folder, '/');
        if (slash && slash != folder)
            *slash = 0;
        else
            strcpy(folder, "/sdcard");
        list_folder();
        return;
    }
    if (operation == 103) {
        snprintf(folder, sizeof(folder), "/sdcard");
        list_folder();
        return;
    }

    request.op = (storage_op_t)operation;
    snprintf(request.path, sizeof(request.path), "%s", lv_textarea_get_text(pathbox));
    snprintf(request.target, sizeof(request.target), "%s", lv_textarea_get_text(targetbox));
    if (operation == 102) {
        for (unsigned i = 0; i < files.count; i++) {
            const char *name = strrchr(request.path, '/');
            if (name && !strcmp(name + 1, files.entries[i].name) &&
                files.entries[i].directory) {
                snprintf(folder, sizeof(folder), "%s", request.path);
                list_folder();
                return;
            }
        }
        if (image_preview_supported(request.path)) {
            show_image_preview(request.path);
            return;
        }
        message("请选择图片或文件夹。");
        return;
    }
    if (request.op == ST_MKDIR)
        snprintf(request.path, sizeof(request.path), "%s", request.target);
    if (request.op == ST_WRITE) {
        snprintf(request.data, sizeof(request.data), "%s", lv_textarea_get_text(editor));
        request.length = strlen(request.data);
    }
    if (request.op == ST_DELETE || request.op == ST_WRITE) {
        char text[FM_PATH_MAX + 64];
        lv_obj_t *box;
        lv_obj_t *button_obj;

        pending = request;
        snprintf(text, sizeof(text), "%s\n%s",
                 request.op == ST_DELETE ? "确认删除此文件？" : "确认写入此文件？",
                 request.path);
        box = lv_msgbox_create(NULL);
        lv_msgbox_add_title(box, "操作确认");
        lv_msgbox_add_text(box, text);
        button_obj = lv_msgbox_add_footer_button(box, "执行");
        lv_obj_add_event_cb(button_obj, confirmed_file, LV_EVENT_CLICKED, box);
        button_obj = lv_msgbox_add_footer_button(box, "取消");
        lv_obj_add_event_cb(button_obj, close_dialog, LV_EVENT_CLICKED, box);
        style_dialog(box);
        return;
    }
    submit(&request);
}

static void confirm_sd(lv_event_t *event)
{
    lv_msgbox_close(lv_event_get_user_data(event));
    if (ota_manager_request("/sdcard/UPDATE/ota.cpio", NULL, NULL, 0))
        message("升级服务繁忙，请稍后重试。");
}

static void confirm_reboot(lv_event_t *event)
{
    lv_msgbox_close(lv_event_get_user_data(event));
    ota_manager_reboot();
}

static void ota_action(lv_event_t *event)
{
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *box = lv_msgbox_create(NULL);
    lv_obj_t *button_obj;

    lv_msgbox_add_title(box, action == 1 ? "确认安装升级包？" : "确认重启系统？");
    lv_msgbox_add_text(box, action == 1 ?
        "安装后将自动重启。请保持供电。" :
        "请保存当前操作。");
    button_obj = lv_msgbox_add_footer_button(box, "确认");
    lv_obj_add_event_cb(button_obj, action == 1 ? confirm_sd : confirm_reboot,
                        LV_EVENT_CLICKED, box);
    button_obj = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_add_event_cb(button_obj, close_dialog, LV_EVENT_CLICKED, box);
    style_dialog(box);
}

static void remote_answer(lv_event_t *event)
{
    ota_manager_decide((int)(uintptr_t)lv_event_get_user_data(event));
    if (remote_dialog) {
        lv_msgbox_close(remote_dialog);
        remote_dialog = NULL;
    }
}

enum {
    WIFI_CONNECT_TYPED = 1,
    WIFI_CONNECT_SELECTED,
    WIFI_SCAN,
    WIFI_USE_SCAN
};

static void wifi_action(lv_event_t *event)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    int result = -1;
    close_keyboard();
    if (action == WIFI_CONNECT_TYPED && wifi_ssid_input && wifi_password_input) {
        result = wifi_manager_connect(lv_textarea_get_text(wifi_ssid_input),
                                      lv_textarea_get_text(wifi_password_input));
        lv_textarea_set_text(wifi_password_input, "");
    } else if (action == WIFI_CONNECT_SELECTED && wifi_saved_dropdown) {
        result = wifi_manager_connect_saved(lv_dropdown_get_selected(wifi_saved_dropdown));
    } else if (action == WIFI_SCAN) {
        if (wifi_scan_dropdown)
            lv_dropdown_set_options(wifi_scan_dropdown, "扫描中");
        result = wifi_manager_scan();
    } else if (action == WIFI_USE_SCAN && wifi_scan_dropdown && wifi_ssid_input) {
        char ssid[33];
        result = wifi_manager_scan_ssid(lv_dropdown_get_selected(wifi_scan_dropdown), ssid, sizeof(ssid));
        if (!result) lv_textarea_set_text(wifi_ssid_input, ssid);
    }
    if (result) {
        if (wifi_status_label)
            lv_label_set_text(wifi_status_label, "操作失败");
        message("Wi-Fi 操作失败");
    } else if (wifi_status_label && action != WIFI_USE_SCAN) {
        lv_label_set_text(wifi_status_label, action == WIFI_SCAN ?
                          "扫描中" :
                          "连接中");
    }
}

static void wifi_refresh_saved(void)
{
    char options[5 * 34 + 48];
    if (!wifi_saved_dropdown)
        return;
    if (wifi_manager_saved_options(options, sizeof(options)) >= 0) {
        lv_dropdown_set_options(wifi_saved_dropdown, options);
        wifi_saved_display_generation = wifi_manager_saved_generation();
        if (wifi_saved_connect_button) {
            if (wifi_manager_saved_count())
                lv_obj_clear_state(wifi_saved_connect_button, LV_STATE_DISABLED);
            else
                lv_obj_add_state(wifi_saved_connect_button, LV_STATE_DISABLED);
        }
    }
}

/* 检测退出后恢复首页，测试期间独立屏幕负责遮蔽普通界面。 */
static void screen_test_return_to_menu(void *context)
{
    LV_UNUSED(context);
    back_page = 0;
    screen(0);
}

static void start_screen_test(lv_event_t *event)
{
    LV_UNUSED(event);
    if (screen_test_is_active())
        return;
    if (image_overlay)
        close_image_preview(NULL);
    finish_preview_close();
    if (screen_test_start(lv_screen_active(), screen_test_return_to_menu, NULL) != 0)
        message("屏幕检测启动失败");
}

static void start_screen_test_from_key(void)
{
    start_screen_test(NULL);
}

static void app_back_from_key(void)
{
    if (lv_obj_get_child_count(lv_layer_top()))
        return;
    if (image_overlay) {
        close_image_preview(NULL);
    } else if (page != 0U) {
        back_navigation(NULL);
    }
}

/* 窄屏表单纵向排列；宽屏并排时也给字段边界留出间距。 */
static void flow_field_pair(lv_obj_t *card, lv_obj_t **left, lv_obj_t **right)
{
    int wide = lv_obj_get_width(content) >= 690;
    lv_obj_t *row = flow_container(card, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN, 10);

    *left = flow_container(row, LV_FLEX_FLOW_COLUMN, 4);
    *right = flow_container(row, LV_FLEX_FLOW_COLUMN, 4);
    if (wide) {
        lv_obj_set_width(*left, lv_pct(49));
        lv_obj_set_width(*right, lv_pct(49));
    }
}

static void build_file_page(void)
{
    lv_obj_t *card = app_card(NULL);
    lv_obj_t *row = flow_container(card, LV_FLEX_FLOW_ROW_WRAP, 8);
    lv_obj_t *tools;

    icon_action(row, LV_SYMBOL_SD_CARD, file_action, 103);
    icon_action(row, LV_SYMBOL_UP, file_action, 101);
    icon_action(row, LV_SYMBOL_REFRESH, file_action, 100);
    flow_button(row, "打开", 84, file_action, 102, 1);
    pathbox = input(card, folder, 0, 0, 240, 48);
    lv_obj_set_width(pathbox, lv_pct(100));

    tools = flow_container(card, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_add_flag(tools, LV_OBJ_FLAG_HIDDEN);
    section_button(card, "文件操作", tools);
    targetbox = flow_input(tools, "目标路径", "/sdcard/new_folder");
    row = flow_container(tools, LV_FLEX_FLOW_ROW_WRAP, 8);
    flow_button(row, "新建", 86, file_action, ST_MKDIR, 0);
    flow_button(row, "复制", 86, file_action, ST_COPY, 0);
    flow_button(row, "重命名", 96, file_action, ST_RENAME, 0);
    flow_button(row, "删除", 86, file_action, ST_DELETE, 0);

    card = app_card(NULL);
    details = flow_text(card, "", UI_COLOR_MUTED);
    lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN);
    file_grid = lv_obj_create(card);
    lv_obj_set_size(file_grid, lv_pct(100), LV_CLAMP(176, app_content_height() * 3 / 5, 320));
    lv_obj_set_style_bg_opa(file_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(file_grid, 0, 0);
    lv_obj_set_style_radius(file_grid, 0, 0);
    lv_obj_set_style_pad_all(file_grid, 0, 0);
    lv_obj_set_scroll_dir(file_grid, LV_DIR_VER);
    lv_obj_add_event_cb(file_grid, scroll_refresh, LV_EVENT_SCROLL, NULL);
    lv_obj_set_scrollbar_mode(file_grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(file_grid, lv_color_hex(UI_COLOR_ACCENT), LV_PART_SCROLLBAR);
    list_folder();
}

static void build_upgrade_page(void)
{
    lv_obj_t *card = app_card(NULL);
    lv_obj_t *row, *metrics;

    details = flow_text(card, "待机", UI_COLOR_ACCENT_DARK);
    progress = lv_bar_create(card);
    lv_obj_set_size(progress, lv_pct(100), 16);
    lv_obj_set_style_bg_color(progress, lv_color_hex(UI_COLOR_SURFACE_ALT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(progress, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(progress, 8, LV_PART_INDICATOR);
    info_row(card, "固件", HMI_VERSION);

    card = app_card(NULL);
    row = flow_container(card, LV_FLEX_FLOW_ROW_WRAP, 10);
    flow_button(row, "从 SD 卡安装", 154, ota_action, 1, 1);
    flow_button(row, "重启", 86, ota_action, 2, 0);
    metrics = flow_container(card, LV_FLEX_FLOW_COLUMN, 8);
    flow_text(metrics, "/sdcard/UPDATE/ota.cpio", UI_COLOR_MUTED);
    ota_metrics = flow_text(metrics, "", UI_COLOR_MUTED);
    lv_obj_add_flag(metrics, LV_OBJ_FLAG_HIDDEN);
    section_button(card, "详情", metrics);
}

static void build_device_info(lv_obj_t *card, const char *display_info)
{
    char name[HMI_DEVICE_NAME_MAX_BYTES + 1U];

    system_manager_get_device_name(name, sizeof(name));
    device_name_label = info_row(card, "设备", name);
    info_row(card, "固件", HMI_VERSION);
    info_row(card, "屏幕", display_info);
}

static void build_device_overview(const char *display_info)
{
    lv_obj_t *card = app_card(NULL);
    lv_obj_t *diagnostics;

    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_MINT_TINT), 0);
    build_device_info(card, display_info);
    device_slot = info_row(card, "当前槽", "");

    card = app_card(NULL);
    diagnostics = flow_container(card, LV_FLEX_FLOW_COLUMN, 6);
    details = flow_text(diagnostics, "", UI_COLOR_MUTED);
    lv_obj_add_flag(diagnostics, LV_OBJ_FLAG_HIDDEN);
    section_button(card, "诊断", diagnostics);
}

static void build_system_info(const char *display_info)
{
    lv_obj_t *card = app_card(NULL);

    build_device_info(card, display_info);
    info_row(card, "产品", HMI_PRODUCT);
    info_row(card, "硬件", HMI_HARDWARE);

    card = app_card(NULL);
    info_row(card, "系统", "RT-Thread / Luban-Lite");
    info_row(card, "图形", "LVGL 9.1");
    info_row(card, "内存", "SRAM 1 MiB  |  PSRAM 8 MiB");
    info_row(card, "存储", "SPI NOR 16 MiB  |  DATA");
}

static void build_log_page(void)
{
    lv_obj_t *card = app_card(NULL);

    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_STATUS), 0);
    details = flow_text(card, "暂无系统日志", UI_COLOR_WHITE);
    lv_obj_set_style_text_line_space(details, 8, 0);
}

static lv_obj_t *flow_dropdown(lv_obj_t *card, const char *initial)
{
    lv_obj_t *obj = lv_dropdown_create(card);

    lv_obj_set_size(obj, lv_pct(100), 48);
    lv_obj_set_style_text_font(obj, small_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(obj), small_font(), 0);
    lv_obj_add_event_cb(obj, keep_header_above_list, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(lv_dropdown_get_list(obj), scroll_refresh, LV_EVENT_SCROLL, NULL);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_BORDER), 0);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_16, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_dropdown_set_options(obj, initial);
    return obj;
}

static void build_network_page(void)
{
    lv_obj_t *card = app_card(NULL);
    lv_obj_t *row, *left, *right;

    details = flow_text(card, "", UI_COLOR_ACCENT_DARK);
    wifi_status_label = flow_text(card, "", UI_COLOR_MUTED);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_MINT_TINT), 0);

    card = app_card("可用网络");
    row = flow_container(card, LV_FLEX_FLOW_ROW, 8);
    wifi_scan_dropdown = flow_dropdown(row, "扫描中");
    lv_obj_set_width(wifi_scan_dropdown, 1);
    lv_obj_set_flex_grow(wifi_scan_dropdown, 1);
    icon_action(row, LV_SYMBOL_REFRESH, wifi_action, WIFI_SCAN);
    flow_button(card, "选用", 96, wifi_action, WIFI_USE_SCAN, 0);

    card = app_card(NULL);
    flow_field_pair(card, &left, &right);
    wifi_ssid_input = flow_input(left, "网络名称", "");
    lv_textarea_set_max_length(wifi_ssid_input, 32);
    wifi_password_input = flow_input(right, "密码", "");
    lv_textarea_set_max_length(wifi_password_input, 63);
    lv_textarea_set_password_mode(wifi_password_input, true);
    flow_button(card, "连接并保存", 136, wifi_action, WIFI_CONNECT_TYPED, 1);

    card = app_card("已保存网络");
    wifi_saved_dropdown = flow_dropdown(card, "");
    wifi_refresh_saved();
    wifi_saved_connect_button = flow_button(card, "连接", 96,
                                            wifi_action, WIFI_CONNECT_SELECTED, 0);
    if (!wifi_manager_saved_count())
        lv_obj_add_state(wifi_saved_connect_button, LV_STATE_DISABLED);
    /* 页面先返回到事件循环，之后才提交扫描任务。 */
    wifi_auto_scan_pending = 1;
}

static void build_screen_support(const display_profile_info_t *active)
{
    display_profile_choice_t choices[DISPLAY_PROFILE_MAX_CHOICES];
    size_t count = display_profile_choices(choices, DISPLAY_PROFILE_MAX_CHOICES);

    if (count == 0U) {
        flow_text(app_card(NULL), "暂无可用的屏幕型号", UI_COLOR_MUTED);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        display_profile_info_t profile;
        char line[144];
        lv_obj_t *card;
        if (display_profile_get_configured(choices[i].slot, &profile) != DISPLAY_PROFILE_OK)
            continue;
        card = app_card(choices[i].code);
        if (!strcmp(choices[i].code, active->code)) {
            lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_MINT_TINT), 0);
            lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_ACCENT), 0);
            flow_text(card, "当前屏幕", UI_COLOR_ACCENT_DARK);
        }
        snprintf(line, sizeof(line), "%s\n%u × %u  |  触摸 %s", profile.panel,
                 (unsigned)profile.width, (unsigned)profile.height,
                 profile.touch_enabled ? profile.touch : "未启用");
        flow_text(card, line, UI_COLOR_MUTED);
    }
}

/* 设置卡只显示控件和实际值，拖动时更新读数，松手后提交一次。 */
static void settings_show_volume(unsigned value)
{
    char text[16];

    snprintf(text, sizeof(text), "%u%%", value);
    set_text_if_changed(settings_volume_value, text);
}

static void settings_set_sound_controls(unsigned enabled, unsigned volume)
{
    if (enabled) {
        lv_obj_add_state(settings_buzzer_switch, LV_STATE_CHECKED);
        lv_obj_remove_state(settings_volume_slider, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(settings_buzzer_switch, LV_STATE_CHECKED);
        lv_obj_add_state(settings_volume_slider, LV_STATE_DISABLED);
    }
    if (enabled && volume)
        lv_obj_remove_state(settings_preview_button, LV_STATE_DISABLED);
    else
        lv_obj_add_state(settings_preview_button, LV_STATE_DISABLED);
    if ((unsigned)lv_slider_get_value(settings_volume_slider) != volume)
        lv_slider_set_value(settings_volume_slider, (int32_t)volume, LV_ANIM_OFF);
    settings_show_volume(volume);
}

static void settings_commit_sound(void)
{
    unsigned enabled = lv_obj_has_state(settings_buzzer_switch, LV_STATE_CHECKED) ? 1U : 0U;
    unsigned volume = (unsigned)lv_slider_get_value(settings_volume_slider);

    if (buzzer_manager_set_settings(enabled, volume) != BUZZER_MANAGER_OK) {
        buzzer_manager_get_settings(&enabled, &volume);
        message("声音设置失败");
    }
    settings_set_sound_controls(enabled, volume);
}

static void settings_sound_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (target == settings_buzzer_switch) {
        settings_commit_sound();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        settings_show_volume((unsigned)lv_slider_get_value(settings_volume_slider));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        settings_commit_sound();
    }
}

static void settings_preview_sound(lv_event_t *event)
{
    LV_UNUSED(event);
    if (buzzer_manager_beep(160U) != BUZZER_MANAGER_OK)
        message("试音失败");
}

static void settings_refresh_interval(void)
{
    char value[20];
    unsigned interval = screen_test_interval_ms();

    if (!settings_interval_value || interval == settings_interval_shown)
        return;
    settings_interval_shown = interval;
    snprintf(value, sizeof(value), "%u.%u s", interval / 1000U,
             (interval % 1000U) / 100U);
    set_text_if_changed(settings_interval_value, value);
    if (interval > HMI_TEST_INTERVAL_MIN_MS)
        lv_obj_remove_state(settings_interval_down, LV_STATE_DISABLED);
    else
        lv_obj_add_state(settings_interval_down, LV_STATE_DISABLED);
    if (interval < HMI_TEST_INTERVAL_MAX_MS)
        lv_obj_remove_state(settings_interval_up, LV_STATE_DISABLED);
    else
        lv_obj_add_state(settings_interval_up, LV_STATE_DISABLED);
}

static void settings_change_interval(lv_event_t *event)
{
    int32_t step = (uintptr_t)lv_event_get_user_data(event) ?
                   HMI_TEST_INTERVAL_STEP_MS : -HMI_TEST_INTERVAL_STEP_MS;
    int32_t next = (int32_t)screen_test_interval_ms() + step;

    screen_test_set_interval_ms((unsigned)LV_CLAMP(HMI_TEST_INTERVAL_MIN_MS, next,
                                                 HMI_TEST_INTERVAL_MAX_MS));
    settings_refresh_interval();
}

static lv_obj_t *settings_row(lv_obj_t *card)
{
    lv_obj_t *row = flow_container(card, LV_FLEX_FLOW_ROW, 8);

    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                         LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *settings_row_text(lv_obj_t *row, const char *text, uint32_t color)
{
    lv_obj_t *item = flow_text(row, text, color);

    lv_obj_set_width(item, LV_SIZE_CONTENT);
    return item;
}

static void build_settings_page(void)
{
    lv_obj_t *card = app_card("声音");
    lv_obj_t *row = settings_row(card);
    unsigned enabled, volume;

    settings_row_text(row, "蜂鸣器", UI_COLOR_TEXT);
    settings_buzzer_switch = lv_switch_create(row);
    lv_obj_set_size(settings_buzzer_switch, 56, 30);
    lv_obj_set_ext_click_area(settings_buzzer_switch, 7);
    lv_obj_set_style_bg_color(settings_buzzer_switch, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(settings_buzzer_switch, settings_sound_event, LV_EVENT_VALUE_CHANGED, NULL);

    row = settings_row(card);
    settings_row_text(row, "音量", UI_COLOR_TEXT);
    settings_volume_value = settings_row_text(row, "", UI_COLOR_ACCENT_DARK);
    settings_volume_slider = lv_slider_create(card);
    lv_obj_set_size(settings_volume_slider, lv_pct(100), 16);
    lv_obj_set_ext_click_area(settings_volume_slider, 14);
    lv_slider_set_range(settings_volume_slider, 0, 100);
    lv_obj_set_style_bg_color(settings_volume_slider, lv_color_hex(UI_COLOR_MINT_TINT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(settings_volume_slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(settings_volume_slider, lv_color_hex(UI_COLOR_ACCENT_DARK), LV_PART_KNOB);
    lv_obj_add_event_cb(settings_volume_slider, settings_sound_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(settings_volume_slider, settings_sound_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(settings_volume_slider, settings_sound_event, LV_EVENT_PRESS_LOST, NULL);
    row = settings_row(card);
    settings_preview_button = flow_button(row, "试音", 94, settings_preview_sound, 0, 0);
    settings_save_error = settings_row_text(row, "保存失败", 0xB42318U);
    lv_obj_add_flag(settings_save_error, LV_OBJ_FLAG_HIDDEN);
    buzzer_manager_get_settings(&enabled, &volume);
    settings_set_sound_controls(enabled, volume);

    card = app_card("屏幕检测");
    row = settings_row(card);
    settings_row_text(row, "轮播间隔", UI_COLOR_TEXT);
    settings_interval_value = settings_row_text(row, "", UI_COLOR_ACCENT_DARK);
    row = flow_container(card, LV_FLEX_FLOW_ROW_WRAP, 8);
    settings_interval_down = flow_button(row, "-", 52, settings_change_interval, 0, 0);
    settings_interval_up = flow_button(row, "+", 52, settings_change_interval, 1, 0);
    settings_refresh_interval();

    card = app_card(NULL);
    flow_button(card, "系统信息", 136, navigation, PAGE_SYSTEM_INFO, 0);
}

static void screen(unsigned number)
{
    lv_obj_t *next, *old;
    display_profile_info_t display_profile = {0};
    char display_info[144];
    int create;

    if (number == PAGE_SCREEN_TEST) {
        start_screen_test(NULL);
        return;
    }
    if (number == 8U)
        number = 4U;
    if (number >= sizeof(titles) / sizeof(titles[0]) || (content && number == page))
        return;

    next = number == 0U ? home_content : number == PAGE_ALL_APPS ? apps_content : NULL;
    create = next == NULL;
    if (create) {
        next = create_page();
        if (!next)
            return;
    }

    close_keyboard();
    if (keyboard)
        lv_keyboard_set_textarea(keyboard, NULL);
    close_network_lists();
    old = content;
    content = next;
    page = number;
    if (number == 0U)
        home_content = next;
    else if (number == PAGE_ALL_APPS)
        apps_content = next;
    retire_page(old);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_header);

    set_text_if_changed(page_title, titles[number]);
    if (number == 0U) {
        lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(logo_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(page_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(logo_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_indicator, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
    details = file_grid = pathbox = targetbox = editor = progress = ota_metrics = NULL;
    memset(file_grid_cards, 0, sizeof(file_grid_cards));
    file_selected_index = -1;
    wifi_ssid_input = wifi_password_input = NULL;
    wifi_saved_dropdown = wifi_scan_dropdown = wifi_saved_connect_button = NULL;
    wifi_scan_display_generation = wifi_saved_display_generation = UINT32_MAX;
    wifi_auto_scan_pending = 0;
    wifi_status_label = NULL;
    device_name_label = device_slot = NULL;
    settings_save_error = settings_interval_value = NULL;
    settings_interval_shown = 0;
    device_last_refresh = diagnostic_last_refresh = lv_tick_get() - 1000U;

    if (!create)
        return;

    /* 只有首次建页需要布局；从应用返回时直接使用已建好的宫格。 */
    lv_obj_update_layout(content);
    if (number == 3U || number == 6U || number == 9U) {
        if (display_profile_get_active(&display_profile) == DISPLAY_PROFILE_OK)
            snprintf(display_info, sizeof(display_info), "%s\n%u × %u  |  %s",
                     display_profile.code, display_profile.width, display_profile.height,
                     display_profile.touch_enabled ? display_profile.touch : "未启用");
        else
            strcpy(display_info, "屏幕配置不可用");
    }

    if (number == 0U) {
        build_home();
    } else if (number == PAGE_ALL_APPS) {
        build_all_apps();
    } else {
        use_app_stack();
        switch (number) {
        case 1:
            build_file_page();
            break;
        case 2:
            build_upgrade_page();
            break;
        case 3:
            build_system_info(display_info);
            break;
        case 4:
            build_network_page();
            break;
        case 5:
            build_log_page();
            break;
        case 6:
            build_device_overview(display_info);
            break;
        case 9:
            build_screen_support(&display_profile);
            break;
        case PAGE_SETTINGS:
            build_settings_page();
            break;
        default:
            break;
        }
    }
}

static const char *ota_state_text(hmi_ota_state_t state)
{
    switch (state) {
    case OTA_CONFIRM: return "等待确认";
    case OTA_CHECK:   return "正在校验";
    case OTA_WRITE:   return "正在写入";
    case OTA_READY:   return "待重启";
    case OTA_FAILED:  return "升级失败";
    case OTA_RECEIVE: return "接收中";
    case OTA_IDLE:
    default:          return "空闲";
    }
}

/* 单槽命令交接，通信线程只提交编号，LVGL 线程执行控件操作。 */
static volatile int remote_test_target = -1;
int hmi_ui_remote_test(int target)
{
    if (target < 0 || !app_manager_ready()) return -1;
    if (target > HMI_TEST_TOGGLE_MODE &&
        (target < HMI_TEST_INTERVAL_MIN_MS || target > HMI_TEST_INTERVAL_MAX_MS ||
         target % HMI_TEST_INTERVAL_STEP_MS != 0)) return -1;
    return __sync_bool_compare_and_swap(&remote_test_target, -1, target) ? 0 : -2;
}
static void timer(lv_timer_t *timer_obj)
{
    static storage_result_t result;
    static hmi_ota_state_t previous_state = OTA_IDLE;
    static unsigned ready_ticks;
    static unsigned ota_failure_pending;
    char text[2048];
    char network[160];
    rt_size_t heap_total = 0, heap_used = 0, heap_peak = 0;
    boot_status_t boot;
    hmi_ota_status_t ota;

    LV_UNUSED(timer_obj);
    boot_manager_ui_heartbeat();
    boot_manager_get(&boot);
    ota_manager_get(&ota);
    int remote_target = __sync_lock_test_and_set(&remote_test_target, -1);
    if (remote_target >= 0 && (ota.state == OTA_IDLE || ota.state == OTA_FAILED)) {
        if (remote_target >= HMI_TEST_INTERVAL_MIN_MS) {
            /* 调整间隔只更新定时器，不打开测试或关闭图片预览。 */
            screen_test_set_interval_ms((unsigned)remote_target);
        } else {
            if (image_overlay) close_image_preview(NULL);
            if (remote_target != HMI_TEST_EXIT && !screen_test_is_active()) start_screen_test(NULL);
            screen_test_remote_select((unsigned)remote_target);
        }
    }
    /* 远程接收一开始就在界面线程进入升级页。 */
    if (ota.remote && ota.state == OTA_RECEIVE &&
        (page != 2 || screen_test_is_active() || image_overlay)) {
        /* 远程测试覆盖层在进入升级页前由 LVGL 线程退出。 */
        if (screen_test_is_active()) screen_test_remote_select(HMI_TEST_EXIT);
        if (image_overlay) close_image_preview(NULL);
        screen(2);
    }
    int testing = screen_test_is_active();
    if (page == 0 && !testing) {
        rt_uint32_t faults = boot_manager_health_faults();
        if (faults) {
            snprintf(text, sizeof(text), "启动健康检查：0x%02lx",
                     (unsigned long)faults);
            lv_label_set_text(top, text);
            lv_obj_clear_flag(top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(top, lv_color_hex(0xFDE8E7), LV_PART_MAIN);
            lv_obj_set_style_text_color(top, lv_color_hex(0xB42318), LV_PART_MAIN);
        } else {
            lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!testing && page == 4U) {
        char ssid[33], ip[32], wifi_info[160];
        wifi_manager_indicator_t state = wifi_manager_indicator_state();
        const char *status = state == WIFI_INDICATOR_CONNECTED ? "已连接" :
                             state == WIFI_INDICATOR_CONNECTING ? "连接中" :
                             state == WIFI_INDICATOR_FAILED ? "连接失败" :
                             state == WIFI_INDICATOR_IDLE ? "未连接" : "Wi-Fi 未开启";
        wifi_manager_link_info(ssid, sizeof(ssid), ip, sizeof(ip));
        set_text_if_changed(details, ssid[0] ? ssid : status);
        if (state == WIFI_INDICATOR_FAILED) {
            wifi_manager_status(wifi_info, sizeof(wifi_info));
            set_text_if_changed(wifi_status_label, wifi_info);
        } else {
            set_text_if_changed(wifi_status_label, ip[0] ? ip : ssid[0] ? status : "");
        }
        if (wifi_auto_scan_pending) {
            wifi_auto_scan_pending = 0;
            wifi_manager_scan();
        }
        if (wifi_saved_dropdown && !lv_dropdown_is_open(wifi_saved_dropdown) &&
            wifi_manager_saved_generation() != wifi_saved_display_generation)
            wifi_refresh_saved();
        if (wifi_scan_dropdown && !lv_dropdown_is_open(wifi_scan_dropdown) &&
            !wifi_manager_scan_in_progress() &&
            wifi_scan_display_generation != wifi_manager_scan_generation()) {
            char options[1024];
            wifi_scan_display_generation = wifi_manager_scan_generation();
            if (wifi_manager_scan_options(options, sizeof(options)) >= 0)
                lv_dropdown_set_options(wifi_scan_dropdown, options);
        }
    }
    if (!testing && (page == 3U || page == 6U) && device_name_label &&
        lv_tick_elaps(device_last_refresh) >= 1000U) {
        char name[HMI_DEVICE_NAME_MAX_BYTES + 1U];
        system_manager_get_device_name(name, sizeof(name));
        set_text_if_changed(device_name_label, name);
        if (device_slot) {
            snprintf(text, sizeof(text), "%c  |  %s", boot.current,
                     boot.confirmed ? "已确认" : "健康检查中");
            set_text_if_changed(device_slot, text);
        }
        device_last_refresh = lv_tick_get();
    }
    if (!testing && page == PAGE_SETTINGS && settings_save_error) {
        int failed = buzzer_manager_settings_save_state() == BUZZER_MANAGER_SAVE_ERROR;
        if (failed)
            lv_obj_clear_flag(settings_save_error, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(settings_save_error, LV_OBJ_FLAG_HIDDEN);
        settings_refresh_interval();
    }
    if (!testing && page == 0U && home_clock && lv_tick_elaps(home_last_refresh) >= 1000U)
        update_home_status();

    if (ota.state == OTA_READY && !ota.remote) {
        if (++ready_ticks >= 15)
            ota_manager_reboot();
    } else {
        ready_ticks = 0;
    }
    if (ota.state != previous_state) {
        snprintf(text, sizeof(text), "升级状态：%s，进度：%u%%，错误码：%d",
                 ota_state_text(ota.state), ota.percent, ota.error);
        system_manager_log(text);
        ota_failure_pending = ota.state == OTA_FAILED;
        previous_state = ota.state;
    }
    /* 全屏检测期间保留事件，不在测试图上弹出升级信息。 */
    if (!testing && ota_failure_pending) {
        ota_failure_pending = 0;
        snprintf(text, sizeof(text), "升级失败，错误码：%d", ota.error);
        message(text);
    }
    if (!testing && ota.state == OTA_CONFIRM && !remote_dialog) {
        lv_obj_t *button_obj;
        remote_dialog = lv_msgbox_create(NULL);
        lv_msgbox_add_title(remote_dialog, "远程升级确认");
        lv_msgbox_add_text(remote_dialog,
            "工具箱请求安装固件。请保持供电。");
        button_obj = lv_msgbox_add_footer_button(remote_dialog, "允许升级");
        lv_obj_add_event_cb(button_obj, remote_answer, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)1);
        button_obj = lv_msgbox_add_footer_button(remote_dialog, "拒绝");
        lv_obj_add_event_cb(button_obj, remote_answer, LV_EVENT_CLICKED, NULL);
        style_dialog(remote_dialog);
    }
    if (ota.state != OTA_CONFIRM && remote_dialog) {
        lv_msgbox_close(remote_dialog);
        remote_dialog = NULL;
    }

    while (!testing && !image_overlay && storage_manager_poll(&result) == 0) {
        if (result.error) {
            snprintf(text, sizeof(text), "文件操作失败，错误码：%d", result.error);
            message(text);
            continue;
        }
        if (page == 1 && result.op == ST_LIST) {
            files = result.list;
            file_grid_render();
            if (details && files.truncated) {
                set_text_if_changed(details, "列表已截断");
                lv_obj_clear_flag(details, LV_OBJ_FLAG_HIDDEN);
            } else if (details) {
                lv_obj_add_flag(details, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (page == 1 && result.op == ST_READ && editor) {
            lv_textarea_set_text(editor, result.data);
        } else if (result.op == ST_CAPACITY) {
            snprintf(text, sizeof(text), "总容量：%lu MiB，可用：%lu MiB",
                     (unsigned long)(result.total >> 20),
                     (unsigned long)(result.free_bytes >> 20));
            message(text);
        } else if (page == 1) {
            list_folder();
        }
    }

    if (!testing && details && !image_overlay) {
        if (page == 6 && !lv_obj_has_flag(lv_obj_get_parent(details), LV_OBJ_FLAG_HIDDEN) &&
            lv_tick_elaps(diagnostic_last_refresh) >= 1000U) {
            unsigned long dsi_overflow = 0;
            diagnostic_last_refresh = lv_tick_get();
            network_manager_status(network, sizeof(network));
#ifdef AIC_DISP_MIPI_DSI_DRV
            dsi_overflow = (unsigned long)aic_dsi_fifo_overflow_count();
#endif
            rt_memory_info(&heap_total, &heap_used, &heap_peak);
            snprintf(text, sizeof(text),
                     "当前槽：%c    更新目标：%c\n下次启动：%c    固定资源分区：A\n健康状态：%s\n"
                     "健康故障位：0x%02lx\n网络：%s\nSD 卡：%s\n运行时间：%lu 秒\n"
                     "SRAM 堆：%lu / %lu KiB\nPSRAM：8 MiB\nGE：%s，已完成 %lu 次\n"
                     "DE 欠载：%lu\nMIPI FIFO 溢出：%lu",
                     boot.current, ui_update_target(&boot), boot.next,
                     boot.confirmed ? "已确认" : "健康检查中",
                     (unsigned long)boot_manager_health_faults(), network,
                     sd_manager_ready() ? "已就绪" : "未插入",
                     (unsigned long)(rt_tick_get() / RT_TICK_PER_SECOND),
                     (unsigned long)(heap_used / 1024U),
                     (unsigned long)(heap_total / 1024U),
                     get_ge2d_device() ? "硬件已启用" : "软件绘制",
                     (unsigned long)lv_draw_ge2d_completed(),
                     (unsigned long)aic_de_underflow_count(), dsi_overflow);
            set_text_if_changed(details, text);
        } else if (page == 2) {
            snprintf(text, sizeof(text), "%s  %u%%\n目标槽：%c",
                     ota_state_text(ota.state), ota.percent, ui_update_target(&boot));
            set_text_if_changed(details, text);
            lv_bar_set_value(progress, ota.percent, LV_ANIM_OFF);
            if (ota.state != OTA_IDLE &&
                !lv_obj_has_flag(lv_obj_get_parent(ota_metrics), LV_OBJ_FLAG_HIDDEN)) {
                snprintf(text, sizeof(text),
                         "版本：%s    错误码：%d    速度：%lu KiB/s\n"
                         "签名 %lu ms  |  确认 %lu ms  |  写入 %lu ms\n"
                         "校验 %lu ms  |  提交 %lu ms  |  总计 %lu ms",
                         ota.version, ota.error,
                         (unsigned long)(ota.state == OTA_RECEIVE ? ota.bytes_per_second / 1024U : 0U),
                         (unsigned long)ota.phase_ms[1], (unsigned long)ota.phase_ms[2],
                         (unsigned long)ota.phase_ms[3], (unsigned long)ota.phase_ms[4],
                         (unsigned long)ota.phase_ms[5], (unsigned long)ota.total_ms);
                set_text_if_changed(ota_metrics, text);
                lv_obj_clear_flag(ota_metrics, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ota_metrics, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (page == 5 && lv_tick_elaps(diagnostic_last_refresh) >= 1000U) {
            diagnostic_last_refresh = lv_tick_get();
            system_manager_logs(text, sizeof(text));
            if (text[0])
                set_text_if_changed(details, text);
        }
    }
}

void hmi_ui_init(void)
{
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *header_band;
    lv_obj_t *header_accent;

    hmi_serial_log("[boot_time] hmi_ui_enter=%u ms\n",app_manager_boot_elapsed_ms());
    /* 字体由只读资源分区提供；加载失败时不启动健康心跳，保留 A/B 回滚保护。 */
    hmi_font = lv_freetype_font_create(HMI_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 18, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!hmi_font) {
        lv_obj_t *error = lv_label_create(root);
        lv_label_set_text(error, "Chinese font load failed; check /rodata/hmi/fonts.");
        lv_obj_set_pos(error, 20, 30);
        hmi_serial_log("[hmi] Chinese font load failed; health confirmation blocked\n");
        return;
    }
    /* 两种界面字号复用同一个字体实例，避免重复创建 FreeType 字体对象。 */
    hmi_font_small = hmi_font;
    /* 数字时钟使用加粗大字；失败时退回正文字体。 */
    hmi_clock_font = lv_freetype_font_create(HMI_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 50, LV_FREETYPE_FONT_STYLE_BOLD);
    /* 减少动画固定开启，按压直接更新颜色。 */
    hmi_serial_log("[boot_time] chinese_font_ready=%u ms\n",app_manager_boot_elapsed_ms());
    lv_obj_set_style_text_font(root, body_font(), LV_PART_MAIN);
    if (!app_manager_ready()) {
        label(root, "应用服务初始化失败，请检查串口日志。", 20, 30, 900);
        hmi_serial_log("[hmi] application services are not ready; health confirmation blocked\n");
        return;
    }
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    header_band = lv_obj_create(root);
    ui_header = header_band;
    lv_obj_set_pos(header_band, 0, 0);
    lv_obj_set_size(header_band, lv_obj_get_width(root), UI_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(header_band, lv_color_hex(UI_COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_band, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_band, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header_band, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header_band, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    header_accent = lv_obj_create(ui_header);
    lv_obj_set_pos(header_accent, 0, UI_HEADER_HEIGHT - 1);
    lv_obj_set_size(header_accent, lv_obj_get_width(root), 1);
    lv_obj_set_style_bg_color(header_accent, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_accent, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header_accent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_accent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    logo_image = lv_image_create(ui_header);
    lv_image_set_src(logo_image, &hmi_logo_image);
    lv_image_set_pivot(logo_image, 0, 0);
    lv_image_set_scale(logo_image, 158);
    lv_obj_set_pos(logo_image, 28, 13);
    lv_obj_clear_flag(logo_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 左侧只放品牌 Logo，页面标题仅在子页显示。 */
    page_title = label(ui_header, "", 86, 24, lv_obj_get_width(root) - 104);
    lv_obj_set_height(page_title, lv_font_get_line_height(body_font()));
    lv_label_set_long_mode(page_title, LV_LABEL_LONG_DOT);
    top = label(ui_header, "", 123, 19, lv_obj_get_width(root) - 220);
    lv_obj_set_height(top, 39);
    lv_label_set_long_mode(top, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_hor(top, 10, 0);
    lv_obj_set_style_pad_top(top, 8, 0);
    lv_obj_set_style_radius(top, 10, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);

    wifi_indicator = button(ui_header, LV_SYMBOL_WIFI, lv_obj_get_width(root) - 76, 12,
                            50, navigation, (void *)(uintptr_t)4);
    lv_obj_set_style_border_width(wifi_indicator, 0, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(wifi_indicator, 0), &lv_font_montserrat_16, 0);
    lv_obj_set_height(lv_obj_get_child(wifi_indicator, 0), lv_font_get_line_height(&lv_font_montserrat_16));
    back_button = button(ui_header, LV_SYMBOL_LEFT, 12, 12, 64, back_navigation, NULL);
    use_symbol_font(back_button);
    lv_obj_set_ext_click_area(back_button, 5);

    ui_root = root;
    page_cleanup_timer = lv_timer_create(collect_hidden_views, 110, NULL);
    if (page_cleanup_timer)
        lv_timer_pause(page_cleanup_timer);
    screen(0);
    screen_test_keys_start(start_screen_test_from_key, app_back_from_key);
    lv_timer_create(timer, 200, NULL);
    if (lv_display_get_default()) {
        lv_display_add_event_cb(lv_display_get_default(), boot_display_timing,
                                LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(lv_display_get_default(), boot_display_timing,
                                LV_EVENT_FLUSH_FINISH, NULL);
        lv_display_add_event_cb(lv_display_get_default(), collect_after_refresh,
                                LV_EVENT_REFR_READY, NULL);
    }
    hmi_serial_log("[boot_time] hmi_widgets_ready=%u ms\n",app_manager_boot_elapsed_ms());
}
