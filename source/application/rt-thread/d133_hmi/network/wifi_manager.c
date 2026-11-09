#include "wifi_manager.h"
#include "../system/env_manager.h"
#include "system_manager.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_fs.h>
#include <wlan_mgnt.h>
#include <wlan_cfg.h>
#include <wlan_prot.h>
#include <lwip/netif.h>
#include <netdev.h>
#include <aic_crc32.h>
#include <aic_core.h>
#include <aic_drv.h>
#include <drivers/mmcsd_core.h>
#include <general_wireless_pm.h>
#include <general_wireless_pin.h>
#include <ctype.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__has_include)
#if __has_include("hmi_wifi_private.h")
#include "hmi_wifi_private.h"
#endif
#endif
#ifndef HMI_DEFAULT_WIFI_SSID
#define HMI_DEFAULT_WIFI_SSID ""
#define HMI_DEFAULT_WIFI_PASSWORD ""
#endif

#define WIFI_PROFILE_PATH "/data/wifi_profiles.bin"
#define WIFI_PROFILE_TEMP "/data/wifi_profiles.tmp"
#define WIFI_PROFILE_ENV "wifi_profiles_hex"
#define WIFI_BOOTSTRAP_PATH "/data/wifi_bootstrap"
#define WIFI_PROFILE_MAGIC 0x57465031U
#define WIFI_JOB_CONNECT 1
#define WIFI_JOB_CONNECT_SAVED 2
#define WIFI_JOB_SCAN 3
#define WIFI_SCAN_MAX 20

typedef struct {
    uint32_t magic;
    uint16_t size;
    uint16_t reserved;
    uint32_t crc;
    struct rt_wlan_cfg_info entries[5];
} wifi_profile_file_t;

typedef struct {
    uint8_t command;
    uint8_t index;
    char ssid[33];
    char key[65];
} wifi_job_t;

static rt_mq_t jobs;
static struct rt_mutex state_lock;
static char state_text[160] = "Wi-Fi 初始化中";
static char saved_names[5][33];
static unsigned saved_count, saved_generation;
static wifi_manager_indicator_t indicator_state = WIFI_INDICATOR_DISABLED;
static int radio_initialized;
static int scan_in_progress;
static int persistence_ready;
static int env_profile_preferred;
static volatile int profile_save_error;
static struct { char ssid[33]; int rssi; } scan_items[WIFI_SCAN_MAX];
static unsigned scan_count, scan_generation;
static void refresh_saved_names(void);
static const struct rt_wlan_cfg_ops wifi_cfg_ops;

