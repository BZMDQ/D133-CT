#ifndef HMI_OTA_MANAGER_H
#define HMI_OTA_MANAGER_H
#include <stdint.h>
/* 接收状态追加在末尾，保持既有工具箱状态编号不变。 */
typedef enum { OTA_IDLE,OTA_CONFIRM,OTA_CHECK,OTA_WRITE,OTA_READY,OTA_FAILED,OTA_RECEIVE } hmi_ota_state_t;
typedef struct {hmi_ota_state_t state;unsigned percent;int error,remote;uint32_t bytes_per_second,total_ms,phase_ms[6];unsigned phase;char version[32];} hmi_ota_status_t;
int ota_manager_init(void);
/* 控制与所有OTA入口在同一个状态mutex内预约，避免检查后再启动的竞争。
 * 返回0时由调用者持有预约，必须release；重启成功时保留至复位。
 * 仅应用线程调用，不允许在USB/硬件中断中等待状态mutex。 */
int ota_manager_control_reserve(void);
void ota_manager_control_release(void);
int ota_manager_request(const char *path,const unsigned char *sha,const char *version,int remote);
int ota_manager_receive_begin(const char *version);
void ota_manager_receive_progress(uint32_t written,uint32_t total);
void ota_manager_receive_abort(int error);
void ota_manager_decide(int accept);
/* 阶段：1签名、2确认、3接收写入、4校验、5提交；0结束。 */
void ota_manager_phase(unsigned phase);
int ota_manager_stream_confirm(void);
/* 可中止的流式确认：存活回调只读链路epoch，由应用线程轮询。 */
int ota_manager_stream_confirm_live(int (*alive)(void *),void *context);
void ota_manager_stream_state(hmi_ota_state_t state,unsigned percent,int error);
/* 失败状态和阶段结束在同一mutex内提交，禁止旧清理覆盖新SD预约。 */
void ota_manager_stream_fail(int error);
void ota_manager_get(hmi_ota_status_t *out);
void ota_manager_reboot(void);
/* 产品可注册签名验证器；无验证器时明确返回未支持，不伪造验证成功。 */
typedef int (*ota_signature_verifier_t)(const char *path,const unsigned char digest[32]);
void ota_manager_set_signature_verifier(ota_signature_verifier_t verify);
#endif

