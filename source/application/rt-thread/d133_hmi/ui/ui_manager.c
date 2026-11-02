#include "ui_manager.h"
#include "hmi_serial_log.h"
#include "app_manager.h"
#include "lvgl.h"
#include <rtthread.h>
#include "storage_manager.h"
#include "sd_manager.h"
#include "ota_manager.h"
#include "ota_package.h"
#include "boot_manager.h"
#include "network_manager.h"
#include "system_manager.h"
#include "image_preview.h"
#include "display_profile_manager.h"
#include "hmi_logo.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define HMI_FONT_PATH "/rodata/hmi/fonts/NotoSansSC-HMI.ttf"

/* 复用 STM32F750 主界面的白底、浅蓝卡片和青蓝强调色。 */
#define UI_COLOR_WHITE       0xFFFFFFU
#define UI_COLOR_HEADER      0xF5F9FEU
#define UI_COLOR_SURFACE     0xEAF4FFU
#define UI_COLOR_SURFACE_ALT 0xDCECFBU
#define UI_COLOR_BORDER      0xDCE4EEU
#define UI_COLOR_ACCENT      0x087F91U
#define UI_COLOR_BLUE        0x2778D4U
#define UI_COLOR_GREEN       0x15945AU
#define UI_COLOR_VIOLET      0x7451BDU
#define UI_COLOR_TEXT        0x1C2B41U
#define UI_COLOR_MUTED       0x63748AU

static lv_obj_t *content;
static lv_obj_t *top;
static lv_obj_t *page_title;
static lv_obj_t *back_button;
static lv_obj_t *details;
static lv_obj_t *table;
static lv_obj_t *pathbox;
static lv_obj_t *targetbox;
static lv_obj_t *editor;
static lv_obj_t *progress;
static lv_obj_t *keyboard;
static lv_obj_t *remote_dialog;
static lv_obj_t *image_overlay;
static lv_obj_t *image_area;
static lv_obj_t *image_status_label;
static lv_obj_t *image_object;
static lv_timer_t *image_ui_timer;
static lv_obj_t *ip_input;
static lv_obj_t *mask_input;
static lv_obj_t *gw_input;
static lv_obj_t *logo_image;
static lv_font_t *hmi_font;
static lv_font_t *hmi_font_small;
static int image_preview_ready;
static int first_render_logged;
static int first_flush_logged;
static unsigned page;
static char folder[256] = "/sdcard";
static fm_list_t files;
static storage_request_t pending;

static const char *titles[] = {
    "首页", "文件管理", "固件升级", "系统信息", "网络设置", "系统日志", "设备总览"
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

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       int x, int y, int width)
{
    lv_obj_t *obj = lv_label_create(parent);

    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, body_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    return obj;
}

/* 创建 STM32 风格的浅蓝信息卡，卡片和强调条不拦截触摸事件。 */
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int width, int height,
                       uint32_t color, uint32_t accent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_t *rail;

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(obj);

    if (accent != 0U) {
        rail = lv_obj_create(obj);
        lv_obj_set_pos(rail, 0, 0);
        lv_obj_set_size(rail, 4, height);
        lv_obj_set_style_bg_color(rail, lv_color_hex(accent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(rail, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(rail, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(rail, 0, LV_PART_MAIN);
        lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y,
                        int width, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_t *text_obj;

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, 48);
    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, small_font(), LV_PART_MAIN);
    text_obj = lv_label_create(obj);
    lv_label_set_text(text_obj, text);
    lv_obj_set_width(text_obj, width - 12);
    lv_obj_set_style_text_align(text_obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(text_obj, small_font(), LV_PART_MAIN);
    lv_obj_center(text_obj);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, user_data);
    return obj;
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
    apply_dialog_font(box);
}

static void keyboard_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY ||
        lv_event_get_code(event) == LV_EVENT_CANCEL)
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

/* 键盘只在用户首次聚焦输入框时创建，避免首页启动时生成大量按键对象。 */
static int keyboard_ensure(void)
{
    if (keyboard)
        return 0;

    keyboard = lv_keyboard_create(lv_screen_active());
    if (!keyboard)
        return -1;
    lv_obj_set_size(keyboard, 1024, 235);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_ALL, NULL);
    return 0;
}

static void focus(lv_event_t *event)
{
    if (keyboard_ensure()) {
        message("无法创建输入键盘。");
        return;
    }
    lv_keyboard_set_textarea(keyboard, lv_event_get_target(event));
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
}

