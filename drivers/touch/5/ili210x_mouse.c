// SPDX-License-Identifier: GPL-2.0-only
/*
 * ILI210X 触摸芯片 -> 鼠标事件翻译驱动（基于 ili210x.c 拷贝改造）
 *
 * 手势 -> 鼠标事件映射（只取第一根手指，忽略移动与多指）：
 *   - 按下后 2s 内松开          -> BTN_LEFT 单击（快速两次 tap 即双击）
 *   - 按下持续 >= 2s（按下即按住） -> BTN_RIGHT 按下，松开手指时释放
 *   - 按下 300ms ~ 2s 之间松开   -> 无事件（防误触发窗口）
 *
 * 注意：与 ili210x.ko 的 DT compatible 相同，二者只能加载其一。
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <asm/unaligned.h>

#define ILI2XXX_POLL_PERIOD	15

#define ILI210X_DATA_SIZE	64
#define ILI211X_DATA_SIZE	43
#define ILI251X_DATA_SIZE1	31
#define ILI251X_DATA_SIZE2	20

/* 触摸命令寄存器（与 ili210x.c 相同） */
#define REG_TOUCHDATA		0x10
#define REG_PANEL_INFO		0x20
#define REG_FIRMWARE_VERSION	0x40
#define REG_PROTOCOL_VERSION	0x42
#define REG_KERNEL_VERSION	0x61
#define REG_IC_BUSY		0x80
#define REG_IC_BUSY_NOT_BUSY	0x50
#define REG_GET_MODE		0xc0
#define REG_GET_MODE_AP		0x5a
#define REG_GET_MODE_BL		0x55
#define REG_SET_MODE_AP		0xc1
#define REG_SET_MODE_BL		0xc2
#define REG_WRITE_DATA		0xc3
#define REG_WRITE_ENABLE	0xc4
#define REG_READ_DATA_CRC	0xc7
#define REG_CALIBRATE		0xcc

/* 手势判定参数 */
#define GESTURE_LONGPRESS_MS	1000	/* 按住此时长且未移动 -> 右键单击 */
#define GESTURE_LONGPRESS_TOL	20	/* 长按判定允许的累计位移(px) */
#define GESTURE_SETTLE_MS	150	/* 落指稳定期，期间不做位移判定 */

/* 手势状态机 */
enum ili_mouse_state {
	GS_IDLE = 0,	/* 无手指 */
	GS_LEFT_HELD,	/* 左键按住中（按下即发），移动则发 REL */
	GS_CONSUMED,	/* 已转换右键单击，忽略该手指直到松开 */
};

struct ili2xxx_chip {
	int (*read_reg)(struct i2c_client *client, u8 reg,
			void *buf, size_t len);
	int (*get_touch_data)(struct i2c_client *client, u8 *data);
	bool (*parse_touch_data)(const u8 *data, unsigned int finger,
				 unsigned int *x, unsigned int *y,
				 unsigned int *z);
	unsigned int max_touches;
	unsigned int resolution;
	bool has_panel_info;	/* 可从 REG_PANEL_INFO 读每轴实际分辨率 */
};

struct ili210x {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	const struct ili2xxx_chip *chip;
	bool stop;
	enum ili_mouse_state state;
	unsigned long down_jiffies;
	bool last_contact;	/* 读数失败时沿用，防止误判手指抬起 */
	bool moved;		/* 距按下起点已超容差（拖拽，取消长按转换） */
	unsigned int last_x, last_y;	/* 最近一次接触的原始坐标 */
	unsigned int down_sx, down_sy;	/* 按下起点的屏幕像素坐标 */
	unsigned int sent_sx, sent_sy;	/* 上次发 REL 时的屏幕像素坐标 */
	unsigned int raw_max_x, raw_max_y; /* 各轴原始坐标量程上限 */
	struct touchscreen_properties prop;
	struct timer_list poll_timer;
	struct work_struct poll_work;
};

