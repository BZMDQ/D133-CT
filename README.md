# D133-控制终端

工程针对 D133CBV-QFN88-V1-2、D133CBS、ArtInChip Luban-Lite、RT-Thread 和 16 MiB SPI NOR。支持 CS000（1024×600 LVDS/GT911）与 CS001（800×1280 ILI9881C MIPI-DSI，触摸暂禁用）两个启动 profile；板级方案保留官方驱动，在独立 target 中选择显示接口和应用。

固件启动后直接进入中文 LVGL 9.1 HMI，不显示旧演示页。启动时钟按 SDK 板级目标配置：CPU 480 MHz、AXI/AHB 200 MHz、APB 100 MHz；SPI NOR 使用 SDK 默认的 QSPI 四线、100 MHz、半周期采样。100 MHz 是当前有官方配置依据的速度基准，板级实际最大稳定值仍需按 NOR 具体型号和实机冷启动验证；没有盲目超频。

旧 v1.0.0 镜像的 SDK 配置仍选择了 AIC_DISP_COLOR_BLOCK，所以烧录成功后会看到彩色色块。当前工厂镜像 v1.0.39 关闭 Bootloader 预启动 Logo，OS 黑屏初始化后直接显示中文应用中心，采用 STM32F750 工程的 5×3 图标启动器布局；设备总览在独立页面中打开，屏幕检测作为独立入口，首页隐藏状态胶囊。

启动优化保留原 QSPI/NOR 时钟和 LVDS 预热；v1.0.27 继续静默 RT 控制台、FinSH Shell 和 ULOG。UART0 profile 服务启动时发送当前代号，由专用线程轮询接收；CS000 的触摸健康检查按 SDK 的 gt911 设备名匹配。/data 挂载失败和 GT911 名称大小写不再触发 15 秒看门狗循环。v1.0.20 热复位实测 1.632 秒；v1.0.31 尚未上板测量，1 秒目标未达成。

本目录的 `source/` 保存新增应用、专用 target/defconfig 与本工程对 SDK 的变更。完整 Luban-Lite 基础源码不包含在本仓库；构建脚本逐文件同步 overlay 到通过 `-SdkRoot` 指定的 SDK 构建树。板级分区配置跟踪在 `source/target/d13x/d133cbv-hmi-nor/pack/`，构建日志和说明在 `docs/`，当前工厂镜像在 `downloads/`。研发工具箱 D133 接入源码在 `toolbox/Source/`，GitHub 源码仓库和手动下载包发布流程在 `toolbox/YF-Tbox/`（`BZMDQ/YF-Tbox`）。

```powershell
.\scripts\build.ps1 -Target Bootloader -LogName stage_fastboot_v1.0.31_bootloader.log
.\scripts\build.ps1 -Target OS -LogName stage_cs001_backlight_v1.0.39_os.log
.\scripts\build.ps1 -Target OtaCandidate -LogName local_ota_candidate_build.log
.\scripts\build.ps1 -Target OtaProfile -LogName stage44_rt_init_profile_v1.0.19_build.log
```

先生成配套 Bootloader，再生成 OS/工厂镜像。日常 OTA 不包含 SPL、ENV 或 DATA。生成镜像不代表板级验收；本工程没有自动连接开发板或写 Flash。

文件管理页选中 SD 图片并按“打开 / 读取”后，图片按原始像素 1:1 显示：小图居中，大图中心裁去屏幕以外部分；左上角固定“返回”键。BMP 只读取可见区域并申请屏幕大小的 RGB565 输出区，支持 16/24/32 位 BI_RGB 与 16/32 位 BI_BITFIELDS；JPG/JPEG、PNG 使用 SDK MPP 硬件裁剪窗口。JPEG/PNG 压缩输入上限为 4 MiB，PNG 仍遵循 SDK 800 万像素限制，解码硬件与 LVGL 图像描述符上限仍适用。当前只完成编译，需在目标板验证颜色、裁剪和 CMA 余量。

首页新增“屏幕检测”页面，提供全屏白屏划线、色条/灰阶、边框、测试图片和纯色测试。测试画面只由 UP/DOWN 按键切换；左上角“返回”按键返回说明页。触摸只在白屏页绘线。图片只从 `/sdcard/测试图像使用/` 选择与当前活动分辨率完全匹配的可解码 BMP/JPG/JPEG/PNG；缺卡或缺少匹配图时显示“SD没有本机测试图片”。扫描与解码在线程中运行，详细操作见[屏幕检测功能说明](docs/屏幕检测功能说明.md)。

