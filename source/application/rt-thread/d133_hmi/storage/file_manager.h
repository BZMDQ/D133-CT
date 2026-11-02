#ifndef HMI_FILE_MANAGER_H
#define HMI_FILE_MANAGER_H
#include <stddef.h>
#include <stdint.h>
#define FM_PATH_MAX 256
#define FM_LIST_MAX 32
/* 文件服务错误码：0 成功，负值表示参数、路径或 IO 错误。 */
typedef struct { char name[256]; uint32_t size; int directory; } fm_entry_t;
typedef struct { unsigned count; int truncated; fm_entry_t entries[FM_LIST_MAX]; } fm_list_t;
int file_manager_list(const char *path, fm_list_t *out);
int file_manager_read(const char *path, void *buf, size_t capacity, size_t *read_size);
int file_manager_write(const char *path, const void *buf, size_t size);
int file_manager_copy(const char *from, const char *to);
int file_manager_delete(const char *path);
int file_manager_mkdir(const char *path);
int file_manager_rename(const char *from, const char *to);
int file_manager_size(const char *path, uint32_t *size);
int file_manager_capacity(const char *path, uint64_t *total, uint64_t *free_bytes);
#endif