static int write_all(int fd, const void *data, size_t size)
{
    const uint8_t *p = data;
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, p + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int file_replace(const char *temporary, const char *path,
                        const void *data, size_t size)
{
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int result = -1;
    if (fd < 0)
        return -1;
    if (!write_all(fd, data, size) && !fsync(fd))
        result = 0;
    if (close(fd))
        result = -1;
    if (!result && rename(temporary, path))
        result = -1;
    if (result)
        remove(temporary);
    return result;
}

static int wifi_data_mounted(void)
{
    struct dfs_filesystem *fs = dfs_filesystem_lookup("/data");
    /* 根文件系统匹配 /data 不能代表 DATA 已挂载。 */
    return fs && fs->path && !strcmp(fs->path, "/data");
}

static int wifi_profile_env_len(void)
{
    char encoded[2U * (16U + sizeof(struct rt_wlan_cfg_info) * 5U) + 1U];
    size_t length;

    if (env_manager_get(WIFI_PROFILE_ENV, encoded, sizeof(encoded)) != 0 || !encoded[0])
        return -1;
    length = strlen(encoded);
    if ((length & 1U) || length < 32U ||
        length > 2U * (16U + sizeof(struct rt_wlan_cfg_info) * 5U))
        return -1;
    return (int)(length / 2U);
}

static int wifi_profile_env_read(void *buffer, int size)
{
    static const char digits[] = "0123456789abcdef";
    char encoded[2U * (16U + sizeof(struct rt_wlan_cfg_info) * 5U) + 1U];
    size_t i;

    if (!buffer || size <= 0 || env_manager_get(WIFI_PROFILE_ENV, encoded,
            sizeof(encoded)) != 0 || strlen(encoded) != (size_t)size * 2U)
        return -1;
    for (i = 0; i < (size_t)size; i++) {
        char high = (char)tolower((unsigned char)encoded[i * 2U]);
        char low = (char)tolower((unsigned char)encoded[i * 2U + 1U]);
        const char *high_digit = strchr(digits, high);
        const char *low_digit = strchr(digits, low);
        if (!high_digit || !low_digit)
            return -1;
        ((uint8_t *)buffer)[i] = (uint8_t)(((high_digit - digits) << 4) |
                                           (low_digit - digits));
    }
    return size;
}

static int wifi_profile_env_write(const void *buffer, int size)
{
    static const char digits[] = "0123456789abcdef";
    char encoded[2U * (16U + sizeof(struct rt_wlan_cfg_info) * 5U) + 1U];
    size_t i;

    if (!buffer || size < 16 || size > (int)(16U + sizeof(struct rt_wlan_cfg_info) * 5U))
        return -1;
    for (i = 0; i < (size_t)size; i++) {
        uint8_t value = ((const uint8_t *)buffer)[i];
        encoded[i * 2U] = digits[value >> 4];
        encoded[i * 2U + 1U] = digits[value & 0x0FU];
    }
    encoded[(size_t)size * 2U] = '\0';
    return env_manager_set(WIFI_PROFILE_ENV, encoded);
}

static int wifi_persistence_init(void)
{
    char probe[2U * (16U + sizeof(struct rt_wlan_cfg_info) * 5U) + 1U];
    int env_ready = env_manager_get(WIFI_PROFILE_ENV, probe, sizeof(probe)) == 0;
    struct stat st;
    int data_ready = wifi_data_mounted();
    int data_has_config = data_ready && !stat(WIFI_PROFILE_PATH, &st) &&
        st.st_size >= 16 && st.st_size <= (long)(16 + sizeof(struct rt_wlan_cfg_info) * 5);

    /* DATA 未挂载时仍由官方 ENV 接口持久保存 WLAN 配置，正常挂载时两处同步。 */
    rt_wlan_cfg_set_ops(&wifi_cfg_ops);
    if (data_has_config) {
        env_profile_preferred = 0;
        if (rt_wlan_cfg_cache_refresh() == RT_EOK) {
            refresh_saved_names();
            return 0;
        }
    }
    if (wifi_profile_env_len() > 0) {
        /* DATA 文件缺失或配置 CRC 异常时，回退到冗余 ENV 镜像。 */
        env_profile_preferred = 1;
        if (rt_wlan_cfg_cache_refresh() == RT_EOK) {
            refresh_saved_names();
            return 0;
        }
    }
    if (data_ready || env_ready) {
        env_profile_preferred = !data_ready;
        refresh_saved_names();
        return 0;
    }
    return -1;
}

/* SDK WLAN 配置序列化接口把最多五组 STA 凭据原子保存到 DATA。 */
static int wifi_cfg_get_len(void)
{
    struct stat st;
    if (env_profile_preferred)
        return wifi_profile_env_len();
    if (wifi_data_mounted() && !stat(WIFI_PROFILE_PATH, &st) && st.st_size >= 16 &&
        st.st_size <= (long)(16 + sizeof(struct rt_wlan_cfg_info) * 5))
        return (int)st.st_size;
    return wifi_profile_env_len();
}

static int wifi_cfg_read(void *buffer, int size)
{
    int fd;
    size_t done = 0;

    if (env_profile_preferred || !wifi_data_mounted())
        return wifi_profile_env_read(buffer, size);
    fd = open(WIFI_PROFILE_PATH, O_RDONLY);
    if (fd < 0)
        return wifi_profile_env_read(buffer, size);
    while (done < (size_t)size) {
        ssize_t n = read(fd, (uint8_t *)buffer + done, (size_t)size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return wifi_profile_env_read(buffer, size);
        }
        done += (size_t)n;
    }
    close(fd);
    if (done != (size_t)size)
        return wifi_profile_env_read(buffer, size);
    return (int)done;
}

static int wifi_cfg_write(void *buffer, int size)
{
    int data_result = -1;
    int env_result;
    if (size < 16 || size > (int)(16 + sizeof(struct rt_wlan_cfg_info) * 5))
        return -1;
    if (wifi_data_mounted())
        data_result = file_replace(WIFI_PROFILE_TEMP, WIFI_PROFILE_PATH, buffer, (size_t)size);
    env_result = wifi_profile_env_write(buffer, size);
    profile_save_error = data_result != 0 && env_result != 0;
    env_profile_preferred = data_result != 0 && env_result == 0;
    return profile_save_error ? -1 : size;
}

static const struct rt_wlan_cfg_ops wifi_cfg_ops = {
    .read_cfg = wifi_cfg_read,
    .get_len = wifi_cfg_get_len,
    .write_cfg = wifi_cfg_write,
};

static void refresh_saved_names(void)
{
    unsigned i, n = (unsigned)rt_wlan_cfg_get_num();
    char names[5][33] = {{0}};
    int changed;
    if (n > 5)
        n = 5;
    for (i = 0; i < n; i++) {
        struct rt_wlan_cfg_info item;
        memset(&item, 0, sizeof(item));
        if (rt_wlan_cfg_read_index(&item, (int)i) == 1) {
            unsigned len = item.info.ssid.len;
            if (len > 32)
                len = 32;
            memcpy(names[i], item.info.ssid.val, len);
            names[i][len] = 0;
        }
    }
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    changed = saved_count != n || memcmp(saved_names, names, sizeof(saved_names)) != 0;
    memcpy(saved_names, names, sizeof(saved_names));
    saved_count = n;
    if (changed)
        saved_generation++;
    rt_mutex_release(&state_lock);
}

static void update_status(const char *message)
{
    struct rt_wlan_info info;
    char connected_ssid[33] = "未连接";
    int connected = rt_wlan_is_connected();
    if (connected && rt_wlan_get_info(&info) == RT_EOK && info.ssid.len <= 32) {
        memcpy(connected_ssid, info.ssid.val, info.ssid.len);
        connected_ssid[info.ssid.len] = 0;
    }
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    snprintf(state_text, sizeof(state_text), "当前网络：%s | %s",
             connected_ssid, message ? message : "就绪");
    if (connected)
        indicator_state = WIFI_INDICATOR_CONNECTED;
    else if (!radio_initialized)
        indicator_state = (message && strstr(message, "失败")) ?
            WIFI_INDICATOR_FAILED : WIFI_INDICATOR_CONNECTING;
    else if (message && (strstr(message, "失败") || strstr(message, "不可用")))
        indicator_state = WIFI_INDICATOR_FAILED;
    else if (scan_in_progress || (message && (strstr(message, "正在") || strstr(message, "连接"))))
        indicator_state = WIFI_INDICATOR_CONNECTING;
    else
        indicator_state = WIFI_INDICATOR_FAILED;
    rt_mutex_release(&state_lock);
}

static int sdio_card_ready(struct rt_mmcsd_host *host)
{
    int ready;
    mmcsd_host_lock(host);
    ready = host->card && host->card->sdio_function_num != 0 &&
        (host->card->card_type == CARD_TYPE_SDIO ||
         host->card->card_type == CARD_TYPE_SDIO_COMBO);
    mmcsd_host_unlock(host);
    return ready;
}

static int init_radio(void)
{
    struct rt_mmcsd_host *host = aic_sdmc_get_rthost(0);
    rt_err_t result;
    if (!host)
        return -1;
    /* 若启动阶段已枚举模块，不能再触发一次 mmcsd_change；SDK 会把
     * 已存在的 SDIO 卡当作拔卡处理。未枚举时才复位并等待重新发现。 */
    if (!sdio_card_ready(host)) {
        if (aic_platform_wlan_hw_reset())
            return -1;
        rt_thread_mdelay(100);
        mmcsd_change(host);
        for (unsigned wait = 0; wait < 150 && !sdio_card_ready(host); wait++)
            rt_thread_mdelay(100);
        if (!sdio_card_ready(host))
            return -RT_ETIMEOUT;
    }
    result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
    if (result != RT_EOK)
        return result;
    /* READY 表示联网协议已就绪，不能在首次连接之前等待它。 */
    /* 当前产品仅启用路由器客户端 STA，不启动开发板热点。 */
    rt_wlan_config_autoreconnect(RT_TRUE);
    /* SDIO 函数固件加载后给 STA/AP VIF 留出启动时间。 */
    rt_thread_mdelay(1000);
    return 0;
}

static void wifi_scan_event(int event, struct rt_wlan_buff *buff, void *parameter)
{
    struct rt_wlan_info *info;
    unsigned i, len;
    (void)event; (void)parameter;
    if (!buff || !buff->data || buff->len < (int)sizeof(*info)) return;
    info = (struct rt_wlan_info *)buff->data;
    len = info->ssid.len;
    if (!len || len > 32) return;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    for (i = 0; i < scan_count; i++)
        if (strlen(scan_items[i].ssid) == len &&
            !memcmp(scan_items[i].ssid, info->ssid.val, len)) break;
    if (i < WIFI_SCAN_MAX) {
        if (i == scan_count) scan_count++;
        if (!scan_items[i].ssid[0] || info->rssi > scan_items[i].rssi) {
            memcpy(scan_items[i].ssid, info->ssid.val, len);
            scan_items[i].ssid[len] = 0;
            scan_items[i].rssi = info->rssi;
            /* 报告回调可能晚于扫描完成事件，仍标记列表变化供UI合并刷新。 */
            scan_generation++;
        }
    }
    rt_mutex_release(&state_lock);
}

static void wifi_do_scan(void)
{
    rt_err_t result;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    memset(scan_items, 0, sizeof(scan_items));
    scan_count = 0;
    scan_generation++;
    scan_in_progress = 1;
    indicator_state = WIFI_INDICATOR_CONNECTING;
    rt_mutex_release(&state_lock);
    update_status("正在扫描附近网络");
    result = rt_wlan_scan_with_info(RT_NULL);
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    scan_in_progress = 0;
    /* 扫描结束后一次刷新列表，避免每条结果反复重建下拉框。 */
    scan_generation++;
    rt_mutex_release(&state_lock);
    update_status(result == RT_EOK ? "扫描完成" : "扫描失败，请检查无线模块");
}

static int bootstrap_load(char *ssid, char *password)
{
    FILE *file = fopen(WIFI_BOOTSTRAP_PATH, "rb");
    char *line;
    size_t len;
    if (!file) {
        /* 本机定制版本可内置默认网络；公开版本使用空配置。 */
        if (!HMI_DEFAULT_WIFI_SSID[0]) return -1;
        rt_snprintf(ssid, 34, "%s", HMI_DEFAULT_WIFI_SSID);
        rt_snprintf(password, 66, "%s", HMI_DEFAULT_WIFI_PASSWORD);
        return 0;
    }
    line = fgets(ssid, 34, file);
    if (!line) {
        fclose(file);
        return -1;
    }
    len = strlen(ssid);
    while (len && (ssid[len - 1] == '\r' || ssid[len - 1] == '\n')) ssid[--len] = 0;
    line = fgets(password, 66, file);
    fclose(file);
    if (!line)
        return -1;
    len = strlen(password);
    while (len && (password[len - 1] == '\r' || password[len - 1] == '\n')) password[--len] = 0;
    return (ssid[0] && len <= 63) ? 0 : -1;
}

static void wifi_worker(void *parameter)
{
    wifi_job_t job;
    char bootstrap_ssid[34] = {0}, bootstrap_key[66] = {0};
    int persistence_ready_local = 0, bootstrap_attempted = 0;
    (void)parameter;
    update_status("初始化 SDIO 无线模块");
    if (init_radio()) {
        rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
        radio_initialized = 0;
        rt_mutex_release(&state_lock);
        update_status("无线模块初始化失败");
        system_manager_log("AIC8800D40L SDIO 初始化失败");
        while (1)
            rt_thread_mdelay(10000);
    }
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    radio_initialized = 1;
    rt_mutex_release(&state_lock);
    rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_REPORT, wifi_scan_event, RT_NULL);
    /* 配置可能由驱动在启动阶段载入；即使 DATA 尚未挂载，也先显示缓存项。 */
    refresh_saved_names();
    wifi_do_scan();
    while (1) {
        /* DATA 若稍晚挂载，Wi-Fi 仍已启动；后续重试配置恢复与持久化。 */
        if (!persistence_ready_local && wifi_persistence_init() == 0) {
            persistence_ready_local = 1;
            persistence_ready = 1;
            rt_wlan_config_autoreconnect(RT_TRUE);
                refresh_saved_names();
                update_status("已加载本地网络配置");
        }
        if (persistence_ready_local && !bootstrap_attempted && !rt_wlan_cfg_get_num()) {
            bootstrap_attempted = 1;
            if (bootstrap_load(bootstrap_ssid, bootstrap_key) == 0) {
                update_status("连接默认路由器");
                if (rt_wlan_connect(bootstrap_ssid, bootstrap_key) != RT_EOK)
                    update_status("默认路由器连接失败，可在 WiFi 页面重试");
                else { rt_thread_mdelay(300); refresh_saved_names(); }
                rt_memset(bootstrap_key, 0, sizeof(bootstrap_key));
                rt_memset(bootstrap_ssid, 0, sizeof(bootstrap_ssid));
            }
        }
        if (rt_mq_recv(jobs, &job, sizeof(job), rt_tick_from_millisecond(2000)) == RT_EOK) {
            int result = -1;
            if (job.command == WIFI_JOB_SCAN) {
                wifi_do_scan();
            } else if (job.command == WIFI_JOB_CONNECT) {
                update_status("正在连接路由器");
                result = rt_wlan_connect(job.ssid, job.key);
                rt_memset(job.key, 0, sizeof(job.key));
                if (result == RT_EOK) {
                    /* WLAN 事件工作队列稍后落盘；先刷新内存缓存，再由定时轮询更新界面。 */
                    rt_thread_mdelay(300);
                    refresh_saved_names();
                    update_status(!persistence_ready ? "已连接；数据分区不可用，网络未保存" :
                                  (profile_save_error ? "已连接，但网络凭据保存失败" : "路由器连接成功"));
                } else update_status("路由器连接失败，请检查密码和信号");
            } else if (job.command == WIFI_JOB_CONNECT_SAVED) {
                struct rt_wlan_cfg_info saved;
                memset(&saved, 0, sizeof(saved));
                if (rt_wlan_cfg_read_index(&saved, job.index) == 1) {
                    char ssid[RT_WLAN_SSID_MAX_LENGTH + 1], key[RT_WLAN_PASSWORD_MAX_LENGTH + 1];
                    memcpy(ssid, saved.info.ssid.val, saved.info.ssid.len);
                    ssid[saved.info.ssid.len] = 0;
                    memcpy(key, saved.key.val, saved.key.len);
                    key[saved.key.len] = 0;
                    update_status("正在连接已保存网络");
                    result = rt_wlan_connect(ssid, key);
                    rt_memset(key, 0, sizeof(key));
                    update_status(result == RT_EOK ? "已保存网络连接成功" : "已保存网络连接失败");
                }
            }
        }
        /* SDK 的连接事件异步保存，可能晚于连接返回；定期同步已保存列表。 */
        if (persistence_ready_local) refresh_saved_names();
    }
}

