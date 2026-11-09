#ifndef HMI_OTA_STREAM_H
#define HMI_OTA_STREAM_H
#include <stdint.h>
/* 调用者持有升级事务锁；仅官方 OTA 引擎负责分区擦写。 */
int ota_stream_begin(uint32_t total,const unsigned char hash[32],const char *version,const unsigned char *signature,unsigned signature_size);
int ota_stream_feed(const unsigned char *data,unsigned length);
int ota_stream_finish(const unsigned char actual_hash[32]);
void ota_stream_abort(int error);
#endif

