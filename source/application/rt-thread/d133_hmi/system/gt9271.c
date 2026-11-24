#include "gt9271.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/touch.h>
#include <aic_drv_gpio.h>
#include <string.h>

#if defined(HMI_DISPLAY_PROFILE_SERIAL) && defined(AIC_USING_CTP)
#define CONTACTS 10
#define CONFIG_REG 0x8047
#define ID_REG 0x8140
#define STATUS_REG 0x814e

static struct rt_touch_device touch;
static struct rt_i2c_bus_device *bus;
static rt_uint16_t address;
static struct rt_touch_data previous[CONTACTS];

/* WJ101E5 配套 GT9271 配置，含厂家校验和与刷新标志。 */
static const rt_uint8_t config[] = {
    0x47, 0x20, 0x03, 0x00, 0x05, 0x0A, 0x35, 0x00, 0x11, 0x08, 0x28, 0x08,
    0x44, 0x32, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19,
    0x1B, 0x1D, 0x14, 0x90, 0x30, 0xAA, 0x50, 0x4E, 0x0F, 0x0A, 0x00, 0x00,
    0x00, 0xDB, 0x02, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x32, 0x00, 0x46, 0x69, 0x02, 0xC0, 0x06, 0x08, 0x00, 0x00, 0x04,
    0x82, 0x48, 0x00, 0x7F, 0x4F, 0x00, 0x7E, 0x55, 0x00, 0x7E, 0x5D, 0x00,
    0x7E, 0x64, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x11, 0x10,
    0x0F, 0x0E, 0x0D, 0x0C, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x04, 0x06, 0x07, 0x08, 0x0A, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
    0x13, 0x14, 0x2A, 0x29, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
    0x20, 0x1F, 0x1E, 0x1C, 0x1B, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xDA, 0x01,
};

static int read_reg(rt_uint16_t reg, void *data, rt_uint16_t size)
{
    rt_uint8_t reg_bytes[] = {reg >> 8, reg & 0xff};
    struct rt_i2c_msg messages[] = {
        {.addr=address, .flags=RT_I2C_WR, .len=2, .buf=reg_bytes},
        {.addr=address, .flags=RT_I2C_RD, .len=size, .buf=data},
    };
    return rt_i2c_transfer(bus, messages, 2) == 2 ? RT_EOK : -RT_EIO;
}

static int write_bytes(rt_uint8_t *data, rt_uint16_t size)
{
    struct rt_i2c_msg message = {
        .addr=address, .flags=RT_I2C_WR, .len=size, .buf=data
    };
    return rt_i2c_transfer(bus, &message, 1) == 1 ? RT_EOK : -RT_EIO;
}

/* INT 在复位期间选择 I2C 地址，随后交还给触摸中断。 */
static void reset_panel(int rst, int irq)
{
    rt_pin_mode(rst, PIN_MODE_OUTPUT);
    rt_pin_write(rst, PIN_LOW);
    rt_thread_mdelay(12);
    rt_pin_mode(irq, PIN_MODE_OUTPUT);
    rt_pin_write(irq, address == 0x14 ? PIN_HIGH : PIN_LOW);
    rt_thread_mdelay(2);
    rt_pin_write(rst, PIN_HIGH);
    rt_thread_mdelay(6);
    rt_pin_write(irq, PIN_LOW);
    rt_thread_mdelay(50);
    rt_pin_mode(irq, PIN_MODE_INPUT);
}

static rt_err_t control(struct rt_touch_device *dev, int command, void *arg)
{
    RT_UNUSED(dev);
    if (command == RT_TOUCH_CTRL_GET_ID && arg)
        return read_reg(ID_REG, arg, 6);
    if (command == RT_TOUCH_CTRL_GET_INFO && arg) {
        *(struct rt_touch_info *)arg = touch.info;
        return RT_EOK;
    }
    return -RT_ENOSYS;
}