int wifi_manager_scan(void)
{
    wifi_job_t job = {0};

    if (!jobs)
        return -1;
    job.command = WIFI_JOB_SCAN;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    if(scan_in_progress){rt_mutex_release(&state_lock);return 0;}
    scan_in_progress = 1;
    indicator_state = WIFI_INDICATOR_CONNECTING;
    /* 此接口可能由LVGL调用，不读取需要WLAN管理锁的驱动信息。 */
    rt_snprintf(state_text,sizeof(state_text),"正在刷新周围 Wi-Fi 网络");
    rt_mutex_release(&state_lock);
    if (rt_mq_send(jobs, &job, sizeof(job)) == RT_EOK)
        return 0;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    scan_in_progress = 0;
    indicator_state = WIFI_INDICATOR_FAILED;
    rt_snprintf(state_text,sizeof(state_text),"Wi-Fi 刷新请求失败");
    rt_mutex_release(&state_lock);
    return -1;
}

unsigned wifi_manager_scan_generation(void)
{
    unsigned value;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    value = scan_generation;
    rt_mutex_release(&state_lock);
    return value;
}

unsigned wifi_manager_saved_generation(void)
{
    unsigned value;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    value = saved_generation;
    rt_mutex_release(&state_lock);
    return value;
}

int wifi_manager_scan_in_progress(void)
{
    int value;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    value = scan_in_progress;
    rt_mutex_release(&state_lock);
    return value;
}

