#include "usb_cdc_control.h"
/* 先载入生成的RT配置，再判断功能开关；否则真实SDK会把本文件编译为空。 */
#include <rtthread.h>

#ifdef HMI_USB_CDC_CONTROL
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "display_profile_manager.h"
#include "screen_test.h"
#include "network_manager.h"
#include "ota_package.h"
#include "usb_ota_receiver.h"
#include <rthw.h>
#ifdef RT_USING_PM
#include <drivers/pm.h>
#endif
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_USB_HS
#error "F750 PA11/PA12控制链路必须选择CherryUSB Device FS"
#endif

#define CDC_IN_EP       0x82
#define CDC_OUT_EP      0x02
#define CDC_INT_EP      0x81
#define CDC_PACKET_SIZE 64U
/* 留出头部和环形缓冲的空槽；维持 CDC 逐包接收及旧的溢出处理。 */
#define CDC_RX_CAPACITY (USB_OTA_MAX_DATA + 1024U)
#define CDC_LINE_SIZE   32U
#define CDC_REPLY_SIZE  1020U
#define CDC_TX_TIMEOUT  1000U

/* 环形缓冲空出一个槽区分满和空，仍须容纳完整DOTA帧。 */
#if CDC_RX_CAPACITY <= USB_OTA_HEADER + USB_OTA_MAX_DATA
#error "CDC软件环容量不足以容纳最大DOTA帧"
#endif

/* 沿用SDK示例身份用于本板间接口，正式产品应使用已获分配的VID/PID。
 * D133由F750提供的固定VBUS供电；实际整板电流预算仍需板级确认。 */
static const uint8_t usb_cdc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, 0x33C3, 0x7788, 0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(9 + CDC_ACM_DESCRIPTOR_LEN, 2, 1, USB_CONFIG_BUS_POWERED, 500),
    CDC_ACM_DESCRIPTOR_INIT(0, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_PACKET_SIZE, 2),
    USB_LANGID_INIT(0x0409),
    6, USB_DESCRIPTOR_TYPE_STRING, 'W',0,'S',0,
    26, USB_DESCRIPTOR_TYPE_STRING, 'D',0,'1',0,'3',0,'3',0,' ',0,
    'U',0,'S',0,'B',0,' ',0,'C',0,'D',0,'C',0,
    16, USB_DESCRIPTOR_TYPE_STRING, 'D',0,'1',0,'3',0,'3',0,'C',0,'D',0,'C',0,
    0
};

/* DMA只访问这两个独立缓冲；沿用CherryUSB对齐及端口cache维护约定。
 * TX在IN及可能的ZLP完成前保持不变，不与RX环形缓冲共用内存。 */
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_rx_packet[CDC_PACKET_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_tx_packet[CDC_REPLY_SIZE];
/* USB回调写入，usb_ctl线程读取，DMA只使用上面的独立端点缓冲。 */
static uint8_t usb_rx_ring[CDC_RX_CAPACITY];
static volatile unsigned usb_rx_head, usb_rx_tail;
static volatile unsigned usb_epoch;
static volatile int usb_configured, usb_dtr;
static volatile int usb_tx_busy, usb_tx_zlp, usb_tx_fault;
static volatile int usb_rx_discard, usb_rx_overflow, usb_rx_reset_line;
static int usb_rebooting;
static unsigned usb_parser_epoch;
static char usb_line[CDC_LINE_SIZE];
static unsigned usb_line_used;
static int usb_line_discard;
static int usb_line_cr;
static struct usbd_interface usb_cdc_control_intf, usb_cdc_data_intf;
/* 虚拟串口参数只用于CDC控制请求回显，不修改任何物理UART配置。 */
static const struct cdc_line_coding usb_default_line_coding = {115200, 0, 0, 8};
static struct cdc_line_coding usb_line_coding = {115200, 0, 0, 8};

/* 软件环不是二次幂，读写下标用比较回绕，避免逐字节取模。 */
static inline unsigned usb_cdc_next_rx_index(unsigned index)
{
    ++index;
    return index == CDC_RX_CAPACITY ? 0U : index;
}

/* 只在已屏蔽USB中断或USB回调中调用，令半行不能跨会话继续执行。 */
static void usb_cdc_reset_rx(void)
{
    usb_rx_head = usb_rx_tail = 0;
    usb_rx_discard = usb_rx_overflow = 0;
    usb_rx_reset_line = 1;
    usb_epoch++;
}

static void usb_cdc_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (!usb_tx_busy) return;
    if (!usb_tx_zlp && nbytes && nbytes % CDC_PACKET_SIZE == 0U) {
        usb_tx_zlp = 1;
        if (usbd_ep_start_write(0, CDC_IN_EP, NULL, 0) != 0) {
            usb_tx_fault = 1;
            usb_tx_busy = 0;
        }
    } else {
        usb_tx_busy = 0;
    }
}

