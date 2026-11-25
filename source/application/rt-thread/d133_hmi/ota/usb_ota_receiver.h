#ifndef HMI_USB_OTA_RECEIVER_H
#define HMI_USB_OTA_RECEIVER_H
#include <stdint.h>
/* DOTA头仍为20字节，DATA上限也用于CDC软件环的容量检查。 */
#define USB_OTA_HEADER   20U
#define USB_OTA_MAX_DATA 131072U
/* 所有入口只由usb_ctl线程调用；USB中断仅负责收发缓冲及epoch。 */
void usb_ota_receiver_init(int (*send)(const void *,unsigned,unsigned),int (*alive)(unsigned));
void usb_ota_receiver_poll(unsigned epoch);
/* 返回空闲清理的剩余毫秒数；-1 表示可以等待下一个 USB 事件。 */
int32_t usb_ota_receiver_wait_ms(void);
/* 0交给原ASCII解析器，1已消费二进制片段，2已处理完整帧。 */
int usb_ota_receiver_input(uint8_t value,unsigned epoch);
/* 批量入口只接收当前DOTA帧的负载，调用方保留后续命令的字节。 */
unsigned usb_ota_receiver_payload_remaining(void);
int usb_ota_receiver_push_payload(const uint8_t *data, unsigned length, unsigned epoch);
int usb_ota_receiver_owned(void);
#endif
