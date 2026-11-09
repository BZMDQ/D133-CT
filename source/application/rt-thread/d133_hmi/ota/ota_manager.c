#include "ota_manager.h"
#include "ota_package.h"
#include "ota_signature.h"
#include "boot_manager.h"
#include "system_manager.h"
#include "hmi_serial_log.h"
#include "reboot_manager.h"
#include <rtthread.h>
#include <stdbool.h>
#include <mbedtls/sha256.h>
#include <ota.h>
#include <env.h>
#include <absystem_os.h>
#include <stdio.h>
#include <string.h>
typedef struct {char path[256],version[32];unsigned char sha[32];int has_sha,remote;} job_t;
static uint32_t receive_sample_ms,receive_sample_bytes,transaction_start,phase_start;
static rt_mq_t queue;static struct rt_mutex mutex;static hmi_ota_status_t status;static volatile int decision;static ota_signature_verifier_t verifier;
void ota_manager_set_signature_verifier(ota_signature_verifier_t v){verifier=v;}
void ota_manager_get(hmi_ota_status_t *out){rt_mutex_take(&mutex,RT_WAITING_FOREVER);*out=status;
    if(status.phase){uint32_t now=rt_tick_get_millisecond();out->phase_ms[status.phase]+=now-phase_start;out->total_ms=now-transaction_start;}
    rt_mutex_release(&mutex);}
