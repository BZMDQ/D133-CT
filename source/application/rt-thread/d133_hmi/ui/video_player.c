#include "video_player.h"
#include "system_manager.h"
#include <rtthread.h>
#include <aic_player.h>
#include <aic_core.h>
#include <mpp_dec_type.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>

#define VIDEO_MAX_WIDTH 640U
#define VIDEO_MAX_HEIGHT 480U
#define VIDEO_PATH_SIZE 256

static struct rt_mutex video_lock;
static rt_mq_t video_jobs;
static volatile int stop_requested;
static volatile int pause_requested;
static video_state_t state = VIDEO_IDLE;
static unsigned frame_width, frame_height, frame_sequence;
static uint8_t *frame_buffer;
static char state_text[128] = "视频播放器未启动";

static void set_state(video_state_t next, const char *text)
{
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    state = next;
    rt_snprintf(state_text, sizeof(state_text), "%s", text);
    rt_mutex_release(&video_lock);
    if (next == VIDEO_ERROR) system_manager_log(text);
}

static void video_worker(void *parameter)
{
    char path[VIDEO_PATH_SIZE];
    (void)parameter;
    while (rt_mq_recv(video_jobs, path, sizeof(path), RT_WAITING_FOREVER) == RT_EOK) {
        struct aic_player *player = RT_NULL;
        struct av_media_info media = {0};
        uint8_t *pixels = RT_NULL;
        unsigned width = 0, height = 0;
        int started = 0, failed = 0, paused = 0;
        int64_t first_pts = -1;
        rt_uint32_t start_ms = 0;
        set_state(VIDEO_LOADING, "正在打开视频……");
        player = aic_player_create(RT_NULL);
        if (!player || aic_player_set_uri(player, path) || aic_player_prepare_sync(player) ||
            aic_player_get_media_info(player, &media) || !media.has_video || media.has_audio ||
            media.video_stream.width <= 0 || media.video_stream.height <= 0 ||
            media.video_stream.width > VIDEO_MAX_WIDTH || media.video_stream.height > VIDEO_MAX_HEIGHT ||
            (media.video_stream.width & 1)) {
            set_state(VIDEO_ERROR, "视频不支持：请使用不超过 640×480 的无音频 AVI/MJPEG");
            failed = 1;
            goto cleanup;
        }
        width = (unsigned)media.video_stream.width;
        height = (unsigned)media.video_stream.height;
        if (stop_requested) goto cleanup;
        {
            int start_result = aic_player_start(player);
            if (start_result) {
                rt_size_t total = 0, used = 0, peak = 0;
                char diagnostic[128];
                /* 返回码来自 SDK；系统堆与 DMA 内存是不同统计范围。 */
                rt_memory_info(&total, &used, &peak);
                rt_snprintf(diagnostic, sizeof(diagnostic),
                            "解码器启动失败：%d\n系统堆空闲：%lu KiB  视频：%ux%u",
                            start_result, (unsigned long)((total >= used ? total - used : 0) / 1024U),
                            width, height);
                set_state(VIDEO_ERROR, diagnostic);
                failed = 1; goto cleanup;
            }
        }
        started = 1;
        set_state(VIDEO_PLAYING, "正在播放");
        while (!stop_requested) {
            struct mpp_frame frame = {0};
            if (pause_requested) {
                pause_requested = 0;
                if (!paused && aic_player_pause(player) == 0) {
                    paused = 1; set_state(VIDEO_PAUSED, "已暂停");
                } else if (paused && aic_player_play(player) == 0) {
                    paused = 0; first_pts = -1; set_state(VIDEO_PLAYING, "正在播放");
                }
            }
            if (paused) { rt_thread_mdelay(20); continue; }
            int result = aic_player_get_frame(player, &frame);
            if (result < 0) { set_state(VIDEO_ERROR, "视频解码失败或 SD 卡已移除"); failed = 1; break; }
            if (result > 0) { rt_thread_mdelay(5); continue; }
            if (frame.flags & FRAME_FLAG_EOS) { aic_player_put_frame(player, &frame); break; }
            if (!frame.buf.phy_addr[0] || frame.buf.buf_type != MPP_PHY_ADDR ||
                frame.buf.format != MPP_FMT_RGB_565 || frame.buf.stride[0] < width * 2U ||
                frame.buf.size.width < width || frame.buf.size.height < height) {
                aic_player_put_frame(player, &frame);
                set_state(VIDEO_ERROR, "视频帧格式不受支持"); failed = 1; break;
            }
            /* 首帧出现后再申请交换缓存并通知 LVGL，避免显示缓存抢占解码器启动内存。 */
            if (!pixels) {
                pixels = rt_malloc(width * height * 2U);
                if (!pixels) {
                    aic_player_put_frame(player, &frame);
                    set_state(VIDEO_ERROR, "首帧已解码，但视频交换缓冲内存不足");
                    failed = 1; break;
                }
            }
            if (frame.pts >= 0) {
                if (first_pts < 0) { first_pts = frame.pts; start_ms = rt_tick_get_millisecond(); }
                while (!stop_requested && frame.pts >= first_pts &&
                       (frame.pts - first_pts) / 1000 > (rt_uint32_t)(rt_tick_get_millisecond() - start_ms) &&
                       (frame.pts - first_pts) / 1000 < 86400000LL)
                    rt_thread_mdelay(5);
            }
            aicos_dcache_invalid_range((void *)(uintptr_t)frame.buf.phy_addr[0],
                                       (int64_t)frame.buf.stride[0] * height);
            rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
            for (unsigned y = 0; y < height; y++)
                memcpy(pixels + y * width * 2U,
                       (const uint8_t *)(uintptr_t)frame.buf.phy_addr[0] + y * frame.buf.stride[0], width * 2U);
            frame_buffer = pixels; frame_width = width; frame_height = height;
            frame_sequence++;
            rt_mutex_release(&video_lock);
            aic_player_put_frame(player, &frame);
        }
        if (!stop_requested && !failed) set_state(VIDEO_ENDED, "播放完成，点击返回");
cleanup:
        if (started) aic_player_stop(player);
        if (player) aic_player_destroy(player);
        /* 保留错误提示和最后一帧，直到用户退出，避免任务交错。 */
        while (!stop_requested) rt_thread_mdelay(50);
        rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
        frame_buffer = RT_NULL; frame_width = frame_height = frame_sequence = 0;
        rt_mutex_release(&video_lock);
        if (pixels) rt_free(pixels);
        stop_requested = 0;
        pause_requested = 0;
        set_state(VIDEO_IDLE, "视频已关闭");
    }
}

