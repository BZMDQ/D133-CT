#ifndef HMI_NETWORK_MANAGER_H
#define HMI_NETWORK_MANAGER_H
#define D133_TCP_PORT 5000
#define D133_INFO_TCP_PORT D133_TCP_PORT
int network_manager_init(void);
void network_manager_status(char *out,unsigned capacity);
#endif

