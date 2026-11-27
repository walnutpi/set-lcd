// SPDX-License-Identifier: GPL-2.0-only
/*
 * GT9xx 触摸芯片 
 *
 *
 * 手势 -> 鼠标事件（状态机）：
 *   单指：
 *     - 按下     -> 报 ABS_X/Y + BTN_LEFT 按下（光标跳到手指处）
 *     - 按住期间 -> 坐标变化才报 ABS_X/Y，左键保持
 *     - 松开     -> BTN_LEFT 释放
 *     - 双击冻结：距上次左键点击 <=500ms 且落点差 <=30px 时，
 *                本次按下沿用上次坐标（press/release 全程一致），
 *                位移超容差即解冻恢复实时坐标
 *     - 按住 >= longpress_ms 且累计位移 <= tol -> 先释放左键，再发一次右键单击，
 *                                                之后忽略该手指直到松开
 *   双指（进入前会先短暂发过 BTN_LEFT 按下，进入时立即释放）：
 *     - 两指共同上下滑动 -> REL_WHEEL 滚轮（每 scroll_step_px 一格）
 *     - 两指远离/靠近    -> Ctrl+滚轮 模拟缩放（每 pinch_step_px 一步）
 *     - 任一指抬起       -> 手势组结束，忽略残余手指直到全部抬起
 *
 * 纯轮询模式（timer + work），不使用 INT 中断。
 *
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>

#define GT9XX_POLL_INTERVAL_MS	17	/* 17ms = 60fps */
#define GT9XX_FIRST_POLL_DELAY_MS	300

#define GT9XX_REG_COORD		0x814E	/* status + 第一个点 */
#define GT9XX_READ_LEN		12	/* status(1) + point(8) + key(1) + rsv */

#define GT9XX_MAX_POINTS	2	/* 手势最多用到两个点 */

/* buffer-status / touch-key / touch-count 位定义 */
#define GT9XX_BUF_STATUS	BIT(7)
#define GT9XX_HAVE_KEY		BIT(4)
#define GT9XX_TOUCH_COUNT	0x0f

/* 手势判定参数（默认值，模块参数可调） */
static unsigned int longpress_ms = 1000;
module_param_named(longpress_ms, longpress_ms, uint, 0644);
MODULE_PARM_DESC(longpress_ms, "static hold duration to emit right click (ms)");

static unsigned int move_tol = 20;
module_param_named(move_tol, move_tol, uint, 0644);
MODULE_PARM_DESC(move_tol, "max accumulated movement (px) during longpress");

static unsigned int scroll_step_px = 30;
module_param_named(scroll_step_px, scroll_step_px, uint, 0644);
MODULE_PARM_DESC(scroll_step_px, "two-finger scroll pixels per wheel detent");

static unsigned int pinch_step_px = 40;
module_param_named(pinch_step_px, pinch_step_px, uint, 0644);
MODULE_PARM_DESC(pinch_step_px, "two-finger pinch pixels per zoom step");

#define GESTURE_SETTLE_MS	150	/* 落指稳定期，期间不做位移判定 */
#define TWO_FINGER_SETTLE_MS	120	/* 双指手势模式判定观测期 */

/* 双击冻结：连续两次落点近（<=30px）且间隔近（<=500ms）时沿用上次坐标，
 * 补偿绝对坐标鼠标两次落点漂移超出桌面双击容差的问题 */
#define DC_WINDOW_MS		500
#define DC_MAX_DIST_PX		30

/* 调试开关：dump 原始状态字节 + 坐标 */
static bool dbg_raw = false;
module_param_named(dbg_raw, dbg_raw, bool, 0644);

struct gt9xx_point {
	u8 id;
	u16 x, y;
};

/* 双指手势模式 */
enum two_finger_mode {
	TF_UNDECIDED = 0,
	TF_SCROLL,	/* 两指共同上下滑动 -> REL_WHEEL */
	TF_PINCH,	/* 两指远离/靠近 -> Ctrl+滚轮 缩放 */
};

/* 手势状态机 */
enum gt9xx_mouse_state {
	GS_IDLE = 0,	/* 无手指 */
	GS_LEFT_HELD,	/* 左键按住中（按下即发），移动则报 ABS */
	GS_TWO_FINGER,	/* 双指手势进行中（滚动 / 捏合） */
	GS_CONSUMED,	/* 手势已消费，忽略残余手指直到全部抬起 */
};

