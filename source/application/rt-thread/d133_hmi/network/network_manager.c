#include "network_manager.h"
#include "ota_manager.h"
#include "ota_stream.h"
#include "ota_package.h"
#include "file_manager.h"
#include "sd_manager.h"
#include "system_manager.h"
#include "device_event_manager.h"
#include "boot_manager.h"
#include "display_profile_manager.h"
#include "wifi_manager.h"
#include <rtthread.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <errno.h>
#include <unistd.h>
#include <mbedtls/sha256.h>
#include <aic_crc32.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#if defined(__has_include)
#if __has_include("hmi_toolbox_key_private.h")
#include "hmi_toolbox_key_private.h"
#endif
#endif
#ifndef HMI_TOOLBOX_KEY_HEX
#define HMI_TOOLBOX_KEY_HEX ""
#endif
#define OP_INFO 0xD100
#define OP_BEGIN 0xD101
#define OP_DATA 0xD102
#define OP_COMMIT 0xD103
#define OP_SIGNATURE 0xD104
/* 新工具箱用独立健康查询，避免升级重启轮询重复触发连接提示音。 */
#define OP_HEALTH 0xD105
#define OP_STREAM_BEGIN 0xD106
#define STAGING "/sdcard/UPDATE/ota.cpio.part"
#define PACKAGE "/sdcard/UPDATE/ota.cpio"
static struct rt_mutex upload_lock;
static rt_mq_t clients;
/* 0 正在启动，1 正在监听，-1 最近一次监听尝试失败。 */
static volatile int tcp_listener_state[1];
typedef struct { int fd; } tcp_client_job_t;
/* 每个报文有绝对截止时间，避免半包持续到达或失联连接占满工作线程。 */
static int exact(int fd,void *data,unsigned size,int sending)
{
    unsigned off=0;rt_uint32_t started=rt_tick_get_millisecond();
    while(off<size){
        rt_uint32_t elapsed=rt_tick_get_millisecond()-started;
        fd_set ready;struct timeval wait;
        if(elapsed>=15000U)return -1;
        wait.tv_sec=(15000U-elapsed)/1000U;wait.tv_usec=((15000U-elapsed)%1000U)*1000U;
        FD_ZERO(&ready);FD_SET(fd,&ready);
        int result=select(fd+1,sending?NULL:&ready,sending?&ready:NULL,NULL,&wait);
        if(result<0&&errno==EINTR)continue;
        if(result<=0)return -1;
        int n=sending?send(fd,(char*)data+off,size-off,MSG_DONTWAIT):recv(fd,(char*)data+off,size-off,MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EWOULDBLOCK||errno==EAGAIN))continue;
        if(n<=0)return -1;off+=(unsigned)n;
    }
    return 0;
}
/* 整块提交 DFS，避免 stdio 将 16 KiB 下载块拆成小块写卡。 */
static int write_upload(FILE *file,const unsigned char *data,unsigned size)
{
    unsigned offset=0;
    while(offset<size){ssize_t n=write(fileno(file),data+offset,size-offset);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;offset+=(unsigned)n;}
    return 0;
}
static int reply(int fd,uint32_t op,uint32_t status,const void *body,uint32_t length)
{uint32_t h[5];memcpy(h,"LNET",4);h[1]=op;h[2]=status;h[3]=length;h[4]=crc32(crc32(0,(void*)h,16),body,length);return exact(fd,h,20,1)||exact(fd,(void*)body,length,1);}

/* 工具箱发送 32 位小写十六进制 ASCII Key；比较过程不提前退出。 */
static int toolbox_key_matches(const unsigned char *provided, size_t length)
{
    static const char expected[] = HMI_TOOLBOX_KEY_HEX;
    size_t expected_length = sizeof(expected) - 1U;
    size_t i, compare_length = length < expected_length ? length : expected_length;
    unsigned difference = (unsigned)(length ^ expected_length);

    for (i = 0; i < compare_length; i++)
        difference |= (unsigned)(provided[i] ^ (unsigned char)expected[i]);
    return difference == 0;
}

