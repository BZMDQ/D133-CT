#ifndef HMI_USB_CDC_CONTROL_H
#define HMI_USB_CDC_CONTROL_H

/* 在network_manager初始化完成后启动；中断不执行屏幕、ENV或重启操作。 */
int usb_cdc_control_init(void);

#endif
