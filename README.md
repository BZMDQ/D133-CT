公开候选 v1.0.130：支持单资源分区、ENV必写迁移、SPL启动变量修复、OTA直写与签名版本回刷。[工厂IMG](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/D133-CT_factory_v1.0.130.zip)、[OS-only OTA](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/D133-CT_OTA_v1.0.130.zip)、[研发工具箱0.8.38](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/ResearchToolbox_0.8.38.zip)、[SHA-256校验文件](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/SHA256SUMS_v1.0.130.txt)、[工具箱更新说明](docs/研发工具箱_0.8.38_更新说明.md)、[四项变更](docs/候选版本_v1.0.130_四项.md)。本机私有固件仅存内部部署。真实烧录和A/B启动尚未验收。

基于 ArtInChip Luban-Lite 1.3.2、RT-Thread 和 LVGL 9.1，目标板 D133CBV-QFN88-V1-2 / D133CBS，16 MiB SPI NOR、1 MiB SRAM 与 8 MiB PSRAM。

## 当前候选资源

- [v1.0.130 首次部署工厂 IMG 包](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/D133-CT_factory_v1.0.130.zip)
- [v1.0.130 签名 OS-only OTA 包](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/D133-CT_OTA_v1.0.130.zip)
- [研发工具箱 0.8.38 完整包](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/ResearchToolbox_0.8.38.zip)（[SHA-256](https://github.com/BZMDQ/D133-CT/releases/download/v1.0.130/SHA256SUMS_v1.0.130.txt)）
- [直写协议、阶段计时及验证边界](docs/TCP直写与阶段耗时_v1.0.113.md)

公开包不包含私有工具箱 Key 或默认 WiFi 密码。本机定制版为 v1.0.112，仅保存在本地内部部署目录。当前资源属于候选版本，真实 Flash 写入、掉电恢复及 A/B 回滚未验收。

## 功能

中文应用卡片首页、文件图标管理、图片居中与裁剪、自动视频播放及只读进度条、屏幕显示/多指划线检测、WiFi STA 网络配置与最多五组保存网络、TCP 5000 系统信息和 OTA、设备重命名、NTP 自动校时。当前不提供 AP 或以太网配置。

视频只支持经测试流程准备的无音频 MJPEG 封装，不支持常见 MP4/H.264 硬解。小视频维持源尺寸，大视频采用半尺寸硬件解码并铺满屏幕；比例不同时会拉伸。已移除原始画质选项。连续播放和重复打开仍需目标板验收，不能保证 720P 60 fps。

BMP 使用扇区对齐、双缓冲预读和通用像素格式转换，24/32位常见格式加入四像素展开。主机像素窗口验证通过，不代表实际 SD 打开速度已测量。

## TCP OTA 使用

首次请手动使用 AiBurn 烧录配套工厂 IMG；USB BROM 工厂恢复保留，禁止把日常 OTA 当成首次部署镜像。日常 OTA 不包含 SPL、DATA 或 ENV 镜像。

手动退出研发工具箱后，以 PowerShell 7 运行补丁包中的 `安装补丁.ps1`，再重新打开工具箱。补丁仅适用于其 SHA256 检查匹配的已核对模块，先备份再替换 D133 客户端，不修改其它开发板。工具箱实际加载根目录 `.pyc`，只编辑 Source 中的 `.py` 不会生效。

工具箱选择 D133CBS 和设备 IP、端口 5000，选择 `.cpio`，同名 `.cpio.sig` 必须放在一起。新协议先发送签名摘要，设备预检后要求屏幕确认；随后调用官方 aic-ota 直接写非活动 OS 槽，接收结束校验完整性后通过官方 aic-env 提交切换。A 运行写 B，B 运行写 A。失败不激活未完成槽。设备允许安装签名有效、版本相同或更低的 OS-only 包，以支持重装和回刷；分区布局和资源槽不随日常 OTA 改变，回刷包须与当前分区布局兼容。

开发板显示目标槽、进度、下载速度、签名/确认/接收写入/校验/提交耗时及总耗时。工具箱另外显示连接、重启及健康确认耗时。接收与写入合并计时，不重复累加。

旧暂存协议在新固件中被明确拒绝；新客户端对旧固件也会提示不支持直写，不会静默回退到 SD 暂存。

## 源码与构建

`source/` 是需要同步到原 SDK 的实际源码覆盖层，包含应用、目标板配置以及必要的 MPP 中间件修改；不包含完整 SDK、工具链或私钥。构建脚本使用 SDK 官方 SCons、镜像和 OTA 打包工具。

```powershell
# 请使用 PowerShell 7，显式指定本机原 SDK 路径。
.\scripts\build.ps1 -Target OS -SdkRoot 'D:\path\to\luban-lite-master' -LogName local_os.log
.\scripts\build.ps1 -Target Bootloader -SdkRoot 'D:\path\to\luban-lite-master' -LogName local_bootloader.log
```

`OtaCandidate` 可构建独立 OTA profile；工厂 OS 构建也由官方工具生成同版 `ota.cpio`。版本化包经 `prepare_ota.py` 修复官方打包元数据 CRC 并预检，再由 `sign_ota.ps1 -PrivateKey <本机私钥>` 离线签名。量产需更换开发公钥及受控私钥，私钥不得进入仓库或固件。

本机定制可传入 `-ToolboxKeyFile` 和 `-WifiBootstrapFile`，后者为 SSID、密码各一行。临时私有头文件只在 SDK 构建期间存在，结束时清理。私有镜像禁止上传公开仓库。

## 验证与故障定位

当前已完成 SDK OS 编译、CPIO 白名单/破坏数据/SPL 映射拒绝、RSA 验签，以及实际编译 Python3.8 客户端在模拟设备下的正常提交、旧能力拒绝和错误偏移不提交测试。C 流式校验器已交叉编译，未进行本机原生 C 运行测试或真实 Flash 写入验收。

旧协议的两次获准 SD 暂存接收诊断（不发送提交或重启）分别在不同进度复现超时，另一状态连接持续可用。这个结果不等于新直写协议已通过实机验收。

视频超时时会显示 SDK 返回码、帧数以及文件读取快照。`SDK=3` 表示没有可解码的数据包；若重复打开后复现，请保留完整 I/O 请求/返回/errno 信息。[历史故障排查](docs/故障排查.md)和 `docs/` 中旧版说明仅供追溯，协议与资源以本页当前版本为准。

