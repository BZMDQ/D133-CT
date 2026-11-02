# STM32F750 UI 风格对齐

## 参考来源

参考用户提供的 STM32F750Z8 工程：`高速流水_1.4.5_0.8.32/工程源码/STM32F750Z8_DisplayTouch/MDK-ARM/STM32F750Z8_DisplayTouch.uvprojx`，以及同一工程的 `Src/gui/src/screen1_screen/Screen1View.cpp`。对应 OTA 文件 `STM32F750_1.4.7.ota.bin` 用于确认参考固件版本；不从 OTA 二进制反向提取或修改 UI，也不改动 STM32 工程。

## 设计语言

沿用参考工程的白色背景、浅蓝应用卡片、顶部品牌和状态区、青色细分隔线、低饱和文字与蓝色主操作。D133 改为无常驻侧栏的 5×3 应用启动器；当前只绘制六个已实现功能卡片，其余网格位置留白，不显示占位入口。首页不显示设备总览详情或状态胶囊；点击“设备总览”卡片后查看槽位、健康状态、网络、运行时间和硬件信息。进入子页面后显示顶部返回键。WINSTAR Logo 顺时针旋转 90° 后横向正读；顶栏加高后与青色分隔线分开。

| 用途 | 颜色 |
| --- | --- |
| 白色背景 | `#FFFFFF` |
| 顶部浅色带 | `#F5F9FE` |
| 卡片浅蓝 | `#EAF4FF` |
| 输入框浅蓝 | `#DCECFB` |
| 细边框 | `#DCE4EE` |
| 青色强调 | `#087F91` |
| 蓝色主按钮 | `#2778D4` |
| 主文字 | `#1C2B41` |
| 次要文字 | `#63748A` |

## 页面映射

1. 应用中心只显示 Logo、工程标题和 5×3 网格中的六个功能卡片；首页状态胶囊与设备总览摘要已移除，点击“设备总览”卡片进入独立状态详情页。
2. 文件管理保留路径、目标路径、操作栏、文件表格和文本区，改为 1024 宽屏的大卡片布局。
3. 固件升级保留 SD/TCP 来源说明、签名与升级状态、进度条和 SD 安装/重启按钮。
4. 系统信息和网络设置分别使用设备信息卡和连接配置卡。
5. 系统日志置于可滚动信息卡中；日志来源和后台任务逻辑不变。

所有现有 LVGL 事件、文件操作、OTA 流程、网络配置和 A/B 状态更新仍使用原模块接口。Logo 原图保存在 `source/application/rt-thread/d133_hmi/ui/assets/wslogo.bmp`，`scripts/build.ps1` 会通过 `scripts/convert_hmi_logo.py` 生成 LVGL RGB565 资源，随后由官方 SCons 构建系统编入 OS。图片全屏预览的深色背景与左上角返回键保留，用于保证图片可读性和操作区对比度。

## 修改与验证边界

- D133 页面修改位于 `source/application/rt-thread/d133_hmi/ui/ui_manager.c`。
- 修改前的源码和配置保存在 `00_原始配置/20261102_before_stm32_ui_alignment/`。
- `docs/stm32_ui_alignment.patch` 是可审阅的 unified diff。
- `docs/logo_factory_fw.patch` 记录 Logo 接入；`docs/app_shell_factory.patch` 记录应用启动器和工程命名变更；`docs/app_grid_5x3_factory.patch` 记录 5×3 网格；`docs/home_overview_move.patch` 记录首页总览分离；`docs/boot_speed_header.patch` 记录启动优化、顶栏尺寸与运行时版本宏修复。BMP 与 `.img` 是独立二进制文件。
- 工厂 OS 编译并生成 v1.0.16 镜像，日志为 `stage40_startup_header_factory_v1.0.16_build.log`。SDK 构建会在其 `output/` 目录生成镜像和未签名 `ota.cpio`；工厂镜像复制到 `downloads/` 并生成 SHA-256 清单。本次没有签名或替换日常 OTA 包，也没有写入开发板。
- 编译成功只证明源码和资源能通过当前 Luban-Lite 工具链构建；D133 实机显示、触摸间距和颜色效果尚未验证，需要人工烧录或 OTA 后查看屏幕。
