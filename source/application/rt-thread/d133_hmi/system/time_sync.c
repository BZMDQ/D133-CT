#include "time_sync.h"
#include "system_manager.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <time.h>
#include <wlan_mgnt.h>
#include <lwip/tcpip.h>
#include <netdev.h>
static void apply_network_time(time_t value)
{
    if(set_timestamp(value)==RT_EOK)system_manager_log("网络校时成功（北京时间）");
    else system_manager_log("网络时间已收到，但 RTC 写入失败");
}
/* 复用 SDK 自带 SNTP 实现，只在本模块设置 RTC 回调和 DNS 选项。
 * SDK 的 lwIP SConscript 未将 sntp.c 加入默认源文件列表。 */
#define SNTP_SERVER_DNS 1
#define SNTP_MAX_SERVERS 3
#define SNTP_STARTUP_DELAY 0
#define SNTP_RETRY_TIMEOUT_MAX 30000
#define SNTP_SET_SYSTEM_TIME(sec) apply_network_time((time_t)(sec))
#include "../../../../kernel/rt-thread/components/net/lwip/lwip-2.1.2/src/apps/sntp/sntp.c"

static void sync_control(void *parameter)
{
    /* SNTP 仅在 lwIP 核心线程启停，内部定时器负责失败重试和周期校准。 */
    if (parameter) {
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "ntp.aliyun.com");
        sntp_setservername(1, "time.cloudflare.com");
        sntp_setservername(2, "pool.ntp.org");
        sntp_init();
        system_manager_log("网络校时已启动（北京时间）");
    } else sntp_stop();
}
static void sync_worker(void *parameter)
{
    int active=0;
    (void)parameter;
    while (1) {
        /* 不依赖驱动是否上报 WLAN READY，直接检查当前网卡的地址。 */
        struct netdev *network=netdev_default;
        int ready=network && netdev_is_up(network) && netdev_is_link_up(network) &&
            !ip_addr_isany(&network->ip_addr);
        if (ready!=active && tcpip_callback(sync_control,ready?(void*)1:RT_NULL)==ERR_OK)
            active=ready;
        rt_thread_mdelay(2000);
    }
}
int time_sync_init(void)
{
    rt_thread_t worker=rt_thread_create("hmi_time",sync_worker,RT_NULL,2048,25,10);
    return worker?rt_thread_startup(worker):-1;
}
