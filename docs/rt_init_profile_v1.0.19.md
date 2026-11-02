# v1.0.19 RT-Thread 早期初始化诊断包

## 用途

用户的 v1.0.18 日志显示 `Startup time` 约 1.63 秒，而 HMI 的 `display_flush_return` 约 219 毫秒。较大的耗时位于 HMI 标记之前。此诊断 profile 启用 SDK 自带 `RT_DEBUG_INIT_CONFIG`，并为 board init、系统 timer、scheduler 和 init thread 打阶段标记；RT-Thread 同时输出各个 `INIT_BOARD/DEVICE/COMPONENT/FS/ENV/APP` 初始化函数的耗时。

profile 还会输出较多 UART 日志，因此整机 `Startup time` 会被日志开销影响。请用每个初始化函数的 `cost` 排序来定位慢点，不要把诊断固件的整机时间当作性能基准。

## 编译与校验

Windows PowerShell 中运行：

```powershell
.\scripts\build.ps1 -Target OtaProfile -LogName stage44_rt_init_profile_v1.0.19_build.log
```

脚本应用 `d13x_d133cbv-hmi-nor_rt-thread_d133_hmi_ota_profile_defconfig`，并确认 `CONFIG_RT_DEBUG_INIT_CONFIG=y`。诊断 OTA 由官方 `ota-subimgs.cfg` 生成，只含 `ota_info.bin`、OS 和 RODATA，不含 SPL、ENV 或 DATA。随后使用 `prepare_ota.py`、`sign_ota.ps1`、`verify_ota_host.py` 和 `verify_ota_signature.ps1` 生成及验证版本化 CPIO。

诊断 OTA CPIO 为 3,931,136 字节，SHA-256：`ff783f19bb476509175ddb0fa962732368e9b0c760302dbf526971d4e1ca8c0b`；RSA 签名为 256 字节，SHA-256：`d482e01b1d3140b366c179e4ce71446e84286a72779eaf92e29c12b1a0f594cb`。下载文件和校验清单见 [v1.0.19 RT 初始化诊断预发布](https://github.com/BZMDQ/D133-CT/releases/tag/v1.0.19-initdiag)。

## 手动部署与日志采集

v1.0.19 诊断 OTA 可从 v1.0.18 安装。使用 SD 或 TCP OTA 时，按正式包流程选择 CPIO 和同名 `.sig`，并在设备屏幕确认。完成重启后保存从上电到 `Startup time` 后各初始化函数日志，优先整理 `cost` 最大的项目以及 `[RT_INIT_STAGE]` 结果。

OTA 版本保护不允许回退到 v1.0.18；诊断包安装后，后续正式升级必须使用高于 v1.0.19 的版本。诊断包使用开发测试签名密钥，不用于量产。此次没有自动连接设备、烧录或写入 Flash。