static void usb_cdc_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    unsigned i;

    if (usb_configured && usb_dtr) {
        if (nbytes > sizeof(usb_rx_packet)) {
            usb_rx_head = usb_rx_tail = 0;
            usb_rx_discard = usb_rx_overflow = usb_rx_reset_line = 1;
            nbytes = 0;
        }
        for (i = 0; i < nbytes; ++i) {
            uint8_t value = usb_rx_packet[i];
            unsigned next;
            if (usb_rx_discard) {
                if (value == '\n') usb_rx_discard = 0;
                continue;
            }
            next = usb_cdc_next_rx_index(usb_rx_head);
            if (next == usb_rx_tail) {
                /* 二进制溢出不能按换行恢复，负载可能包含ASCII控制字。
                 * ISR仅使epoch失效，线程负责清理自己持有的OTA预约。 */
                usb_rx_head = usb_rx_tail = 0;
                usb_rx_discard = 1;
                usb_rx_overflow = usb_rx_reset_line = 1;
                usb_tx_fault = 1;
                usb_epoch++;
                continue;
            }
            usb_rx_ring[usb_rx_head] = value;
            usb_rx_head = next;
        }
    }
    /* 每次仅接收一个FS包，回调不解析业务或等待互斥。 */
    if (usb_configured)
        usbd_ep_start_read(0, CDC_OUT_EP, usb_rx_packet, sizeof(usb_rx_packet));
}

static struct usbd_endpoint usb_cdc_out_ep = { CDC_OUT_EP, usb_cdc_bulk_out };
static struct usbd_endpoint usb_cdc_in_ep = { CDC_IN_EP, usb_cdc_bulk_in };

static void usb_cdc_event(uint8_t busid, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
    case USBD_EVENT_DEINIT:
        usb_line_coding = usb_default_line_coding;
        usb_configured = usb_dtr = 0;
        usb_tx_busy = usb_tx_zlp = 0;
        usb_tx_fault = 1;
        usb_cdc_reset_rx();
        break;
    case USBD_EVENT_CONFIGURED:
        usb_cdc_reset_rx();
        usb_configured = 1;
        usb_dtr = 0;
        usb_tx_busy = usb_tx_zlp = usb_tx_fault = 0;
        usbd_ep_start_read(0, CDC_OUT_EP, usb_rx_packet, sizeof(usb_rx_packet));
        break;
    default:
        break;
    }
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    if (busid != 0 || intf != 0) return;
    if (usb_dtr != (int)dtr) {
        usb_cdc_reset_rx();
        /* DTR变化不能释放仍被DMA读取的TX缓冲，须等待真实完成或总线reset。 */
        usb_tx_fault = usb_tx_busy ? 1 : 0;
    }
    usb_dtr = dtr && usb_configured;
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *coding)
{
    rt_base_t level;
    if (busid != 0 || intf != 0 || !coding) return;
    /* 接受并保存主机的完整7字节虚拟参数，USB数据速率仍由FS总线决定。 */
    level = rt_hw_interrupt_disable();
    memcpy(&usb_line_coding, coding, sizeof(usb_line_coding));
    rt_hw_interrupt_enable(level);
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *coding)
{
    rt_base_t level;
    if (busid != 0 || intf != 0 || !coding) return;
    level = rt_hw_interrupt_disable();
    memcpy(coding, &usb_line_coding, sizeof(usb_line_coding));
    rt_hw_interrupt_enable(level);
}

/* 仅应用线程调用；超时后保留busy，防止晚到DMA完成前重写TX内存。 */
static int usb_cdc_send_bytes(const void *data,unsigned length,unsigned epoch)
{
    rt_uint32_t started;
    rt_base_t level = rt_hw_interrupt_disable();

    if (!length || length > sizeof(usb_tx_packet) || !usb_configured || !usb_dtr ||
        usb_epoch != epoch || usb_tx_busy || usb_tx_fault) {
        rt_hw_interrupt_enable(level);
        return 0;
    }
    memcpy(usb_tx_packet, data, length);
    usb_tx_zlp = 0;
    usb_tx_busy = 1;
    if (usbd_ep_start_write(0, CDC_IN_EP, usb_tx_packet, length) != 0) {
        usb_tx_busy = 0;
        usb_tx_fault = 1;
    }
    rt_hw_interrupt_enable(level);
    started = rt_tick_get_millisecond();
    while (usb_tx_busy && usb_epoch == epoch && usb_configured && usb_dtr &&
           rt_tick_get_millisecond() - started < CDC_TX_TIMEOUT)
        rt_thread_mdelay(2);
    if (usb_tx_busy) usb_tx_fault = 1;
    return !usb_tx_busy && !usb_tx_fault && usb_epoch == epoch &&
           usb_configured && usb_dtr;
}
static int usb_cdc_send(const char *text,unsigned epoch)
{
    return usb_cdc_send_bytes(text,(unsigned)strlen(text),epoch);
}
static int usb_cdc_alive(unsigned epoch)
{
    return usb_epoch==epoch&&usb_configured&&usb_dtr&&!usb_tx_fault;
}

