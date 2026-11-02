#include "system_manager.h"
#include "hmi_serial_log.h"
#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static struct rt_mutex lock;static char logbuf[2048];static rt_mq_t messages;
/* 持久化由专用日志线程完成，LVGL 只写有界队列。 */
static void logger(void *arg)
{
    char line[512];struct stat st;
    while(1){rt_mq_recv(messages,line,sizeof(line),RT_WAITING_FOREVER);
        if(!stat("/data/hmi.log",&st)&&st.st_size>65536){remove("/data/hmi.log.1");rename("/data/hmi.log","/data/hmi.log.1");}
        FILE *f=fopen("/data/hmi.log","ab");if(f){fprintf(f,"%lu %s\n",(unsigned long)(rt_tick_get()/RT_TICK_PER_SECOND),line);fclose(f);}
    }
}
int system_manager_init(void)
{
    rt_thread_t t;FILE *f;rt_mutex_init(&lock,"hmilog",RT_IPC_FLAG_PRIO);
    f=fopen("/data/hmi.log","rb");if(f){fseek(f,0,SEEK_END);long n=ftell(f);if(n>2047)fseek(f,n-2047,SEEK_SET);else rewind(f);fread(logbuf,1,2047,f);fclose(f);}
    messages=rt_mq_create("hmilogs",512,8,RT_IPC_FLAG_FIFO);if(!messages)return -1;
    t=rt_thread_create("hmi_logger",logger,0,3072,25,10);if(!t)return -1;rt_thread_startup(t);system_manager_log("D133CBS 控制终端启动");return 0;
}
void system_manager_log(const char *s)
{
    char line[512];snprintf(line,sizeof(line),"%.510s",s);unsigned n=strlen(line);
    rt_mutex_take(&lock,RT_WAITING_FOREVER);if(strlen(logbuf)+n+2>=sizeof(logbuf))logbuf[0]=0;strcat(logbuf,line);strcat(logbuf,"\n");rt_mutex_release(&lock);
    if(messages)rt_mq_send(messages,line,sizeof(line));
    hmi_serial_log("[hmi] %s\n",line);
}
void system_manager_logs(char *out,unsigned n){if(!n)return;rt_mutex_take(&lock,RT_WAITING_FOREVER);rt_strncpy(out,logbuf,n-1);out[n-1]=0;rt_mutex_release(&lock);}
/* 只供 OTA/健康线程调用，必须在返回前落盘。ENV 仍是启动状态的权威来源。 */
int system_manager_record_upgrade(const char *message)
{
    int fd=open("/data/upgrade.state",O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;size_t n=strlen(message);int r=write(fd,message,n)==n?0:-1;if(!r&&fsync(fd))r=-1;if(close(fd))r=-1;return r;
}
int system_manager_upgrade_state(char *out,unsigned cap)
{
    FILE *f;if(!out||!cap)return -1;out[0]=0;f=fopen("/data/upgrade.state","rb");if(!f)return -1;size_t n=fread(out,1,cap-1,f);out[n]=0;int r=ferror(f)?-1:0;fclose(f);return r;
}


