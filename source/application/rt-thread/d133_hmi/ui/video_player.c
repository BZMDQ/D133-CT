#include "video_player.h"
#include "system_manager.h"
#include <rtthread.h>
#include <aic_player.h>
#include <aic_core.h>
#include <mpp_dec_type.h>
#include <mpp_mem.h>
#include <frame_allocator.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>

#define VIDEO_MAX_WIDTH 1280U
#define VIDEO_MAX_HEIGHT 720U
#define VIDEO_COPY_LIMIT (640U * 480U)
#define VIDEO_PATH_SIZE 256
#define VIDEO_NO_FRAME_TIMEOUT_MS 3000U

static struct rt_mutex video_lock;
static rt_mq_t video_jobs;
static volatile int stop_requested;
static volatile int pause_requested;
static video_state_t state = VIDEO_IDLE;
static unsigned frame_width, frame_height, frame_sequence;
static unsigned play_position_ms,play_duration_ms;
void video_player_progress(unsigned *position_ms,unsigned *duration_ms)
{
    rt_mutex_take(&video_lock,RT_WAITING_FOREVER);
    if(position_ms)*position_ms=play_position_ms;
    if(duration_ms)*duration_ms=play_duration_ms;
    rt_mutex_release(&video_lock);
}
static uint8_t *frame_buffer;
static unsigned direct_stride;
static int direct_mode;
/* 留下 SDK 帧分配诊断，串口关闭时仍可在播放器界面查看。 */
static volatile int alloc_error, alloc_format, alloc_stride, alloc_height;
static char state_text[128] = "视频播放器未启动";

/* 大分辨率仅分配一帧物理缓存，复用 SDK 现有的外部分配接口。 */
static int direct_alloc(struct frame_allocator *allocator, struct mpp_frame *frame,
                        int stride, int height, enum mpp_pixel_format format)
{
    (void)allocator;
    alloc_format=(int)format;alloc_stride=stride;alloc_height=height;
    if (!frame || format != MPP_FMT_RGB_565 || stride <= 0 || height <= 0 ||
        (uint64_t)stride * (unsigned)height > 1280U * 736U * 2U) {
        alloc_error=1;
        return -1;
    }
    frame->buf.buf_type = MPP_PHY_ADDR;
    frame->buf.format = format;
    frame->buf.stride[0] = (unsigned)stride;
    frame->buf.size.height = (unsigned)height;
    frame->buf.phy_addr[0] = mpp_phy_alloc((size_t)stride * (unsigned)height);
    if (!frame->buf.phy_addr[0]) alloc_error=2;
    return frame->buf.phy_addr[0] ? 0 : -1;
}

static int direct_free(struct frame_allocator *allocator, struct mpp_frame *frame)
{
    (void)allocator;
    if (frame && frame->buf.phy_addr[0]) {
        mpp_phy_free(frame->buf.phy_addr[0]);
        frame->buf.phy_addr[0] = 0;
    }
    return 0;
}