static rt_size_t read_points(struct rt_touch_device *dev, void *buf, rt_size_t slots)
{
    struct rt_touch_data *out = buf;
    rt_uint8_t status, data[CONTACTS * 8], clear[] = {0x81, 0x4e, 0};
    unsigned count, i, seen = 0;
    RT_UNUSED(dev);

    if (read_reg(STATUS_REG, &status, 1) || !(status & 0x80))
        return 0;
    count = status & 15;
    if (count > CONTACTS || (count && read_reg(STATUS_REG + 1, data, count * 8))) {
        write_bytes(clear, sizeof(clear));
        return 0;
    }
    if (write_bytes(clear, sizeof(clear)))
        return 0;
    if (slots > CONTACTS) slots = CONTACTS;
    memset(out, 0, slots * sizeof(*out));
    for (i = 0; i < count; ++i) {
        const rt_uint8_t *p = data + 8 * i;
        unsigned id = p[0] & 15;
        unsigned x = p[1] | (p[2] << 8), y = p[3] | (p[4] << 8);
        if (id >= slots || x >= 800 || y >= 1280)
            continue;
        seen |= 1U << id;
        out[id].event = previous[id].event ? RT_TOUCH_EVENT_MOVE : RT_TOUCH_EVENT_DOWN;
        out[id].track_id = id;
        out[id].x_coordinate = x;
        out[id].y_coordinate = y;
        out[id].width = p[5];
        out[id].timestamp = rt_touch_get_ts();
        previous[id] = out[id];
    }
    for (i = 0; i < slots; ++i) {
        if (!(seen & (1U << i)) && previous[i].event) {
            out[i] = previous[i];
            out[i].event = RT_TOUCH_EVENT_UP;
            out[i].timestamp = rt_touch_get_ts();
            previous[i].event = 0;
        }
    }
    return slots;
}

static struct rt_touch_ops ops = {
    .touch_readpoint = read_points, .touch_control = control
};

/* 软重启后触摸 IC 可能晚于 I2C 初始化，保持接线不变，限次等待 ID。 */
static int probe_id(void)
{
    rt_uint8_t id[6];
    for (unsigned i = 0; i < 3; ++i) {
        if (read_reg(ID_REG, id, sizeof(id)) == RT_EOK && memcmp(id, "9271", 4) == 0)
            return RT_EOK;
        if (i < 2)
            rt_thread_mdelay(10);
    }
    return -RT_EIO;
}

int gt9271_init(void)
{
    rt_uint8_t current[sizeof(config)];
    rt_uint8_t packet[sizeof(config) + 2] = {0x80, 0x47};
    int rst = drv_pin_get(AIC_TOUCH_PANEL_RST_PIN);
    int irq = drv_pin_get(AIC_TOUCH_PANEL_INT_PIN);

    bus = rt_i2c_bus_device_find(AIC_TOUCH_PANEL_I2C_CHAN);
    if (!bus || rst < 0 || irq < 0) return -RT_ERROR;
    address = 0x5d;
    reset_panel(rst, irq);
    if (probe_id() != RT_EOK) {
        address = 0x14;
        reset_panel(rst, irq);
        if (probe_id() != RT_EOK)
            return -RT_EIO;
    }
    /* 比对配置正文。芯片接收后会清除最后一个刷新字节。 */
    if (read_reg(CONFIG_REG, current, sizeof(current)) ||
        memcmp(current, config, sizeof(config) - 1)) {
        memcpy(packet + 2, config, sizeof(config));
        if (write_bytes(packet, sizeof(packet))) return -RT_EIO;
        /* 给控制器留出应用配置的时间；严格核对分辨率和触点配置。 */
        unsigned attempt;
        for (attempt = 0; attempt < 3; ++attempt) {
            rt_thread_mdelay(attempt == 0 ? 20 : 10);
            if (read_reg(CONFIG_REG, current, 6) == RT_EOK && memcmp(current, config, 6) == 0)
                break;
        }
        if (attempt == 3)
            return -RT_EIO;
    }
    touch.info.type = RT_TOUCH_TYPE_CAPACITANCE;
    touch.info.vendor = RT_TOUCH_VENDOR_GT;
    touch.info.range_x = 800;
    touch.info.range_y = 1280;
    touch.info.point_num = CONTACTS;
    touch.config.dev_name = AIC_TOUCH_PANEL_I2C_CHAN;
    touch.config.rst_pin = rst;
    touch.config.irq_pin.pin = irq;
    /* 厂家配置为下降沿，使用 RT-Thread 对应的上拉输入模式。 */
    touch.config.irq_pin.mode = PIN_MODE_INPUT_PULLUP;
    touch.ops = &ops;
    return rt_hw_touch_register(&touch, GT9271_DEVICE_NAME, RT_DEVICE_FLAG_INT_RX, RT_NULL);
}
#else
int gt9271_init(void) { return -RT_ENOSYS; }
#endif