struct gt9xx_mouse {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	bool stop;
	enum gt9xx_mouse_state state;
	unsigned long down_jiffies;
	bool last_contact;	/* 读数失败时沿用，防止误判手指抬起 */
	bool moved;		/* 距按下起点已超容差（取消长按转换） */
	unsigned int last_x, last_y;	/* 最近一次接触的原始坐标 */
	unsigned int down_sx, down_sy;	/* 按下起点的屏幕像素坐标 */
	unsigned int sent_sx, sent_sy;	/* 上次报 ABS 时的屏幕像素坐标 */
	unsigned int raw_max_x, raw_max_y;
	struct touchscreen_properties prop;

	/* 双指手势 */
	enum two_finger_mode tf_mode;
	unsigned long tf_jiffies;	/* 进入双指状态的时刻 */
	u8 tf_id0, tf_id1;		/* 两指的 track id */
	struct gt9xx_point tf_start0, tf_start1; /* 手势起点 */
	long scroll_acc;		/* 滚动累计位移（已消费部分被扣减） */
	long pinch_acc;			/* 捏合累计间距变化 */

	/* 双击冻结（沿用上次点击坐标） */
	bool dc_valid;			/* dc_* 记录的是一次完整左键点击 */
	unsigned long dc_up_jiffies;	/* 上次左键释放时刻 */
	unsigned int dc_sx, dc_sy;	/* 上次左键按下的屏幕像素坐标 */
	bool click_frozen;		/* 本次点击正沿用旧坐标 */

	struct timer_list poll_timer;
	struct work_struct poll_work;
};

/* 16 位大端寄存器地址 + 连续读（自实现，不依赖旧驱动的 gtp_i2c_read） */
static int gt9xx_read_reg(struct i2c_client *client, u16 reg,
			  void *buf, size_t len)
{
	struct i2c_msg msg[] = {
		{
			.addr	= client->addr,
			.flags	= 0,
			.len	= 2,
			.buf	= (u8[]){ reg >> 8, reg & 0xff },
		},
		{
			.addr	= client->addr,
			.flags	= I2C_M_RD,
			.len	= len,
			.buf	= buf,
		}
	};
	int ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));

	if (ret != ARRAY_SIZE(msg)) {
		dev_err(&client->dev, "%s failed: %d\n", __func__,
			ret < 0 ? ret : -EIO);
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

static int gt9xx_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret = i2c_master_send(client, buf, sizeof(buf));

	if (ret != sizeof(buf)) {
		dev_err(&client->dev, "%s failed: %d\n", __func__,
			ret < 0 ? ret : -EIO);
		return ret < 0 ? ret : -EIO;
	}
	return 0;
}

/* 原始触摸坐标 -> 屏幕像素坐标（DT 旋转/镜像 + 逐轴缩放） */
static void gt9xx_to_screen(struct gt9xx_mouse *priv,
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

	*sx = raw_x * input_abs_get_max(input, ABS_X) / priv->raw_max_x;
	*sy = raw_y * input_abs_get_max(input, ABS_Y) / priv->raw_max_y;
}

/* 上报按键事件，附带当前位置的绝对坐标（用当前已上报坐标，
 * 保证冻结点击的 press/release 位置一致） */
static void gt9xx_report_click(struct gt9xx_mouse *priv, unsigned int code,
			       bool pressed)
{
	struct input_dev *input = priv->input;

	input_report_abs(input, ABS_X, priv->sent_sx);
	input_report_abs(input, ABS_Y, priv->sent_sy);
	input_report_key(input, code, pressed ? 1 : 0);
	input_sync(input);
}

/*
 * 单指手势状态机（IDLE / LEFT_HELD 部分）：
 * contact = 本帧是否有手指，x/y 为屏幕像素坐标。
 * 返回 true 表示已进入双指状态（调用方切换处理）。
 */
