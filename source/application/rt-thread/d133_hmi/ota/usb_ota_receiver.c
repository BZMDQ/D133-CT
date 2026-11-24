#include <rtthread.h>
#ifdef HMI_USB_CDC_CONTROL
#include "usb_ota_receiver.h"
#include "ota_stream.h"
#include "ota_manager.h"
#include "ota_package.h"
#include "boot_manager.h"
#include "network_manager.h"
#include "reboot_manager.h"
#include "hardware_config.h"
#include "app_manager.h"
#include <mbedtls/sha256.h>
#include <aic_crc32.h>
#include <string.h>

/* 保留旧版 32/64 KiB 能力值，只有识别 v3 的主机才会发送 128 KiB。 */
#define USB_OTA_LEGACY_MAX_DATA 32768U
#define USB_OTA_V2_MAX_DATA 65536U
#define USB_OTA_LEGACY_LIMIT 4096U
#define USB_OTA_MAX_TOTAL (5248U*1024U+8192U)
#define USB_OTA_IDLE_MS 30000U

static struct {
    unsigned char frame[USB_OTA_HEADER+USB_OTA_MAX_DATA];
    unsigned char reply[USB_OTA_HEADER+1000U];
    unsigned char hash[32];
    unsigned have,want,epoch;
    uint32_t session,total,written,last_ms;
    int locked,active,committed,blocked,hash_ready;
    mbedtls_sha256_context sha;
    int (*send)(const void *,unsigned,unsigned);
    int (*alive)(unsigned);
} usb_ota;

static uint32_t usb_ota_u32(const unsigned char *p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void usb_ota_put32(unsigned char *p,uint32_t value)
{
    for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));
}
static int usb_ota_live(void *context)
{
    return usb_ota.alive&&usb_ota.alive((unsigned)(uintptr_t)context);
}
static int usb_ota_session_live(void *context)
{
    /* 确认跳过只随当前持锁的CDC会话生效，DOTA报文没有确认开关。 */
    return usb_ota.locked&&usb_ota.epoch==(unsigned)(uintptr_t)context&&usb_ota_live(context);
}
static void usb_ota_abort(int error)
{
    /* ENV已成功提交的事务不可撤销，保留上传锁至真正重启。 */
    if(usb_ota.committed)return;
    if(usb_ota.session)ota_stream_abort_session(usb_ota.session,error);
    usb_ota.session=0;usb_ota.active=0;
    if(usb_ota.hash_ready){mbedtls_sha256_free(&usb_ota.sha);usb_ota.hash_ready=0;}
    if(usb_ota.locked){network_manager_usb_ota_release();usb_ota.locked=0;}
}
static int usb_ota_reply(uint32_t op,unsigned status,const void *data,unsigned length,unsigned epoch)
{
    unsigned char *p=usb_ota.reply;
    if(length>1000U||!usb_ota_live((void *)(uintptr_t)epoch))return 0;
    memcpy(p,"DOTA",4);usb_ota_put32(p+4,op|0x80000000U);
    usb_ota_put32(p+8,status);usb_ota_put32(p+12,length);
    if(length)memcpy(p+20,data,length);
    usb_ota_put32(p+16,crc32(crc32(0,p,16),p+20,length));
    return usb_ota.send&&usb_ota.send(p,20+length,epoch);
}
static unsigned usb_ota_error(int result)
{
    unsigned error=(unsigned)(result<0?-result:result);
    if(error==23)return 9;
    return error?error:25;
}
/* INFO 的列表和 JSON 缓冲独占调用栈，BEGIN 验签路径无需同时保留这些局部变量。 */
static __attribute__((noinline)) void usb_ota_send_info(uint32_t op, unsigned epoch)
{
    char info[944];boot_status_t boot;
#ifdef HMI_DISPLAY_PROFILE_SERIAL
    char display_status[672];
    char profiles[DISPLAY_PROFILE_MAX_CHOICES *
                  (DISPLAY_PROFILE_CODE_LENGTH + DISPLAY_PROFILE_SLOT_LENGTH) + 1U] = {0};
    display_profile_choice_t choices[DISPLAY_PROFILE_MAX_CHOICES];
    display_profile_info_t active;
    size_t count = display_profile_choices(choices, DISPLAY_PROFILE_MAX_CHOICES);
    /* 固定长度 DOC 字段保持为 CSddd=CSddd;，避免额外转义或截断。 */
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        int length = rt_snprintf(profiles + used, sizeof(profiles) - used,
                                 "%s=%s;", choices[i].code, choices[i].slot);
        if (length < 0 || (size_t)length >= sizeof(profiles) - used)
            break;
        used += (size_t)length;
    }
    display_profile_get_active(&active);
    rt_snprintf(display_status, sizeof(display_status),
                ",\"usb_hwcfg\":4,\"screen\":\"%s\",\"screen_slot\":\"%s\","
                "\"screen_width\":%u,\"screen_height\":%u,\"touch\":\"%s\",\"screen_profiles\":\"%s\"",
                active.code, display_profile_active_slot(), (unsigned)active.width,
                (unsigned)active.height, active.touch, profiles);
