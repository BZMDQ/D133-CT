#include "system_manager.h"
#include "env_manager.h"
#include "hmi_serial_log.h"
#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define HMI_DEVICE_NAME_ENV "hmi_device_name"
#define HMI_DEVICE_NAME_DEFAULT "D133-控制终端"
typedef struct { char name[HMI_DEVICE_NAME_MAX_BYTES + 1U]; } device_name_job_t;
static struct rt_mutex lock;
static struct rt_mutex device_name_lock;
static char logbuf[2048];
static char device_name[HMI_DEVICE_NAME_MAX_BYTES + 1U] = HMI_DEVICE_NAME_DEFAULT;
static rt_mq_t messages;
static rt_mq_t device_name_jobs;
static int device_name_save_state = HMI_DEVICE_NAME_SAVE_IDLE;

static int device_name_valid(const char *name, size_t length)
{
    size_t i;

    if (!name || length > HMI_DEVICE_NAME_MAX_BYTES)
        return 0;
    for (i = 0; i < length; i++) {
        unsigned char value = (unsigned char)name[i];
        if (value < 0x20U || value == 0x7FU)
            return 0;
    }
    return 1;
}

int system_manager_get_device_name(char *out, size_t capacity)
{
    size_t length;

    if (!out || !capacity)
        return -1;
    rt_mutex_take(&device_name_lock, RT_WAITING_FOREVER);
    length = strlen(device_name);
    if (length >= capacity)
        length = capacity - 1U;
    memcpy(out, device_name, length);
    out[length] = '\0';
    rt_mutex_release(&device_name_lock);
    return 0;
}

int system_manager_set_device_name(const char *name, size_t length)
{
    char value[HMI_DEVICE_NAME_MAX_BYTES + 1U];
    const char *stored_value;

    if (!device_name_valid(name, length))
        return -1;
    memcpy(value, name, length);
    value[length] = '\0';
    stored_value = length ? value : "";
    if (env_manager_set(HMI_DEVICE_NAME_ENV, stored_value) != 0)
        return -2;

    rt_mutex_take(&device_name_lock, RT_WAITING_FOREVER);
    if (length)
        memcpy(device_name, value, length + 1U);
    else
        rt_snprintf(device_name, sizeof(device_name), "%s", HMI_DEVICE_NAME_DEFAULT);
    device_name_save_state = HMI_DEVICE_NAME_SAVE_OK;
    rt_mutex_release(&device_name_lock);
    return 0;
}

static void device_name_worker(void *parameter)
{
    device_name_job_t job;

    (void)parameter;
    while (rt_mq_recv(device_name_jobs, &job, sizeof(job), RT_WAITING_FOREVER) == RT_EOK) {
        int result = system_manager_set_device_name(job.name, strlen(job.name));
        if (result)
            system_manager_log("设备名称写入 ENV 失败");
        rt_mutex_take(&device_name_lock, RT_WAITING_FOREVER);
        device_name_save_state = result ? HMI_DEVICE_NAME_SAVE_ERROR : HMI_DEVICE_NAME_SAVE_OK;
        rt_mutex_release(&device_name_lock);
        rt_memset(&job, 0, sizeof(job));
    }
}

int system_manager_request_device_name(const char *name, size_t length)
{
    device_name_job_t job = {{0}};

    if (!device_name_jobs || !device_name_valid(name, length))
        return -1;
    memcpy(job.name, name, length);
    rt_mutex_take(&device_name_lock, RT_WAITING_FOREVER);
    device_name_save_state = HMI_DEVICE_NAME_SAVE_PENDING;
    rt_mutex_release(&device_name_lock);
    if (rt_mq_send(device_name_jobs, &job, sizeof(job)) != RT_EOK) {
        rt_mutex_take(&device_name_lock, RT_WAITING_FOREVER);
        device_name_save_state = HMI_DEVICE_NAME_SAVE_ERROR;
        rt_mutex_release(&device_name_lock);
        return -2;
    }
    return 0;
}

int system_manager_device_name_save_state(void)
{
    int result;

    rt_mutex_take(&device_name_lock, RT_WAITING_FOREVER);
    result = device_name_save_state;
    rt_mutex_release(&device_name_lock);
    return result;
}

static void device_name_load(void)
{
    char value[HMI_DEVICE_NAME_MAX_BYTES + 1U] = {0};

    if (env_manager_get(HMI_DEVICE_NAME_ENV, value, sizeof(value)) == 0 && value[0] &&
        device_name_valid(value, strlen(value)))
        rt_snprintf(device_name, sizeof(device_name), "%s", value);
}

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
    rt_thread_t t, name_thread;FILE *f;
    if (env_manager_init() != 0)
        return -1;
    rt_mutex_init(&lock,"hmilog",RT_IPC_FLAG_PRIO);
    rt_mutex_init(&device_name_lock, "hmidevnm", RT_IPC_FLAG_PRIO);
    device_name_load();
    device_name_jobs = rt_mq_create("devname", sizeof(device_name_job_t), 2, RT_IPC_FLAG_FIFO);
    if (!device_name_jobs)
        return -1;
    f=fopen("/data/hmi.log","rb");if(f){fseek(f,0,SEEK_END);long n=ftell(f);if(n>2047)fseek(f,n-2047,SEEK_SET);else rewind(f);fread(logbuf,1,2047,f);fclose(f);}
    messages=rt_mq_create("hmilogs",512,8,RT_IPC_FLAG_FIFO);if(!messages)return -1;
    name_thread=rt_thread_create("hmi_name",device_name_worker,RT_NULL,2048,24,10);
    if(!name_thread||rt_thread_startup(name_thread)!=RT_EOK)return -1;
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