static int ili210x_read_reg(struct i2c_client *client,
			    u8 reg, void *buf, size_t len)
{
	struct i2c_msg msg[] = {
		{
			.addr	= client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= client->addr,
			.flags	= I2C_M_RD,
			.len	= len,
			.buf	= buf,
		}
	};
	int error, ret;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret != ARRAY_SIZE(msg)) {
		error = ret < 0 ? ret : -EIO;
		dev_err(&client->dev, "%s failed: %d\n", __func__, error);
		return error;
	}

	return 0;
}

static int ili210x_read_touch_data(struct i2c_client *client, u8 *data)
{
	return ili210x_read_reg(client, REG_TOUCHDATA,
				data, ILI210X_DATA_SIZE);
}

static bool ili210x_touchdata_to_coords(const u8 *touchdata,
					unsigned int finger,
					unsigned int *x, unsigned int *y,
					unsigned int *z)
{
	if (!(touchdata[0] & BIT(finger)))
		return false;

	*x = get_unaligned_be16(touchdata + 1 + (finger * 4) + 0);
	*y = get_unaligned_be16(touchdata + 1 + (finger * 4) + 2);

	return true;
}

static const struct ili2xxx_chip ili210x_chip = {
	.read_reg		= ili210x_read_reg,
	.get_touch_data		= ili210x_read_touch_data,
	.parse_touch_data	= ili210x_touchdata_to_coords,
	.max_touches		= 2,
};

static int ili211x_read_touch_data(struct i2c_client *client, u8 *data)
{
	s16 sum = 0;
	int error;
	int ret;
	int i;

	ret = i2c_master_recv(client, data, ILI211X_DATA_SIZE);
	if (ret != ILI211X_DATA_SIZE) {
		error = ret < 0 ? ret : -EIO;
		dev_err(&client->dev, "%s failed: %d\n", __func__, error);
		return error;
	}

	/* This chip uses custom checksum at the end of data */
	for (i = 0; i < ILI211X_DATA_SIZE - 1; i++)
		sum = (sum + data[i]) & 0xff;

	if ((-sum & 0xff) != data[ILI211X_DATA_SIZE - 1]) {
		dev_err(&client->dev,
			"CRC error (crc=0x%02x expected=0x%02x)\n",
			sum, data[ILI211X_DATA_SIZE - 1]);
		return -EIO;
	}

	return 0;
}

static bool ili211x_touchdata_to_coords(const u8 *touchdata,
					unsigned int finger,
					unsigned int *x, unsigned int *y,
					unsigned int *z)
{
	u32 data;

	data = get_unaligned_be32(touchdata + 1 + (finger * 4) + 0);
	if (data == 0xffffffff)	/* Finger up */
		return false;

	*x = ((touchdata[1 + (finger * 4) + 0] & 0xf0) << 4) |
	     touchdata[1 + (finger * 4) + 1];
	*y = ((touchdata[1 + (finger * 4) + 0] & 0x0f) << 8) |
	     touchdata[1 + (finger * 4) + 2];

	return true;
}

static const struct ili2xxx_chip ili211x_chip = {
	.read_reg		= ili210x_read_reg,
	.get_touch_data		= ili211x_read_touch_data,
	.parse_touch_data	= ili211x_touchdata_to_coords,
	.max_touches		= 10,
	.resolution		= 2048,
};

static bool ili212x_touchdata_to_coords(const u8 *touchdata,
					unsigned int finger,
					unsigned int *x, unsigned int *y,
					unsigned int *z)
{
	u16 val;

	val = get_unaligned_be16(touchdata + 3 + (finger * 5) + 0);
	if (!(val & BIT(15)))	/* Touch indication */
		return false;

	*x = val & 0x3fff;
	*y = get_unaligned_be16(touchdata + 3 + (finger * 5) + 2);

	return true;
}

static const struct ili2xxx_chip ili212x_chip = {
	.read_reg		= ili210x_read_reg,
	.get_touch_data		= ili210x_read_touch_data,
	.parse_touch_data	= ili212x_touchdata_to_coords,
	.max_touches		= 10,
	.resolution		= 16384,
};

