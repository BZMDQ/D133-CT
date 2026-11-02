# D133-控制终端

工程针对 D133CBV-QFN88-V1-2、D133CBS、ArtInChip Luban-Lite、RT-Thread、16 MiB SPI NOR 和 1024×600 LVDS。板级方案保留官方 demo88-nor 的驱动与 pinmux，在独立目标 d133cbv-hmi-nor 中配置分区和应用。

固件启动后直接进入中文 LVGL 9.1 HMI，不显示旧演示页。启动时钟按 SDK 板级目标配置：CPU 480 MHz、AXI/AHB 200 MHz、APB 100 MHz；SPI NOR 使用 SDK 默认的 QSPI 四线、100 MHz、半周期采样。100 MHz 是当前有官方配置依据的速度基准，板级实际最大稳定值仍需按 NOR 具体型号和实机冷启动验证；没有盲目超频。

旧 v1.0.0 镜像的 SDK 配置仍选择了 `AIC_DISP_COLOR_BLOCK`，所以烧录成功后会看到彩色色块。当前工厂镜像 v1.0.16 关闭 Bootloader 预启动 Logo，OS 黑屏初始化后直接显示中文应用中心，采用 STM32F750 工程的 5×3 图标启动器布局，并显示六个已实现功能；设备总览在独立页面中打开，首页隐藏状态胶囊。

启动优化取消了配套 Bootloader 的预启动黑屏显示；OS 保留 LVDS 预热。Info 日志降为 Warning，并关闭当前 HMI 未使用的 CAN、音频、TSEN、GPAI 外设。HMI 复用一个中文字体实例，并增加首帧时间标记。1 秒目标尚未实机测量；标记不能代表从上电开始的完整时间。详见 [启动优化与实测](docs/启动优化与实测.md)。

本目录的 `source/` 保存新增应用、专用 target/defconfig 与本工程对 SDK 的变更。完整 Luban-Lite 基础源码不包含在本仓库；构建脚本逐文件同步 overlay 到通过 `-SdkRoot` 指定的 SDK 构建树。板级分区配置跟踪在 `source/target/d13x/d133cbv-hmi-nor/pack/`，构建日志和说明在 `docs/`，当前工厂镜像在 `downloads/`，研发工具箱 D133 源码在 `toolbox/Source/`。

```powershell
.\scripts\build.ps1 -Target Bootloader -LogName stage24_bootloader_fast_start_build.log
.\scripts\build.ps1 -Target OS -LogName stage40_startup_header_factory_v1.0.16_build.log
```

先生成配套 Bootloader，再生成 OS/工厂镜像。日常 OTA 不包含 SPL、ENV 或 DATA。生成镜像不代表板级验收；本工程没有自动连接开发板或写 Flash。

文件管理页选中 SD 图片并按“打开 / 读取”后，会裁切铺满屏幕显示，左上角固定“返回”键。JPG/JPEG、PNG、BMP 均由 SDK MPP 解码，在工作线程中原位归一到屏幕 RGB565，不额外申请大块图像缓存；PNG 透明像素按预览底色合成。PNG/BMP 最大支持 1024×768。

- [实施与构建说明](docs/实施说明.md)
- [STM32F750 UI 风格对齐说明](docs/STM32_UI_风格对齐.md)
- [研发工具箱 TCP OTA 协议](docs/工具箱OTA接入说明.md)
- [故障排查](docs/故障排查.md)
- [分区配置](source/target/d13x/d133cbv-hmi-nor/pack/image_cfg.json)
- [ENV 配置](source/target/d13x/d133cbv-hmi-nor/pack/env.txt)
- [OTA 子镜像配置](source/target/d13x/d133cbv-hmi-nor/pack/ota-subimgs.cfg)

当前工厂镜像为 `downloads/d13x_d133cbv-hmi-nor_v1.0.16.img`，大小 4,569,600 字节，SHA-256 见同名 `.sha256` 文件。镜像包含首次部署用配套 SPL/Bootloader，供人工使用 AiBurn；本工程不会自动连接开发板、烧录或擦除 Flash。已签名 OTA v1.0.9 低于该镜像版本，不应安装到 v1.0.16 系统。本仓库只跟踪当前工厂镜像，不提交旧 OTA 包。

## 仓库内容

仓库包含 Luban-Lite 应用 overlay、Windows 构建/OTA 脚本、工程文档、研发工具箱 D133 源码，以及当前工厂镜像。完整 SDK 需另行准备；本仓库不包含 00_原始配置备份、历史固件和预编译的完整研发工具箱。

开发签名私钥不会存入仓库。需要生成 OTA 时，请在本地提供私钥文件，并将私钥保存在 `keys/development_private.pem` 或通过 `sign_ota.ps1 -PrivateKey` 指定路径。当前 OTA 候选配置为 v1.0.17，本仓库未包含已签名的 v1.0.17 OTA 包。

## 从仓库构建

在 Windows PowerShell 中准备 Luban-Lite SDK，然后显式指定 SDK 路径：

```powershell
.\scripts\build.ps1 -Target OS -SdkRoot "D:\path\to\luban-lite-master" -LogName local_factory_build.log
```

构建脚本会先从 `source/` 同步工程 overlay，再由 SDK 官方 SCons 系统编译。镜像输出在 SDK 的 `output/d13x_d133cbv-hmi-nor_rt-thread_d133_hmi/images/`。构建不会烧录开发板。

## 界面页面

首页采用 5×3 应用网格，仅显示六个已实现入口；剩余位置留白。首页不显示设备总览详情和顶部状态胶囊。点击“设备总览”进入单独页面，查看 A/B 槽位、网络、SD 卡、健康状态、运行时间与硬件信息。文件管理、固件升级、系统信息、网络设置和系统日志页面均提供顶部返回键。

v1.0.10 实机串口记录报告启动时间 1.627 秒。本仓库版本加入延迟图片预览初始化、减少首页 LVGL 控件并调整顶部栏；新版实际启动时间尚未测量，需手动部署后复测。CPU/总线与 QSPI 维持已验证配置，不以未经实机验证的超频换取启动速度。
