# D133-控制终端

工程针对 D133CBV-QFN88-V1-2、D133CBS、ArtInChip Luban-Lite、RT-Thread 和 16 MiB SPI NOR。支持 CS000（1024×600 LVDS/GT911）与 CS001（1280×800 MIPI-DSI，触摸暂禁用）两个启动 profile；板级方案保留官方驱动，在独立 target 中选择显示接口和应用。

固件启动后直接进入中文 LVGL 9.1 HMI，不显示旧演示页。启动时钟按 SDK 板级目标配置：CPU 480 MHz、AXI/AHB 200 MHz、APB 100 MHz；SPI NOR 使用 SDK 默认的 QSPI 四线、100 MHz、半周期采样。100 MHz 是当前有官方配置依据的速度基准，板级实际最大稳定值仍需按 NOR 具体型号和实机冷启动验证；没有盲目超频。

旧 v1.0.0 镜像的 SDK 配置仍选择了 AIC_DISP_COLOR_BLOCK，所以烧录成功后会看到彩色色块。当前工厂镜像 v1.0.26 关闭 Bootloader 预启动 Logo，OS 黑屏初始化后直接显示中文应用中心，采用 STM32F750 工程的 5×3 图标启动器布局，并显示六个已实现功能；设备总览在独立页面中打开，首页隐藏状态胶囊。

启动优化保留原 QSPI/NOR 时钟和 LVDS 预热；v1.0.26 继续静默 RT 控制台、FinSH Shell 和 ULOG。UART0 profile 服务启动时发送当前代号，由专用线程轮询接收；CS000 的触摸健康检查按 SDK 的 gt911 设备名匹配。/data 挂载失败和 GT911 名称大小写不再触发 15 秒看门狗循环。v1.0.20 热复位实测 1.632 秒；v1.0.26 尚未上板测量，1 秒目标未达成。

本目录的 `source/` 保存新增应用、专用 target/defconfig 与本工程对 SDK 的变更。完整 Luban-Lite 基础源码不包含在本仓库；构建脚本逐文件同步 overlay 到通过 `-SdkRoot` 指定的 SDK 构建树。板级分区配置跟踪在 `source/target/d13x/d133cbv-hmi-nor/pack/`，构建日志和说明在 `docs/`，当前工厂镜像在 `downloads/`。研发工具箱 D133 接入源码在 `toolbox/Source/`，GitHub 源码仓库和手动下载包发布流程在 `toolbox/YF-Tbox/`（`BZMDQ/YF-Tbox`）。

```powershell
.\scripts\build.ps1 -Target Bootloader -LogName stage24_bootloader_fast_start_build.log
.\scripts\build.ps1 -Target OS -LogName stage_touch_device_health_v1.0.26_build.log
.\scripts\build.ps1 -Target OtaCandidate -LogName stage_touch_device_health_v1.0.26_ota_build.log
.\scripts\build.ps1 -Target OtaProfile -LogName stage44_rt_init_profile_v1.0.19_build.log
```

先生成配套 Bootloader，再生成 OS/工厂镜像。日常 OTA 不包含 SPL、ENV 或 DATA。生成镜像不代表板级验收；本工程没有自动连接开发板或写 Flash。

文件管理页选中 SD 图片并按“打开 / 读取”后，图片按原始像素 1:1 显示：小图居中，大图中心裁去屏幕以外部分；左上角固定“返回”键。BMP 只读取可见区域并申请屏幕大小的 RGB565 输出区，支持 16/24/32 位 BI_RGB 与 16/32 位 BI_BITFIELDS；JPG/JPEG、PNG 使用 SDK MPP 硬件裁剪窗口。JPEG/PNG 压缩输入上限为 4 MiB，PNG 仍遵循 SDK 800 万像素限制，解码硬件与 LVGL 图像描述符上限仍适用。当前只完成编译，需在目标板验证颜色、裁剪和 CMA 余量。

UART0 接收 CS000/CS001 屏幕代号并返回状态；profile 改变时保存到官方 ENV 并重启。CS000 为 1024×600 LVDS/GT911；CS001 为 EK79202DA 1280×800 MIPI-DSI，触摸暂时关闭。启动时串口打印当前代号；时序与硬件验证边界见[屏幕代号串口协议](docs/屏幕代号串口协议.md)和[CS001 MIPI 接入说明](docs/CS001_MIPI接入说明.md)。

- [实施与构建说明](docs/实施说明.md)
- [STM32F750 UI 风格对齐说明](docs/STM32_UI_风格对齐.md)
- [研发工具箱 TCP OTA 协议](docs/工具箱OTA接入说明.md)
- [屏幕代号串口协议](docs/屏幕代号串口协议.md)
- [故障排查](docs/故障排查.md)
- [v1.0.26 触摸设备名健康检查修复构建日志](docs/stage_touch_device_health_v1.0.26_build.log)
- [v1.0.26 看门狗健康检查源码差异](docs/v1.0.26_touch_health_source.patch)
- [v1.0.22 图片预览与屏幕串口源码差异](docs/panel_serial_images_v1.0.22_source.patch)
- [v1.0.21 静默启动代码差异](docs/quiet_startup_v1.0.21_source.patch)
- [分区配置](source/target/d13x/d133cbv-hmi-nor/pack/image_cfg.json)
- [ENV 配置](source/target/d13x/d133cbv-hmi-nor/pack/env.txt)
- [OTA 子镜像配置](source/target/d13x/d133cbv-hmi-nor/pack/ota-subimgs.cfg)

当前工厂镜像为 [v1.0.26 IMG](downloads/d13x_d133cbv-hmi-nor_v1.0.26.img)，大小 4,270,592 字节，SHA-256 2946ce672651c73b1b287f0b2715fe0acbb59fb0d827917e77194bf9acde88ac。镜像包含首次部署用配套 SPL/Bootloader，供人工使用 AiBurn；本工程不会自动连接开发板、烧录或擦除 Flash。未生成日常 OTA 包。/data 持久化与 15 秒复位停止仍待板上验证。

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

首页采用 5×3 应用网格，仅显示六个已实现入口；剩余位置留白。首页不显示设备总览详情和顶部状态胶囊。点击“设备总览”进入单独页面，查看 A/B 槽位、网络、SD 卡、健康状态、运行时间与硬件信息。文件管理、固件升级、系统信息、网络设置和系统日志页面均提供顶部返回键。

用户提供的 v1.0.20 外部引脚热复位日志为 1.632 秒；同类 v1.0.18 日志为 1.630 秒，整机未见可测提速。v1.0.23 保持静默 RT 控制台、FinSH/MSH 和 ULOG 关闭，同时用 UART0 接收代号并输出启动 profile。此次启动耗时尚未上板测量，QSPI/NOR 时钟保持不变。

板载蜂鸣器已按 D133CBS 原理图接入 PWM1_A/PE.11，工具箱 LNET 会话建立及 OTA 开始时会记录设备消息并异步请求 1 秒提示音。无线 Wi-Fi 暂未启用：板卡模块为 RTL8723DS，而当前 Luban-Lite WLAN 选择项/预编译库只覆盖 RTL8733、RTL8189 等芯片，没有匹配的 RTL8723DS RT-Thread 驱动和 E907F 库。具体依赖与接入步骤见 [无线与蜂鸣器集成状态](docs/无线与蜂鸣器集成.md)。
