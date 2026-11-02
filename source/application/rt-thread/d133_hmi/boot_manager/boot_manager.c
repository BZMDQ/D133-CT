#include "boot_manager.h"
#include "hmi_serial_log.h"
#include "system_manager.h"
#include "ota_package.h"
#include "display_profile_manager.h"
#include <stdio.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <env.h>
#include <absystem_os.h>
#include <dfs_fs.h>
#include <string.h>
static boot_status_t state;
static int pending_at_boot;
static volatile rt_tick_t heartbeat;
static struct rt_mutex lock;
void boot_manager_get(boot_status_t *out){rt_mutex_take(&lock,RT_WAITING_FOREVER);*out=state;rt_mutex_release(&lock);}
void boot_manager_ui_heartbeat(void){heartbeat=rt_tick_get();}
int boot_manager_confirm(void)
{
    int r=aic_ota_status_update();
    if(!r){rt_mutex_take(&lock,RT_WAITING_FOREVER);state.pending=0;state.confirmed=1;rt_mutex_release(&lock);
        char record[128];snprintf(record,sizeof(record),"confirmed version=%s target=%c",HMI_VERSION,state.current);system_manager_record_upgrade(record);system_manager_log("新系统健康检查通过，启动槽位已确认");}
    hmi_serial_log("[boot] health confirmation=%d\n",r);return r;
}
static void monitor(void *arg)
{
    rt_device_t wdt=rt_device_find("wdt");rt_uint32_t timeout=15;unsigned healthy=0;int watchdog_ok=0;
    /* 持续检查 UI 心跳和必要文件系统；失败时不确认，交给看门狗复位和官方计数回滚。 */
    if(wdt&&!rt_device_init(wdt)&&!rt_device_control(wdt,RT_DEVICE_CTRL_WDT_SET_TIMEOUT,&timeout)&&!rt_device_control(wdt,RT_DEVICE_CTRL_WDT_START,0))watchdog_ok=1;
    if(!watchdog_ok)system_manager_log("看门狗未能启动，已阻止新系统健康确认");
    while(1){
        boot_status_t snapshot;boot_manager_get(&snapshot);
        display_profile_info_t profile;display_profile_get_active(&profile);
        struct dfs_filesystem *ro=dfs_filesystem_lookup("/rodata"),*data=dfs_filesystem_lookup("/data");
        int touch_ok=!profile.touch_enabled || rt_device_find(profile.touch);
        int ok=watchdog_ok && touch_ok && snapshot.current==snapshot.resource && heartbeat && rt_tick_get()-heartbeat<rt_tick_from_millisecond(3000) && ro&&data&&!strcmp(ro->path,"/rodata")&&!strcmp(data->path,"/data");
        if(ok){if(wdt)rt_device_control(wdt,RT_DEVICE_CTRL_WDT_KEEPALIVE,0);if(healthy<30)healthy++;if(healthy==30&&pending_at_boot&&!state.confirmed)boot_manager_confirm();}else healthy=0;
        rt_thread_mdelay(1000);
    }
}
int boot_manager_refresh(void)
{
    char *v;if(fw_env_open())return -1;rt_mutex_take(&lock,RT_WAITING_FOREVER);
    v=fw_getenv("osAB_now");state.current=v?v[0]:'?';v=fw_getenv("osAB_next");state.next=v?v[0]:'?';
    v=fw_getenv("rodataAB_now");state.resource=v?v[0]:'?';v=fw_getenv("upgrade_available");state.pending=v&&v[0]=='1';
    rt_mutex_release(&lock);fw_env_close();return 0;
}
int boot_manager_init(void)
{
    rt_thread_t t;rt_mutex_init(&lock,"bootstate",RT_IPC_FLAG_PRIO);if(boot_manager_refresh())return -1;
    /* 只确认本次开机时已经处于试启动状态的系统，旧系统绝不能确认刚提交的下一槽。 */
        pending_at_boot=state.pending;state.confirmed=!state.pending;
    char previous[160];if(!system_manager_upgrade_state(previous,sizeof(previous))&&strstr(previous,"committed")){
        char *target=strstr(previous,"target=");
        if(target&&!state.pending&&target[7]&&target[7]!=state.current){system_manager_log("检测到启动回滚，系统与资源已切回上一槽位");system_manager_record_upgrade("rolled_back");}
    }
    t=rt_thread_create("hmi_health",monitor,0,3072,20,10);return t?rt_thread_startup(t):-1;
}



