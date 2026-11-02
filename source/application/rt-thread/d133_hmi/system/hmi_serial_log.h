#ifndef D133_HMI_SERIAL_LOG_H
#define D133_HMI_SERIAL_LOG_H

/* 仅诊断构建输出串口日志；正式固件由配置在编译期移除格式化开销。 */
void hmi_serial_log(const char *format, ...);

#endif

