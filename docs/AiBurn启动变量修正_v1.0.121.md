# v1.0.121 AiBurn 启动变量修正版

16 MiB 分区：SPL 512 KiB，ENV/ENV_R 各 128 KiB，OS_A/OS_B 各 5248 KiB，固定 RODATA 4096 KiB，DATA 1024 KiB；总和16 MiB，所有边界64 KiB对齐。

烧录失败进度约53%对应AIC镜像资源数据开始位置。旧镜像元数据仍将资源写到`rodata;rodata_r`，但新表已经删除`rodata_r`。另外ENV/ENV_R为`mtd;optional`，AiBurn全片擦除迁移时若跳过ENV，U-Boot就缺少`osAB_now`并在进入OS前异常。

新IMG逐项核实：系统镜像组件`os;os_r`，资源组件`rodata`，ENV和ENV_R均为`mtd;required`。构建脚本校验正式与隐藏镜像配置分区表一致，并检查ENV必烧，避免AiBurn继续使用隐藏旧配置。

迁移步骤：备份DATA和WiFi；AiBurn勾选全片擦除并烧录新工厂IMG；之后恢复DATA并重新保存WiFi。OS-only日常OTA只写非活动系统槽，不改RODATA/DATA且不含SPL。USB BROM恢复入口保留。

编译、AiBurn组件元数据、OS-only CPIO白名单及RSA验签通过。尚未在目标板重新烧录验收。
