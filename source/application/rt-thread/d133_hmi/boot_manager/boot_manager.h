#ifndef HMI_BOOT_MANAGER_H
#define HMI_BOOT_MANAGER_H
/* 槽位快照由管理器缓存，UI 不直接打开 ENV。 */
typedef struct {char current,next,resource;int pending,confirmed;} boot_status_t;
int boot_manager_init(void);
void boot_manager_ui_heartbeat(void);
void boot_manager_get(boot_status_t *out);
int boot_manager_confirm(void);
int boot_manager_refresh(void);
#endif

