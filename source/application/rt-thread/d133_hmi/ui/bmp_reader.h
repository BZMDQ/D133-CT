#ifndef HMI_BMP_READER_H
#define HMI_BMP_READER_H
#include <rtthread.h>
#include <stdio.h>
#include <stdint.h>

/* 请求与结果通过信号量交接，文件仅由读取线程访问。 */
typedef struct {
    FILE *file;
    struct rt_semaphore request, complete, stopped;
    uint8_t *buffer;
    long offset;
    size_t size;
    int result;
    volatile int stop;
    int active;
} bmp_reader_t;

int bmp_reader_start(bmp_reader_t *reader, FILE *file);
void bmp_reader_submit(bmp_reader_t *reader, uint8_t *buffer, long offset, size_t size);
int bmp_reader_wait(bmp_reader_t *reader);
void bmp_reader_stop(bmp_reader_t *reader);
#endif