wifi_manager_indicator_t wifi_manager_indicator_state(void)
{
    wifi_manager_indicator_t value;
    if (!jobs)
        return WIFI_INDICATOR_DISABLED;
    if (rt_wlan_is_connected())
        return WIFI_INDICATOR_CONNECTED;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    value = indicator_state;
    if (!radio_initialized && value != WIFI_INDICATOR_FAILED)
        value = WIFI_INDICATOR_DISABLED;
    rt_mutex_release(&state_lock);
    return value;
}

int wifi_manager_scan_options(char *text, size_t capacity)
{
    unsigned i; size_t used = 0;
    if (!text || !capacity) return -1;
    text[0] = 0;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    for (i = 0; i < scan_count; i++) {
        int n = rt_snprintf(text + used, capacity - used, "%s%s  (%d dBm)",
                            i ? "\n" : "", scan_items[i].ssid, scan_items[i].rssi);
        if (n < 0 || (size_t)n >= capacity - used) break;
        used += (size_t)n;
    }
    i = scan_count;
    rt_mutex_release(&state_lock);
    if (!used) rt_snprintf(text, capacity, "%s",
        wifi_manager_scan_in_progress() ? "正在刷新周围网络…" : "暂无附近网络");
    return (int)i;
}

int wifi_manager_scan_ssid(unsigned index, char *ssid, size_t capacity)
{
    if (!ssid || !capacity) return -1;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    if (index >= scan_count) { rt_mutex_release(&state_lock); return -1; }
    rt_snprintf(ssid, capacity, "%s", scan_items[index].ssid);
    rt_mutex_release(&state_lock);
    return 0;
}

