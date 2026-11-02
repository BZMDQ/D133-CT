#include "quiet_console.h"
#include <rtconfig.h>

#if defined(KERNEL_RTTHREAD) && defined(RT_USING_DEVICE) && \
    defined(RT_USING_CONSOLE) && defined(HMI_QUIET_CONSOLE)

#include <rtdevice.h>
#include <rtthread.h>

#define HMI_QUIET_CONSOLE_NAME "hmi_quiet"

static struct rt_device quiet_console;

/* 静默设备只吞掉控制台写入，不触碰 BootROM/SPL 的早期 UART。 */
static rt_err_t quiet_open(rt_device_t device, rt_uint16_t oflag)
{
    (void)device;
    (void)oflag;
    return RT_EOK;
}

static rt_err_t quiet_close(rt_device_t device)
{
    (void)device;
    return RT_EOK;
}

static rt_size_t quiet_read(rt_device_t device, rt_off_t position,
                            void *buffer, rt_size_t size)
{
    (void)device;
    (void)position;
    (void)buffer;
    (void)size;
    return 0;
}

static rt_size_t quiet_write(rt_device_t device, rt_off_t position,
                             const void *buffer, rt_size_t size)
{
    (void)device;
    (void)position;
    (void)buffer;
    return size;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops quiet_ops =
{
    RT_NULL,
    quiet_open,
    quiet_close,
    quiet_read,
    quiet_write,
    RT_NULL,
};
#endif

int hmi_quiet_console_register(void)
{
    rt_memset(&quiet_console, 0, sizeof(quiet_console));
    quiet_console.type = RT_Device_Class_Char;
#ifdef RT_USING_DEVICE_OPS
    quiet_console.ops = &quiet_ops;
#else
    quiet_console.open = quiet_open;
    quiet_console.close = quiet_close;
    quiet_console.read = quiet_read;
    quiet_console.write = quiet_write;
#endif

    if (rt_device_register(&quiet_console, HMI_QUIET_CONSOLE_NAME,
                           RT_DEVICE_FLAG_RDWR) != RT_EOK)
        return -RT_ERROR;

    rt_console_set_device(HMI_QUIET_CONSOLE_NAME);
    return rt_console_get_device() == &quiet_console ? RT_EOK : -RT_ERROR;
}

#else

int hmi_quiet_console_register(void)
{
    return 0;
}

#endif