UART0 接收 CS000/CS001 屏幕代号并返回状态；profile 改变时保存到官方 ENV 并重启。CS000 为 1024×600 LVDS/GT911；CS001 为 ILI9881C-04T00GA 800×1280 MIPI-DSI，触摸暂时关闭。启动时串口打印当前代号；时序与硬件验证边界见[屏幕代号串口协议](docs/屏幕代号串口协议.md)和[CS001 MIPI 接入说明](docs/CS001_MIPI接入说明.md)。

- [实施与构建说明](docs/实施说明.md)
- [STM32F750 UI 风格对齐说明](docs/STM32_UI_风格对齐.md)
- [研发工具箱 TCP OTA 协议](docs/工具箱OTA接入说明.md)
- [屏幕代号串口协议](docs/屏幕代号串口协议.md)
- [屏幕检测功能说明](docs/屏幕检测功能说明.md)
- [图片与划线性能优化及板上对比方法](docs/图片与划线性能优化.md)
- [v1.0.31 Bootloader 构建日志](docs/stage_fastboot_v1.0.31_bootloader.log)
- [v1.0.28 屏幕检测 OS 构建日志](docs/stage_screen_test_v1.0.28_os_build.log)
- [v1.0.28 屏幕检测源码差异](docs/screen_test_page_v1.0.28_source.patch)
- [v1.0.29 按键切页与返回键 OS 构建日志](docs/stage_screen_test_buttons_v1.0.29_os_build.log)
- [v1.0.29 按键切页与返回键源码差异](docs/screen_test_buttons_v1.0.29_source.patch)
- [v1.0.30 图片与划线构建日志](docs/stage_speed_v1.0.30_final_os_build.log)
- [v1.0.30 源码差异](docs/speed_v1.0.30_source.patch)
- [故障排查](docs/故障排查.md)
- [v1.0.27 触摸设备名健康检查修复构建日志](docs/stage_uart_config_v1.0.27_build.log)
- [v1.0.27 看门狗健康检查源码差异](docs/v1.0.26_touch_health_source.patch)
- [v1.0.22 图片预览与屏幕串口源码差异](docs/panel_serial_images_v1.0.22_source.patch)
- [v1.0.21 静默启动代码差异](docs/quiet_startup_v1.0.21_source.patch)
- [分区配置](source/target/d13x/d133cbv-hmi-nor/pack/image_cfg.json)
- [ENV 配置](source/target/d13x/d133cbv-hmi-nor/pack/env.txt)
- [OTA 子镜像配置](source/target/d13x/d133cbv-hmi-nor/pack/ota-subimgs.cfg)

当前工厂镜像为 [v1.0.39 IMG](downloads/d13x_d133cbv-hmi-nor_v1.0.39.img)，大小 4,295,168 字节，SHA-256 `653d58bde523d1936169db28dda8c6a5fb5cfbdf63bbd072f52ea4eb5fa5c36b`。镜像包含首次部署用配套 SPL/Bootloader，供人工使用 AiBurn；本工程不会自动连接开发板、烧录或擦除 Flash。此次未生成日常 OTA 包。按键导航、返回键、触摸划线和 SD 图片仍需上板确认。

## 仓库内容

