# v1.0.120 公开候选版：AiBurn 双 OS 槽和单 RODATA 元数据修正

NOR布局：SPL512 KiB，ENV/ENV_R各128 KiB，OS_A/OS_B各5248 KiB，固定RODATA4096 KiB，DATA1024 KiB；总计16 MiB，64 KiB边界对齐。

AiBurn 的 `d13x_os.itb` 组件元数据必须写为 `os;os_r`，单资源必须写为 `rodata`。旧隐藏配置仍携带 `rodata;rodata_r`，会让U-Boot阶段在资源区写入时失败。已同步 `.image_cfg.json.tmp`，并在构建脚本加入分区表一致性检查。已从新IMG逐项读取并核对组件元数据：OS为`os;os_r`，RODATA为`rodata`，不存在`rodata_r`。

OS-only OTA CPIO仅包含 `ota_info.bin` 与 `d13x_os.itb:os`；RODATA只有工厂镜像写入。固件编译和 OTA 白名单、SHA256/RSA 验签通过。旧分区迁移请先备份DATA，AiBurn全片擦除烧录工厂IMG后再恢复DATA。未做目标板实际烧录验收。
