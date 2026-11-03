#include "reboot_manager.h"
#include "boot_manager.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <aic_reboot_reason.h>

int reboot_manager_request(void)
{
    rt_device_t wdt = rt_device_find("wdt");
    rt_uint16_t timeout = 1;
    rt_err_t ret;

    if (!wdt)
        return -1;
    ret = rt_device_init(wdt);
    if (ret != RT_EOK && ret != -RT_EBUSY)
        return -2;

    /* 停止健康线程喂狗，避免重启请求被并发刷新。 */
    boot_manager_set_rebooting(1);
#ifdef AIC_WRI_DRV
    aic_set_reboot_reason(REBOOT_REASON_CMD_REBOOT);
#endif
    ret = rt_device_control(wdt, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &timeout);
    if (ret == RT_EOK)
        ret = rt_device_control(wdt, RT_DEVICE_CTRL_WDT_START, RT_NULL);
    if (ret != RT_EOK) {
        timeout = 15;
        rt_device_control(wdt, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &timeout);
        rt_device_control(wdt, RT_DEVICE_CTRL_WDT_START, RT_NULL);
        boot_manager_set_rebooting(0);
        return -3;
    }
    return 0;
}
