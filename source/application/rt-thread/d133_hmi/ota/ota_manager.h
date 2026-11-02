#ifndef HMI_OTA_MANAGER_H
#define HMI_OTA_MANAGER_H
#include <stdint.h>
typedef enum { OTA_IDLE,OTA_CONFIRM,OTA_CHECK,OTA_WRITE,OTA_READY,OTA_FAILED } hmi_ota_state_t;
typedef struct {hmi_ota_state_t state;unsigned percent;int error,remote;char version[32];} hmi_ota_status_t;
int ota_manager_init(void);
int ota_manager_request(const char *path,const unsigned char *sha,const char *version,int remote);
void ota_manager_decide(int accept);
void ota_manager_get(hmi_ota_status_t *out);
void ota_manager_reboot(void);
/* 产品可注册签名验证器；无验证器时明确返回未支持，不伪造验证成功。 */
typedef int (*ota_signature_verifier_t)(const char *path,const unsigned char digest[32]);
void ota_manager_set_signature_verifier(ota_signature_verifier_t verify);
#endif