void ota_manager_phase(unsigned phase){
    if(phase>5)return;rt_mutex_take(&mutex,RT_WAITING_FOREVER);uint32_t now=rt_tick_get_millisecond();
    if(status.phase)status.phase_ms[status.phase]+=now-phase_start;
    if(!transaction_start)transaction_start=now;
    status.phase=phase;phase_start=now;status.total_ms=now-transaction_start;rt_mutex_release(&mutex);
}
void ota_manager_stream_state(hmi_ota_state_t s,unsigned p,int e){rt_mutex_take(&mutex,RT_WAITING_FOREVER);status.state=s;status.percent=p;status.error=e;rt_mutex_release(&mutex);}
int ota_manager_stream_confirm(void){decision=0;ota_manager_phase(2);ota_manager_stream_state(OTA_CONFIRM,0,0);
    for(unsigned i=0;i<600&&decision==0;i++)rt_thread_mdelay(100);
    return decision==1?0:-14;
}
static void update(hmi_ota_state_t s,unsigned p,int err){rt_mutex_take(&mutex,RT_WAITING_FOREVER);status.state=s;status.percent=p;status.error=err;rt_mutex_release(&mutex);}
void ota_manager_decide(int accept){decision=accept?1:-1;}
void ota_manager_reboot(void){if(reboot_manager_request())system_manager_log("重启请求失败");}
/* 接收过程只更新共享状态，不在网络线程操作 LVGL 或 Flash。 */
int ota_manager_receive_begin(const char *version)
{
    int result=0;rt_mutex_take(&mutex,RT_WAITING_FOREVER);
    if(status.state!=OTA_IDLE&&status.state!=OTA_FAILED)result=-1;
    else{memset(status.phase_ms,0,sizeof(status.phase_ms));status.phase=0;transaction_start=rt_tick_get_millisecond();phase_start=transaction_start;receive_sample_ms=rt_tick_get_millisecond();receive_sample_bytes=0;status.bytes_per_second=0;status.state=OTA_RECEIVE;status.remote=1;status.percent=0;status.error=0;rt_snprintf(status.version,sizeof(status.version),"%s",version?version:"");}
    rt_mutex_release(&mutex);return result;
}
void ota_manager_receive_progress(uint32_t written,uint32_t total)
{
    rt_mutex_take(&mutex,RT_WAITING_FOREVER);
    if(status.state==OTA_RECEIVE&&total){
        uint32_t now=rt_tick_get_millisecond(),elapsed=now-receive_sample_ms;
        status.percent=(unsigned)((uint64_t)written*100U/total);
        /* 每半秒统计已落盘字节，不将网络接收缓存计入下载速度。 */
        if(elapsed>=500U){status.bytes_per_second=(uint32_t)((uint64_t)(written-receive_sample_bytes)*1000U/elapsed);receive_sample_ms=now;receive_sample_bytes=written;}
    }
    rt_mutex_release(&mutex);
}
void ota_manager_receive_abort(int error)
{
    rt_mutex_take(&mutex,RT_WAITING_FOREVER);
    if(status.state==OTA_RECEIVE){status.state=OTA_FAILED;status.error=error;}
    rt_mutex_release(&mutex);
}
static void worker(void *unused)
{
    job_t j;char version[32],buf[2048];unsigned char hash[32];FILE *f;long total,written;size_t n;int error,initialized,hash_initialized;mbedtls_sha256_context stream_hash;
    while(1){rt_mq_recv(queue,&j,sizeof(j),RT_WAITING_FOREVER);error=-1;initialized=0;hash_initialized=0;f=0;
        if(j.remote){decision=0;update(OTA_CONFIRM,0,0);for(unsigned i=0;i<600&&decision==0;i++)rt_thread_mdelay(100);if(decision!=1){error=-14;goto fail;}}
        update(OTA_CHECK,0,0);
        if(ota_package_check(j.path,version)){error=-20;goto fail;}
        if(j.version[0]&&strcmp(j.version,version)){error=-21;goto fail;}
        if(ota_package_sha256(j.path,hash)){error=-22;goto fail;}
        if(j.has_sha&&memcmp(hash,j.sha,32)){error=-23;goto fail;}
        /* 必须通过配置的签名验证器；无后端或签名无效均拒绝升级。 */
        if(!verifier||verifier(j.path,hash)){error=-24;goto fail;}
        rt_mutex_take(&mutex,RT_WAITING_FOREVER);snprintf(status.version,32,"%s",version);rt_mutex_release(&mutex);
        f=fopen(j.path,"rb");if(!f)goto fail;fseek(f,0,SEEK_END);total=ftell(f);rewind(f);written=0;
        /* 每次事务重置官方状态，避免失败重试沿用上一包的成员完成标志。 */
        extern bool os_status,rodatafs_status,datafs_status;os_status=rodatafs_status=datafs_status=0;
                mbedtls_sha256_init(&stream_hash);if(mbedtls_sha256_starts_ret(&stream_hash,0))goto fail;hash_initialized=1;
        if(ota_init())goto fail;
        initialized=1;update(OTA_WRITE,0,0);
        while((n=fread(buf,1,sizeof(buf),f))){if(mbedtls_sha256_update_ret(&stream_hash,(unsigned char*)buf,n)){error=-22;goto fail;}if(ota_shard_download_fun(buf,n)<0){error=-25;goto fail;}written+=n;update(OTA_WRITE,(unsigned)(written*100/total),0);/* 每 64 KiB 让出 CPU，使网络查询和界面能及时运行。 */if((written&0xFFFF)==0)rt_thread_mdelay(1);}
        if(ferror(f)||!os_status){error=-26;goto fail;}
        unsigned char written_hash[32];if(mbedtls_sha256_finish_ret(&stream_hash,written_hash)||memcmp(written_hash,hash,32)){error=-23;goto fail;}mbedtls_sha256_free(&stream_hash);hash_initialized=0;
        fclose(f);f=0;ota_deinit();initialized=0;
                char record[160];boot_status_t boot;boot_manager_get(&boot);
        snprintf(record,sizeof(record),"prepared version=%s target=%c",version,boot.current=='A'?'B':'A');
        if(system_manager_record_upgrade(record)){error=-28;goto fail;}
        if(aic_upgrade_end()){error=-27;goto fail;}
        snprintf(record,sizeof(record),"committed version=%s target=%c",version,boot.current=='A'?'B':'A');
        if(system_manager_record_upgrade(record))system_manager_log("警告：ENV 已提交，但升级状态文件写入失败");
        boot_manager_refresh();update(OTA_READY,100,0);hmi_serial_log("[ota] verified inactive slots ready; reboot required\n");continue;
fail:if(f)fclose(f);if(initialized)ota_deinit();if(hash_initialized)mbedtls_sha256_free(&stream_hash);update(OTA_FAILED,0,error);char failure_record[128];snprintf(failure_record,sizeof(failure_record),"failed error=%d version=%s",error,j.version[0]?j.version:"unknown");system_manager_record_upgrade(failure_record);{char log_text[128];snprintf(log_text,sizeof(log_text),"升级失败，错误码：%d，版本：%s",error,j.version[0]?j.version:"未知");system_manager_log(log_text);}hmi_serial_log("[ota] failed=%d; current slot retained\n",error);
    }
}
int ota_manager_init(void){rt_thread_t t;verifier=ota_signature_verify;rt_mutex_init(&mutex,"otastate",RT_IPC_FLAG_PRIO);queue=rt_mq_create("otajob",sizeof(job_t),1,RT_IPC_FLAG_FIFO);if(!queue)return -1;t=rt_thread_create("hmi_ota",worker,0,8192,21,10);return t?rt_thread_startup(t):-1;}
int ota_manager_request(const char *path,const unsigned char *sha,const char *version,int remote)
{
    job_t j;int r;memset(&j,0,sizeof(j));if(!path||strlen(path)>=sizeof(j.path))return -1;
    rt_mutex_take(&mutex,RT_WAITING_FOREVER);if(status.state!=OTA_IDLE&&status.state!=OTA_FAILED&&!(remote&&status.state==OTA_RECEIVE)){rt_mutex_release(&mutex);return -3;}status.state=OTA_CHECK;status.remote=remote;status.error=0;status.percent=0;if(version)snprintf(status.version,32,"%s",version);else status.version[0]=0;rt_mutex_release(&mutex);
    snprintf(j.path,sizeof(j.path),"%s",path);if(version)snprintf(j.version,32,"%s",version);if(sha){memcpy(j.sha,sha,32);j.has_sha=1;}j.remote=remote;r=rt_mq_send(queue,&j,sizeof(j));if(r)update(OTA_FAILED,0,r);return r;
}










