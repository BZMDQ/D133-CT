/*
 * D133 CS001 面板驱动
 * EK79202D 1280x800 MIPI-DSI 模式，使用标准 DCS 初始化命令。
 * 时序与 lane strap 仍需按模组 FPC 和硬件配置进行实板确认。
 */

#include "panel_com.h"
#include "panel_dsi.h"

static int ek79202d_panel_enable(struct aic_panel *panel)
{
    panel_di_enable(panel, 0);
    panel_dsi_send_perpare(panel);

    /* 芯片资料列出标准 DCS 软件复位、退出休眠和显示打开命令。 */
    panel_dsi_dcs_send_seq(panel, 0x01);
    aic_delay_ms(10);
    panel_dsi_dcs_send_seq(panel, 0x11);
    aic_delay_ms(120);
    panel_dsi_dcs_send_seq(panel, 0x29);
    aic_delay_ms(20);

    panel_dsi_setup_realmode(panel);
    panel_de_timing_enable(panel, 0);
    panel_backlight_enable(panel, 0);
    return 0;
}

static struct aic_panel_funcs ek79202d_panel_funcs = {
    .prepare = panel_default_prepare,
    .enable = ek79202d_panel_enable,
    .disable = panel_default_disable,
    .unprepare = panel_default_unprepare,
    .register_callback = panel_register_callback,
};

static struct display_timing ek79202d_timing = {
    .pixelclock = 72400000,
    .hactive = 1280,
    .hback_porch = 68,
    .hfront_porch = 72,
    .hsync_len = 20,
    .vactive = 800,
    .vback_porch = 21,
    .vfront_porch = 15,
    .vsync_len = 2,
};

static struct panel_dsi ek79202d_dsi = {
    .mode = DSI_MOD_VID_PULSE,
    .format = DSI_FMT_RGB888,
    .lane_num = 4,
};

struct aic_panel dsi_ek79202d = {
    .name = "panel-ek79202d-mipi-1280x800",
    .timings = &ek79202d_timing,
    .funcs = &ek79202d_panel_funcs,
    .dsi = &ek79202d_dsi,
    .connector_type = AIC_MIPI_COM,
};