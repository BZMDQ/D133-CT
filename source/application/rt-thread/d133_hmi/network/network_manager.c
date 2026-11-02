#include "network_manager.h"
#include "ota_manager.h"
#include "ota_package.h"
#include "file_manager.h"
#include "sd_manager.h"
#include "system_manager.h"
#include "device_event_manager.h"
#include "boot_manager.h"
#include <rtthread.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <netdev.h>
#include <aic_crc32.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#define OP_INFO 0xD100
#define OP_BEGIN 0xD101
#define OP_DATA 0xD102
#define OP_COMMIT 0xD103
#define OP_SIGNATURE 0xD104
/* 新工具箱用独立健康查询，避免升级重启轮询重复触发连接提示音。 */
#define OP_HEALTH 0xD105
#define STAGING "/sdcard/UPDATE/ota.cpio.part"
#define PACKAGE "/sdcard/UPDATE/ota.cpio"
static unsigned char payload[16384];
static int exact(int fd,void *data,unsigned size,int sending)
{unsigned off=0;while(off<size){int n=sending?send(fd,(char*)data+off,size-off,0):recv(fd,(char*)data+off,size-off,0);if(n<=0)return -1;off+=n;}return 0;}
static int reply(int fd,uint32_t op,uint32_t status,const void *body,uint32_t length)
{uint32_t h[5];memcpy(h,"LNET",4);h[1]=op;h[2]=status;h[3]=length;h[4]=crc32(crc32(0,(void*)h,16),body,length);return exact(fd,h,20,1)||exact(fd,(void*)body,length,1);}
void network_manager_status(char *out,unsigned cap)
{if(netif_default)rt_snprintf(out,cap,"以太网%s | IP：%s | TCP %u",netif_is_link_up(netif_default)?"已连接":"未连接",ipaddr_ntoa(&netif_default->ip_addr),D133_TCP_PORT);else rt_snprintf(out,cap,"以太网初始化中 | TCP %u",D133_TCP_PORT);}
static void session(int fd)
{
    uint32_t h[5],total=0,written=0;unsigned char sha[32],actual[32];char version[32]={0};FILE *file=0;int active=0,tcp_event_sent=0;
    struct timeval timeout={120,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    while(!exact(fd,h,20,0)){
        if(memcmp(h,"LNET",4)||h[3]>sizeof(payload))break;
        if(exact(fd,payload,h[3],0))break;
        if(h[4]!=crc32(crc32(0,(void*)h,16),payload,h[3])){reply(fd,h[1],9,0,0);break;}
        if(h[1]==OP_INFO||h[1]==OP_HEALTH){
            char info[512],event_name[32],device_message[192];boot_status_t boot;rt_uint32_t event_id=0;
            if(h[1]==OP_INFO&&!tcp_event_sent){device_event_manager_notify(DEVICE_EVENT_TOOLBOX_TCP_CONNECTED);tcp_event_sent=1;}
            boot_manager_get(&boot);
            device_event_manager_snapshot(event_name,sizeof(event_name),device_message,sizeof(device_message),&event_id);
            int length=snprintf(info,sizeof(info),"{\"product\":\"D133CBS\",\"hardware\":\"D133CBV-QFN88-V1-2\",\"version\":\"" HMI_VERSION "\",\"protocol\":1,\"package\":\"aic-cpio-crc\",\"slot\":\"%c\",\"next_slot\":\"%c\",\"confirmed\":%s,\"event_id\":%lu,\"event\":\"%s\",\"device_message\":\"%s\"}",boot.current,boot.next,boot.confirmed?"true":"false",(unsigned long)event_id,event_name,device_message);
            if(reply(fd,h[1],0,info,length))break;
            continue;
        }
        if(h[1]==OP_BEGIN){
            hmi_ota_status_t state;ota_manager_get(&state);uint64_t capacity,free_bytes;
            if(active||h[3]!=132||(state.state!=OTA_IDLE&&state.state!=OTA_FAILED)){reply(fd,h[1],3,0,0);continue;}
            memcpy(&total,payload,4);
            if(!total||total>7*1024*1024+8192||payload[67]||payload[99]||payload[131]||strcmp((char*)payload+36,HMI_PRODUCT)||strcmp((char*)payload+68,HMI_HARDWARE)||!ota_version_newer((char*)payload+100)){reply(fd,h[1],4,0,0);continue;}
            if(!sd_manager_ready()||file_manager_capacity("/sdcard",&capacity,&free_bytes)||free_bytes<total+65536){reply(fd,h[1],5,0,0);continue;}
            memcpy(sha,payload+4,32);memcpy(version,payload+100,32);mkdir("/sdcard/UPDATE",0777);file=fopen(STAGING,"wb");if(!file){reply(fd,h[1],5,0,0);continue;}
            written=0;active=1;
            if(!tcp_event_sent){device_event_manager_notify(DEVICE_EVENT_TOOLBOX_TCP_CONNECTED);tcp_event_sent=1;}
            device_event_manager_notify(DEVICE_EVENT_TOOLBOX_OTA_STARTED);
            {
                char ack[320],event_name[32],device_message[192];rt_uint32_t event_id=0;
                device_event_manager_snapshot(event_name,sizeof(event_name),device_message,sizeof(device_message),&event_id);
                int ack_length=snprintf(ack,sizeof(ack),"{\"event_id\":%lu,\"event\":\"%s\",\"device_message\":\"%s\"}",(unsigned long)event_id,event_name,device_message);
                if(reply(fd,h[1],0,ack,(uint32_t)ack_length))break;
            }
            continue;
        }
        if(h[1]==OP_DATA){
            if(!active||h[2]!=written||!h[3]||h[3]>total-written){reply(fd,h[1],6,0,0);break;}
            if(fwrite(payload,1,h[3],file)!=h[3]){reply(fd,h[1],5,0,0);break;}written+=h[3];if(reply(fd,h[1],0,&written,4))break;continue;
        }
                if(h[1]==OP_SIGNATURE){
            if(!active||!h[3]||h[3]>512||file_manager_write(STAGING ".sig",payload,h[3])){reply(fd,h[1],9,0,0);break;}
            reply(fd,h[1],0,0,0);continue;
        }
        if(h[1]==OP_COMMIT){
            if(!active||written!=total||h[3]){reply(fd,h[1],6,0,0);break;}
            int close_result=fclose(file);file=0;active=0;
            if(close_result||ota_package_sha256(STAGING,actual)||memcmp(sha,actual,32)){reply(fd,h[1],9,0,0);break;}
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
    /* 断线只保留未激活的暂存文件，绝不提交 ENV。 */
}
static void server(void *unused)
{
    int listener=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in addr;int yes=1;
    if(listener<0){system_manager_log("TCP 服务套接字创建失败");return;}setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));memset(&addr,0,sizeof(addr));addr.sin_family=AF_INET;addr.sin_port=htons(D133_TCP_PORT);addr.sin_addr.s_addr=INADDR_ANY;
    if(bind(listener,(void*)&addr,sizeof(addr))||listen(listener,1)){closesocket(listener);system_manager_log("TCP 服务端口绑定或监听失败");return;}
    system_manager_log("D133CBS 工具箱 OTA 服务已启动，端口 8899");while(1){int fd=accept(listener,0,0);if(fd<0){rt_thread_mdelay(100);continue;}session(fd);closesocket(fd);}
}
typedef struct {char ip[16],mask[16],gateway[16];int dhcp;} network_config_t;
static rt_mq_t config_queue;
static void configure_worker(void *unused)
{
    network_config_t c;ip_addr_t ip,mask,gw;
    while(1){rt_mq_recv(config_queue,&c,sizeof(c),RT_WAITING_FOREVER);
        if(!netdev_default){system_manager_log("网络设备尚未就绪");continue;}
        if(c.dhcp){netdev_dhcp_enabled(netdev_default,RT_TRUE);system_manager_log("已启用 DHCP，等待获取地址");continue;}
        if(!ipaddr_aton(c.ip,&ip)||!ipaddr_aton(c.mask,&mask)||!ipaddr_aton(c.gateway,&gw)){system_manager_log("IPv4 参数无效");continue;}
        netdev_dhcp_enabled(netdev_default,RT_FALSE);netdev_set_ipaddr(netdev_default,&ip);netdev_set_netmask(netdev_default,&mask);netdev_set_gw(netdev_default,&gw);system_manager_log("静态 IPv4 参数已应用（仅本次运行有效）");
    }
}
int network_manager_configure(const char *ip,const char *mask,const char *gw,int dhcp)
{
    network_config_t c;memset(&c,0,sizeof(c));if(!ip||!mask||!gw||strlen(ip)>15||strlen(mask)>15||strlen(gw)>15)return -1;
    snprintf(c.ip,16,"%s",ip);snprintf(c.mask,16,"%s",mask);snprintf(c.gateway,16,"%s",gw);c.dhcp=dhcp;return config_queue?rt_mq_send(config_queue,&c,sizeof(c)):-1;
}
int network_manager_init(void)
{
    rt_thread_t t;config_queue=rt_mq_create("netcfg",sizeof(network_config_t),2,RT_IPC_FLAG_FIFO);if(!config_queue)return -1;
    t=rt_thread_create("hmi_netcfg",configure_worker,0,3072,24,10);if(!t)return -1;rt_thread_startup(t);
    t=rt_thread_create("hmi_tcp",server,0,8192,23,10);return t?rt_thread_startup(t):-1;
}