#else
    const char *display_status = "";
#endif
    boot_manager_get(&boot);
    int n=rt_snprintf(info,sizeof(info),
                      "{\"product\":\"" HMI_PRODUCT "\",\"hardware\":\"" HMI_HARDWARE
                      "\",\"version\":\"" HMI_VERSION "\",\"protocol\":1,\"stream_ota\":2,"
                      "\"usb_ota\":1,\"usb_screen_confirm\":false,\"usb_max_chunk\":%u,\"usb_max_chunk_v2\":%u,\"usb_max_chunk_v3\":%u,"
                      "\"slot\":\"%c\",\"next_slot\":\"%c\",\"confirmed\":%s%s}",
                      USB_OTA_LEGACY_MAX_DATA,USB_OTA_V2_MAX_DATA,USB_OTA_MAX_DATA,boot.current,boot.next,
                      boot.confirmed?"true":"false",display_status);
    usb_ota_reply(op,n<0||n>=(int)sizeof(info)?4:0,info,n>0&&(unsigned)n<sizeof(info)?(unsigned)n:0,epoch);
}

static void usb_ota_execute(unsigned epoch)
{
    const unsigned char *p=usb_ota.frame+20;
    uint32_t op=usb_ota_u32(usb_ota.frame+4),offset=usb_ota_u32(usb_ota.frame+8);
    unsigned length=usb_ota_u32(usb_ota.frame+12),status=0,reply_length=0;
    unsigned char count[4];const void *reply_data=0;
    int result=0,commit_ack=0;
    if(!usb_ota_live((void *)(uintptr_t)epoch)){usb_ota_abort(-101);return;}
    if(usb_ota_u32(usb_ota.frame+16)!=crc32(crc32(0,usb_ota.frame,16),p,length)){
        usb_ota_abort(-1);usb_ota_reply(op,1,0,0,epoch);return;
    }
    if(op==0xD100U||op==0xD105U){
        if(offset||length){usb_ota_reply(op,4,0,0,epoch);return;}
        usb_ota_send_info(op, epoch);
        return;
    }
#ifdef HMI_DISPLAY_PROFILE_SERIAL
    if (op == 0xD10AU) {
        hardware_config_profile_t config;
        boot_status_t boot;
        if (offset) {status=4;goto reply;}
        if (usb_ota.locked || usb_ota.committed || !app_manager_ready()) {status=3;goto reply;}
        boot_manager_get(&boot);
        if (boot.pending || !boot.confirmed || boot.current != boot.next) {status=3;goto reply;}
        status = hardware_config_verify(p, length, &config);
        if (status) goto reply;
        /* 验签期间若 USB 已重连，不把旧会话的配置写入设备。 */
        if (!usb_ota_live((void *)(uintptr_t)epoch)) return;
        /* 旧版内置包验签后已有对应选项，自定义包只登记。 */
        if (config.custom) {
            status = network_manager_register_screen(config.code, config.slot);
            if (status) goto reply;
        }
        usb_ota_reply(op, 0, config.code, (unsigned)strlen(config.code), epoch);
        return;
    }
#endif
    if(op==0xD106U){
        if(usb_ota.locked||usb_ota.committed){status=3;goto reply;}
        if(offset||length<=132U||length>644U){status=4;goto reply;}
        uint32_t total=usb_ota_u32(p);
        if(!total||total>USB_OTA_MAX_TOTAL||p[67]||p[99]||p[131]||
           strcmp((const char *)p+36,HMI_PRODUCT)||strcmp((const char *)p+68,HMI_HARDWARE)||
           !ota_version_newer((const char *)p+100)){status=4;goto reply;}
        if(network_manager_usb_ota_reserve()){status=3;goto reply;}
        usb_ota.locked=1;usb_ota.epoch=epoch;usb_ota.total=total;usb_ota.written=0;
        memcpy(usb_ota.hash,p+4,32);
        result=ota_stream_begin_usb_session(total,p+4,(const char *)p+100,p+132,length-132,
                                           &usb_ota.session,usb_ota_session_live,(void *)(uintptr_t)epoch);
        /* 验签期间断线后，新会话不能继续上一会话的事务或收到旧ACK。 */
        if(!usb_ota_live((void *)(uintptr_t)epoch)){usb_ota_abort(-101);return;}
        if(result){status=usb_ota_error(result);usb_ota_abort(result);goto reply;}
        mbedtls_sha256_init(&usb_ota.sha);usb_ota.hash_ready=1;
        if(mbedtls_sha256_starts_ret(&usb_ota.sha,0)){status=9;usb_ota_abort(-23);goto reply;}
        usb_ota.active=1;goto reply;
    }
    if(op==0xD107U){
        if(offset||length){status=4;goto reply;}
        if(usb_ota.committed){status=27;goto reply;}
        usb_ota_abort(-14);goto reply;
    }
    if(op==0xD102U){
        if(!usb_ota.active||offset!=usb_ota.written||!length||length>usb_ota.total-usb_ota.written){status=6;usb_ota_abort(-6);goto reply;}
        result=ota_stream_feed_session(usb_ota.session,p,length);
        if(!usb_ota_live((void *)(uintptr_t)epoch)){usb_ota_abort(-101);return;}
        if(result){status=usb_ota_error(result);usb_ota_abort(result);goto reply;}
        if(mbedtls_sha256_update_ret(&usb_ota.sha,p,length)){status=9;usb_ota_abort(-23);goto reply;}
        usb_ota.written+=length;ota_manager_receive_progress(usb_ota.written,usb_ota.total);
        usb_ota_put32(count,usb_ota.written);reply_data=count;reply_length=4;goto reply;
    }
    if(op==0xD103U){
        if(offset||length){status=4;goto reply;}
        if(!usb_ota.active||usb_ota.written!=usb_ota.total){status=6;usb_ota_abort(-6);goto reply;}
        unsigned char actual[32];
        if(mbedtls_sha256_finish_ret(&usb_ota.sha,actual)||memcmp(actual,usb_ota.hash,32)){status=9;usb_ota_abort(-23);goto reply;}
        result=ota_stream_finish_session(usb_ota.session,actual);
        if(result){status=usb_ota_error(result);usb_ota_abort(result);goto reply;}
        usb_ota.committed=1;usb_ota.active=0;usb_ota.session=0;
        mbedtls_sha256_free(&usb_ota.sha);usb_ota.hash_ready=0;
        usb_ota_put32(count,usb_ota.written);reply_data=count;reply_length=4;commit_ack=1;goto reply;
    }
    status=11;
reply:
    usb_ota.last_ms=rt_tick_get_millisecond();
    if(usb_ota_reply(op,status,reply_data,reply_length,epoch)){
        /* send只在IN及可选ZLP完成后返回成功；ENV提交不等于链路ACK完成。 */
        if(commit_ack&&usb_ota_live((void *)(uintptr_t)epoch))reboot_manager_request();
    }else usb_ota_abort(-101);
}