static bool gt9xx_gesture_one_finger(struct gt9xx_mouse *priv, bool contact,
				     unsigned int sx, unsigned int sy)
{
	enum gt9xx_mouse_state old_state = priv->state;
	long dx, dy;

	switch (priv->state) {
	case GS_IDLE:
		if (contact) {
			/*
			 * 双击冻结：与上次左键点击时间近且落点近 -> 沿用上次
			 * 按下坐标，让桌面把两次点击判为同点双击。
			 */
			priv->click_frozen = false;
			if (priv->dc_valid &&
			    time_before(jiffies, priv->dc_up_jiffies +
					msecs_to_jiffies(DC_WINDOW_MS))) {
				long ddx = (long)sx - (long)priv->dc_sx;
				long ddy = (long)sy - (long)priv->dc_sy;

				if (ddx * ddx + ddy * ddy <=
				    (long)DC_MAX_DIST_PX * DC_MAX_DIST_PX) {
					sx = priv->dc_sx;
					sy = priv->dc_sy;
					priv->click_frozen = true;
					dev_dbg(&priv->client->dev,
						"double-click freeze at %u,%u\n",
						sx, sy);
				}
			}

			priv->state = GS_LEFT_HELD;
			priv->down_jiffies = jiffies;
			priv->moved = false;
			priv->down_sx = sx;
			priv->down_sy = sy;
			priv->sent_sx = sx;
			priv->sent_sy = sy;
			gt9xx_report_click(priv, BTN_LEFT, true);
		}
		break;

	case GS_LEFT_HELD:
		if (!contact) {
			gt9xx_report_click(priv, BTN_LEFT, false);
			/*
			 * 完整左键点击（含冻结点击）记为双击候选；
			 * 长按右键 / 双指手势走 GS_CONSUMED，不经过这里。
			 */
			priv->dc_valid = true;
			priv->dc_up_jiffies = jiffies;
			priv->dc_sx = priv->down_sx;
			priv->dc_sy = priv->down_sy;
			priv->click_frozen = false;
			priv->state = GS_IDLE;
			break;
		}

		dx = (long)sx - (long)priv->down_sx;
		dy = (long)sy - (long)priv->down_sy;

		/*
		 * 冻结点击解冻：位移超容差说明用户在拖拽而非双击，
		 * 立即以当前位置为起点恢复实时坐标（无需稳定期，
		 * 冻结起点本身是稳定的上次点击位置）。
		 */
		if (priv->click_frozen &&
		    dx * dx + dy * dy > (long)move_tol * move_tol) {
			priv->click_frozen = false;
			priv->down_sx = sx;
			priv->down_sy = sy;
			priv->sent_sx = sx;
			priv->sent_sy = sy;
			dev_dbg(&priv->client->dev,
				"double-click unfreeze at %u,%u\n", sx, sy);
			dx = 0;
			dy = 0;
		}

		/*
		 * 累计位移超容差 -> 已移动，取消长按转换。
		 * 落指稳定期内不判定：IC 落指瞬态坐标跳动大，
		 * 否则 moved 会在头几帧被误置位，长按永远无法转换。
		 */
		if (!priv->moved &&
		    time_after(jiffies, priv->down_jiffies +
			       msecs_to_jiffies(GESTURE_SETTLE_MS)) &&
		    dx * dx + dy * dy > (long)move_tol * move_tol) {
			priv->moved = true;
			dev_dbg(&priv->client->dev,
				"gesture: moved cancel longpress dx=%ld dy=%ld\n",
				dx, dy);
		}

		if (!priv->moved &&
		    time_after(jiffies, priv->down_jiffies +
			       msecs_to_jiffies(longpress_ms))) {
			/* 原地长按 -> 释放左键 + 右键单击，之后忽略 */
			gt9xx_report_click(priv, BTN_LEFT, false);
			gt9xx_report_click(priv, BTN_RIGHT, true);
			gt9xx_report_click(priv, BTN_RIGHT, false);
			priv->state = GS_CONSUMED;
		} else if (sx != priv->sent_sx || sy != priv->sent_sy) {
			input_report_abs(priv->input, ABS_X, sx);
			input_report_abs(priv->input, ABS_Y, sy);
			input_sync(priv->input);
			priv->sent_sx = sx;
			priv->sent_sy = sy;
		}
		break;

	default:
		break;
	}

	if (old_state != priv->state)
		dev_dbg(&priv->client->dev, "gesture: %d -> %d\n",
			old_state, priv->state);

	return priv->state == GS_TWO_FINGER;
}

