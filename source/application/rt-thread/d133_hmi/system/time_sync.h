#ifndef HMI_TIME_SYNC_H
#define HMI_TIME_SYNC_H
#include <time.h>
/* 后台校时服务；返回 0 表示线程创建成功。 */
int time_sync_init(void);
/* RTC 有效时输出北京时间，失败时由界面显示未校时状态。 */
int time_sync_local_time(struct tm *local);
#endif
