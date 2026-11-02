# CS001 MIPI-DSI 接入说明

## 适用对象

- 板：D133CBV-QFN88-V1-2 / D133CBS
- 面板控制芯片：EK79202D（模组料号 ICEK7920321XXXXXXX01）
- 接口：MIPI-DSI
- profile：CS001
- 应用分辨率：1280×800
- 触摸：当前禁用

## 工程实现

v1.0.25 保留 CS000 的 1024×600 LVDS/GT911，同时在同一个固件中编译 LVDS 与 MIPI-DSI 控制器。aicfb_probe() 前读取官方 ENV 键 hmi_screen，按 profile 选择 connector 和显示引脚复用：

- CS000：LVDS connector，PD18–PD27 function 3，ArtInChip simple 1024×600 panel。
- CS001：MIPI-DSI connector，PD18–PD27 function 4，EK79202D 1280×800 panel。
- 屏幕代号通过 UART0 选择，ENV 写入后软件重启应用新接口。

CS001 的候选显示参数为 72.4 MHz、4-lane RGB888、DSI Video Pulse，H timing 为 20/68/1280/72，V timing 为 2/21/800/15。面板仅发送 PDF 列出的标准 DCS 软件复位、退出休眠和显示打开命令。

## 资料限制和实机检查

PDF 的正文是 EK79202D 芯片 datasheet，不是该模组的连接器/硬件 strap 清单。资料没有确认：

- 模组 IFSEL 是否设为 MIPI 4-lane，以及 LNSW/PNSW 实际 strap 电阻。
- 模组 FPC 中 D0–D3、clock lane 与 D133 PD18–PD27 的映射、极性。
- 模组电源、RESET、背光具体控制脚和时序。

当前驱动采用 SDK 默认 DSI lane mapping/polarity。上板前应向模组供应方确认上述连接；首次验证时确认 CS001,READY、MIPI clock/data lane 活动、1280×800 图像、颜色/方向和背光。若没有出图，不要通过盲目改换 LVDS 参数排查，因为 CS001 物理接口是 MIPI。该 build 只证明固件可以编译和打包，不代表 MIPI PHY/模组已在硬件验证。

用户已要求 CS001 暂不启动触摸输入。ATMXT1066TD 驱动和配置仍未接入。