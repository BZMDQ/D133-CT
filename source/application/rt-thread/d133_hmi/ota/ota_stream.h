#ifndef HMI_OTA_STREAM_H
#define HMI_OTA_STREAM_H
#include <stdint.h>
/* 调用者持有升级事务锁；仅官方 OTA 引擎负责分区擦写。 */
int ota_stream_begin(uint32_t total,const unsigned char hash[32],const char *version,const unsigned char *signature,unsigned signature_size);
int ota_stream_feed(const unsigned char *data,unsigned length);
int ota_stream_finish(const unsigned char actual_hash[32]);
void ota_stream_abort(int error);
/* 非零令牌标识真实receive_begin预约；失败/断线只能清理自己的事务。
 * 调用者仍须持有upload_lock。alive用于确认等待及ENV提交前检查断线。 */
int ota_stream_begin_session(uint32_t total,const unsigned char hash[32],const char *version,const unsigned char *signature,unsigned signature_size,uint32_t *session,int (*alive)(void *),void *context);
/* 仅限已持有上传锁的物理USB CDC接收器，alive必须核对当前epoch。 */
int ota_stream_begin_usb_session(uint32_t total,const unsigned char hash[32],
                                 const char *version,const unsigned char *signature,
                                 unsigned signature_size,uint32_t *session,
                                 int (*alive)(void *),void *context);
int ota_stream_feed_session(uint32_t session,const unsigned char *data,unsigned length);
int ota_stream_finish_session(uint32_t session,const unsigned char actual_hash[32]);
int ota_stream_abort_session(uint32_t session,int error);
#endif
