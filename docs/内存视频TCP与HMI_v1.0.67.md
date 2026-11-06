# v1.0.67 内存、视频、TCP 与 HMI 更新

## SRAM 与 PSRAM

目标板按 1 MiB 片内 SRAM、8 MiB PSRAM 配置。RT-Thread 系统堆继续使用片内 SRAM；MPP DMA/CMA 视频帧、BMP 扇区预读缓冲和图片显示缓冲放入 PSRAM CMA，减少大像素块占用系统 SRAM。SDK 板级内存布局未改动。

## 视频首帧与显示

720P MPP 外部解码帧数改为 Luban-Lite 官方播放器示例的配置，解码启动后才开始计算 3 秒无帧超时，避免把文件打开和容器索引时间算入首帧等待。小视频交换帧也使用 PSRAM CMA；图片 BMP 批读缓存和显示输出统一使用 PSRAM CMA。播放卡顿时保留上一帧并在状态标签中区分首帧等待和中途断帧。

## TCP 与设备名称

TCP 仍使用 5000 端口。固件支持 ResearchToolbox 的 `LNET` 认证 `op 0`、设备信息 `op 1`、快速发现 `op 27`、设备改名 `op 26`；设备名称存入 ArtInChip 冗余 ENV，并显示在设备总览和工具箱信息 JSON 中。快速发现 UID 使用 Wi-Fi MAC。

公开包不包含 `YFGY.key`，只接受空 Key，以便查看信息并在板端确认 OTA。需要通用 Key 认证的部署使用本机私有 v1.0.68 构建，命令参数为 `-ToolboxKeyFile`；Key 内容只被读取到临时 SDK 头文件，编译结束后删除，不提交到 GitHub。工具箱需导入同一个 `YFGY.key`。

## Wi-Fi 与文件管理

Wi-Fi 状态增加灰/黄/绿/红指示灯；刷新扫描立即显示状态。已保存网络下拉框显示扫描/保存配置更新，配置最多五组，DATA 未挂载时通过 aic-env 冗余 ENV 备份，DATA 可用时同时写入 `/data/wifi_profiles.bin`。文件管理改成可滚动图标卡片，仍保留选中文件后的原有操作按钮。

## 验收边界

OS 与 OTA 候选均完成 SDK 编译，日志分别为 `stage_full_hmi_v1.0.67_os.log`、`stage_full_hmi_v1.0.67_ota.log`。实际 `.config` 核对为 SRAM 系统堆、8 MiB PSRAM CMA、Wi-Fi STA DHCP 客户端开启，GMAC/CLK_OUT2 和 DHCP 服务端关闭。CPIO 篡改、SPL 映射与尾随数据检查通过，RSA-2048/SHA256 验签通过。

本版交付包含通用 IMG 与签名 OTA；版本、体积和 SHA-256 以 `downloads/SHA256SUMS_current.txt` 为准。OS/OTA 编译日志末尾显示 SCons 构建完成。下载包中的 CPIO 仅含官方 OTA 所需 OS 与 RODATA，不更新 SPL、ENV 或 DATA。

公开工厂 IMG 大小 6,840,832 bytes，SHA-256 为 `f489af0ff4798ad9c53115ec7c645d46dcc48e1ec0511b81d4f52ebdb1089196`；公开 OTA CPIO 大小 6,175,744 bytes，SHA-256 为 `c37982148e53d9ee8c5cc8bc8e4667dc246a7dd79cfc7916abecf7bb0ff2cc3e`。本机私有 Key OTA 包位于 `资料整理/D133-控制终端/内部部署/YFGY_TCP_Key_v1.0.68`，不在公开仓库。

- 工厂 IMG SHA-256：`f489af0ff4798ad9c53115ec7c645d46dcc48e1ec0511b81d4f52ebdb1089196`。
- 通用 OTA CPIO SHA-256：`c37982148e53d9ee8c5cc8bc8e4667dc246a7dd79cfc7916abecf7bb0ff2cc3e`。

未执行真实硬件烧录；需在开发板验证 YFGY.key 私有包认证、TCP 设备信息/改名、Wi-Fi 五组配置保存恢复、BMP/JPEG/PNG 和 720P60 MJPEG 首帧、SD 吞吐与连续播放稳定性。私有 v1.0.68 包单独保存在本机，不上传 GitHub。
