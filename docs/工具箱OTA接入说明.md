# 研发工具箱 0.8.36 手动更新与 D133 OTA

## 工具箱更新

0.8.36 源码位于 `toolbox/Source/`，已同步到 GitHub 仓库 [`BZMDQ/YF-Tbox`](https://github.com/BZMDQ/YF-Tbox)。仓库包含 Windows x86 构建脚本和手动 Release 工作流。工具箱界面不提供自动更新入口。

完整更新文件位于 `downloads/`：

- `研发工具箱_0.8.36_Onedir_Portable_Windows.zip`：完整便携目录，包含 CubeProgrammer、ST-LINK 驱动、许可证和源码。
- `研发工具箱_0.8.36_安装程序.exe`：安装程序，支持自定义安装目录并可创建桌面快捷方式。
- `研发工具箱_0.8.36_Manual_Overlay.zip`：从 GitHub Release 手动下载的应用文件覆盖包；不含 CubeProgrammer、驱动、许可证或用户配置。
- 三个下载文件各有同名 `.sha256` 校验文件。

新版本发布：修改 `toolbox/YF-Tbox/Source/local_tcp_gui.py` 中的 `APP_VERSION`，提交到 `main`。Actions 使用 Python 3.8 x86 生成手动覆盖 ZIP 并发布 Release。升级时由用户自行下载 Release 包并覆盖，或运行完整安装程序；工具箱不会联网检查、下载或安装更新。

## D133CBS TCP OTA 约束

STM32F750 `.ota.bin` 协议与 ArtInChip `ota.cpio` 不兼容，不能把 CPIO 改名或伪装成 F750 包。工具箱在原“固件升级”页面提供独立的 D133CBS 设备类型和 CPIO 入口，电脑作为 TCP 客户端连接开发板 8899 端口并推送；板端使用 SDK `aic-ota` 写入。

板端先将包暂存到 SD 卡，检查型号、硬件版本、CPIO 完整性、版本和 SHA-256，再执行 `ota_init` / `ota_shard_download_fun` / `aic_upgrade_end`。固件还需 RSA-2048/SHA256 签名校验，不能用 CRC32 或 SHA256 摘要代替签名。

工具箱消息仍使用 LNET 20 字节帧和 CRC32。设备信息响应增加可忽略的 `event_id`、`event`、`device_message` JSON 字段；接受 `BEGIN` 后返回设备消息 JSON。旧客户端忽略这些应答字段仍可继续传输，新版工具箱把设备消息写入升级状态和日志。连接提示仅在每条有效 LNET 会话首次 `INFO` 时触发；升级开始时再次触发。新版健康轮询使用 `0xD105`，不重复触发连接提示，旧固件不识别时回退 `0xD100`。

TCP 设备消息会记录在设备系统日志页；工具箱连接消息和 OTA 开始消息均会请求板载 PWM 蜂鸣器鸣响 1 秒。板级原理图将蜂鸣器接到 PE.11/PWM1_A，工程启用 SDK PWM1 驱动，以 4 kHz、50% 占空比输出。

## 当前设备验证边界

D133 工具箱界面和主机端包校验已集成。GitHub Release 构建完成后，更新包的发布状态以 Actions 结果为准。板端 TCP 握手、NOR 写入、重启、回滚与健康状态仍需在真实开发板上验证；不能用主机端构建或 ZIP 校验代替板级验证。日常 OTA 不包含 SPL、Bootloader、ENV 或 DATA。

