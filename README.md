# D133-控制终端

工程针对 D133CBV-QFN88-V1-2、D133CBS、ArtInChip Luban-Lite、RT-Thread、16 MiB SPI NOR 和 1024×600 LVDS。板级方案保留官方 demo88-nor 的驱动与 pinmux，在独立目标 d133cbv-hmi-nor 中配置分区和应用。

固件启动后直接进入中文 LVGL 9.1 HMI，不显示旧演示页。启动时钟按 SDK 板级目标配置：CPU 480 MHz、AXI/AHB 200 MHz、APB 100 MHz；SPI NOR 使用 SDK 默认的 QSPI 四线、100 MHz、半周期采样。100 MHz 是当前有官方配置依据的速度基准，板级实际最大稳定值仍需按 NOR 具体型号和实机冷启动验证；没有盲目超频。

旧 v1.0.0 镜像的 SDK 配置仍选择了 `AIC_DISP_COLOR_BLOCK`，所以烧录成功后会看到彩色色块。当前工厂镜像 v1.0.18 关闭 Bootloader 预启动 Logo，OS 黑屏初始化后直接显示中文应用中心，采用 STM32F750 工程的 5×3 图标启动器布局，并显示六个已实现功能；设备总览在独立页面中打开，首页隐藏状态胶囊。

启动优化取消了配套 Bootloader 的预启动黑屏显示；OS 保留 LVDS 预热。Info 日志降为 Warning，并关闭当前 HMI 未使用的 CAN、音频、TSEN、GPAI 外设。HMI 复用一个中文字体实例，并增加首帧时间标记。1 秒目标尚未实机测量；标记不能代表从上电开始的完整时间。详见 [启动优化与实测](docs/启动优化与实测.md)。

本目录的 `source/` 保存新增应用、专用 target/defconfig 与本工程对 SDK 的变更。完整 Luban-Lite 基础源码不包含在本仓库；构建脚本逐文件同步 overlay 到通过 `-SdkRoot` 指定的 SDK 构建树。板级分区配置跟踪在 `source/target/d13x/d133cbv-hmi-nor/pack/`，构建日志和说明在 `docs/`，当前工厂镜像在 `downloads/`。研发工具箱 0.8.35 源码在 `toolbox/Source/`，GitHub 源码仓库和更新发布流程在 `toolbox/YF-Tbox/`（`BZMDQ/YF-Tbox`）。

```powershell
.\scripts\build.ps1 -Target Bootloader -LogName stage24_bootloader_fast_start_build.log
.\scripts\build.ps1 -Target OS -LogName stage43_factory_v1.0.18_build.log
.\scripts\build.ps1 -Target OtaCandidate -LogName stage42_ota_v1.0.18_build.log
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

当前工厂镜像为 `downloads/d13x_d133cbv-hmi-nor_v1.0.18.img`，大小 4,553,216 字节，SHA-256 `41dd77b2ca7df40340d66214c84abc11ea0bc68ced3d6ddf504963df302b812d`。镜像包含首次部署用配套 SPL/Bootloader，供人工使用 AiBurn；本工程不会自动连接开发板、烧录或擦除 Flash。针对 v1.0.16/v1.0.17 的日常升级包 v1.0.18 已在 [GitHub Release](https://github.com/BZMDQ/D133-CT/releases/tag/v1.0.18) 发布，页面提供工厂镜像、OTA CPIO、签名和校验文件的仓库下载链接。OTA 包通过主机侧 CPIO 白名单、版本、SHA-256 与 RSA 签名验证；尚未完成板上升级与回滚验收。该包使用本地开发测试密钥签名，不用于量产。工厂镜像用于首次部署，OTA CPIO 用于日常升级，两者不可混用。

## 仓库内容

仓库包含 Luban-Lite 应用 overlay、Windows 构建/OTA 脚本、工程文档、研发工具箱 D133 源码、当前工厂镜像和 v1.0.18 OTA 包及签名。工具箱的完整 0.8.35 便携包、自定义目录安装程序和 GitHub 原位更新包位于 `downloads/`。完整 SDK 需另行准备；本仓库不包含 00_原始配置备份和未发布历史固件。

开发签名私钥不会存入仓库。需要生成 OTA 时，请在本地提供私钥文件，并将私钥保存在 `keys/development_private.pem` 或通过 `sign_ota.ps1 -PrivateKey` 指定路径。当前跟踪的 v1.0.18 OTA、签名与元数据位于 `downloads/`，Release 页面链接这些仓库文件；当前 Release 使用开发测试密钥，量产前必须替换为受控密钥并完成目标板验收。

## 从仓库构建

在 Windows PowerShell 中准备 Luban-Lite SDK，然后显式指定 SDK 路径：

```powershell
.\scripts\build.ps1 -Target OS -SdkRoot "D:\path\to\luban-lite-master" -LogName local_factory_build.log
```

构建脚本会先从 `source/` 同步工程 overlay，再由 SDK 官方 SCons 系统编译。镜像输出在 SDK 的 `output/d13x_d133cbv-hmi-nor_rt-thread_d133_hmi/images/`。构建不会烧录开发板。

## 界面页面

首页采用 5×3 应用网格，仅显示六个已实现入口；剩余位置留白。首页不显示设备总览详情和顶部状态胶囊。点击“设备总览”进入单独页面，查看 A/B 槽位、网络、SD 卡、健康状态、运行时间与硬件信息。文件管理、固件升级、系统信息、网络设置和系统日志页面均提供顶部返回键。

用户提供的 v1.0.16 实机串口记录报告 `Startup time: 1.625 sec`，SPL 读取 1,398,124 字节耗时 29,018 μs；显示标记为 `hmi_widgets_ready=131 ms`、`display_flush_return=221 ms`、`lvgl_render_ready=224 ms`。v1.0.18 延迟创建输入键盘、关闭未使用的驱动测试命令、缩短图片结果轮询并批量读取 BMP 行数据；新版本启动和图片打开时间仍待上板复测。CPU/总线与 QSPI 维持原配置，不调整 NOR 读取时钟。