int wifi_manager_init(void)
{
    rt_thread_t thread;
    rt_mutex_init(&state_lock, "wifistat", RT_IPC_FLAG_PRIO);
    jobs = rt_mq_create("wificmd", sizeof(wifi_job_t), 4, RT_IPC_FLAG_FIFO);
    if (!jobs)
        return -1;
    thread = rt_thread_create("hmi_wifi", wifi_worker, RT_NULL, 8192, 24, 10);
    return thread ? rt_thread_startup(thread) : -1;
}

int wifi_manager_connect(const char *ssid, const char *password)
{
    wifi_job_t job = {0};
    size_t ssid_len, key_len;
    if (!ssid || !password || (ssid_len = strlen(ssid)) == 0 || ssid_len > 32 ||
        (key_len = strlen(password)) > 63)
        return -1;
    job.command = WIFI_JOB_CONNECT;
    memcpy(job.ssid, ssid, ssid_len);
    memcpy(job.key, password, key_len);
    return rt_mq_send(jobs, &job, sizeof(job));
}

int wifi_manager_connect_saved(unsigned index)
{
    wifi_job_t job = {0};
    if (index >= 5)
        return -1;
    job.command = WIFI_JOB_CONNECT_SAVED;
    job.index = (uint8_t)index;
    return rt_mq_send(jobs, &job, sizeof(job));
}