static lv_obj_t *input(lv_obj_t *parent, const char *text,
                       int x, int y, int width, int height)
{
    lv_obj_t *obj = lv_textarea_create(parent);

    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_textarea_set_text(obj, text);
    lv_textarea_set_max_length(obj, 255);
    lv_obj_set_style_text_font(obj, small_font(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_SURFACE_ALT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
    if (height < 60)
        lv_textarea_set_one_line(obj, 1);
    lv_obj_add_event_cb(obj, focus, LV_EVENT_FOCUSED, NULL);
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

    request.op = ST_LIST;
    snprintf(request.path, sizeof(request.path), "%s", folder);
    submit(&request);
}

static void screen(unsigned number);

static void navigation(lv_event_t *event)
{
    screen((unsigned)(uintptr_t)lv_event_get_user_data(event));
}

/* 创建 STM32 五列启动器样式的应用卡片，整张卡片都可触摸打开。 */
static lv_obj_t *app_tile(lv_obj_t *parent, const char *symbol,
                          const char *title,
                          int x, int y, int width, int height,
                          unsigned target, uint32_t accent)
{
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_t *icon_tile;
    lv_obj_t *title_label;

    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, width, height);
    lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(tile, lv_color_hex(UI_COLOR_SURFACE_ALT),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(tile, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* 图标底板和符号合并成一个 Label，减少启动首页的 LVGL 对象数。 */
    icon_tile = lv_label_create(tile);
    lv_label_set_text(icon_tile, symbol);
    lv_obj_set_pos(icon_tile, (width - 56) / 2, 10);
    lv_obj_set_size(icon_tile, 56, 56);
    lv_obj_set_style_bg_color(icon_tile, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon_tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon_tile, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(icon_tile, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon_tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(icon_tile, 20, LV_PART_MAIN);
    lv_obj_set_style_text_font(icon_tile, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_tile, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_text_align(icon_tile, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_clear_flag(icon_tile, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    title_label = lv_label_create(tile);
    lv_label_set_text(title_label, title);
    lv_obj_set_pos(title_label, 8, 74);
    lv_obj_set_size(title_label, width - 16, 28);
    lv_obj_set_style_text_font(title_label, body_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_add_event_cb(tile, navigation, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)target);
    return tile;
}

static void close_image_preview(lv_event_t *event)
{
    lv_obj_t *overlay = image_overlay;

    LV_UNUSED(event);
    if (image_ui_timer) {
        lv_timer_delete(image_ui_timer);
        image_ui_timer = NULL;
    }
    image_overlay = NULL;
    image_area = NULL;
    image_status_label = NULL;
    image_object = NULL;
    if (overlay)
        lv_obj_delete(overlay);
    image_preview_cancel();
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
                lv_image_set_scale(image_object, 256U);
                lv_obj_center(image_object);
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
    if (image_preview_request(path)) {
        message("图片正在加载，或预览服务繁忙。");
        return;
    }

    if (keyboard)
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
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
    image_status_label = label(image_overlay, "正在读取并解码图片……",
                                148, 22, screen_width - 164);
    lv_label_set_long_mode(image_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(image_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(image_status_label, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(image_status_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(image_status_label, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(image_status_label, 8, LV_PART_MAIN);

    /* 返回键始终固定在屏幕左上角，覆盖图片且便于触摸。 */
    back_button = button(image_overlay, "返回", 16, 16, 112,
                         close_image_preview, NULL);
    lv_obj_set_height(back_button, 56);
    lv_obj_move_foreground(image_overlay);

    image_ui_timer = lv_timer_create(image_preview_ui_timer, 20, NULL);
    if (!image_ui_timer) {
        close_image_preview(NULL);
        message("无法创建图片刷新任务。");
    }
}

static void table_event(lv_event_t *event)
{
    uint32_t row;
    uint32_t column;

    LV_UNUSED(event);
    lv_table_get_selected_cell(table, &row, &column);
    LV_UNUSED(column);
    if (row && row <= files.count) {
        char path[FM_PATH_MAX];
        size_t folder_len = strlen(folder);
        size_t name_len = strlen(files.entries[row - 1].name);

        if (folder_len + name_len + 2 > sizeof(path)) {
            message("文件路径过长。");
            return;
        }
        memcpy(path, folder, folder_len);
        path[folder_len] = '/';
        memcpy(path + folder_len + 1, files.entries[row - 1].name, name_len + 1);
        lv_textarea_set_text(pathbox, path);
    }
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
        request.op = ST_READ;
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
        apply_dialog_font(box);
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
        "系统将校验签名并更新非活动系统槽和资源槽。成功后设备会自动重启。\n请保持供电。" :
        "即将重启设备，请确认当前操作已保存。重启期间请保持供电。");
    button_obj = lv_msgbox_add_footer_button(box, "确认");
    lv_obj_add_event_cb(button_obj, action == 1 ? confirm_sd : confirm_reboot,
                        LV_EVENT_CLICKED, box);
    button_obj = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_add_event_cb(button_obj, close_dialog, LV_EVENT_CLICKED, box);
    apply_dialog_font(box);
}

static void remote_answer(lv_event_t *event)
{
    ota_manager_decide((int)(uintptr_t)lv_event_get_user_data(event));
    if (remote_dialog) {
        lv_msgbox_close(remote_dialog);
        remote_dialog = NULL;
    }
}

static void apply_network(lv_event_t *event)
{
    int dhcp = (int)(uintptr_t)lv_event_get_user_data(event);

    if (network_manager_configure(lv_textarea_get_text(ip_input),
                                  lv_textarea_get_text(mask_input),
                                  lv_textarea_get_text(gw_input), dhcp))
        message("网络配置繁忙，或 IPv4 参数格式无效。");
}

static void screen(unsigned number)
{
    static const char *app_symbols[6] = {
        LV_SYMBOL_HOME, LV_SYMBOL_SD_CARD, LV_SYMBOL_DOWNLOAD,
        LV_SYMBOL_SETTINGS, LV_SYMBOL_WIFI, LV_SYMBOL_LIST
    };
    static const char *app_titles[6] = {
        "设备总览", "文件管理", "固件升级", "系统信息", "网络设置", "系统日志"
    };
    static const unsigned app_targets[6] = {6, 1, 2, 3, 4, 5};
    static const uint32_t app_colors[6] = {
        UI_COLOR_ACCENT, UI_COLOR_BLUE, UI_COLOR_VIOLET,
        UI_COLOR_GREEN, UI_COLOR_BLUE, UI_COLOR_VIOLET
    };
    static const char *file_buttons[] = {
        "刷新", "上一级", "打开 / 读取", "新建文件夹", "复制", "重命名", "删除"
    };
    static const unsigned file_operations[] = {
        100, 101, 102, ST_MKDIR, ST_COPY, ST_RENAME, ST_DELETE
    };
    display_profile_info_t display_profile;
    char display_info[96];
    unsigned i;

    if (display_profile_get_active(&display_profile) == DISPLAY_PROFILE_OK) {
        snprintf(display_info, sizeof(display_info), "%s  %ux%u LVDS  触摸：%s",
                 display_profile.code, display_profile.width, display_profile.height,
                 display_profile.touch_enabled ? display_profile.touch : "未启用");
    } else {
        snprintf(display_info, sizeof(display_info), "屏幕配置不可用");
    }

    if (number >= 7)
        return;
    page = number;
    lv_label_set_text(page_title, number == 0 ? "D133-控制终端" : titles[number]);
    if (number == 0) {
        lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(logo_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(page_title, 160, 30);
        lv_obj_set_width(page_title, 400);
    } else {
        lv_obj_clear_flag(back_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(logo_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(page_title, 124, 30);
        lv_obj_set_width(page_title, 400);
        lv_label_set_text(top, "正在读取系统状态……");
    }
    if (keyboard)
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(content);
    details = table = pathbox = targetbox = editor = progress = NULL;
    ip_input = mask_input = gw_input = NULL;

    if (number == 0) {
        /* 首页只显示已实现的应用入口，设备状态在独立总览页查看。 */
        /* 5×3 网格保留 STM32 应用启动器的列布局，仅绘制六个现有功能。 */
        for (i = 0; i < 6; i++) {
            unsigned column = i % 5U;
            unsigned row = i / 5U;
            app_tile(content, app_symbols[i], app_titles[i],
                     16 + (int)column * 196, 16 + (int)row * 126,
                     180, 112, app_targets[i], app_colors[i]);
        }
    } else if (number == 1) {
        panel(content, 16, 8, 960, 482, UI_COLOR_SURFACE, UI_COLOR_ACCENT);
        pathbox = input(content, folder, 32, 24, 928, 44);
        targetbox = input(content, "/sdcard/new_folder", 32, 76, 928, 44);
        for (i = 0; i < 7; i++)
            button(content, file_buttons[i], 32 + (int)i * 132, 132, 120,
                   file_action, (void *)(uintptr_t)file_operations[i]);
        table = lv_table_create(content);
        lv_obj_set_pos(table, 32, 196);
        lv_obj_set_size(table, 550, 240);
        lv_table_set_column_count(table, 2);
        lv_table_set_column_width(table, 0, 420);
        lv_table_set_column_width(table, 1, 112);
        lv_obj_set_style_bg_color(table, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(table, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
        lv_obj_set_style_text_color(table, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
        lv_obj_set_style_text_font(table, small_font(), LV_PART_ITEMS);
        lv_obj_add_event_cb(table, table_event, LV_EVENT_VALUE_CHANGED, NULL);
        editor = input(content, "", 602, 196, 358, 178);
        lv_textarea_set_max_length(editor, 511);
        button(content, "保存文本", 602, 386, 168, file_action, (void *)(uintptr_t)ST_WRITE);
        button(content, "容量查询", 782, 386, 178, file_action, (void *)(uintptr_t)ST_CAPACITY);
        details = label(content, "选择文件后可读取、复制、重命名或删除。",
                        32, 448, 550);
        list_folder();
    } else if (number == 2) {
        panel(content, 16, 8, 960, 442, UI_COLOR_SURFACE, UI_COLOR_ACCENT);
        label(content,
              "SD 卡升级包：/sdcard/UPDATE/ota.cpio\n"
              "TCP 推送：研发工具箱，选择 D133CBS 开发板\n"
              "签名校验通过后调用 ArtInChip 官方升级引擎。\n"
              "升级期间请保持设备供电。",
              32, 28, 920);
        details = label(content, "等待升级操作。", 32, 168, 920);
        progress = lv_bar_create(content);
        lv_obj_set_pos(progress, 32, 240);
        lv_obj_set_size(progress, 920, 24);
        lv_obj_set_style_bg_color(progress, lv_color_hex(UI_COLOR_SURFACE_ALT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(progress, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_radius(progress, 8, LV_PART_MAIN);
        lv_obj_set_style_radius(progress, 8, LV_PART_INDICATOR);
        button(content, "从 SD 卡安装", 32, 310, 270, ota_action, (void *)(uintptr_t)1);
        button(content, "重启设备", 322, 310, 250, ota_action, (void *)(uintptr_t)2);
    } else if (number == 3) {
        panel(content, 16, 8, 960, 244, UI_COLOR_SURFACE, UI_COLOR_BLUE);
        label(content,
              "产品型号：" HMI_PRODUCT "\n"
              "硬件版本：" HMI_HARDWARE "\n"
              "固件版本：" HMI_VERSION "\n"
              "操作系统：RT-Thread / ArtInChip Luban-Lite\n"
              "图形库：LVGL 9.1\n",
              32, 28, 920);
        label(content, display_info, 32, 158, 920);
        label(content, "启动存储：16 MiB SPI NOR | DATA：单分区",
              32, 190, 920);
        panel(content, 16, 270, 960, 180, UI_COLOR_SURFACE, UI_COLOR_ACCENT);
        label(content, "当前网络、A/B 槽位、健康状态和运行时间请查看设备总览。",
              32, 294, 920);
    } else if (number == 4) {
        panel(content, 16, 8, 960, 438, UI_COLOR_SURFACE, UI_COLOR_ACCENT);
        label(content, "RMII 以太网 / RTL8201F | TCP OTA 端口：8899", 32, 24, 920);
        details = label(content, "正在等待以太网连接……", 32, 60, 920);
        label(content, "IPv4 地址", 32, 120, 280);
        label(content, "子网掩码", 346, 120, 280);
        label(content, "默认网关", 660, 120, 280);
        ip_input = input(content, "192.168.1.200", 32, 150, 280, 52);
        mask_input = input(content, "255.255.255.0", 346, 150, 280, 52);
        gw_input = input(content, "192.168.1.1", 660, 150, 280, 52);
        button(content, "应用静态地址", 32, 226, 260, apply_network, NULL);
        button(content, "启用 DHCP", 310, 226, 240, apply_network, (void *)(uintptr_t)1);
        label(content, "网络参数仅在本次运行期间生效，重启后恢复编译配置。",
              32, 300, 920);
    } else if (number == 5) {
        lv_obj_t *log_card = panel(content, 16, 8, 960, 480,
                                   UI_COLOR_SURFACE, UI_COLOR_VIOLET);
        lv_obj_add_flag(log_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(log_card, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(log_card, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_pad_all(log_card, 16, LV_PART_MAIN);
        lv_obj_set_style_bg_color(log_card, lv_color_hex(UI_COLOR_ACCENT),
                                  LV_PART_SCROLLBAR);
        details = label(log_card, "暂无系统日志。", 8, 8, 920);
    } else {
        panel(content, 16, 8, 460, 468, UI_COLOR_SURFACE, UI_COLOR_ACCENT);
        panel(content, 516, 8, 460, 468, UI_COLOR_SURFACE, UI_COLOR_BLUE);
        label(content, "系统状态", 40, 28, 410);
        details = label(content, "系统状态正在读取……", 40, 68, 410);
        label(content, "设备信息", 540, 28, 410);
        label(content,
              "产品型号：" HMI_PRODUCT "\n"
              "硬件版本：" HMI_HARDWARE "\n"
              "固件版本：" HMI_VERSION "\n"
              "操作系统：RT-Thread / ArtInChip Luban-Lite\n"
              "图形界面：LVGL 9.1\n"
              "屏幕配置：",
              540, 68, 410);
        label(content, display_info, 540, 222, 410);
        label(content,
              "启动存储：16 MiB SPI NOR\n"
              "DATA：单分区",
              540, 254, 410);
    }
}

static const char *ota_state_text(hmi_ota_state_t state)
{
    switch (state) {
    case OTA_CONFIRM: return "等待确认";
    case OTA_CHECK:   return "正在校验";
    case OTA_WRITE:   return "正在写入";
    case OTA_READY:   return "升级完成，准备重启";
    case OTA_FAILED:  return "升级失败";
    case OTA_IDLE:
    default:          return "空闲";
    }
}

static void timer(lv_timer_t *timer_obj)
{
    static storage_result_t result;
    static hmi_ota_state_t previous_state = OTA_IDLE;
    static unsigned ready_ticks;
    char text[2048];
    char network[160];
    boot_status_t boot;
    hmi_ota_status_t ota;

    LV_UNUSED(timer_obj);
    boot_manager_ui_heartbeat();
    boot_manager_get(&boot);
    ota_manager_get(&ota);
    network_manager_status(network, sizeof(network));

    if (page != 0) {
        snprintf(text, sizeof(text), "当前槽 %c  下次启动 %c  资源槽 %c  SD 卡 %s",
                 boot.current, boot.next, boot.resource,
                 sd_manager_ready() ? "已就绪" : "未插入");
        lv_label_set_text(top, text);
    }

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
        if (ota.state == OTA_FAILED)
            message(text);
        previous_state = ota.state;
    }
    if (ota.state == OTA_CONFIRM && !remote_dialog) {
        lv_obj_t *button_obj;
        remote_dialog = lv_msgbox_create(NULL);
        lv_msgbox_add_title(remote_dialog, "远程升级确认");
        lv_msgbox_add_text(remote_dialog,
            "电脑工具箱请求安装固件。确认后将写入非活动系统槽和资源槽。\n是否继续？");
        button_obj = lv_msgbox_add_footer_button(remote_dialog, "允许升级");
        lv_obj_add_event_cb(button_obj, remote_answer, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)1);
        button_obj = lv_msgbox_add_footer_button(remote_dialog, "拒绝");
        lv_obj_add_event_cb(button_obj, remote_answer, LV_EVENT_CLICKED, NULL);
        apply_dialog_font(remote_dialog);
    }
    if (ota.state != OTA_CONFIRM && remote_dialog) {
        lv_msgbox_close(remote_dialog);
        remote_dialog = NULL;
    }

    while (storage_manager_poll(&result) == 0) {
        if (result.error) {
            snprintf(text, sizeof(text), "文件操作失败，错误码：%d", result.error);
            message(text);
            continue;
        }
        if (page == 1 && result.op == ST_LIST) {
            files = result.list;
            lv_table_set_row_count(table, files.count + 1);
            lv_table_set_cell_value(table, 0, 0, folder);
            lv_table_set_cell_value(table, 0, 1, "大小");
            for (unsigned i = 0; i < files.count; i++) {
                lv_table_set_cell_value(table, i + 1, 0, files.entries[i].name);
                if (files.entries[i].directory)
                    snprintf(text, sizeof(text), "文件夹");
                else
                    snprintf(text, sizeof(text), "%lu",
                             (unsigned long)files.entries[i].size);
                lv_table_set_cell_value(table, i + 1, 1, text);
            }
            if (details) {
                snprintf(text, sizeof(text), "当前目录：%s，共 %u 项%s",
                         folder, files.count, files.truncated ? "（列表已截断）" : "");
                lv_label_set_text(details, text);
            }
        } else if (page == 1 && result.op == ST_READ) {
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

    if (details) {
        if (page == 6) {
            snprintf(text, sizeof(text),
                     "当前系统槽 %c  下次启动 %c  资源槽 %c  健康状态 %s\n"
                     "网络：%s  SD 卡：%s  运行时间：%lu 秒",
                     boot.current, boot.next, boot.resource,
                     boot.confirmed ? "已确认" : "健康检查中", network,
                     sd_manager_ready() ? "已就绪" : "未插入",
                     (unsigned long)(rt_tick_get() / RT_TICK_PER_SECOND));
            lv_label_set_text(details, text);
        } else if (page == 2) {
            snprintf(text, sizeof(text), "状态：%s | 进度：%u%% | 错误码：%d | 版本：%s",
                     ota_state_text(ota.state), ota.percent, ota.error, ota.version);
            lv_label_set_text(details, text);
            lv_bar_set_value(progress, ota.percent, LV_ANIM_OFF);
        } else if (page == 4) {
            lv_label_set_text(details, network);
        } else if (page == 5) {
            system_manager_logs(text, sizeof(text));
            lv_label_set_text(details, text);
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
    hmi_serial_log("[boot_time] chinese_font_ready=%u ms\n",app_manager_boot_elapsed_ms());
    lv_obj_set_style_text_font(root, body_font(), LV_PART_MAIN);
    if (!app_manager_ready()) {
        label(root, "应用服务初始化失败，请检查串口日志。", 20, 30, 900);
        hmi_serial_log("[hmi] application services are not ready; health confirmation blocked\n");
        return;
    }
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    header_band = lv_obj_create(root);
    lv_obj_set_pos(header_band, 0, 0);
    lv_obj_set_size(header_band, 1024, 90);
    lv_obj_set_style_bg_color(header_band, lv_color_hex(UI_COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_band, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_band, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header_band, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    header_accent = lv_obj_create(root);
    lv_obj_set_pos(header_accent, 0, 88);
    lv_obj_set_size(header_accent, 1024, 2);
    lv_obj_set_style_bg_color(header_accent, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_accent, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header_accent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_accent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    logo_image = lv_image_create(root);
    if (logo_image) {
        lv_image_set_src(logo_image, &hmi_logo_image);
        lv_obj_set_pos(logo_image, 16, 8);
        lv_obj_clear_flag(logo_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    page_title = label(root, "D133-控制终端", 160, 30, 400);
    lv_obj_set_style_text_color(page_title, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    top = label(root, "系统启动中……", 590, 20, 410);
    lv_obj_set_height(top, 50);
    lv_obj_set_style_bg_color(top, lv_color_hex(UI_COLOR_SURFACE_ALT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(top, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_align(top, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(top, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(top, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(top, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(top, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(top, 0, LV_PART_MAIN);
    back_button = button(root, "返回", 16, 21, 88, navigation, (void *)(uintptr_t)0);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(UI_COLOR_SURFACE_ALT), LV_PART_MAIN);
    lv_obj_set_style_text_color(back_button, lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_color(back_button, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(back_button, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(UI_COLOR_SURFACE),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_get_child(back_button, 0))
        lv_obj_set_style_text_color(lv_obj_get_child(back_button, 0),
                                    lv_color_hex(UI_COLOR_ACCENT), LV_PART_MAIN);

    content = lv_obj_create(root);
    lv_obj_set_pos(content, 16, 94);
    lv_obj_set_size(content, 992, 492);
    lv_obj_set_style_bg_color(content, lv_color_hex(UI_COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 0, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    screen(0);
    lv_timer_create(timer, 200, NULL);
    if (lv_display_get_default()) {
        lv_display_add_event_cb(lv_display_get_default(), boot_display_timing,
                                LV_EVENT_RENDER_READY, NULL);
        lv_display_add_event_cb(lv_display_get_default(), boot_display_timing,
                                LV_EVENT_FLUSH_FINISH, NULL);
    }
    hmi_serial_log("[boot_time] hmi_widgets_ready=%u ms\n",app_manager_boot_elapsed_ms());
}
