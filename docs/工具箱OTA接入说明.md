# 研发工具箱 OTA 接入核查

用户指定快捷方式：D:\Users\ws2608007\Desktop\研发工具箱.lnk。
实际目标：C:\Users\ws2608007\AppData\Local\Programs\研发工具箱\研发工具箱_0.8.33.exe。
对应构建 spec 指向源码：D:\Users\ws2608007\Desktop\资料整理\PIC24无线烧录\高速流水_1.4.5_0.8.32\工具箱源码\Source。
该源码 APP_VERSION 为 0.8.33，已复制到本工程 toolbox/Source，尚未打包或替换已安装工具箱。

## 已核实的兼容限制

local_tcp_client.py 的 validate_package() 强制检查 F750OTA1、128 字节头和 STM32F750 分区尺寸。
local_tcp_gui.py 的 _upload_ota() 发送整个 STM32 .ota.bin；开始命令 op=2 携带长度与 MD5，数据 op=3 使用 offset 与分块 ACK。
帧结构为小端 <4sIIII，魔数 LNET，字段为 op、offset/status、payload length、CRC32；CRC 覆盖前 16 字节头和 payload。
此机制不能原样用于 ArtInChip ota.cpio。禁止将 CPIO 改名为 .ota.bin 或伪装成 F750OTA1。

## 接入约定

按用户最新指示：D133CBS 作 TCP 服务端，电脑工具箱主动连接并推送。
D133CBS 需要独立的设备类型、CPIO 包入口和能力识别，沿用工具箱连接/进度框架，升级内容通过 SDK 官方 aic-ota 写入。
包先暂存 SD 卡，做型号、硬件版本、CPIO 完整性、版本和 SHA256 检查，再调用 ota_init / ota_shard_download_fun / aic_upgrade_end。
板端实现 RSA-2048/SHA256 签名验证；缺少签名或公钥无法解析时拒绝升级。仅 SHA256 或 CRC32 摘要不能替代固件签名。

## 当前状态

已在原“固件升级”页添加开发板类型选择：默认 STM32F750；选择 D133CBS 后，沿用同页升级设备列表、逐台状态和整体进度，切换回 STM32F750 会恢复旧 `.ota.bin`/App流程。D133CBS 文件选择器要求 `ota.cpio` 与同名 RSA 签名文件。

工具箱 0.8.34 发行目录为 `downloads/研发工具箱_0.8.34/`。已执行隐藏启动检查，确认 D133CBS 入口可加载；检查不打开网络连接。OTA 包由文件选择器从本地选择，当前交付候选包为 v1.0.9。原安装版 0.8.33 与快捷方式均未覆盖。

板端只接入了 TCP server 与编译校验，没有真实板卡完成握手或写入 NOR。首次部署请使用 v1.0.12 工厂镜像。当前已签名 v1.0.9 OTA 低于 v1.0.12；后续 OTA 需生成更高版本候选包。日常 OTA 不含 SPL/Bootloader；TCP 握手和板端写入仍需实机验证。