static void usb_cdc_execute(const char *command, unsigned epoch)
{
    char response[CDC_REPLY_SIZE];
    int reboot_required = 0, delivered;
    unsigned status;
    rt_base_t level = rt_hw_interrupt_disable();
    int current = usb_epoch == epoch && usb_configured && usb_dtr &&
                  !usb_tx_busy && !usb_tx_fault;
    rt_hw_interrupt_enable(level);
    if (!current) return;

    if(usb_ota_receiver_owned()) {
        rt_snprintf(response,sizeof(response),"%s,ERROR,BUSY\r\n",command);
    } else if (!strcmp(command, "SCREENS")) {
        display_profile_choice_t choices[DISPLAY_PROFILE_MAX_CHOICES];
        size_t count = display_profile_choices(choices, DISPLAY_PROFILE_MAX_CHOICES);
        size_t length = sizeof("SCREENS,OK,") - 1U;

        memcpy(response, "SCREENS,OK,", length);
        for (size_t i = 0; i < count; ++i) {
            if (i)
                response[length++] = '|';
            size_t size = strlen(choices[i].code);
            memcpy(response + length, choices[i].code, size);
            length += size;
        }
        if (!count)
            response[length++] = '-';
        memcpy(response + length, "\r\n", 3U);
    } else if (!strcmp(command, "STATUS")) {
        display_profile_info_t active, supported;
        int lvds = display_profile_get_configured("CS000", &supported) == DISPLAY_PROFILE_OK;
        int mipi = display_profile_get_configured("CS001", &supported) == DISPLAY_PROFILE_OK;
        const char *screens = lvds ? (mipi ? "CS000|CS001" : "CS000") : (mipi ? "CS001" : "-");
        const char *slot = display_profile_active_slot();

        display_profile_get_active(&active);
        /* 旧 USB 控制 App 的内置按钮仍可从自定义型号切回原生屏。 */
        if (display_profile_custom_code(active.code))
            slot = active.code;
        /* 保留旧版 STATUS 前缀；新增字段全部为 ASCII，串口接收缓冲为 160 字节。 */
        rt_snprintf(response, sizeof(response),
                    "STATUS,OK,%s,FW=%s,RES=%ux%u,PANEL=%s,TOUCH=%s,SLOT=%s,LIST=%s,TMODE=%s,SCAP=2\r\n",
                    active.code, HMI_VERSION, (unsigned)active.width, (unsigned)active.height,
                    active.panel, active.touch, slot, screens,
                    screen_test_auto_enabled() ? "AUTO" : "MANUAL");
    } else {
        status = network_manager_physical_control(command, &reboot_required);
        if (!status)
            rt_snprintf(response, sizeof(response), "%s,OK,%s\r\n", command,
                        reboot_required ? "REBOOTING" :
                        ((!strncmp(command, "CS", 2) || !strncmp(command, "SCREEN ", 7)) ? "ACTIVE" : "QUEUED"));
        else
            rt_snprintf(response, sizeof(response), "%s,ERROR,%s\r\n", command,
                        status == 3 ? "BUSY" : status == 4 ? "UNSUPPORTED" : "FAILED");
    }
    delivered = usb_cdc_send(response, epoch);
    if (reboot_required) {
        /* 保留上传锁直到ACK和ZLP真实完成；断开、reset、超时均取消本次重启。 */
        status = network_manager_physical_control_finish(delivered);
        if (delivered && !status) usb_rebooting = 1;
        else if (delivered) {
            rt_snprintf(response, sizeof(response), "%s,ERROR,REBOOT_FAILED\r\n", command);
            usb_cdc_send(response, epoch);
        }
    }
}

