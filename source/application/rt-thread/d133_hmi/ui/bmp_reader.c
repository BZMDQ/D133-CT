#include "bmp_reader.h"
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>

int bmp_reader_read(FILE *file, uint8_t *buffer, long offset, size_t size,
                    uint64_t file_size, size_t *prefix)
{
    uint64_t start, end, limit;
    int fd;
    size_t done = 0, wanted;
    if (offset < 0 || !size || (uint64_t)offset + size > file_size)
        return -1;
    /* 4 KiB 包含 SDK 支持的 512/1024/2048/4096 字节扇区。 */
    start = (uint64_t)offset & ~(uint64_t)(BMP_READ_ALIGNMENT - 1U);
    limit = (uint64_t)offset + size;
    end = (limit + BMP_READ_ALIGNMENT - 1U) & ~(uint64_t)(BMP_READ_ALIGNMENT - 1U);
    if (end > file_size)
        end = file_size;
    if (start > LONG_MAX || end - start > SIZE_MAX)
        return -1;
    *prefix = (size_t)((uint64_t)offset - start);
    /* 像素读取直接使用 DFS POSIX 接口，绕过 stdio 的分块缓冲。 */
    fd = fileno(file);
    wanted = (size_t)(end - start);
    if (fd < 0 || lseek(fd, (off_t)start, SEEK_SET) != (off_t)start)
        return -1;
    while (done < wanted) {
        ssize_t n = read(fd, buffer + done, wanted - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static void bmp_reader_thread(void *parameter)
{
    bmp_reader_t *r = (bmp_reader_t *)parameter;
    for (;;) {
        rt_sem_take(&r->request, RT_WAITING_FOREVER);
        if (r->stop)
            break;
        r->result = bmp_reader_read(r->file, r->buffer, r->offset, r->size,
                                   r->file_size, &r->prefix);
        rt_sem_release(&r->complete);
    }
    rt_sem_release(&r->stopped);
}

int bmp_reader_start(bmp_reader_t *r, FILE *file, uint64_t file_size)
{
    rt_thread_t thread;
    memset(r, 0, sizeof(*r));
    r->file = file;
    r->file_size = file_size;
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
