#ifndef HMI_VIDEO_PLAYER_H
#define HMI_VIDEO_PLAYER_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VIDEO_IDLE, VIDEO_LOADING, VIDEO_PLAYING, VIDEO_PAUSED, VIDEO_ENDED, VIDEO_ERROR, VIDEO_STOPPING
} video_state_t;

/* 所有读取和 MPP 调用由工作线程完成，界面线程只复制已完成帧。 */
int video_player_init(void);
int video_player_supported(const char *path);
int video_player_open(const char *path);
void video_player_close(void);
void video_player_toggle_pause(void);
video_state_t video_player_status(unsigned *width, unsigned *height, char *text, size_t size);
int video_player_copy_frame(void *dst, size_t size, unsigned *sequence);
/* 大画面使用单个官方 MPP 帧，界面渲染结束后归还帧。 */
int video_player_direct_mode(void);
int video_player_acquire_frame(const uint8_t **data, unsigned *stride, unsigned *sequence);
void video_player_release_frame(unsigned sequence);
#endif
