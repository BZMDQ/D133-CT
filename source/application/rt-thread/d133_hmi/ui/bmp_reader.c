#include "bmp_reader.h"
#include <string.h>

static void bmp_reader_thread(void *parameter)
{
    bmp_reader_t *r = (bmp_reader_t *)parameter;
    long next = -1;
    for (;;) {
        rt_sem_take(&r->request, RT_WAITING_FOREVER);
        if (r->stop)
            break;
        r->result = ((r->offset == next || fseek(r->file, r->offset, SEEK_SET) == 0) &&
                     fread(r->buffer, 1, r->size, r->file) == r->size) ? 0 : -1;
        next = r->result ? -1 : r->offset + (long)r->size;
        rt_sem_release(&r->complete);
    }
    rt_sem_release(&r->stopped);
}

int bmp_reader_start(bmp_reader_t *r, FILE *file)
{
    rt_thread_t thread;
    memset(r, 0, sizeof(*r));
    r->file = file;
    if (rt_sem_init(&r->request, "brreq", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
        return -1;
    if (rt_sem_init(&r->complete, "brdone", 0, RT_IPC_FLAG_FIFO) != RT_EOK) {
        rt_sem_detach(&r->request);
        return -1;
    }
    if (rt_sem_init(&r->stopped, "brstop", 0, RT_IPC_FLAG_FIFO) != RT_EOK) {
        rt_sem_detach(&r->complete);
        rt_sem_detach(&r->request);
        return -1;
    }
    /* 与转换线程同优先级，SD 等待期间允许转换继续运行。 */
    thread = rt_thread_create("bmpread", bmp_reader_thread, r, 4096, 26, 2);
    if (!thread) {
        rt_sem_detach(&r->stopped);
        rt_sem_detach(&r->complete);
        rt_sem_detach(&r->request);
        return -1;
    }
    if (rt_thread_startup(thread) != RT_EOK) {
        rt_thread_delete(thread);
        rt_sem_detach(&r->stopped);
        rt_sem_detach(&r->complete);
        rt_sem_detach(&r->request);
        return -1;
    }
    r->active = 1;
    return 0;
}

void bmp_reader_submit(bmp_reader_t *r, uint8_t *buffer, long offset, size_t size)
{
    r->buffer = buffer;
    r->offset = offset;
    r->size = size;
    rt_sem_release(&r->request);
}

int bmp_reader_wait(bmp_reader_t *r)
{
    rt_sem_take(&r->complete, RT_WAITING_FOREVER);
    return r->result;
}

void bmp_reader_stop(bmp_reader_t *r)
{
    if (!r->active)
        return;
    r->stop = 1;
    rt_sem_release(&r->request);
    rt_sem_take(&r->stopped, RT_WAITING_FOREVER);
    rt_sem_detach(&r->stopped);
    rt_sem_detach(&r->complete);
    rt_sem_detach(&r->request);
    r->active = 0;
}