仓库包含 Luban-Lite 应用 overlay、Windows 构建/OTA 脚本、工程文档、研发工具箱 D133 源码和当前工厂镜像。研发工具箱 0.8.37 手动覆盖包已在 [YF-Tbox Release](https://github.com/BZMDQ/YF-Tbox/releases/tag/v0.8.37) 发布；本地副本和校验文件在 `downloads/`。首次安装仍可用 0.8.36 完整便携包/安装程序。完整 SDK 需另行准备；本仓库不包含 00_原始配置备份和未发布历史固件。

开发签名私钥不会存入仓库。需要生成 OTA 时，请在本地提供私钥文件，并将私钥保存在 `keys/development_private.pem` 或通过 `sign_ota.ps1 -PrivateKey` 指定路径。当前跟踪的 v1.0.18 OTA、签名与元数据位于 `downloads/`，Release 页面链接这些仓库文件；当前 Release 使用开发测试密钥，量产前必须替换为受控密钥并完成目标板验收。

早期启动逐项计时使用单独的 v1.0.19 RT_DEBUG_INIT 诊断 OTA，见 [诊断包说明](docs/rt_init_profile_v1.0.19.md) 和 [诊断 Release](https://github.com/BZMDQ/D133-CT/releases/tag/v1.0.19-initdiag)。它会输出大量启动日志并增加启动耗时，仅用于分析初始化瓶颈，不作为生产固件；安装后版本变为 1.0.19，后续正式 OTA 需使用更高版本。

## 从仓库构建

在 Windows PowerShell 中准备 Luban-Lite SDK，然后显式指定 SDK 路径：

```powershell
.\scripts\build.ps1 -Target OS -SdkRoot "D:\path\to\luban-lite-master" -LogName local_factory_build.log
```

构建脚本会先从 `source/` 同步工程 overlay，再由 SDK 官方 SCons 系统编译。镜像输出在 SDK 的 `output/d13x_d133cbv-hmi-nor_rt-thread_d133_hmi/images/`。构建不会烧录开发板。

## 界面页面

首页采用 5×3 应用网格，显示七个已实现入口；剩余位置留白。首页不显示设备总览详情和顶部状态胶囊。点击“设备总览”进入单独页面，查看 A/B 槽位、网络、SD 卡、健康状态、运行时间与硬件信息。点击“屏幕检测”进入说明页，再启动全屏测试；测试页仅由 UP/DOWN 按键切换，并在左上角提供返回键。文件管理、固件升级、系统信息、网络设置和系统日志页面均提供顶部返回键。

用户提供的 v1.0.20 外部引脚热复位日志为 1.632 秒；同类 v1.0.18 日志为 1.630 秒，整机未见可测提速。v1.0.23 保持静默 RT 控制台、FinSH/MSH 和 ULOG 关闭，同时用 UART0 接收代号并输出启动 profile。此次启动耗时尚未上板测量，QSPI/NOR 时钟保持不变。

板载蜂鸣器已按 D133CBS 原理图接入 PWM1_A/PE.11，工具箱 LNET 会话建立及 OTA 开始时会记录设备消息并异步请求 1 秒提示音。无线 Wi-Fi 暂未启用：板卡模块为 RTL8723DS，而当前 Luban-Lite WLAN 选择项/预编译库只覆盖 RTL8733、RTL8189 等芯片，没有匹配的 RTL8723DS RT-Thread 驱动和 E907F 库。具体依赖与接入步骤见 [无线与蜂鸣器集成状态](docs/无线与蜂鸣器集成.md)。

v1.0.27 修复 UART0 配置覆盖：先用 RT_SERIAL_GET_CONFIG 读取 SDK 注册配置，再修改波特率，避免默认模板 uart_index=15 导致打开失败。SDK 编译与板上验收分别记录；硬件结果仍需确认。

v1.0.28 新增全屏屏幕检测和 SD 分辨率精确匹配。v1.0.29 将测试页切换限制为 GPAI2 UP/DOWN 按键，并增加左上角触摸返回键；白屏页触摸仅绘制蓝线。SDK OS 构建和工厂镜像封装已完成，未连接开发板验证。

v1.0.30 将触摸笔迹分为局部线段，减少单次重绘区域；BMP 可视区域按文件偏移正向读取，并在内存中处理倒置行，文件管理与屏幕测试共用。最多保留 64 段笔迹，超出时移除最旧线段。屏幕测试复用已读取的图片头，JPEG/PNG 解码继续使用 SDK MPP；实际打开耗时和显示效果需在板上对比测量。


v1.0.31 在专用 Bootloader 中跳过普通 NOR 开机的 U 盘自动扫描，去除该路径的 1000 ms 连接检查等待。实测提速仍待上板确认。此改动需要人工部署配套工厂 IMG；日常 OS/RODATA OTA 不更新 Bootloader。

- [快速启动说明及验收方法](docs/快速启动_v1.0.31.md)
- [Bootloader 构建日志](docs/stage_fastboot_v1.0.31_bootloader.log)
- [OS 构建日志](docs/stage_fastboot_v1.0.31_os.log)
- [源码差异](docs/fastboot_v1.0.31_source.patch)


v1.0.32 文件管理页采用位置栏、地址栏、工具栏与文件列表布局；BMP 连续完整行分批读取，内存不足时回退逐行读取。JPEG/PNG 继续使用 SDK MPP 解码。界面显示、图片颜色和加速效果仍需在设备上验证。

- [文件管理与图片说明](docs/文件管理_v1.0.32.md)
- [OS 最终构建日志](docs/stage_explorer_v1.0.32_final_os.log)
- [源码差异](docs/explorer_v1.0.32_source.patch)


v1.0.33 加快常见 BMP 像素转换：RGB565 直接复制，RGB555 使用等价位运算，标准 32 位通道顺序采用直接转换。其他掩码沿用通用转换；JPEG/PNG 继续使用 SDK MPP。实际解码时间、颜色和大图内存仍需上板验证。

- [解码优化说明](docs/解码优化_v1.0.33.md)
- [OS 构建日志](docs/stage_decode_v1.0.33_os.log)
- [源码差异](docs/decode_v1.0.33_source.patch)


v1.0.34 为 24 位及标准 32 位 BMP 增加逐行直接转换路径，绕过通用像素格式分派及逐像素 memcpy。其他 BMP 掩码沿用通用转换；JPEG/PNG 仍由 SDK MPP 解码。速度和屏幕颜色需上板测量。

- [BMP 解码优化说明](docs/bmp_decode_v1.0.34.md)
- [OS 构建日志](docs/stage_bmp_decode_v1.0.34_os.log)
- [源码差异](docs/bmp_decode_v1.0.34_source.patch)


v1.0.35 将完整行 BMP 连续读取批次提升至最多 16 行，缓冲上限 128 KiB，分配不足时逐级回退；24/32 位路径保留快速像素转换。速度改善仍需使用实际 SD 卡在板上测量。

- [BMP 批量读取优化说明](docs/bmp_batch_v1.0.35.md)
- [OS 构建日志](docs/stage_bmp_batch_v1.0.35_os.log)
- [源码差异](docs/bmp_batch_v1.0.35_source.patch)


v1.0.36 屏幕检测白屏页通过 GT911 track ID 接收最多五个触点，为每根手指分别绘线。多指输入数据由现有触摸线程采集，LVGL 线程读取快照并更新笔迹。多指效果仍需 GT911 屏幕实机验证。

- [多指划线说明](docs/多指划线_v1.0.36.md)
- [OS 构建日志](docs/stage_multitouch_v1.0.36_os.log)
- [源码差异](docs/multitouch_v1.0.36_source.patch)


v1.0.37 为屏幕检测五个 GT911 触点 ID 分配蓝、红、绿、橙、紫五种固定笔迹颜色。多指显示效果仍需板上验证。

- [v1.0.38 CS001 ILI9881C 构建日志](docs/stage_cs001_backlight_v1.0.39_os.log)
- [源码差异](docs/finger_colors_v1.0.37_source.patch)


v1.0.38 将 CS001 切换为 SDK ILI9881C-04 面板配置，按 WJ101E5 图纸设置为 800×1280 四通道 MIPI。屏幕控制器初始化由 SDK 面板驱动提供，lane/极性、RESET 和出图仍待实板确认。

- [CS001 ILI9881C MIPI 接入说明](docs/CS001_MIPI接入说明.md)
- [v1.0.38 OS 构建日志](docs/stage_cs001_backlight_v1.0.39_os.log)


v1.0.38 将 CS001 profile 更正为 WJ101E5TAAUMNG10# / ILI9881C-04T00GA，原生分辨率 800×1280，使用 SDK ILI9881C_04_DEFAULT 四通道 MIPI 驱动。panel 选择配置和面板时序已在主机侧构建；MIPI 出图和竖屏界面适配仍需上板确认。

CS001 原 MIPI 面板型号已更正为 WJ101E5TAAUMNG10# / ILI9881C-04T00GA。v1.0.38 使用 SDK ILI9881C_04_DEFAULT 和 800×1280 四通道 MIPI 配置；其驱动、FPC lane/极性、RESET、出图和竖屏 UI 仍需实板确认。

- [CS001 ILI9881C panel and mode build info](docs/CS001_MIPI接入说明.md)


v1.0.39 在 ILI9881C-04 初始化命令前打开 PE.13 背光使能，减少 DSI 初始化异常时黑屏难以区分的情况。VLED 背光供电仍需确认模组与载板实际连接。

CS001 ILI9881C 面板 v1.0.39 在 DSI 初始化前打开 PE.13 背光使能，初始化后再次确认使能。若仍无背光，应核实模组 VLED+/VLED− 供电和载板 LED 电流驱动；软件不能替代缺失的背光电源。

- [CS001 背光说明和检查步骤](docs/CS001_backlight_v1.0.39.md)
- [v1.0.39 OS 构建日志](docs/stage_cs001_backlight_v1.0.39_os.log)

- [v1.0.39 ILI9881C 背光使能源码差异](docs/cs001_backlight_v1.0.39_source.patch)
