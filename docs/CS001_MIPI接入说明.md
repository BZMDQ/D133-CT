# CS001 ILI9881C MIPI-DSI 接入说明

## 模组资料

- 模组：WJ101E5TAAUMNG10#
- 显示 IC：ILI9881C-04T00GA
- 接口：4 lane MIPI-DSI，RGB
- 原生分辨率：800×1280（竖屏）
- 触摸：本版仍禁用

这些参数来自随工程提供的 WJ101E5TAAUMNG10# 获批图纸；图纸标出 800×RGB×1280、4-lane MIPI 和 GT9271 触摸 IC。GT9271 软件与触摸接线尚未接入。供应商目录的 SSD2828 示例只用于核对 ILI9881C 初始化资料，D133 直接驱动面板，不包含 SSD2828。

## D133 配置

CS001 仍经 UART0 写入官方 ENV `hmi_screen=CS001` 并重启。panel probe 根据该值选 MIPI connector；CS000 仍为 1024×600 LVDS/GT911。v1.0.38 选择 SDK 的 `panel_dsi_ili9881c.c` 匹配项 `ILI9881C_04_DEFAULT`，该项为 800×1280、4-lane RGB888。SDK 对应的候选时序为像素时钟 95 MHz、H sync/back porch/active/front porch 为 10/200/800/200，V sync/back porch/active/front porch 为 3/12/1280/12。面板复位由该 SDK 驱动指定 PD.14。

屏幕 profile 的 RES 字段使用面板原生方向，返回 `800x1280`。LVGL 布局的竖屏适配尚未完成；现有控制终端页面按横向布局设计，出图后需确认页面排布。严禁把 `1280x800` 作为此模组的原生分辨率。

## 构建与验证

Bootloader/OS 的编译和工厂 IMG 封装不验证 MIPI PHY、FPC lane 顺序、极性、PD.14 复位脚、背光或色彩。首次上板应确认串口 `CS001,READY,RES=800x1280,TOUCH=DISABLED`、面板出图、方向、颜色和冷启动；异常时可从串口切回 CS000。当前未执行实机烧录或 MIPI 验收。