/* 发一步滚动：delta>0 向下滚，delta<0 向上滚 */
static void gt9xx_scroll_step(struct gt9xx_mouse *priv, int delta)
{
	input_report_rel(priv->input, REL_WHEEL, delta);
	input_sync(priv->input);
}

/* 发一步缩放：delta>0 放大（滚轮上），delta<0 缩小（滚轮下），Ctrl 伴随 */
static void gt9xx_pinch_step(struct gt9xx_mouse *priv, int delta)
{
	input_report_key(priv->input, KEY_LEFTCTRL, 1);
	input_report_rel(priv->input, REL_WHEEL, delta);
	input_report_key(priv->input, KEY_LEFTCTRL, 0);
	input_sync(priv->input);
}

/* 两指间距（仅用于比较，不追求亚像素精度） */
static long gt9xx_point_dist(const struct gt9xx_point *a,
			     const struct gt9xx_point *b)
{
	long dx = (long)a->x - (long)b->x;
	long dy = (long)a->y - (long)b->y;

	return dx * dx + dy * dy;
}

/*
 * 双指手势处理：每帧（两点均在屏上）调用一次。
 * 模式在观测期后锁定，一次手势只做一种动作，避免滚动/捏合来回跳。
 */
static void gt9xx_gesture_two_finger(struct gt9xx_mouse *priv,
				     const struct gt9xx_point *p0,
				     const struct gt9xx_point *p1)
{
	long dist_now, dist_start;
	long cy_now, cy_start;
	long d_dist, d_cent;
	int sign;

	dist_now = gt9xx_point_dist(p0, p1);
	dist_start = gt9xx_point_dist(&priv->tf_start0, &priv->tf_start1);
	cy_now = ((long)p0->y + (long)p1->y) / 2;
	cy_start = ((long)priv->tf_start0.y + (long)priv->tf_start1.y) / 2;

	/* 观测期内只记录不动手 */
	if (priv->tf_mode == TF_UNDECIDED) {
		if (time_before(jiffies, priv->tf_jiffies +
				msecs_to_jiffies(TWO_FINGER_SETTLE_MS)))
			return;

		/*
		 * 模式判定：间距变化用平方差与质心位移量级不一致，
		 * 改用间距平方差的符号近似 + 质心位移绝对值比较。
		 * dist 为平方量级，开方比较：|Δdist|/dist vs |Δcy|。
		 */
		d_dist = dist_now - dist_start;
		d_cent = cy_now - cy_start;

		if (abs(d_cent) * abs(d_cent) * 4 > abs(d_dist)) {
			priv->tf_mode = TF_SCROLL;
			dev_dbg(&priv->client->dev,
				"2f: mode scroll d_cent=%ld d_dist=%ld\n",
				d_cent, d_dist);
		} else {
			priv->tf_mode = TF_PINCH;
			dev_dbg(&priv->client->dev,
				"2f: mode pinch d_cent=%ld d_dist=%ld\n",
				d_cent, d_dist);
		}
		priv->scroll_acc = 0;
		priv->pinch_acc = 0;
		priv->tf_start0 = *p0;
		priv->tf_start1 = *p1;
		return;
	}

	if (priv->tf_mode == TF_SCROLL) {
		long cy_prev = ((long)priv->tf_start0.y +
				(long)priv->tf_start1.y) / 2;

		priv->scroll_acc += cy_now - cy_prev;
		priv->tf_start0 = *p0;
		priv->tf_start1 = *p1;

		while (abs(priv->scroll_acc) >= scroll_step_px) {
			sign = priv->scroll_acc > 0 ? 1 : -1;
			/* 手指下滑 -> 内容上滚 -> 滚轮向下(+1) */
			gt9xx_scroll_step(priv, sign);
			priv->scroll_acc -= sign * scroll_step_px;
		}
	} else {	/* TF_PINCH */
		long dist_prev = gt9xx_point_dist(&priv->tf_start0,
						  &priv->tf_start1);

		/* 用 sqrt 差值近似间距变化量（px） */
		d_dist = int_sqrt((unsigned long)dist_now) -
			 int_sqrt((unsigned long)dist_prev);

		priv->pinch_acc += d_dist;
		priv->tf_start0 = *p0;
		priv->tf_start1 = *p1;

		while (abs(priv->pinch_acc) >= pinch_step_px) {
			sign = priv->pinch_acc > 0 ? 1 : -1;
			/* 两指远离 -> 放大（滚轮向上 -1）；靠近 -> 缩小(+1) */
			gt9xx_pinch_step(priv, -sign);
			priv->pinch_acc -= sign * pinch_step_px;
		}
	}
}