int video_player_init(void)
{
    rt_thread_t thread;
    if (video_jobs) return 0;
    rt_mutex_init(&video_lock, "vidlock", RT_IPC_FLAG_PRIO);
    video_jobs = rt_mq_create("vidjobs", VIDEO_PATH_SIZE, 1, RT_IPC_FLAG_FIFO);
    if (!video_jobs) return -1;
    thread = rt_thread_create("hmi_video", video_worker, RT_NULL, 8192, 24, 10);
    if (!thread || rt_thread_startup(thread) != RT_EOK) {
        if (thread) rt_thread_delete(thread);
        rt_mq_delete(video_jobs); video_jobs = RT_NULL;
        rt_mutex_detach(&video_lock);
        return -1;
    }
    return 0;
}

int video_player_supported(const char *path)
{
    const char *ext = path ? strrchr(path, '.') : RT_NULL;
    return ext && !strcasecmp(ext, ".avi");
}

int video_player_open(const char *path)
{
    char job[VIDEO_PATH_SIZE] = {0};
    int busy;
    if (!video_jobs || !path || strncmp(path, "/sdcard/", 8) || strlen(path) >= 128) return -1;
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    busy = state != VIDEO_IDLE;
    if (!busy) state = VIDEO_LOADING;
    rt_mutex_release(&video_lock);
    if (busy) return -1;
    rt_snprintf(job, sizeof(job), "%s", path);
    if (rt_mq_send(video_jobs, job, sizeof(job)) != RT_EOK) { set_state(VIDEO_IDLE, "视频任务繁忙"); return -1; }
    return 0;
}

void video_player_close(void) { stop_requested = 1; }
void video_player_toggle_pause(void) { pause_requested = 1; }

video_state_t video_player_status(unsigned *width, unsigned *height, char *text, size_t size)
{
    video_state_t result;
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    result = state;
    if (width) *width = frame_width;
    if (height) *height = frame_height;
    if (text && size) rt_snprintf(text, size, "%s", state_text);
    rt_mutex_release(&video_lock);
    return result;
}

int video_player_copy_frame(void *dst, size_t size, unsigned *sequence)
{
    size_t bytes; int copied = 0;
    if (!dst || !sequence) return -1;
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    bytes = frame_width * frame_height * 2U;
    if (frame_buffer && frame_sequence && *sequence != frame_sequence && bytes <= size) {
        memcpy(dst, frame_buffer, bytes); *sequence = frame_sequence; copied = 1;
    }
    rt_mutex_release(&video_lock);
    return copied;
}
