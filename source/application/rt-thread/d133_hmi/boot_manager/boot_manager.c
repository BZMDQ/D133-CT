#include "boot_manager.h"
#include "hmi_serial_log.h"
#include "system_manager.h"
#include "ota_package.h"
#include "display_profile_manager.h"
#include "env_manager.h"
#include <stdio.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <absystem_os.h>
#include <dfs_fs.h>
#include <string.h>
static boot_status_t state;
static int pending_at_boot;
static volatile rt_tick_t heartbeat;
static volatile int rebooting;
static volatile rt_uint32_t health_faults;
static struct rt_mutex lock;
void boot_manager_get(boot_status_t *out){rt_mutex_take(&lock,RT_WAITING_FOREVER);*out=state;rt_mutex_release(&lock);}
void boot_manager_ui_heartbeat(void){heartbeat=rt_tick_get();}
void boot_manager_set_rebooting(int value)
{
    rt_enter_critical();
    rebooting = value;
    rt_exit_critical();
}
rt_uint32_t boot_manager_health_faults(void)
{
    rt_uint32_t value;
    rt_enter_critical();
    value = health_faults;
    rt_exit_critical();
    return value;
}
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
    /* /data 仅保存日志和状态；挂载失败不应阻断正常系统或触发看门狗复位。 */
    if(wdt&&!rt_device_init(wdt)&&!rt_device_control(wdt,RT_DEVICE_CTRL_WDT_SET_TIMEOUT,&timeout)&&!rt_device_control(wdt,RT_DEVICE_CTRL_WDT_START,0))watchdog_ok=1;
    if(!watchdog_ok)system_manager_log("看门狗未能启动，已阻止新系统健康确认");
    while(1){
        boot_status_t snapshot;boot_manager_get(&snapshot);
        display_profile_info_t profile;display_profile_get_active(&profile);
        struct dfs_filesystem *ro=dfs_filesystem_lookup("/rodata");
        rt_tick_t now=rt_tick_get();
        int touch_ok=!profile.touch_enabled || (profile.touch_device[0] && rt_device_find(profile.touch_device));
        int ui_alive=heartbeat && now-heartbeat<rt_tick_from_millisecond(3000);
        int slot_ok=(snapshot.current=='A'||snapshot.current=='B')&&(snapshot.next=='A'||snapshot.next=='B')&&snapshot.resource=='A';
        int ro_ok=ro && !strcmp(ro->path, "/rodata");
        int ok=watchdog_ok && touch_ok && slot_ok && ui_alive && ro_ok;
        rt_uint32_t faults=(!watchdog_ok?1U:0U)|(!touch_ok?2U:0U)|(!slot_ok?4U:0U)|(!ui_alive?8U:0U)|(!ro_ok?16U:0U);
        rt_enter_critical();health_faults=faults;rt_exit_critical();
        /* 已确认系统只因可诊断的外设/挂载检查失败而停止喂狗，会形成永久重启环。
         * 有效LVGL心跳仍在时维持运行并报告失败位；试运行槽仍严格回退。 */
        if(!rebooting && watchdog_ok && (ok || (!pending_at_boot && ui_alive))){rt_enter_critical();if(wdt && !rebooting)rt_device_control(wdt,RT_DEVICE_CTRL_WDT_KEEPALIVE,0);rt_exit_critical();}
        if(ok && !rebooting){if(healthy<30)healthy++;if(healthy==30&&pending_at_boot&&!state.confirmed&&!rebooting)boot_manager_confirm();}else healthy=0;
        rt_thread_mdelay(1000);
    }
}
int boot_manager_refresh(void)
{
    static const char *names[] = {
        "osAB_now", "osAB_next", "rodataAB_now", "upgrade_available"
    };
    char values_storage[4][16] = {{0}};
    char *values[] = {values_storage[0], values_storage[1],
                      values_storage[2], values_storage[3]};
    size_t capacities[] = {sizeof(values_storage[0]), sizeof(values_storage[1]),
                            sizeof(values_storage[2]), sizeof(values_storage[3])};

    if (env_manager_get_many(names, values, capacities, 4U) != 0)
        return -1;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    state.current = values[0][0] ? values[0][0] : '?';
    state.next = values[1][0] ? values[1][0] : '?';
    state.resource = values[2][0] ? values[2][0] : '?';
    state.pending = values[3][0] == '1';
    rt_mutex_release(&lock);
    return 0;
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




