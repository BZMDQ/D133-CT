#ifndef HMI_BMP_READER_H
#define HMI_BMP_READER_H
#include <rtthread.h>
#include <stdio.h>
#include <stdint.h>
#define BMP_READ_ALIGNMENT 4096U
#define BMP_READ_PADDING (2U * BMP_READ_ALIGNMENT)

/* 请求与结果通过信号量交接，文件仅由读取线程访问。 */
typedef struct {
    FILE *file;
    uint64_t file_size;
    size_t prefix;
    struct rt_semaphore request, complete, stopped;
    uint8_t *buffer;
    long offset;
    size_t size;
    int result;
    volatile int stop;
    int active;
} bmp_reader_t;

int bmp_reader_start(bmp_reader_t *reader, FILE *file, uint64_t file_size);
/* 缓冲需按 BMP_READ_ALIGNMENT 对齐，容量至少 size + BMP_READ_PADDING。
 * 返回的 prefix 是实际像素在缓冲中的偏移，末尾读取不越过文件。 */
int bmp_reader_read(FILE *file, uint8_t *buffer, long offset, size_t size,
                    uint64_t file_size, size_t *prefix);
void bmp_reader_submit(bmp_reader_t *reader, uint8_t *buffer, long offset, size_t size);
int bmp_reader_wait(bmp_reader_t *reader);
void bmp_reader_stop(bmp_reader_t *reader);
#endif