static int direct_close(struct frame_allocator *allocator) { (void)allocator; return 0; }
static struct alloc_ops direct_alloc_ops = { direct_alloc, direct_free, direct_close };
static struct frame_allocator direct_allocator = { &direct_alloc_ops };

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
        int started = 0, failed = 0, paused = 0, decode_scale = 0;
        int64_t first_pts = -1, progress_origin = -1;
        rt_uint32_t start_ms = 0;
        rt_uint32_t last_frame_ms = 0;
        int first_frame_received = 0;
        alloc_error=alloc_format=alloc_stride=alloc_height=0;
        set_state(VIDEO_LOADING, "正在打开视频……");
        player = aic_player_create(RT_NULL);
        if (!player || aic_player_set_uri(player, path) || aic_player_prepare_sync(player) ||
            aic_player_get_media_info(player, &media) || !media.has_video || media.has_audio ||
            media.video_stream.width <= 0 || media.video_stream.height <= 0 ||
            media.video_stream.width > VIDEO_MAX_WIDTH || media.video_stream.height > VIDEO_MAX_HEIGHT ||
            (media.video_stream.width & 1)) {
            set_state(VIDEO_ERROR, "视频不支持：请使用不超过 1280×720 的无音频 MJPEG");
            failed = 1;
            goto cleanup;
        }
        rt_mutex_take(&video_lock,RT_WAITING_FOREVER);
        play_position_ms=0;play_duration_ms=media.duration>0?(unsigned)(media.duration/1000):0;
        rt_mutex_release(&video_lock);
        width = (unsigned)media.video_stream.width;
        height = (unsigned)media.video_stream.height;
        rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
        direct_mode = (width * height > VIDEO_COPY_LIMIT);
        rt_mutex_release(&video_lock);
        if (direct_mode) {
            /* 仅保留自动模式：大画面半尺寸硬件解码，小画面维持源尺寸。 */
            decode_scale=1;
            int extra_frames = 0;
            /* SDK 的 MJPEG 帧数为 1 + extra_frames。界面归还后再解码下一帧，
             * 因此只保留一帧，减少 720P 时约 1.76 MiB 的连续 PSRAM 需求。 */
            if (aic_player_control(player, AIC_PLAYER_CMD_SET_VDEC_EXT_FRAME_NUM, &extra_frames) ||
                aic_player_control(player, AIC_PLAYER_CMD_SET_VDEC_EXT_FRAME_ALLOCATOR, &direct_allocator)) {
                set_state(VIDEO_ERROR, "无法配置视频帧缓存"); failed = 1; goto cleanup;
            }
        }
        if (stop_requested) goto cleanup;
        {
            int start_result = aic_player_start(player);
            if (start_result) {
                rt_size_t total = 0, used = 0, peak = 0;
                char diagnostic[128];
                /* 返回码来自 SDK；系统堆与 DMA 内存是不同统计范围。 */
                rt_memory_info(&total, &used, &peak);
                rt_snprintf(diagnostic, sizeof(diagnostic),
                            "解码器启动失败：%d（仅支持 MJPEG）\n系统堆空闲：%lu KiB  视频：%ux%u",
                            start_result, (unsigned long)((total >= used ? total - used : 0) / 1024U),
                            width, height);
                set_state(VIDEO_ERROR, diagnostic);
                failed = 1; goto cleanup;
            }
        }
        started = 1;
        /* 720P 输入采用硬件半尺寸解码，避免连续 PSRAM 分配失败。 */
        if (direct_mode && decode_scale) {
            if (aic_player_set_decode_scale(player, decode_scale)) {
                set_state(VIDEO_ERROR, "无法配置硬件缩小解码"); failed=1; goto cleanup;
            }
            width >>= 1; height >>= 1;
        }
        /* 首帧等待从解码器真正启动后计时，不把 SD 打开和索引准备算进去。 */
        last_frame_ms = rt_tick_get_millisecond();
        set_state(VIDEO_PLAYING, "解码器已启动，正在等待首帧…");
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
            if (result > 0) {
                if (rt_tick_get_millisecond() - last_frame_ms > VIDEO_NO_FRAME_TIMEOUT_MS) {
                    char diagnostic[128];
                    rt_snprintf(diagnostic,sizeof(diagnostic),
                        first_frame_received ? "视频帧中断超过 3 秒（帧缓存错误 %d，格式 %d，步幅 %d，高 %d）" :
                        "3 秒未收到首帧（帧缓存错误 %d，格式 %d，步幅 %d，高 %d）",
                        alloc_error,alloc_format,alloc_stride,alloc_height);
                    if (!alloc_error) {
                        int io[5];aic_file_stream_read_stats(io);
                        rt_snprintf(diagnostic,sizeof(diagnostic),"SDK=%d 帧=%u I/O=%d\n请求=%d 返回=%d errno=%d 次数=%d",result,frame_sequence,io[0],io[1],io[2],io[3],io[4]);
                    }
                    set_state(VIDEO_ERROR, diagnostic);
                    failed = 1; break;
                }
                rt_thread_mdelay(5); continue;
            }
            if (frame.flags & FRAME_FLAG_EOS) { aic_player_put_frame(player, &frame); break; }
            if (!frame.buf.phy_addr[0] || frame.buf.buf_type != MPP_PHY_ADDR ||
                frame.buf.format != MPP_FMT_RGB_565 || frame.buf.stride[0] < width * 2U ||
                frame.buf.size.width < width || frame.buf.size.height < height ||
                (frame.buf.crop_en && (frame.buf.crop.x || frame.buf.crop.y ||
                 frame.buf.crop.width < (int)width || frame.buf.crop.height < (int)height))) {
                aic_player_put_frame(player, &frame);
                set_state(VIDEO_ERROR, "视频帧格式不受支持"); failed = 1; break;
            }
            /* 小画面使用交换缓存；大画面直接租用 MPP 帧，避免多份 720P 缓存。 */
            if (!direct_mode && !pixels) {
                /* 普通视频交换帧也放入 PSRAM CMA，避免 1 MiB SRAM 堆被大画面占满。 */
                pixels = (uint8_t *)(uintptr_t)mpp_phy_alloc(width * height * 2U);
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
            if (direct_mode) {
                frame_buffer = (uint8_t *)(uintptr_t)frame.buf.phy_addr[0];
                direct_stride = frame.buf.stride[0];
            } else {
                for (unsigned y = 0; y < height; y++)
                    memcpy(pixels + y * width * 2U,
                           (const uint8_t *)(uintptr_t)frame.buf.phy_addr[0] + y * frame.buf.stride[0], width * 2U);
                frame_buffer = pixels;
            }
            if(frame.pts>=0){
                if(progress_origin<0)progress_origin=frame.pts;
                play_position_ms=(unsigned)((frame.pts-progress_origin)/1000);
                if(play_duration_ms && play_position_ms>play_duration_ms)play_position_ms=play_duration_ms;
            }
            frame_width = width; frame_height = height;
            frame_sequence++;
            rt_mutex_release(&video_lock);
            first_frame_received = 1;
            if (direct_mode) {
                /* 持有帧直至 LVGL 绘制完成，避免解码器覆盖正在使用的像素。 */
                rt_uint32_t lease_start_ms = rt_tick_get_millisecond();
                while (!stop_requested) {
                    int leased;
                    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
                    leased = frame_buffer != RT_NULL;
                    rt_mutex_release(&video_lock);
                    if (!leased) break;
                    if (rt_tick_get_millisecond() - lease_start_ms > 1000U) {
                        set_state(VIDEO_ERROR, "显示帧等待 LVGL 超时：视频缓冲未交还");
                        failed = 1; break;
                    }
                    rt_thread_mdelay(2);
                }
                rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
                frame_buffer = RT_NULL;
                rt_mutex_release(&video_lock);
            }
            /* 归还失败必须立即报告，不能继续耗尽唯一解码帧。 */
            int return_result=aic_player_put_frame(player, &frame);
            if (return_result) {
                char error_text[96];rt_snprintf(error_text,sizeof(error_text),"SDK归还视频帧失败：%d",return_result);
                set_state(VIDEO_ERROR,error_text);failed=1;
            }
            if (failed) break;
            last_frame_ms = rt_tick_get_millisecond();
        }
        if (!stop_requested && !failed) set_state(VIDEO_ENDED, "播放完成，点击返回");