int wifi_manager_get_saved(unsigned index, char *ssid, size_t capacity)
{
    size_t len;
    if (!ssid || !capacity)
        return -1;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    if (index >= saved_count) {
        rt_mutex_release(&state_lock);
        return -1;
    }
    len = strlen(saved_names[index]);
    if (len >= capacity) len = capacity - 1;
    memcpy(ssid, saved_names[index], len);
    ssid[len] = 0;
    rt_mutex_release(&state_lock);
    return 0;
}

unsigned wifi_manager_saved_count(void)
{
    unsigned count;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    count = saved_count;
    rt_mutex_release(&state_lock);
    return count;
}

int wifi_manager_saved_options(char *text, size_t capacity)
{
    unsigned i, count;
    size_t used = 0;
    if (!text || !capacity)
        return -1;
    text[0] = 0;
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    count = saved_count;
    for (i = 0; i < count; i++) {
        int written = rt_snprintf(text + used, capacity - used, "%s%s",
                                  i ? "\n" : "", saved_names[i]);
        if (written < 0 || (size_t)written >= capacity - used) {
            rt_mutex_release(&state_lock);
            return -1;
        }
        used += (size_t)written;
    }
    rt_mutex_release(&state_lock);
    if (!used)
        rt_snprintf(text, capacity, "尚无已保存网络");
    return (int)count;
}

void wifi_manager_status(char *text, size_t capacity)
{
    struct rt_wlan_device *sta;
    char sta_ip[20] = "未连接";
    if (!text || !capacity)
        return;
    sta = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    if (sta && sta->netdev && netdev_is_up(sta->netdev))
        ipaddr_ntoa_r(&sta->netdev->ip_addr, sta_ip, sizeof(sta_ip));
    rt_mutex_take(&state_lock, RT_WAITING_FOREVER);
    snprintf(text, capacity, "WiFi IP %s  %s", sta_ip, state_text);
    rt_mutex_release(&state_lock);
}

int wifi_manager_get_uid(char *text, size_t capacity)
{
    struct rt_wlan_device *sta;
    size_t i, length;

    if (!text || !capacity)
        return -1;
    text[0] = '\0';
    sta = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    if (!sta || !sta->netdev || !sta->netdev->hwaddr_len)
        return -1;
    length = sta->netdev->hwaddr_len;
    if (capacity < length * 2U + 1U)
        return -1;
    for (i = 0; i < length; i++)
        rt_snprintf(text + i * 2U, capacity - i * 2U, "%02X", sta->netdev->hwaddr[i]);
    return (int)(length * 2U);
}