/*
 * 总手势状态机：每读到一帧触摸数据调用一次。
 * points/count 为本帧触摸点（已按 track id 排序）。
 */
static void gt9xx_gesture_update(struct gt9xx_mouse *priv,
				 const struct gt9xx_point *points,
				 unsigned int count)
{
	unsigned int sx = 0, sy = 0;
	bool contact = count >= 1;
	enum gt9xx_mouse_state old_state = priv->state;

	if (contact)
		gt9xx_to_screen(priv, points[0].x, points[0].y, &sx, &sy);

	switch (priv->state) {
	case GS_IDLE:
		if (contact) {
			priv->last_x = points[0].x;
			priv->last_y = points[0].y;
			gt9xx_gesture_one_finger(priv, true, sx, sy);
		}
		break;

	case GS_LEFT_HELD:
		/* 只有 count>=1 时 points[] 才有效；松手帧沿用上一帧坐标 */
		if (count >= 1) {
			priv->last_x = points[0].x;
			priv->last_y = points[0].y;
			gt9xx_to_screen(priv, points[0].x, points[0].y, &sx, &sy);
		}

		if (count >= 2) {
			/* 第二指落下 -> 释放左键，进入双指手势 */
			gt9xx_report_click(priv, BTN_LEFT, false);
			priv->state = GS_TWO_FINGER;
			priv->tf_mode = TF_UNDECIDED;
			priv->tf_jiffies = jiffies;
			priv->tf_id0 = points[0].id;
			priv->tf_id1 = points[1].id;
			priv->tf_start0 = points[0];
			priv->tf_start1 = points[1];
			priv->scroll_acc = 0;
			priv->pinch_acc = 0;
		} else {
			gt9xx_gesture_one_finger(priv, contact, sx, sy);
		}
		break;

	case GS_TWO_FINGER:
		if (count >= 2) {
			/* 按 track id 恢复 p0/p1 顺序（IC 偶尔交换点序） */
			struct gt9xx_point a = points[0], b = points[1];

			if ((a.id == priv->tf_id1 && b.id == priv->tf_id0) ||
			    (a.id != priv->tf_id0 && b.id != priv->tf_id0)) {
				swap(a, b);
			}
			gt9xx_gesture_two_finger(priv, &a, &b);
		} else {
			/* 任一指抬起 -> 手势组结束，忽略残余 */
			priv->state = GS_CONSUMED;
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

/* 从原始数据解析前 GT9XX_MAX_POINTS 个点；返回点数 */
static unsigned int gt9xx_parse_points(struct gt9xx_mouse *priv,
				       const u8 *data, u8 touch_num,
				       struct gt9xx_point *points)
{
	unsigned int n = 0, i;
	const u8 *c;

	n = min_t(unsigned int, touch_num, GT9XX_MAX_POINTS);
	for (i = 0; i < n; i++) {
		/*
		 * 注意：我们的 gt9xx_read_reg 缓冲区没有旧驱动 gtp_i2c_read
		 * 的 2 字节地址前缀，data[0]=status，第 i 个点从 data[1+i*8] 起
		 * （旧驱动的 [3+i*8] 是含地址前缀的索引，不能照抄）。
		 */
		c = data + 1 + i * 8;
		points[i].id = c[0];
		points[i].x = c[1] | (c[2] << 8);
		points[i].y = c[3] | (c[4] << 8);
		dev_dbg(&priv->client->dev,
			"point[%u] id=%u x=%u y=%u\n",
			i, points[i].id, points[i].x, points[i].y);
	}
	return n;
}

/* 读一帧触摸数据并喂给手势状态机；返回是否视为有接触 */
static bool gt9xx_read_and_process(struct gt9xx_mouse *priv)
{
	struct i2c_client *client = priv->client;
	u8 data[2 + 8 * GT9XX_MAX_POINTS] = { 0 };
	u8 tail[8 * GT9XX_MAX_POINTS] = { 0 };
	struct gt9xx_point points[GT9XX_MAX_POINTS];
	unsigned int count = 0;
	bool contact;
	u8 status;
	int error;

	error = gt9xx_read_reg(client, GT9XX_REG_COORD, data, GT9XX_READ_LEN);
	if (error) {
		/*
		 * I2C 偶发失败不等价于手指抬起：沿用上一次接触状态，
		 * 否则长按计时被重置，永远凑不满 longpress_ms。
		 */
		return priv->last_contact;
	}

	status = data[0];

	/* buffer-status 未置位：数据未就绪，视为无变化 */
	if (!(status & GT9XX_BUF_STATUS))
		return priv->last_contact;

	if (dbg_raw)
		dev_info(&client->dev,
			 "0x814E=0x%02X raw[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]\n",
			 status, data[1], data[2], data[3], data[4],
			 data[5], data[6], data[7], data[8], data[9], data[10]);

	/* 触摸键 / pen 按本帧无手指处理 */
	if (!(status & GT9XX_HAVE_KEY)) {
		u8 touch_num = status & GT9XX_TOUCH_COUNT;

		if (touch_num > 1) {
			/* 多指补读：第 2 个点在 0x814E+9（+1 status +8 首点）；
			 * 只缓存 GT9XX_MAX_POINTS 个点，多余指不读 */
			unsigned int extra =
				min_t(u8, touch_num, GT9XX_MAX_POINTS) - 1;

			error = gt9xx_read_reg(client,
					       GT9XX_REG_COORD + 9,
					       tail, 8 * extra);
			if (error) {
				dev_err(&client->dev,
					"read extra points failed\n");
			} else {
				memcpy(&data[9], tail, 8 * extra);
			}
		}

		if (!error || touch_num <= 1)
			count = gt9xx_parse_points(priv, data, touch_num,
						   points);
	}

	/* 清 buffer，IC 才会更新下一帧数据 */
	error = gt9xx_write_reg(client, GT9XX_REG_COORD, 0);
	if (error)
		dev_err(&client->dev, "clear buffer failed: %d\n", error);

	contact = count >= 1;
	priv->last_contact = contact;
	gt9xx_gesture_update(priv, points, count);

	return contact;
}

/* 无中断：timer + work 周期轮询 */
static void gt9xx_poll_work(struct work_struct *work)
{
	struct gt9xx_mouse *priv = container_of(work, struct gt9xx_mouse,
						poll_work);

	gt9xx_read_and_process(priv);
}

static void gt9xx_poll_timer_callback(struct timer_list *t)
{
	struct gt9xx_mouse *priv = from_timer(priv, t, poll_timer);

	schedule_work(&priv->poll_work);

	if (!priv->stop)
		mod_timer(&priv->poll_timer,
			  jiffies + msecs_to_jiffies(GT9XX_POLL_INTERVAL_MS));
}

static void gt9xx_hardware_reset(struct gpio_desc *reset_gpio)
{
	gpiod_set_value_cansleep(reset_gpio, 1);
	usleep_range(12000, 15000);
	gpiod_set_value_cansleep(reset_gpio, 0);
	msleep(300);
}

static void gt9xx_power_down(void *data)
{
	struct gpio_desc *reset_gpio = data;

	gpiod_set_value_cansleep(reset_gpio, 1);
}

static void gt9xx_stop(void *data)
{
	struct gt9xx_mouse *priv = data;

	priv->stop = true;
}

static int gt9xx_mouse_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct gt9xx_mouse *priv;
	struct gpio_desc *reset_gpio;
	struct input_dev *input;
	unsigned int max_xy;
	unsigned int swap_xy = 0, inv_x = 0, inv_y = 0;
	int error;

	dev_dbg(dev, "Probing GT9xx I2C mouse-emulating touchscreen driver");

	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset_gpio))
		return PTR_ERR(reset_gpio);

	if (reset_gpio) {
		error = devm_add_action_or_reset(dev, gt9xx_power_down,
						 reset_gpio);
		if (error)
			return error;

		gt9xx_hardware_reset(reset_gpio);
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
	i2c_set_clientdata(client, priv);

	/* 绝对指针：左右键 + 绝对坐标 + 滚轮（双指手势） */
	input->name = "GT9xx Mouse";
	input->id.bustype = BUS_I2C;
	input_set_capability(input, EV_KEY, BTN_LEFT);
	input_set_capability(input, EV_KEY, BTN_RIGHT);
	input_set_capability(input, EV_KEY, KEY_LEFTCTRL);
	input_set_capability(input, EV_REL, REL_WHEEL);

	/* GT9xx 坐标默认 10bit（0~1023），DT touchscreen-size-x/y 可覆盖 */
	max_xy = 1023;
	priv->raw_max_x = max_xy;
	priv->raw_max_y = max_xy;
	input_set_abs_params(input, ABS_X, 0, max_xy, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, max_xy, 0, 0);

	/*
	 * touchscreen-size-x/y 走标准解析（会重写 ABS 轴上限为缩放目标）。
	 * 方向属性不用内核的 touchscreen-swapped-x-y / touchscreen-inverted-*：
	 * 那几个是 presence 语义（存在即生效，写 <0> 也会生效，易踩坑），
	 * 这里改为值语义自行解析：存在且 =<1> 才生效，<0> 或缺省均不生效。
	 * 注意变换在 gt9xx_to_screen 中手动完成，不走 touchscreen_report_pos。
	 */
	touchscreen_parse_properties(input, false, &priv->prop);
	priv->raw_max_x = input_abs_get_max(input, ABS_X);
	priv->raw_max_y = input_abs_get_max(input, ABS_Y);

	device_property_read_u32(dev, "touchscreen-swap-x-y", &swap_xy);
	device_property_read_u32(dev, "touchscreen-invert-x", &inv_x);
	device_property_read_u32(dev, "touchscreen-invert-y", &inv_y);
	if (swap_xy)
		priv->prop.swap_x_y = true;
	if (inv_x)
		priv->prop.invert_x = true;
	if (inv_y)
		priv->prop.invert_y = true;
	if (swap_xy || inv_x || inv_y)
		dev_info(dev, "transform: swap=%u invert_x=%u invert_y=%u\n",
			 swap_xy, inv_x, inv_y);

	timer_setup(&priv->poll_timer, gt9xx_poll_timer_callback, 0);
	INIT_WORK(&priv->poll_work, gt9xx_poll_work);
	mod_timer(&priv->poll_timer,
		  jiffies + msecs_to_jiffies(GT9XX_FIRST_POLL_DELAY_MS));

	error = devm_add_action_or_reset(dev, gt9xx_stop, priv);
	if (error)
		return error;

	error = input_register_device(priv->input);
	if (error) {
		dev_err(dev, "Cannot register input device, err: %d\n", error);
		return error;
	}

	dev_info(dev, "GT9xx mouse driver ready, polling every %dms\n",
		 GT9XX_POLL_INTERVAL_MS);

	return 0;
}

static int gt9xx_mouse_remove(struct i2c_client *client)
{
	struct gt9xx_mouse *priv = i2c_get_clientdata(client);

	del_timer(&priv->poll_timer);
	cancel_work_sync(&priv->poll_work);

	return 0;
}

static const struct i2c_device_id gt9xx_mouse_id[] = {
	{ "wpi-gt9xx-mouse", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gt9xx_mouse_id);

static const struct of_device_id gt9xx_mouse_dt_ids[] = {
	{ .compatible = "goodix,wpi-gt9xx-mouse" },
	{ }
};
MODULE_DEVICE_TABLE(of, gt9xx_mouse_dt_ids);

static struct i2c_driver gt9xx_mouse_driver = {
	.driver = {
		.name = "wpi-gt9xx-mouse",
		.of_match_table = gt9xx_mouse_dt_ids,
	},
	.id_table = gt9xx_mouse_id,
	.probe = gt9xx_mouse_probe,
	.remove = gt9xx_mouse_remove,
};
module_i2c_driver(gt9xx_mouse_driver);

MODULE_AUTHOR("walnutpi");
MODULE_DESCRIPTION("GT9xx I2C Touchscreen -> Mouse Gesture Driver (polling, 2-finger scroll/pinch)");
MODULE_LICENSE("GPL");
