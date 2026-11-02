# D133-控制终端

工程针对 D133CBV-QFN88-V1-2、D133CBS、ArtInChip Luban-Lite、RT-Thread、16 MiB SPI NOR 和 1024×600 LVDS。板级方案保留官方 demo88-nor 的驱动与 pinmux，在独立目标 d133cbv-hmi-nor 中配置分区和应用。

固件启动后直接进入中文 LVGL 9.1 HMI，不显示旧演示页。启动时钟按 SDK 板级目标配置：CPU 480 MHz、AXI/AHB 200 MHz、APB 100 MHz；SPI NOR 使用 SDK 默认的 QSPI 四线、100 MHz、半周期采样。100 MHz 是当前有官方配置依据的速度基准，板级实际最大稳定值仍需按 NOR 具体型号和实机冷启动验证；没有盲目超频。

旧 v1.0.0 镜像的 SDK 配置仍选择了 `AIC_DISP_COLOR_BLOCK`，所以烧录成功后会看到彩色色块。当前工厂镜像 v1.0.21 关闭 Bootloader 预启动 Logo，OS 黑屏初始化后直接显示中文应用中心，采用 STM32F750 工程的 5×3 图标启动器布局，并显示六个已实现功能；设备总览在独立页面中打开，首页隐藏状态胶囊。

启动优化保留原 QSPI/NOR 时钟和 LVDS 预热；v1.0.21 正式配置改为静默 RT 控制台、关闭 FinSH Shell、RT UART0 驱动和 ULOG 输出，应用串口诊断调用在正式配置编译为空。空输出设备在 RT-Thread 板级驱动初始化前绑定，因此 RT-Thread、驱动、HMI 串口数据静默；BootROM/SPL 输出保持。独立诊断 profile 保留 UART 和逐项计时。v1.0.20 热复位实测 1.632 秒，v1.0.21 尚未上板测量；1 秒目标未达成。详见 [启动优化与实测](docs/启动优化与实测.md)。

本目录的 `source/` 保存新增应用、专用 target/defconfig 与本工程对 SDK 的变更。完整 Luban-Lite 基础源码不包含在本仓库；构建脚本逐文件同步 overlay 到通过 `-SdkRoot` 指定的 SDK 构建树。板级分区配置跟踪在 `source/target/d13x/d133cbv-hmi-nor/pack/`，构建日志和说明在 `docs/`，当前工厂镜像在 `downloads/`。研发工具箱 D133 接入源码在 `toolbox/Source/`，GitHub 源码仓库和手动下载包发布流程在 `toolbox/YF-Tbox/`（`BZMDQ/YF-Tbox`）。

```powershell
.\scripts\build.ps1 -Target Bootloader -LogName stage24_bootloader_fast_start_build.log
.\scripts\build.ps1 -Target OS -LogName stage_quiet_startup_v1.0.21_factory_build.log
.\scripts\build.ps1 -Target OtaCandidate -LogName stage_quiet_startup_v1.0.21_ota_build.log
.\scripts\build.ps1 -Target OtaProfile -LogName stage44_rt_init_profile_v1.0.19_build.log
```

先生成配套 Bootloader，再生成 OS/工厂镜像。日常 OTA 不包含 SPL、ENV 或 DATA。生成镜像不代表板级验收；本工程没有自动连接开发板或写 Flash。

文件管理页选中 SD 图片并按“打开 / 读取”后，会裁切铺满屏幕显示，左上角固定“返回”键。JPG/JPEG、PNG、BMP 均由 SDK MPP 解码，在工作线程中原位归一到屏幕 RGB565，不额外申请大块图像缓存；PNG 透明像素按预览底色合成。PNG/BMP 最大支持 1024×768。

- [实施与构建说明](docs/实施说明.md)
- [STM32F750 UI 风格对齐说明](docs/STM32_UI_风格对齐.md)
- [研发工具箱 TCP OTA 协议](docs/工具箱OTA接入说明.md)
- [故障排查](docs/故障排查.md)
- [v1.0.21 静默启动代码差异](docs/quiet_startup_v1.0.21_source.patch)
- [分区配置](source/target/d13x/d133cbv-hmi-nor/pack/image_cfg.json)
- [ENV 配置](source/target/d13x/d133cbv-hmi-nor/pack/env.txt)
- [OTA 子镜像配置](source/target/d13x/d133cbv-hmi-nor/pack/ota-subimgs.cfg)

当前工厂镜像为 [v1.0.21 IMG](downloads/d13x_d133cbv-hmi-nor_v1.0.21.img)，大小 4,250,112 字节，SHA-256 `d6cacd79cc56b24b3b8c50c2a4154a6abfaef5b8b1d31f9ae2ec4820d2760f4c`。镜像包含首次部署用配套 SPL/Bootloader，供人工使用 AiBurn；本工程不会自动连接开发板、烧录或擦除 Flash。此版本未生成日常 OTA 包。v1.0.21 保留 BootROM/SPL 串口输出，静默 RT-Thread/HMI/驱动/Shell 输出；板载 RTL8723DS 暂未启用，依赖说明见 [无线与蜂鸣器集成状态](docs/无线与蜂鸣器集成.md)。OS ITB 从 v1.0.20 的 1,392,640 字节降到 1,081,344 字节；按原日志吞吐估算读取数据减少约 6–7 ms，实际启动速度仍需同条件实机测量。

## 仓库内容

仓库包含 Luban-Lite 应用 overlay、Windows 构建/OTA 脚本、工程文档、研发工具箱 D133 源码和当前工厂镜像。工具箱 0.8.36 完整便携包、自定义目录安装程序和手动覆盖包位于 `downloads/`；设备消息显示更新尚未重新打包到工具箱 EXE。完整 SDK 需另行准备；本仓库不包含 00_原始配置备份和未发布历史固件。

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

用户提供的 v1.0.20 外部引脚热复位日志为 1.632 秒；同类 v1.0.18 日志为 1.630 秒，整机未见可测提速。v1.0.21 生产配置关闭 RT UART0、FinSH/MSH 和 ULOG 输出，并在 RT-Thread 驱动初始化前绑定静默控制台；BootROM/SPL 串口输出不变。OS ITB 减少 311,296 字节，按原吞吐估计 NOR 读取可少约 6–7 ms；新镜像尚未上板测量。QSPI/NOR 时钟保持不变。

板载蜂鸣器已按 D133CBS 原理图接入 PWM1_A/PE.11，工具箱 LNET 会话建立及 OTA 开始时会记录设备消息并异步请求 1 秒提示音。无线 Wi-Fi 暂未启用：板卡模块为 RTL8723DS，而当前 Luban-Lite WLAN 选择项/预编译库只覆盖 RTL8733、RTL8189 等芯片，没有匹配的 RTL8723DS RT-Thread 驱动和 E907F 库。具体依赖与接入步骤见 [无线与蜂鸣器集成状态](docs/无线与蜂鸣器集成.md)。

