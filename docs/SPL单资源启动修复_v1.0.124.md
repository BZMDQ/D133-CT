# v1.0.124 单资源分区 SPL 空指针修复

历史 v1.0.122 在单资源分区配置中删除 `rodataAB_next`，但 SPL 的 `aic_get_os_to_startup()` 仍调用 `fw_getenv("rodataAB_next")` 并直接传给 `strncmp()`，因此出现 `CPU Exception NO.5`、`mtval=0`，在 OS 启动和屏幕初始化之前停止。上一个将问题简单归因于 ENV 没有烧写的判断不准确。

恢复 `rodataAB_next=A` 作为官方 Bootloader 的兼容变量，物理上仍只有一个 RODATA 分区。Bootloader 回滚时固定 `rodataAB_next=A`，不再随 OS A/B 切换；缺失 OS/RODATA/DATA 启动变量时返回错误并输出提示，避免空指针崩溃。重新编译了 Bootloader 和 OS，工厂IMG包含新 Bootloader；普通OTA依旧不包含SPL。

IMG必须将ENV/ENV_R标记为必写；资源成员映射只允许`rodata`，OS映射`os;os_r`。从旧布局迁移时备份DATA，AiBurn全片擦除烧录工厂IMG。新版仅通过构建和镜像静态检查，尚未在开发板上验证启动、OS A/B切换或回滚。