static int ili251x_read_reg_common(struct i2c_client *client,
				   u8 reg, void *buf, size_t len,
				   unsigned int delay)
{
	int error;
	int ret;

	ret = i2c_master_send(client, &reg, 1);
	if (ret == 1) {
		if (delay)
			usleep_range(delay, delay + 500);

		ret = i2c_master_recv(client, buf, len);
		if (ret == len)
			return 0;
	}

	error = ret < 0 ? ret : -EIO;
	dev_err(&client->dev, "%s failed: %d\n", __func__, error);
	return ret;
}

static int ili251x_read_reg(struct i2c_client *client,
			    u8 reg, void *buf, size_t len)
{
	return ili251x_read_reg_common(client, reg, buf, len, 5000);
}

static int ili251x_read_touch_data(struct i2c_client *client, u8 *data)
{
	int error;

	error = ili251x_read_reg_common(client, REG_TOUCHDATA,
					data, ILI251X_DATA_SIZE1, 0);
	if (!error && data[0] == 2) {
		error = i2c_master_recv(client, data + ILI251X_DATA_SIZE1,
					ILI251X_DATA_SIZE2);
		if (error >= 0)
			error = error == ILI251X_DATA_SIZE2 ? 0 : -EIO;
	}

	return error;
}

static bool ili251x_touchdata_to_coords(const u8 *touchdata,
					unsigned int finger,
					unsigned int *x, unsigned int *y,
					unsigned int *z)
{
	u16 val;

	val = get_unaligned_be16(touchdata + 1 + (finger * 5) + 0);
	if (!(val & BIT(15)))	/* Touch indication */
		return false;

	*x = val & 0x3fff;
	*y = get_unaligned_be16(touchdata + 1 + (finger * 5) + 2);
	*z = touchdata[1 + (finger * 5) + 4];

	return true;
}

static const struct ili2xxx_chip ili251x_chip = {
	.read_reg		= ili251x_read_reg,
	.get_touch_data		= ili251x_read_touch_data,
	.parse_touch_data	= ili251x_touchdata_to_coords,
	.max_touches		= 10,
	.resolution		= 16384,	/* 14bit 坐标 (x & 0x3fff) */
	.has_panel_info		= true,
};

/* 取第一根有效手指；无接触返回 false */
static bool ili210x_first_finger(struct ili210x *priv, const u8 *touchdata,
				 unsigned int *x, unsigned int *y)
{
	unsigned int fx, fy, fz;
	unsigned int i;

	for (i = 0; i < priv->chip->max_touches; i++) {
		if (priv->chip->parse_touch_data(touchdata, i, &fx, &fy, &fz)) {
			*x = fx;
			*y = fy;
			return true;
		}
	}

	return false;
}

/* 先缩放到屏幕像素并应用 DT 旋转/镜像，再报坐标 + 按键 */
/* 原始触摸坐标 -> 屏幕像素坐标（DT 旋转/镜像 + 逐轴缩放） */
static void ili210x_to_screen(struct ili210x *priv,
			      unsigned int raw_x, unsigned int raw_y,
			      unsigned int *sx, unsigned int *sy)
{
	struct touchscreen_properties *prop = &priv->prop;
	struct input_dev *input = priv->input;

	/* 与内核 touchscreen_report_pos 同序：先交换轴，再按原始量程翻转 */
	if (prop->swap_x_y)
		swap(raw_x, raw_y);
	if (prop->invert_x)
		raw_x = priv->raw_max_x - raw_x;
	if (prop->invert_y)
		raw_y = priv->raw_max_y - raw_y;

	/* 各轴原始坐标(0~raw_max_*) -> 屏幕像素(0~max_x/max_y)；
	 * DT 未配 size 属性时 max == raw_max_*，恒等不缩放 */
	*sx = raw_x * input_abs_get_max(input, ABS_X) / priv->raw_max_x;
	*sy = raw_y * input_abs_get_max(input, ABS_Y) / priv->raw_max_y;
}

