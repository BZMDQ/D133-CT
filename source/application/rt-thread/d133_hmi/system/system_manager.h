#ifndef HMI_SYSTEM_MANAGER_H
#define HMI_SYSTEM_MANAGER_H
#include <stddef.h>

#define HMI_DEVICE_NAME_MAX_BYTES 63U

enum {
    HMI_DEVICE_NAME_SAVE_IDLE = 0,
    HMI_DEVICE_NAME_SAVE_PENDING = 1,
    HMI_DEVICE_NAME_SAVE_OK = 2,
    HMI_DEVICE_NAME_SAVE_ERROR = -1
};

int system_manager_init(void);
void system_manager_log(const char *message);
int system_manager_record_upgrade(const char *message);
int system_manager_upgrade_state(char *out,unsigned capacity);
void system_manager_logs(char *out,unsigned capacity);
int system_manager_get_device_name(char *out, size_t capacity);
/* TCP 工具箱工作线程可同步写入；LVGL 页面应使用异步请求接口。 */
int system_manager_set_device_name(const char *name, size_t length);
int system_manager_request_device_name(const char *name, size_t length);
int system_manager_device_name_save_state(void);
#endif

