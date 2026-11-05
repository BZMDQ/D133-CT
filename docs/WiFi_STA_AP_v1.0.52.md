# v1.0.52 无线 STA/AP 与看门狗复位处理

本版本按 D133CBS 开发板的 AIC8800D40L SDIO 模块启用 SDMC0，复用 Luban-Lite 的 AIC8800、RT-Thread WLAN 和 lwIP 接口。STA 连接路由器；AP 设置页配置热点。两种接口可同时运行，TCP OTA 仍监听所有 IPv4 接口的 8899 端口，LNET 协议不变。

首次定制部署的 STA 路由器配置只放入本机定制 IMG，不进入 GitHub 公共包。WiFi 配置文件保存在 DATA LittleFS；保存内容为明文，未启用设备侧加密。公开通用镜像需在设备 WiFi 页面输入路由器配置。正常 STA 连接会由 SDK WLAN 配置模块保留最多五组网络；热点设置单独保存。

每个 OS 槽为 3392 KiB，每个资源槽为 3904 KiB，DATA 为 1024 KiB；总分区 16384 KiB，所有分区 64 KiB 对齐。布局改变需要首次手动工厂部署；旧布局设备不能用普通 OTA 跨分区迁移。工厂烧录可能覆盖 ENV 和 DATA，先备份。

本版针对 v1.0.51 复位循环的可见调用路径，在 lwIP 核心锁内读取默认网络接口，并停止在每次 LVGL 定时回调中无锁访问 `netif_default`。串口提供的 `Watchdog-Reset` 与这处并发访问时间相关，尚无足够板端栈信息证明它是唯一触发源。

OS 编译和镜像分区边界检查通过。WiFi SDIO 枚举、STA 关联、STA+AP 共存、DATA LittleFS 挂载、OTA 传输和复位循环修复都必须在板上确认；这次未连接设备或烧录。
