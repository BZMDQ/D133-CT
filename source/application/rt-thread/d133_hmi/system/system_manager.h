#ifndef HMI_SYSTEM_MANAGER_H
#define HMI_SYSTEM_MANAGER_H
int system_manager_init(void);
void system_manager_log(const char *message);
int system_manager_record_upgrade(const char *message);
int system_manager_upgrade_state(char *out,unsigned capacity);
void system_manager_logs(char *out,unsigned capacity);
#endif

