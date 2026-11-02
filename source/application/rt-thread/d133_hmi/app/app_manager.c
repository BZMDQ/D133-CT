#include "app_manager.h"
#include <rtthread.h>
static int ready;
static rt_tick_t boot_start_ms;
int app_manager_ready(void){return ready;}
unsigned int app_manager_boot_elapsed_ms(void)
{
    return (unsigned int)(rt_tick_get_millisecond() - boot_start_ms);
}
#include "storage_manager.h"
#include "system_manager.h"
#include "boot_manager.h"
#include "ota_manager.h"
#ifdef HMI_NETWORK
#include "network_manager.h"
#endif
/* 按依赖顺序创建服务；初始化失败直接向启动线程报告错误。 */
int app_manager_init(void)
{
    boot_start_ms=rt_tick_get_millisecond();
    if(system_manager_init()||storage_manager_init()||ota_manager_init())return -1;
    if(boot_manager_init())return -2;
#ifdef HMI_NETWORK
    if(network_manager_init())return -3;
#endif
    ready=1;system_manager_log("应用服务初始化完成");
    rt_kprintf("[boot_time] app_services_ready=%u ms\n",app_manager_boot_elapsed_ms());
    return 0;
}

INIT_APP_EXPORT(app_manager_init);

