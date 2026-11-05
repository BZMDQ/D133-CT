# v1.0.51 无线 STA/AP 与 A/B 分区

## 无线功能

使用开发板确认的 AIC8800D40L SDIO 模块，SDMC0 使用 PB6–PB11，模块电源控制 PD7；SD 卡继续由 SDMC1 使用。固件复用 SDK AIC8800D40L 驱动、RT-Thread WLAN、lwIP 和 DHCPD，开启 STA `wlan0` 与 AP `wlan1`。TCP OTA 继续监听 8899 的所有本机网络接口，LNET 协议和工具箱设备消息不变。

网络设置页可连接路由器、查看并选择最多五个 SDK 保存的 STA 配置，手动创建/关闭热点。STA/AP 同时工作时 AP 尝试使用已连接 STA 的信道。连接信息和 AP 配置存于 DATA 的 `/data` 文件，未启用加密；需控制设备物理访问。若 DATA 未成功挂载，页面会报告无法持久保存。SSID 和密码不会写入串口日志。

默认路由器账号仅放入本机定制工厂镜像的 `/data/wifi_bootstrap`，公开 GitHub 工厂镜像不含默认路由器密码。定制镜像凭据按明文保存在 DATA，适用于本机部署，不能作为加密凭据方案。

## 新分区布局

分区仍为 16 MiB NOR，总量 16384 KiB，各槽按 64 KiB 对齐。新的 A/B 起始位置改变，必须使用本版本配套工厂镜像和 Bootloader。

| 分区 | 大小 | 偏移 |
|---|---:|---:|
| SPL | 512 KiB | 0x000000 |
| ENV / ENV_R | 各 128 KiB | 0x080000 / 0x0A0000 |
| OS_A / OS_B | 各 3392 KiB | 0x0C0000 / 0x410000 |
| RODATA_A / RODATA_B | 各 3904 KiB | 0x760000 / 0xB30000 |
| DATA | 1024 KiB | 0xF00000 |

本次构建 OS ITB 约 2.73 MiB，资源 FAT 镜像约 3.03 MiB，分别小于新槽位 3.3125 MiB 和 3.8125 MiB。日常 A/B OTA 仍只写非活动 OS/RODATA，不更新 SPL。首次迁移需人工用 AiBurn 烧录工厂 IMG，先备份 DATA 和设备配置。

## 上板验证

构建成功只验证固件链接和分区容量，不代表模块已关联路由器或 STA/AP 共存工作。上板检查：SDIO 枚举、默认 STA 连接、WiFi 页输入/保存五个配置、AP DHCP 地址、STA 与 AP 同时在线、分别经 STA 和 AP 地址向端口 8899 执行工具箱查询及 OTA。
