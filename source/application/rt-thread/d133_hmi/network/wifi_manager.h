#ifndef HMI_WIFI_MANAGER_H
#define HMI_WIFI_MANAGER_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WIFI_INDICATOR_DISABLED = 0,
    WIFI_INDICATOR_CONNECTING,
    WIFI_INDICATOR_CONNECTED,
    WIFI_INDICATOR_FAILED
} wifi_manager_indicator_t;

/* Wi-Fi 命令异步提交；调用者不得从 LVGL 线程执行连接等待。 */
int wifi_manager_init(void);
int wifi_manager_connect(const char *ssid, const char *password);
int wifi_manager_connect_saved(unsigned index);
int wifi_manager_get_saved(unsigned index, char *ssid, size_t capacity);
unsigned wifi_manager_saved_count(void);
unsigned wifi_manager_saved_generation(void);
int wifi_manager_saved_options(char *text, size_t capacity);
/* 扫描请求异步执行；结果按 SSID 去重并由调用者复制。 */
int wifi_manager_scan(void);
unsigned wifi_manager_scan_generation(void);
int wifi_manager_scan_in_progress(void);
wifi_manager_indicator_t wifi_manager_indicator_state(void);
int wifi_manager_scan_options(char *text, size_t capacity);
int wifi_manager_scan_ssid(unsigned index, char *ssid, size_t capacity);
void wifi_manager_status(char *text, size_t capacity);
int wifi_manager_get_uid(char *text, size_t capacity);

#endif