/* 上报按键事件，附带当前位置的绝对坐标 */
static void ili210x_report_click(struct ili210x *priv, unsigned int code,
				 bool pressed)
{
	struct input_dev *input = priv->input;
	unsigned int sx, sy;

	ili210x_to_screen(priv, priv->last_x, priv->last_y, &sx, &sy);
	input_report_abs(input, ABS_X, sx);
	input_report_abs(input, ABS_Y, sy);
	input_report_key(input, code, pressed ? 1 : 0);
	input_sync(input);
}

/*
 * 手势状态机：每读到一帧触摸数据调用一次。
 * contact = 本帧是否有手指。
 *
 * 语义（绝对指针）：
 *   按下     -> 报当前 ABS_X/Y + BTN_LEFT 按下（光标跳到手指处）
 *   按住期间 -> 持续报当前 ABS_X/Y（坐标变化才发），左键保持
 *   松开     -> BTN_LEFT 释放
 *   按住>=2s 且累计位移<=20px -> 先释放左键，再发一次右键单击，
 *                               之后忽略该手指直到松开
 */
static void ili210x_gesture_update(struct ili210x *priv, bool contact,
				   unsigned int x, unsigned int y)
{
	enum ili_mouse_state old_state = priv->state;
	unsigned int sx, sy;
	long dx, dy;

	if (contact) {
		priv->last_x = x;
		priv->last_y = y;
		ili210x_to_screen(priv, x, y, &sx, &sy);
	}

	switch (priv->state) {
	case GS_IDLE:
		if (contact) {
			priv->state = GS_LEFT_HELD;
			priv->down_jiffies = jiffies;
			priv->moved = false;
			priv->down_sx = sx;
			priv->down_sy = sy;
			priv->sent_sx = sx;
			priv->sent_sy = sy;
			/* 按下：绝对坐标 + 左键按下 */
			ili210x_report_click(priv, BTN_LEFT, true);
		}
		break;

	case GS_LEFT_HELD:
		if (!contact) {
			/* 松开 -> 左键释放 */
			ili210x_report_click(priv, BTN_LEFT, false);
			priv->state = GS_IDLE;
			break;
		}

		dx = (long)sx - (long)priv->down_sx;
		dy = (long)sy - (long)priv->down_sy;

		/*
		 * 累计位移超容差 -> 已移动，取消长按转换。
		 * 落指稳定期内不判定：IC 落指瞬态坐标跳动大，
		 * 否则 moved 会在头几帧被误置位，长按永远无法转换。
		 */
		if (!priv->moved &&
		    time_after(jiffies, priv->down_jiffies +
			       msecs_to_jiffies(GESTURE_SETTLE_MS)) &&
		    dx * dx + dy * dy >
		    GESTURE_LONGPRESS_TOL * GESTURE_LONGPRESS_TOL) {
			priv->moved = true;
			dev_dbg(&priv->client->dev,
				"gesture: moved cancel longpress dx=%ld dy=%ld\n",
				dx, dy);
		}

		if (!priv->moved &&
		    time_after(jiffies, priv->down_jiffies +
			       msecs_to_jiffies(GESTURE_LONGPRESS_MS))) {
			/* 原地长按 -> 释放左键 + 右键单击，之后忽略 */
			ili210x_report_click(priv, BTN_LEFT, false);
			ili210x_report_click(priv, BTN_RIGHT, true);
			ili210x_report_click(priv, BTN_RIGHT, false);
			priv->state = GS_CONSUMED;
		} else if (sx != priv->sent_sx || sy != priv->sent_sy) {
			/* 按住期间坐标变化 -> 更新绝对坐标（光标跟随） */
			input_report_abs(priv->input, ABS_X, sx);
			input_report_abs(priv->input, ABS_Y, sy);
			input_sync(priv->input);
			priv->sent_sx = sx;
			priv->sent_sy = sy;
		}
		break;

	case GS_CONSUMED:
		if (!contact)
			priv->state = GS_IDLE;
		break;
	}

	if (old_state != priv->state)
		dev_dbg(&priv->client->dev, "gesture: %d -> %d\n",
			old_state, priv->state);
}

