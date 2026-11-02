#include "storage_manager.h"
#include "sd_manager.h"
#include <rtthread.h>
#include <string.h>
static rt_mq_t jobs,results;
/* 请求和结果均按值复制；工作线程不持有任何 LVGL 对象。 */
static void worker(void *arg)
{
    storage_request_t q;static storage_result_t r;size_t n;
    while(1){if(rt_mq_recv(jobs,&q,sizeof(q),RT_WAITING_FOREVER)!=RT_EOK)continue;
        memset(&r,0,sizeof(r));r.op=q.op;
        switch(q.op){
        case ST_LIST:r.error=file_manager_list(q.path,&r.list);break;
        case ST_COPY:r.error=file_manager_copy(q.path,q.target);break;
        case ST_DELETE:r.error=file_manager_delete(q.path);break;
        case ST_MKDIR:r.error=file_manager_mkdir(q.path);break;
        case ST_RENAME:r.error=file_manager_rename(q.path,q.target);break;
        case ST_READ:n=0;r.error=file_manager_read(q.path,r.data,sizeof(r.data)-1,&n);r.length=n;break;
        case ST_WRITE:r.error=q.length<=sizeof(q.data)?file_manager_write(q.path,q.data,q.length):-1;break;
        case ST_CAPACITY:r.error=file_manager_capacity(q.path,&r.total,&r.free_bytes);break;
        default:r.error=-1;break;}
        rt_kprintf("[storage] op=%d result=%d %s\n",q.op,r.error,q.path);
        /* 结果队列背压仅等待工作线程，LVGL 始终非阻塞轮询。 */
        while(rt_mq_send(results,&r,sizeof(r))!=RT_EOK)rt_thread_mdelay(20);
    }
}
int storage_manager_init(void)
{
    rt_thread_t t;sd_manager_init();jobs=rt_mq_create("fsjobs",sizeof(storage_request_t),4,RT_IPC_FLAG_FIFO);
    results=rt_mq_create("fsdone",sizeof(storage_result_t),4,RT_IPC_FLAG_FIFO);
    if(!jobs||!results)return -1;
    t=rt_thread_create("hmi_fs",worker,0,4096,22,10);if(!t)return -1;return rt_thread_startup(t);
}
static int safe_path(const char *p)
{
    return memchr(p,0,FM_PATH_MAX)&&!strncmp(p,"/sdcard",7)&&(p[7]==0||p[7]=='/')&&!strstr(p,"/../")&&!strstr(p,"/..")&&!strstr(p,"/./");
}
int storage_manager_submit(const storage_request_t *q)
{
    if(!q||!jobs||!safe_path(q->path))return -1;
    if((q->op==ST_COPY||q->op==ST_RENAME)&&!safe_path(q->target))return -1;
    /* OTA 专用暂存目录由升级服务独占，文件页不能修改正在验证的升级包。 */
    if(q->op!=ST_LIST&&q->op!=ST_READ&&q->op!=ST_CAPACITY){
        if(!strcmp(q->path,"/sdcard")||!strncmp(q->path,"/sdcard/UPDATE",14)||!strncmp(q->target,"/sdcard/UPDATE",14))return -5;
    }
    return rt_mq_send(jobs,q,sizeof(*q));
}
int storage_manager_poll(storage_result_t *r){return r&&results?rt_mq_recv(results,r,sizeof(*r),0):-1;}


