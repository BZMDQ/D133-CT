#include "sd_manager.h"
#include <rtthread.h>
#include <dfs_fs.h>
#include <string.h>
/* 插拔和自动挂载复用官方 SDMC1 热插拔线程及 board.c 挂载表。 */
int sd_manager_init(void){rt_kprintf("[sd] SDMC1 /sdcard; detect PC.6\n");return 0;}
int sd_manager_ready(void)
{
    struct dfs_filesystem *fs=dfs_filesystem_lookup("/sdcard");
    return fs && fs->path && !strcmp(fs->path,"/sdcard");
}