/* 读一帧触摸数据并喂给手势状态机；返回是否视为有接触 */
static bool ili210x_read_and_process(struct ili210x *priv)
{
	struct i2c_client *client = priv->client;
	u8 touchdata[ILI210X_DATA_SIZE] = { 0 };
	unsigned int x = 0, y = 0;
	bool contact;
	int error;

	error = priv->chip->get_touch_data(client, touchdata);
	if (error) {
		dev_err(&client->dev, "Unable to get touch data: %d\n", error);
		/*
		 * I2C 偶发失败不等价于手指抬起：沿用上一次接触状态，
		 * 否则长按计时被重置，永远凑不满 2s。
		 */
		return priv->last_contact;
	}

	contact = ili210x_first_finger(priv, touchdata, &x, &y);
	priv->last_contact = contact;
	ili210x_gesture_update(priv, contact, x, y);

	return contact;
}

static irqreturn_t ili210x_irq(int irq, void *irq_data)
{
	struct ili210x *priv = irq_data;
	ktime_t time_next;
	s64 time_delta;
	bool keep_polling;

	do {
		time_next = ktime_add_ms(ktime_get(), ILI2XXX_POLL_PERIOD);
		keep_polling = ili210x_read_and_process(priv);

		/*
		 * 只要手指仍在屏上（或正处于手势等待期）就持续轮询，
		 * 不依赖芯片自身的 continue_polling 语义，保证长按检测
		 * 在所有芯片变体上都能工作。
		 */
		if (keep_polling || priv->state != GS_IDLE) {
			time_delta = ktime_us_delta(time_next, ktime_get());
			if (time_delta > 0)
				usleep_range(time_delta, time_delta + 1000);
		}
	} while (!priv->stop && (keep_polling || priv->state != GS_IDLE));

	return IRQ_HANDLED;
}

/* 无中断的 DT 节点：定时器轮询兜底 */
static void ili210x_poll_work(struct work_struct *work)
{
	struct ili210x *priv = container_of(work, struct ili210x, poll_work);

	ili210x_read_and_process(priv);
}

static void ili210x_poll_timer_callback(struct timer_list *t)
{
	struct ili210x *priv = from_timer(priv, t, poll_timer);

	schedule_work(&priv->poll_work);

	if (!priv->stop)
		mod_timer(&priv->poll_timer,
			  jiffies + msecs_to_jiffies(ILI2XXX_POLL_PERIOD));
}

static void ili210x_hardware_reset(struct gpio_desc *reset_gpio)
{
	/* Reset the controller */
	gpiod_set_value_cansleep(reset_gpio, 1);
	usleep_range(12000, 15000);
	gpiod_set_value_cansleep(reset_gpio, 0);
	msleep(300);
}

static void ili210x_power_down(void *data)
{
	struct gpio_desc *reset_gpio = data;

	gpiod_set_value_cansleep(reset_gpio, 1);
}

static void ili210x_stop(void *data)
{
	struct ili210x *priv = data;

	priv->stop = true;
}

