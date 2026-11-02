#ifndef D133_FINSH_COMPAT_H
#define D133_FINSH_COMPAT_H

/*
 * 中文说明：部分 ArtInChip OTA/MbedTLS 源文件无条件包含 finsh.h，
 * 即使正式配置关闭 FinSH。调试配置继续转入 SDK 的真实 FinSH 头文件；
 * 正式静默配置只使用 rtdef.h 提供的空命令导出宏，不创建 shell。
 */
#ifdef RT_USING_FINSH
#include_next <finsh.h>
#else
#include <rtdef.h>
#endif

#endif

