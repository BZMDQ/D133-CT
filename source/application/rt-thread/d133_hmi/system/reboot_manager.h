#ifndef HMI_REBOOT_MANAGER_H
#define HMI_REBOOT_MANAGER_H

/* 使用 SDK 看门狗设备触发系统复位；成功返回后等待硬件复位。 */
int reboot_manager_request(void);

#endif