cleanup:
        if (started) aic_player_stop(player);
        if (player) aic_player_destroy(player);
        /* 保留错误提示和最后一帧，直到用户退出，避免任务交错。 */
        while (!stop_requested) rt_thread_mdelay(50);
        rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
        frame_buffer = RT_NULL; frame_width = frame_height = frame_sequence = direct_stride = 0;
        direct_mode = 0;
        if (pixels) mpp_phy_free((unsigned int)(uintptr_t)pixels);
        /* 清理完成与状态、停止标志一起发布，避免重开请求看到中间状态。 */
        stop_requested=0;pause_requested=0;state=VIDEO_IDLE;
        rt_snprintf(state_text,sizeof(state_text),"视频已关闭");
        rt_mutex_release(&video_lock);
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
    return ext && (!strcasecmp(ext, ".avi") || !strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".mov"));
}

int video_player_open(const char *path)
{
    char job[VIDEO_PATH_SIZE] = {0};
    int busy;
    if (!video_jobs || !path || strncmp(path, "/sdcard/", 8) || strlen(path) >= 128) return -1;
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    busy = state != VIDEO_IDLE;
    /* 退出正在后台清理时允许排队一次重开，不让界面阻塞等待。 */
    if (!busy) { stop_requested=0; state = VIDEO_LOADING; }
    else if (state==VIDEO_STOPPING) busy=0;
    rt_mutex_release(&video_lock);
    if (busy) return -1;
    rt_snprintf(job, sizeof(job), "%s", path);
    /* FAT 文件名大小写不敏感，SDK 按小写扩展名选择解封装器。 */
    {
        char *ext = strrchr(job, '.');
        if (ext) while (*ext) { if (*ext >= 'A' && *ext <= 'Z') *ext = (char)(*ext + ('a' - 'A')); ext++; }
    }
    if (rt_mq_send(video_jobs, job, sizeof(job)) != RT_EOK) return -1;
    return 0;
}

void video_player_close(void) {
    rt_mutex_take(&video_lock,RT_WAITING_FOREVER);
    if(state!=VIDEO_IDLE){stop_requested=1;state=VIDEO_STOPPING;}
    rt_mutex_release(&video_lock);
}
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
    if (!direct_mode && frame_buffer && frame_sequence && *sequence != frame_sequence && bytes <= size) {
        memcpy(dst, frame_buffer, bytes); *sequence = frame_sequence; copied = 1;
    }
    rt_mutex_release(&video_lock);
    return copied;
}

int video_player_direct_mode(void)
{
    int result;
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    result = direct_mode;
    rt_mutex_release(&video_lock);
    return result;
}

int video_player_acquire_frame(const uint8_t **data, unsigned *stride, unsigned *sequence)
{
    int result = 0;
    if (!data || !stride || !sequence) return -1;
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    if (direct_mode && frame_buffer && frame_sequence && *sequence != frame_sequence) {
        *data = frame_buffer; *stride = direct_stride; *sequence = frame_sequence; result = 1;
    }
    rt_mutex_release(&video_lock);
    return result;
}

void video_player_release_frame(unsigned sequence)
{
    rt_mutex_take(&video_lock, RT_WAITING_FOREVER);
    if (direct_mode && frame_sequence == sequence) frame_buffer = RT_NULL;
    rt_mutex_release(&video_lock);
}