static int ili210x_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	const struct ili2xxx_chip *chip;
	struct ili210x *priv;
	struct gpio_desc *reset_gpio;
	struct input_dev *input;
	unsigned int max_xy;
	int error;

	dev_dbg(dev, "Probing for ILI210X I2C mouse-emulating touchscreen driver");

	chip = device_get_match_data(dev);
	if (!chip && id)
		chip = (const struct ili2xxx_chip *)id->driver_data;
	if (!chip) {
		dev_err(&client->dev, "unknown device model\n");
		return -ENODEV;
	}

	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset_gpio))
		return PTR_ERR(reset_gpio);

	if (reset_gpio) {
		error = devm_add_action_or_reset(dev, ili210x_power_down,
						 reset_gpio);
		if (error)
			return error;

		ili210x_hardware_reset(reset_gpio);
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	priv->client = client;
	priv->input = input;
	priv->reset_gpio = reset_gpio;
	priv->chip = chip;
	i2c_set_clientdata(client, priv);

	/* 注册为绝对指针：左右键 + 绝对坐标（光标绝对跟随手指） */
	input->name = "ILI210x Mouse";
	input->id.bustype = BUS_I2C;
	input_set_capability(input, EV_KEY, BTN_LEFT);
	input_set_capability(input, EV_KEY, BTN_RIGHT);

	/* 先按芯片分辨率兜底，再用 DT 的 touchscreen-size-x/y 覆盖 */
	max_xy = (chip->resolution ?: SZ_64K) - 1;
	priv->raw_max_x = max_xy;
	priv->raw_max_y = max_xy;
	input_set_abs_params(input, ABS_X, 0, max_xy, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, max_xy, 0, 0);

	/*
	 * IC 的满量程只对应长边（等比网格），短边原始上限更小
	 * （如 1920x1080 屏：raw_x 上限 16383，raw_y 上限仅约 9216）。
	 * 有 PANEL_INFO 的芯片读固件上报的每轴实际分辨率来缩放。
	 */
	if (chip->has_panel_info) {
		u8 rs[10] = { 0 };
		u16 resx, resy;

		if (!chip->read_reg(client, REG_PANEL_INFO, &rs, sizeof(rs))) {
			resx = le16_to_cpup((__le16 *)rs);
			resy = le16_to_cpup((__le16 *)(rs + 2));
			if (resx && resx != 0xffff &&
			    resy && resy != 0xffff) {
				priv->raw_max_x = resx - 1;
				priv->raw_max_y = resy - 1;
				dev_info(dev, "panel resolution %ux%u\n",
					 resx, resy);
			}
		}
	}

	/* DT 有 size 属性时把轴上限改写为屏幕像素上限（缩放目标） */
	touchscreen_parse_properties(input, false, &priv->prop);

	if (client->irq) {
		error = devm_request_threaded_irq(dev, client->irq, NULL, ili210x_irq,
					IRQF_ONESHOT, client->name, priv);
		if (error) {
			dev_err(dev, "Unable to request touchscreen IRQ, err: %d\n", error);
			return error;
		}
	} else {
		dev_info(dev, "No interrupt, falling back to timer polling\n");
		timer_setup(&priv->poll_timer, ili210x_poll_timer_callback, 0);
		INIT_WORK(&priv->poll_work, ili210x_poll_work);
		mod_timer(&priv->poll_timer,
			  jiffies + msecs_to_jiffies(ILI2XXX_POLL_PERIOD));
	}

	error = devm_add_action_or_reset(dev, ili210x_stop, priv);
	if (error)
		return error;

	error = input_register_device(priv->input);
	if (error) {
		dev_err(dev, "Cannot register input device, err: %d\n", error);
		return error;
	}

	return 0;
}

static const struct i2c_device_id ili210x_i2c_id[] = {
	{ "ili210x", (long)&ili210x_chip },
	{ "ili2117", (long)&ili211x_chip },
	{ "ili2120", (long)&ili212x_chip },
	{ "ili251x", (long)&ili251x_chip },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ili210x_i2c_id);

static const struct of_device_id ili210x_dt_ids[] = {
	{ .compatible = "ilitek,ili210x", .data = &ili210x_chip },
	{ .compatible = "ilitek,ili2117", .data = &ili211x_chip },
	{ .compatible = "ilitek,ili2120", .data = &ili212x_chip },
	{ .compatible = "ilitek,ili251x", .data = &ili251x_chip },
	{ }
};
MODULE_DEVICE_TABLE(of, ili210x_dt_ids);

static int ili210x_i2c_remove(struct i2c_client *client)
{
	struct ili210x *priv = i2c_get_clientdata(client);

	if (!client->irq) {
		del_timer(&priv->poll_timer);
		cancel_work_sync(&priv->poll_work);
	}

	return 0;
}

static struct i2c_driver ili210x_mouse_driver = {
	.driver = {
		.name = "ili210x_mouse_i2c",
		.of_match_table = ili210x_dt_ids,
	},
	.id_table = ili210x_i2c_id,
	.probe = ili210x_i2c_probe,
	.remove = ili210x_i2c_remove,
};
module_i2c_driver(ili210x_mouse_driver);

MODULE_AUTHOR("walnutpi");
MODULE_DESCRIPTION("ILI210X I2C Touchscreen -> Mouse Gesture Driver");
MODULE_LICENSE("GPL");
