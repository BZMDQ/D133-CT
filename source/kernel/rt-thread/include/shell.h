#ifndef D133_SHELL_COMPAT_H
#define D133_SHELL_COMPAT_H

/*
 * 中文说明：部分 ArtInChip 多媒体示例在关闭 FinSH 后仍包含 shell.h，
 * 但只用 MSH_CMD_EXPORT_ALIAS 导出调试命令。正式静默构建复用 rtdef.h
 * 的空导出宏；诊断配置转发到 SDK 的真实 shell.h。
 */
#ifdef RT_USING_FINSH
#include_next <shell.h>
#else
#include <rtdef.h>
#endif

#endif

