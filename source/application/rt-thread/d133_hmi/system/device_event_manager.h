#ifndef D133_DEVICE_EVENT_MANAGER_H
#define D133_DEVICE_EVENT_MANAGER_H

#include <rtthread.h>

typedef enum
{
    DEVICE_EVENT_NONE = 0,
    DEVICE_EVENT_TOOLBOX_TCP_CONNECTED = 1,
    DEVICE_EVENT_TOOLBOX_OTA_STARTED = 2,
} device_event_type_t;

int device_event_manager_init(void);
void device_event_manager_notify(device_event_type_t event);
void device_event_manager_snapshot(char *event_name, rt_size_t event_name_size,
                                   char *message, rt_size_t message_size,
                                   rt_uint32_t *sequence);

#endif

