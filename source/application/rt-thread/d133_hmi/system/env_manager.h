#ifndef HMI_ENV_MANAGER_H
#define HMI_ENV_MANAGER_H

#include <stddef.h>

/* 通过 ArtInChip 冗余 ENV 接口读写 HMI 参数，避免模块并发操作 ENV 缓冲区。 */
int env_manager_init(void);
int env_manager_get(const char *name, char *value, size_t capacity);
int env_manager_get_many(const char *const *names, char *const *values,
                         const size_t *capacities, size_t count);
int env_manager_set(const char *name, const char *value);
int env_manager_set_many(const char *const *names, const char *const *values,
                         size_t count);

#endif