/* 设备名只参与 JSON 字符串内容，防止引号和反斜线破坏工具箱响应。 */
static int json_escape(char *out, size_t capacity, const char *value)
{
    size_t used = 0;

    if (!out || !capacity || !value)
        return -1;
    while (*value) {
        unsigned char c = (unsigned char)*value++;
        if (c == '"' || c == '\\') {
            if (used + 2U >= capacity)
                return -1;
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c < 0x20U) {
            if (used + 6U >= capacity)
                return -1;
            rt_snprintf(out + used, capacity - used, "\\u%04x", c);
            used += 6U;
        } else {
            if (used + 1U >= capacity)
                return -1;
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    return (int)used;
}

static int make_toolbox_info(char *info, size_t capacity, int include_runtime)
{
    display_profile_info_t display;
    boot_status_t boot;
    char name[HMI_DEVICE_NAME_MAX_BYTES + 1U];
    char escaped_name[HMI_DEVICE_NAME_MAX_BYTES * 2U + 8U];
    char uid[32] = {0};
    int length;

    memset(&display, 0, sizeof(display));
    display_profile_get_active(&display);
    boot_manager_get(&boot);
    system_manager_get_device_name(name, sizeof(name));
    if (wifi_manager_get_uid(uid, sizeof(uid)) < 0)
        rt_snprintf(uid, sizeof(uid), "D133CBS-UNKNOWN");
    if (json_escape(escaped_name, sizeof(escaped_name), name) < 0)
        return -1;
    if (include_runtime) {
        length = snprintf(info, capacity,
            "{\"product\":\"D133CBS\",\"board\":\"D133CBV-QFN88-V1-2\","
            "\"device_name\":\"%s\",\"uid\":\"%s\",\"version\":\"%s\","
            "\"framework\":\"RT-Thread / LVGL\",\"uptime_ms\":%lu,"
            "\"screen_resolution\":\"%ux%u\",\"touch_ready\":%u,"
            "\"slot\":\"%c\",\"next_slot\":\"%c\","
            "\"health_faults\":%lu,\"ota_port\":%u,"
            "\"sram_mb\":1,\"psram_mb\":8}",
            escaped_name, uid, HMI_VERSION, (unsigned long)rt_tick_get_millisecond(),
            (unsigned)display.width, (unsigned)display.height,
            (unsigned)display.touch_enabled, boot.current, boot.next,
            (unsigned long)boot_manager_health_faults(), D133_TCP_PORT);
    } else {
        length = snprintf(info, capacity,
            "{\"product\":\"D133CBS\",\"board\":\"D133CBS\","
            "\"device_name\":\"%s\",\"version\":\"%s\","
            "\"uid\":\"%s\",\"port\":%u}",
            escaped_name, HMI_VERSION, uid, D133_TCP_PORT);
    }
    return length >= 0 && (size_t)length < capacity ? length : -1;
}

void network_manager_status(char *out,unsigned cap)
{
    const char *tcp_state;

    if (!out || !cap)
        return;
    tcp_state = tcp_listener_state[0] > 0 ? "监听中" :
                (tcp_listener_state[0] < 0 ? "启动失败，重试中" : "正在启动");
    /* IP 状态由 Wi-Fi 专属行显示，TCP 行只报告统一服务端口。 */
    rt_snprintf(out,cap,"TCP 服务  %s  端口 %u",tcp_state,D133_TCP_PORT);
}
static void session(int fd)
{
    /* 每个连接独享报文缓冲，避免健康查询覆盖升级数据。 */
    unsigned char payload[16384];
    int owns_upload=0, info_handshake=0,receive_error=-30,stream_mode=0;
    mbedtls_sha256_context receive_hash;mbedtls_sha256_init(&receive_hash);
    uint32_t h[5],total=0,written=0;unsigned char sha[32],actual[32];char version[32]={0};FILE *file=0;int active=0,tcp_event_sent=0;
    struct timeval timeout={15,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    while(1){
        /* 细分接收失败点，健康查询可读取 ota_error，不再全部归为 -30。 */
        receive_error=-101;if(exact(fd,h,20,0))break;
        receive_error=-102;if(memcmp(h,"LNET",4)||h[3]>sizeof(payload))break;
        receive_error=-103;if(exact(fd,payload,h[3],0))break;
        receive_error=-104;if(h[4]!=crc32(crc32(0,(void*)h,16),payload,h[3])){reply(fd,h[1],9,0,0);break;}
        /* 0/1 为 ResearchToolbox 认证与设备信息；27 为无写权限的快速发现。 */
        if(h[1]==0){
            if(h[2]||!toolbox_key_matches(payload,h[3])){reply(fd,0x80000000U,13,0,0);break;}
            info_handshake=1;
            if(!tcp_event_sent){device_event_manager_notify(DEVICE_EVENT_TOOLBOX_TCP_CONNECTED);tcp_event_sent=1;}
            if(reply(fd,0x80000000U,0,0,0))break;
            continue;
        }
        if(h[1]==1){
            if(!info_handshake||h[2]||h[3]){reply(fd,0x80000001U,13,0,0);break;}
            char info[1024];
            int length = make_toolbox_info(info, sizeof(info), 1);
            if(length<0||length>=(int)sizeof(info)||reply(fd,0x80000001U,0,info,(uint32_t)length))break;
            continue;
        }
        if(h[1]==26){
            if(!info_handshake||h[2]||h[3]>HMI_DEVICE_NAME_MAX_BYTES||
               memchr(payload,0,h[3])){reply(fd,0x8000001AU,4,0,0);break;}
            if(system_manager_set_device_name((const char *)payload,h[3])!=0){
                reply(fd,0x8000001AU,5,0,0);break;
            }
            if(reply(fd,0x8000001AU,0,0,0))break;
            continue;
        }
        if(h[1]==27){
            char info[320];
            int length;
            if(h[2]||h[3]){reply(fd,0x8000001BU,4,0,0);break;}
            length=make_toolbox_info(info,sizeof(info),0);
            if(length<0||reply(fd,0x8000001BU,0,info,(uint32_t)length))break;
            continue;
        }
        if(h[1]==OP_INFO||h[1]==OP_HEALTH){
            char info[768],event_name[32],device_message[192];boot_status_t boot;rt_uint32_t event_id=0;
            if(h[1]==OP_INFO&&!tcp_event_sent){device_event_manager_notify(DEVICE_EVENT_TOOLBOX_TCP_CONNECTED);tcp_event_sent=1;}
            boot_manager_get(&boot);
            device_event_manager_snapshot(event_name,sizeof(event_name),device_message,sizeof(device_message),&event_id);
            hmi_ota_status_t ota;ota_manager_get(&ota);
            char device_name[HMI_DEVICE_NAME_MAX_BYTES + 1U];
            char escaped_name[HMI_DEVICE_NAME_MAX_BYTES * 2U + 8U];
            system_manager_get_device_name(device_name, sizeof(device_name));
            if (json_escape(escaped_name, sizeof(escaped_name), device_name) < 0) break;
            int length=snprintf(info,sizeof(info),"{\"product\":\"D133CBS\",\"hardware\":\"D133CBV-QFN88-V1-2\",\"device_name\":\"%s\",\"version\":\"" HMI_VERSION "\",\"protocol\":1,\"stream_ota\":2,\"package\":\"aic-cpio-crc\",\"slot\":\"%c\",\"next_slot\":\"%c\",\"confirmed\":%s,\"health_faults\":%lu,\"event_id\":%lu,\"event\":\"%s\",\"device_message\":\"%s\",\"ota_state\":%u,\"ota_percent\":%u,\"ota_error\":%d,\"sram_mb\":1,\"psram_mb\":8}",escaped_name,boot.current,boot.next,boot.confirmed?"true":"false",(unsigned long)boot_manager_health_faults(),(unsigned long)event_id,event_name,device_message,(unsigned)ota.state,ota.percent,ota.error);
            if(length<0||length>=(int)sizeof(info))break;
            if(reply(fd,h[1],0,info,length))break;
            continue;
        }
        if(h[1]==OP_STREAM_BEGIN){
            hmi_ota_status_t state;ota_manager_get(&state);
            if(active||h[3]<=132||h[3]>644||(state.state!=OTA_IDLE&&state.state!=OTA_FAILED)){reply(fd,h[1],3,0,0);continue;}
            memcpy(&total,payload,4);
            if(!total||total>5248U*1024U+8192U||payload[67]||payload[99]||payload[131]||strcmp((char*)payload+36,HMI_PRODUCT)||strcmp((char*)payload+68,HMI_HARDWARE)||!ota_version_newer((char*)payload+100)){reply(fd,h[1],4,0,0);continue;}
            if(rt_mutex_take(&upload_lock,0)!=RT_EOK){reply(fd,h[1],3,0,0);continue;}
            owns_upload=1;stream_mode=1;memcpy(sha,payload+4,32);memcpy(version,payload+100,32);
            device_event_manager_notify(DEVICE_EVENT_TOOLBOX_OTA_STARTED);
            int result=ota_stream_begin(total,sha,version,payload+132,h[3]-132);
            if(result){reply(fd,h[1],(uint32_t)(-result),0,0);break;}
            if(mbedtls_sha256_starts_ret(&receive_hash,0)){reply(fd,h[1],9,0,0);break;}
            written=0;active=1;if(reply(fd,h[1],0,0,0))break;continue;
        }
        /* 旧工具箱暂存协议明确拒绝，防止用户误以为已切换为直写。 */
        if(h[1]==OP_BEGIN){reply(fd,h[1],11,0,0);continue;}
        if(h[1]==OP_DATA){
            if(!active||h[2]!=written||!h[3]||h[3]>total-written){reply(fd,h[1],6,0,0);break;}
            receive_error=-105;if(stream_mode?ota_stream_feed(payload,h[3]):write_upload(file,payload,h[3])){reply(fd,h[1],5,0,0);break;}
            receive_error=-106;if(mbedtls_sha256_update_ret(&receive_hash,payload,h[3])){reply(fd,h[1],9,0,0);break;}
            written+=h[3];ota_manager_receive_progress(written,total);
            receive_error=-107;if(reply(fd,h[1],0,&written,4))break;continue;
        }
                if(h[1]==OP_SIGNATURE){
            if(stream_mode){reply(fd,h[1],11,0,0);break;}
            if(!active||!h[3]||h[3]>512||file_manager_write(STAGING ".sig",payload,h[3])){reply(fd,h[1],9,0,0);break;}
            reply(fd,h[1],0,0,0);continue;
        }
        if(h[1]==OP_COMMIT){
            if(!active||written!=total||h[3]){reply(fd,h[1],6,0,0);break;}
            if(stream_mode){
                active=0;
                if(mbedtls_sha256_finish_ret(&receive_hash,actual)||memcmp(sha,actual,32)||ota_stream_finish(actual)){reply(fd,h[1],9,0,0);break;}
                reply(fd,h[1],0,&written,4);rt_thread_mdelay(500);ota_manager_reboot();break;
            }
            int close_result=fclose(file);file=0;active=0;
            if(close_result||mbedtls_sha256_finish_ret(&receive_hash,actual)||memcmp(sha,actual,32)){reply(fd,h[1],9,0,0);break;}
            /* 完整收到并校验摘要后才替换 SD 暂存包；官方 OTA 再次预检后才写 Flash。 */
            remove(PACKAGE);remove(PACKAGE ".sig");if(rename(STAGING ".sig",PACKAGE ".sig")||rename(STAGING,PACKAGE)||ota_manager_request(PACKAGE,sha,version,1)){reply(fd,h[1],3,0,0);break;}
            hmi_ota_status_t state;unsigned wait;
            for(wait=0;wait<1800;wait++){ota_manager_get(&state);if(state.state==OTA_READY||state.state==OTA_FAILED)break;rt_thread_mdelay(100);}
            if(wait==1800||state.state!=OTA_READY){reply(fd,h[1],wait==1800?14:(uint32_t)(-state.error),0,0);break;}
            reply(fd,h[1],0,&written,4);rt_thread_mdelay(500);ota_manager_reboot();break;
        }
        reply(fd,h[1],11,0,0);break;
    }
    if(file)fclose(file);
    if(stream_mode){hmi_ota_status_t state;ota_manager_get(&state);if(state.state!=OTA_READY)ota_stream_abort(receive_error);}
    mbedtls_sha256_free(&receive_hash);
    /* 连接中断或接收校验失败时解除占用，已进入官方升级阶段则不改变状态。 */
    if(owns_upload){
        char fault[128];rt_snprintf(fault,sizeof(fault),"OTA接收退出：阶段=%d 已收=%lu/%lu errno=%d",receive_error,(unsigned long)written,(unsigned long)total,errno);
        system_manager_log(fault);ota_manager_receive_abort(receive_error);
    }
    if(owns_upload)rt_mutex_release(&upload_lock);
    /* 断线只保留未激活的暂存文件，绝不提交 ENV。 */
}
/* 一个连接等待校验/写入时，另一个连接仍可查询状态。 */
static void client_worker(void *unused)
{
    tcp_client_job_t job;
    while(1){if(rt_mq_recv(clients,&job,sizeof(job),RT_WAITING_FOREVER)==RT_EOK){session(job.fd);closesocket(job.fd);}}
}
static void server(void *unused)
{
    const unsigned port=D133_TCP_PORT;
    const unsigned index = 0;
    while(1) {
        int listener = socket(AF_INET,SOCK_STREAM,0);
        struct sockaddr_in addr;
        int yes=1;
        if(listener < 0) {
            if(tcp_listener_state[index] != -1)
                system_manager_log("D133 TCP 创建失败，稍后重试");
            tcp_listener_state[index] = -1;
            rt_thread_mdelay(1000);
            continue;
        }
        setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
        memset(&addr,0,sizeof(addr));addr.sin_family=AF_INET;addr.sin_port=htons(port);addr.sin_addr.s_addr=INADDR_ANY;
        if(bind(listener,(void*)&addr,sizeof(addr)) || listen(listener,2)) {
            closesocket(listener);
            if(tcp_listener_state[index] != -1)
                system_manager_log("D133 TCP 5000 绑定失败，稍后重试");
            tcp_listener_state[index] = -1;
            rt_thread_mdelay(1000);
            continue;
        }
        tcp_listener_state[index] = 1;
        system_manager_log("D133 TCP 正在监听 5000（系统信息与 OTA）");
        while(1) {
            tcp_client_job_t job;
            job.fd=accept(listener,0,0);
            if(job.fd<0) break;
            /* 小响应包立即发送，避免逐块确认被 Nagle 延迟。 */
            setsockopt(job.fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
            if(rt_mq_send(clients,&job,sizeof(job))!=RT_EOK) closesocket(job.fd);
        }
        closesocket(listener);
        tcp_listener_state[index] = -1;
        system_manager_log("D133 TCP 监听关闭，正在重启");
        rt_thread_mdelay(1000);
    }
}
int network_manager_init(void)
{
    unsigned i;
    rt_mutex_init(&upload_lock,"otaupload",RT_IPC_FLAG_PRIO);
    clients=rt_mq_create("tcpjobs",sizeof(tcp_client_job_t),2,RT_IPC_FLAG_FIFO);if(!clients)return -1;
    for(i=0;i<2;i++){rt_thread_t worker=rt_thread_create(i?"tcpcli1":"tcpcli0",client_worker,0,32768,23,10);if(!worker)return -1;if(rt_thread_startup(worker)!=RT_EOK)return -1;}
    rt_thread_t t;
    t=rt_thread_create("hmi_tcp",server,RT_NULL,4096,23,10);return t?rt_thread_startup(t):-1;
}