void usb_ota_receiver_init(int (*send)(const void *,unsigned,unsigned),int (*alive)(unsigned))
{
    memset(&usb_ota,0,sizeof(usb_ota));usb_ota.send=send;usb_ota.alive=alive;usb_ota.want=20;
}
void usb_ota_receiver_poll(unsigned epoch)
{
    if(usb_ota.epoch!=epoch){
        usb_ota_abort(-101);usb_ota.have=usb_ota.blocked=0;usb_ota.want=20;usb_ota.epoch=epoch;
    }
    if((usb_ota.have||usb_ota.active)&&rt_tick_get_millisecond()-usb_ota.last_ms>=USB_OTA_IDLE_MS){
        usb_ota_abort(-14);usb_ota.have=0;usb_ota.blocked=1;
    }
}
int usb_ota_receiver_owned(void){return usb_ota.locked;}
int usb_ota_receiver_input(uint8_t value,unsigned epoch)
{
    /* poll与取环形缓冲之间仍可能发生USB reset，逐字节再核对所属epoch。 */
    usb_ota_receiver_poll(epoch);
    if(!usb_ota_live((void *)(uintptr_t)epoch))return 2;
    if(usb_ota.blocked)return 1;
    if(!usb_ota.have&&value!='D')return 0;
    usb_ota.frame[usb_ota.have++]=value;usb_ota.last_ms=rt_tick_get_millisecond();
    if(usb_ota.have<=4U&&value!=(unsigned char)"DOTA"[usb_ota.have-1U]){
        /* 错误魔数不能重解释成TEST/CS等ASCII后缀，须重新建立CDC会话。 */
        usb_ota_abort(-4);usb_ota.blocked=1;usb_ota.have=0;return 2;
    }
    if(usb_ota.have==USB_OTA_HEADER){
        uint32_t op=usb_ota_u32(usb_ota.frame+4);
        unsigned length=usb_ota_u32(usb_ota.frame+12);
        /* 只放宽DATA，保留旧控制帧的通用上限及各命令自己的长度校验。 */
        unsigned limit=op==0xD102U?USB_OTA_MAX_DATA:USB_OTA_LEGACY_LIMIT;
        if(length>limit){
            usb_ota_abort(-4);usb_ota_reply(op,4,0,0,epoch);
            usb_ota.have=0;usb_ota.blocked=1;return 2;
        }
        usb_ota.want=USB_OTA_HEADER+length;
    }
    if(usb_ota.have==usb_ota.want){
        usb_ota_execute(epoch);usb_ota.have=0;usb_ota.want=20;return 2;
    }
    return 1;
}
#endif
