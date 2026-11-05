#ifndef HMI_WIFI_MANAGER_H
#define HMI_WIFI_MANAGER_H
#include <stddef.h>
#include <stdint.h>

/* Wi-Fi 命令异步提交；调用者不得从 LVGL 线程执行连接等待。 */
int wifi_manager_init(void);
int wifi_manager_connect(const char *ssid, const char *password);
int wifi_manager_connect_saved(unsigned index);
int wifi_manager_start_ap(const char *ssid, const char *password);
int wifi_manager_stop_ap(void);
int wifi_manager_get_saved(unsigned index, char *ssid, size_t capacity);
unsigned wifi_manager_saved_count(void);
int wifi_manager_saved_options(char *text, size_t capacity);
void wifi_manager_status(char *text, size_t capacity);

#endif
