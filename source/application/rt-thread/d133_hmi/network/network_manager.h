#ifndef HMI_NETWORK_MANAGER_H
#define HMI_NETWORK_MANAGER_H
#define D133_TCP_PORT 8899
#define D133_INFO_TCP_PORT 5000
int network_manager_init(void);
int network_manager_configure(const char *ip,const char *mask,const char *gateway,int dhcp);
void network_manager_status(char *out,unsigned capacity);
#endif

