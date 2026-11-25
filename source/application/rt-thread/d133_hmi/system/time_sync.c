#include "time_sync.h"
#include "system_manager.h"
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <wlan_mgnt.h>
#include <lwip/sockets.h>
#include <netdev.h>

/* RTC 复位为 2020 年，保留有效电池时钟，否则等待网络校时。 */
#define HMI_MIN_VALID_UTC ((time_t)1704067200)
#define HMI_BEIJING_OFFSET ((time_t)(8 * 3600))
#define HMI_TIME_HOST "192.168.17.4"
#define HMI_TIME_TIMEOUT_MS 4000
#define HMI_TIME_RETRY_MS (30 * 1000)
#define HMI_TIME_REFRESH_MS (60 * 60 * 1000)

int time_sync_local_time(struct tm *local)
{
    time_t now;

    if (!local || get_timestamp(&now) != RT_EOK || now < HMI_MIN_VALID_UTC)
        return -1;
    now += HMI_BEIJING_OFFSET;
    return gmtime_r(&now, local) ? 0 : -1;
}

static int date_number(const char *text, size_t length)
{
    int number = 0;

    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return -1;
        number = number * 10 + (text[i] - '0');
    }
    return number;
}

static int parse_http_date(const char *date, size_t length, time_t *utc_seconds)
{
    static const char months[][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    static const char weekdays[][4] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    struct tm utc = {0};
    struct tm verified;
    time_t seconds;
    int year;
    int day;
    int month = 0;

    /* IIS 返回 IMF-fixdate，例如 Wed, 25 Nov 2026 03:11:30 GMT。 */
    if (length != 29 || memcmp(date + 3, ", ", 2) != 0 ||
        date[7] != ' ' || date[11] != ' ' || date[16] != ' ' ||
        date[19] != ':' || date[22] != ':' ||
        memcmp(date + 25, " GMT", 4) != 0)
        return -1;

    while (month < 12 && memcmp(date + 8, months[month], 3) != 0)
        ++month;
    year = date_number(date + 12, 4);
    day = date_number(date + 5, 2);
    utc.tm_hour = date_number(date + 17, 2);
    utc.tm_min = date_number(date + 20, 2);
    utc.tm_sec = date_number(date + 23, 2);
    if (month == 12 || year < 1970 || day < 1 || day > 31 ||
        utc.tm_hour < 0 || utc.tm_hour > 23 || utc.tm_min < 0 || utc.tm_min > 59 ||
        utc.tm_sec < 0 || utc.tm_sec > 59)
        return -1;

    utc.tm_year = year - 1900;
    utc.tm_mon = month;
    utc.tm_mday = day;
    seconds = timegm(&utc);
    /* 回读校验实际日期，拒绝不存在的日历日期或错误星期。 */
    if (seconds < HMI_MIN_VALID_UTC || !gmtime_r(&seconds, &verified) ||
        verified.tm_year != year - 1900 || verified.tm_mon != month ||
        verified.tm_mday != day || memcmp(date, weekdays[verified.tm_wday], 3) != 0)
        return -1;

    *utc_seconds = seconds;
    return 0;
}

static int parse_http_time(const char *headers, time_t *utc_seconds)
{
    const char *line_end = strstr(headers, "\r\n");

    if (!line_end || (strncmp(headers, "HTTP/1.1 200 ", 13) != 0 &&
                      strncmp(headers, "HTTP/1.0 200 ", 13) != 0))
        return -1;

    for (const char *line = line_end + 2; (line_end = strstr(line, "\r\n")) && line_end != line;
         line = line_end + 2) {
        if (line_end - line >= 5 && strncasecmp(line, "Date:", 5) == 0) {
            line += 5;
            while (line < line_end && (*line == ' ' || *line == '\t'))
                ++line;
            while (line_end > line && (line_end[-1] == ' ' || line_end[-1] == '\t'))
                --line_end;
            return parse_http_date(line, (size_t)(line_end - line), utc_seconds);
        }
    }
    return -1;
}

static int wait_socket(int fd, int writing, rt_tick_t started)
{
    const rt_tick_t limit = rt_tick_from_millisecond(HMI_TIME_TIMEOUT_MS);
    const rt_tick_t elapsed = rt_tick_get() - started;
    struct timeval timeout;
    fd_set sockets;
    rt_tick_t left;

    if (elapsed >= limit)
        return -1;
    left = limit - elapsed;
    timeout.tv_sec = left / RT_TICK_PER_SECOND;
    timeout.tv_usec = (left % RT_TICK_PER_SECOND) * 1000000L / RT_TICK_PER_SECOND;
    FD_ZERO(&sockets);
    FD_SET(fd, &sockets);
    return lwip_select(fd + 1, writing ? NULL : &sockets, writing ? &sockets : NULL,
                       NULL, &timeout) == 1 ? 0 : -1;
}

static int request_lan_time(uint32_t sta_address, time_t *utc_seconds)
{
    /* URL 的 # 是浏览器片段。HEAD 仅获取站点根路径响应头。 */
    static const char request[] = "HEAD / HTTP/1.1\r\nHost: " HMI_TIME_HOST
                                  "\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n";
    struct sockaddr_in address = {0};
    const rt_tick_t started = rt_tick_get();
    char headers[1024];
    size_t sent = 0;
    size_t used = 0;
    int result = -1;
    int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    /* 绑定当前 STA 地址，连接、发送和接收共用一个超时。 */
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = sta_address;
    if (lwip_bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        lwip_fcntl(fd, F_SETFL, O_NONBLOCK) != 0)
        goto close_socket;

    address.sin_port = htons(80);
    address.sin_addr.s_addr = inet_addr(HMI_TIME_HOST);
    if (lwip_connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int error = 0;
        socklen_t length = sizeof(error);

        if (errno != EINPROGRESS || wait_socket(fd, 1, started) != 0 ||
            lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
            goto close_socket;
    }

    while (sent < sizeof(request) - 1) {
        ssize_t count;

        if (wait_socket(fd, 1, started) != 0)
            goto close_socket;
        count = lwip_send(fd, request + sent, sizeof(request) - 1 - sent, 0);
        if (count <= 0)
            goto close_socket;
        sent += (size_t)count;
    }

    while (used < sizeof(headers) - 1) {
        ssize_t count;

        if (wait_socket(fd, 0, started) != 0)
            break;
        count = lwip_recv(fd, headers + used, sizeof(headers) - 1 - used, 0);
        if (count <= 0)
            break;
        used += (size_t)count;
        headers[used] = '\0';
        if (strstr(headers, "\r\n\r\n")) {
            result = parse_http_time(headers, utc_seconds);
            break;
        }
    }

close_socket:
    lwip_close(fd);
    return result;
}

static void sync_worker(void *parameter)
{
    rt_tick_t last_attempt = 0;
    rt_tick_t interval = 0;
    uint32_t last_address = 0;

    (void)parameter;
    while (1) {
        /* Wi-Fi 已关联并获得 STA 地址后，首次连接和重连立即尝试校时。 */
        struct rt_wlan_device *sta = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
        struct netdev *network = sta ? sta->netdev : NULL;
        uint32_t address = network && rt_wlan_is_connected() && netdev_is_up(network) ?
                           ip4_addr_get_u32(ip_2_ip4(&network->ip_addr)) : 0;

        if (address && (address != last_address || rt_tick_get() - last_attempt >= interval)) {
            time_t value;

            interval = rt_tick_from_millisecond(HMI_TIME_RETRY_MS);
            if (request_lan_time(address, &value) == 0) {
                if (set_timestamp(value) == RT_EOK) {
                    interval = rt_tick_from_millisecond(HMI_TIME_REFRESH_MS);
                    system_manager_log("局域网校时成功（北京时间）");
                } else {
                    system_manager_log("局域网时间已收到，但 RTC 写入失败");
                }
            }
            last_attempt = rt_tick_get();
        }
        last_address = address;
        rt_thread_mdelay(2000);
    }
}

int time_sync_init(void)
{
    rt_device_t rtc = rt_device_find("rtc");
    rt_thread_t worker;

    /* 先打开一次，确保 RTC 硬件初始化后再读写时间。 */
    if (!rtc || rt_device_open(rtc, 0) != RT_EOK)
        return -1;
    rt_device_close(rtc);
    worker = rt_thread_create("hmi_time", sync_worker, RT_NULL, 4096, 25, 10);
    return worker ? rt_thread_startup(worker) : -1;
}
