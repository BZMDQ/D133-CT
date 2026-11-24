#ifndef HMI_NETWORK_MANAGER_H
#define HMI_NETWORK_MANAGER_H
#define D133_TCP_PORT 5000
#define D133_INFO_TCP_PORT D133_TCP_PORT
int network_manager_init(void);
void network_manager_status(char *out,unsigned capacity);
/* 板间USB专用：返回0成功、3忙、4命令错误、5执行失败；不经过网络认证。
 * 仅允许同一个应用工作线程调用。reboot_required=1时保留上传锁及OTA控制预约，必须在
 * USB应答完成或取消后调用finish，禁止在USB中断中调用这两个接口。 */
unsigned network_manager_physical_control(const char *command, int *reboot_required);
/* USB 配置包验签后只登记，切屏由 F750 另发明确的选择命令。 */
unsigned network_manager_register_screen(const char *code, const char *slot);
/* ack_sent=1才启动重启；成功重启后继续持锁直到复位，阻止新的OTA写入。 */
unsigned network_manager_physical_control_finish(int ack_sent);
/* USB OTA与TCP/屏幕控制使用同一互斥；仅USB工作线程成对调用。 */
int network_manager_usb_ota_reserve(void);
void network_manager_usb_ota_release(void);
#endif

