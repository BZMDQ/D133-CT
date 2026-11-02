#ifndef HMI_STORAGE_MANAGER_H
#define HMI_STORAGE_MANAGER_H
#include "file_manager.h"
typedef enum {ST_LIST,ST_COPY,ST_DELETE,ST_MKDIR,ST_RENAME,ST_READ,ST_WRITE,ST_CAPACITY} storage_op_t;
typedef struct {storage_op_t op;char path[FM_PATH_MAX],target[FM_PATH_MAX];char data[512];unsigned length;} storage_request_t;
typedef struct {storage_op_t op;int error;fm_list_t list;char data[512];unsigned length;uint64_t total,free_bytes;} storage_result_t;
int storage_manager_init(void);
int storage_manager_submit(const storage_request_t *request);
int storage_manager_poll(storage_result_t *result);
#endif
