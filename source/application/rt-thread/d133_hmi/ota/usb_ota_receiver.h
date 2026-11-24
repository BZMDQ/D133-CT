#ifndef HMI_USB_OTA_RECEIVER_H
#define HMI_USB_OTA_RECEIVER_H
#include <stdint.h>
/* DOTA头仍为20字节，DATA上限也用于CDC软件环的容量检查。 */
#define USB_OTA_HEADER   20U
#define USB_OTA_MAX_DATA 131072U
/* 所有入口只由usb_ctl线程调用；USB中断仅负责收发缓冲及epoch。 */
void usb_ota_receiver_init(int (*send)(const void *,unsigned,unsigned),int (*alive)(unsigned));
void usb_ota_receiver_poll(unsigned epoch);
/* 0交给原ASCII解析器，1已消费二进制片段，2已处理完整帧。 */
int usb_ota_receiver_input(uint8_t value,unsigned epoch);
int usb_ota_receiver_owned(void);
#endif