/* 每次最多处理一条命令；同一USB会话严格顺序收发，便于界定ACK。 */
static int usb_cdc_process(void)
{
    unsigned epoch;
    /* 不能持中断锁调用OTA清理/状态mutex；reset回调只递增epoch。 */
    usb_ota_receiver_poll(usb_epoch);
    rt_base_t level = rt_hw_interrupt_disable();
    if (usb_rx_reset_line || usb_parser_epoch != usb_epoch) {
        usb_line_used = 0;
        usb_line_discard = 0;
        usb_line_cr = 0;
        usb_rx_reset_line = 0;
        usb_parser_epoch = usb_epoch;
    }
    epoch = usb_epoch;
    if (!usb_configured || !usb_dtr || usb_rebooting || usb_tx_busy || usb_tx_fault) {
        rt_hw_interrupt_enable(level);
        return 0;
    }
    if (usb_rx_overflow) {
        usb_rx_overflow = 0;
        rt_hw_interrupt_enable(level);
        usb_cdc_send("UNKNOWN,ERROR,RX_OVERFLOW\r\n", epoch);
        return 1;
    }
    rt_hw_interrupt_enable(level);
    for (;;) {
        uint8_t value;
        level = rt_hw_interrupt_disable();
        if (usb_epoch != epoch || usb_rx_reset_line || usb_rx_tail == usb_rx_head) {
            rt_hw_interrupt_enable(level);
            return 0;
        }
        value = usb_rx_ring[usb_rx_tail];
        usb_rx_tail = usb_cdc_next_rx_index(usb_rx_tail);
        rt_hw_interrupt_enable(level);
        if(!usb_line_used&&!usb_line_discard&&!usb_line_cr){
            int binary=usb_ota_receiver_input(value,epoch);
            if(binary==2)return 1;
            if(binary)continue;
        }
        if (value == '\n') {
            usb_line_cr = 0;
            if (usb_line_discard) {
                int too_long = usb_line_discard == 1;
                usb_line_used = 0;
                usb_line_discard = 0;
                usb_cdc_send(too_long ? "UNKNOWN,ERROR,LINE_TOO_LONG\r\n" :
                             "UNKNOWN,ERROR,INVALID_LINE\r\n", epoch);
                return 1;
            }
            if (!usb_line_used) continue;
            usb_line[usb_line_used] = 0;
            usb_line_used = 0;
            usb_cdc_execute(usb_line, epoch);
            return 1;
        }
        if (usb_line_discard) continue;
        if (value == '\r' && !usb_line_cr) {
            usb_line_cr = 1;
            continue;
        }
        if (usb_line_cr || value < 0x20 || value > 0x7e) {
            usb_line_discard = 2;
            continue;
        }
        if (usb_line_used + 1U >= sizeof(usb_line)) {
            usb_line_discard = 1;
            continue;
        }
        usb_line[usb_line_used++] = (char)value;
    }
}

static void usb_cdc_worker(void *parameter)
{
    (void)parameter;
    while (1) {
        if (!usb_cdc_process()) rt_thread_mdelay(2);
    }
}

int usb_cdc_control_init(void)
{
    rt_thread_t thread;
    usb_ota_receiver_init(usb_cdc_send_bytes,usb_cdc_alive);
#ifdef RT_USING_PM
    /* 常驻控制口须保留PHY；IDLE仍允许WFI，禁止LIGHT/DEEP拆除USB。 */
    rt_pm_request(PM_SLEEP_MODE_IDLE);
#endif
    usbd_desc_register(0, usb_cdc_descriptor);
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &usb_cdc_control_intf));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &usb_cdc_data_intf));
    usbd_add_endpoint(0, &usb_cdc_out_ep);
    usbd_add_endpoint(0, &usb_cdc_in_ep);
    if (usbd_initialize(0, USB_DEV_BASE, usb_cdc_event) != 0) {
        usbd_deinitialize(0);
#ifdef RT_USING_PM
        rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
        return -1;
    }
    /* RSA公钥验证包含2KiB PEM及大数调用栈，沿用OTA线程的8KiB栈预算。 */
    /* USB 升级线程同步执行 RSA 验签及 OTA 初始化，给调用链保留栈余量。 */
    thread = rt_thread_create("usb_ctl", usb_cdc_worker, RT_NULL, 12U * 1024U, 22, 10);
    if (!thread) {
        usbd_deinitialize(0);
#ifdef RT_USING_PM
        rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
        return -2;
    }
    if (rt_thread_startup(thread) != RT_EOK) {
        rt_thread_delete(thread);
        usbd_deinitialize(0);
#ifdef RT_USING_PM
        rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
        return -3;
    }
    return 0;
}
#endif
