#include "hmi_serial_log.h"

#include <rtthread.h>
#include <stdarg.h>

void hmi_serial_log(const char *format, ...)
{
#ifdef HMI_SERIAL_DIAGNOSTICS
    char buffer[512];
    va_list args;

    va_start(args, format);
    rt_vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    rt_kputs(buffer);
#else
    /* 正式配置不拼接、不发送 HMI 调试串口字符串。 */
    (void)format;
#endif
}

