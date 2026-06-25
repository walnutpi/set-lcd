// SPDX-License-Identifier: GPL-2.0-only
/*
 * QST QMA6100P 3-Axis Accelerometer IIO Driver
 *
 * ============================================================
 * 应用层访问路径（固定，不受IIO设备号影响）:
 * ============================================================
 *
 *   # 只有1个 QMA6100P 时 —— 简写路径
 *   /sys/kernel/qma6100p/device/
 *
 *   # 多个 QMA6100P 时 —— 自动按 I2C 地址区分
 *   /sys/kernel/qma6100p-3-0012/device/    # 地址 0x12 的芯片
 *   /sys/kernel/qma6100p-3-0013/device/    # 地址 0x13 的芯片
 *
 * ============================================================
 * 应用层访问路径（iio）:
 * ============================================================
 *   /sys/bus/iio/devices
 *
 * ============================================================
 * 应用层接口说明:
 * ============================================================
 *
 *   文件路径                    | 权限 | 类型      | 说明
 *   ---------------------------|------|-----------|--------------------------
 *   in_accel_x_raw             | 只读 | int       | X轴原始值 (14位有符号)
 *   in_accel_y_raw             | 只读 | int       | Y轴原始值
 *   in_accel_z_raw             | 只读 | int       | Z轴原始值
 *   in_accel_x_input           | 只读 | int       | X轴物理值 (毫g, 1000=1g)
 *   in_accel_y_input           | 只读 | int       | Y轴物理值 (毫g)
 *   in_accel_z_input           | 只读 | int       | Z轴物理值 (毫g)
 *   in_accel_scale             | 只读 | float     | 比例因子 (g/计数)
 *   in_accel_range             | 读写 | int       | 量程 (±2/4/8/16/32 G)
 *   in_accel_sampling_frequency| 读写 | int       | 采样率 (12/25/50/100/200/400/800/1600 Hz)
 *
 *   【量程切换示例】
 *     # 切换到 ±8G 量程（scale 自动同步变更）
 *     echo 8 > /sys/kernel/qma6100p/device/in_accel_range
 *
 *   【采样率切换示例】
 *     # 设置采样率为 400Hz
 *     echo 400 > /sys/kernel/qma6100p/device/in_accel_sampling_frequency
 *
 *   【读取加速度示例】
 *     # 读 X 轴物理值（单位: 毫g, 1000 = 1g）
 *     cat /sys/kernel/qma6100p/device/in_accel_x_input
 *     # 输出: -50  (表示 -0.050g)
 *
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/kobject.h>

#define QMA6100P_CHIP_ID		0x90

/* Register map */
#define QMA6100P_REG_CHIP_ID		0x00
#define QMA6100P_REG_XOUTL		0x01
#define QMA6100P_REG_XOUTH		0x02
#define QMA6100P_REG_YOUTL		0x03
#define QMA6100P_REG_YOUTH		0x04
#define QMA6100P_REG_ZOUTL		0x05
#define QMA6100P_REG_ZOUTH		0x06
#define QMA6100P_REG_RANGE		0x0F
#define QMA6100P_REG_BW_ODR		0x10
#define QMA6100P_REG_POWER_MANAGE	0x11
#define QMA6100P_REG_NVM_STATUS		0x33
#define QMA6100P_REG_RESET		0x36

/* Analog trim registers (vendor recommended) */
#define QMA6100P_REG_TRIM1		0x4A
#define QMA6100P_REG_TRIM2		0x56
#define QMA6100P_REG_TRIM3		0x5F

/* Register values */
#define QMA6100P_RESET_KEY		0xB6
#define QMA6100P_NVM_READY_MASK		0x05
#define QMA6100P_NVM_READY_VAL		0x05
#define QMA6100P_ACTIVE_MODE		0x84
#define QMA6100P_STANDBY_MODE		0x00

/* Range register values */
#define QMA6100P_RANGE_2G		0x01
#define QMA6100P_RANGE_4G		0x02
#define QMA6100P_RANGE_8G		0x04
#define QMA6100P_RANGE_16G		0x08
#define QMA6100P_RANGE_32G		0x0F

/* Scale factors: milli-g per LSB for each range (14-bit resolution) */
#define QMA6100P_MG_SCALE_2G		244
#define QMA6100P_MG_SCALE_4G		488
#define QMA6100P_MG_SCALE_8G		977
#define QMA6100P_MG_SCALE_16G		1953
#define QMA6100P_MG_SCALE_32G		3906

/* BW/ODR register values (QMA6100P_REG_BW_ODR) */
#define QMA6100P_BW_1600HZ		4
#define QMA6100P_BW_800HZ		3
#define QMA6100P_BW_400HZ		2
#define QMA6100P_BW_200HZ		1
#define QMA6100P_BW_100HZ		0
#define QMA6100P_BW_50HZ		5
#define QMA6100P_BW_25HZ		6
#define QMA6100P_BW_12_5HZ		7

#define QMA6100P_NVM_TIMEOUT_MS		100

struct qma6100p_data {
	struct i2c_client *client;
	int scale_mg;		/* milli-g per LSB */
	u8 range_reg;		/* range register value */
	u8 bw_reg;		/* BW/ODR register value */
	int range_g;		/* current full-scale range in G */
	int sampling_freq;	/* current sampling frequency in Hz */
	struct kobject *fixed_kobj;	/* per-device sysfs directory */
};

static int qma6100p_read_reg(struct qma6100p_data *data, u8 reg)
{
	return i2c_smbus_read_byte_data(data->client, reg);
}

static int qma6100p_write_reg(struct qma6100p_data *data, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(data->client, reg, val);
}

static int qma6100p_read_accel_raw(struct qma6100p_data *data,
				   u8 lo_reg, u8 hi_reg, int *val)
{
	int lo, hi;

	lo = qma6100p_read_reg(data, lo_reg);
	if (lo < 0)
		return lo;

	hi = qma6100p_read_reg(data, hi_reg);
	if (hi < 0)
		return hi;

	/*
	 * Chip stores 14-bit signed data in upper 14 bits of 16-bit register.
	 * Cast to int16_t first, then arithmetic right shift preserves sign.
	 * Equivalent to LVGL code: (int16_t)((xh << 8) | xl) >> 2
	 */
	*val = ((int16_t)((hi << 8) | lo)) >> 2;
	return 0;
}

static int qma6100p_wait_nvm_ready(struct qma6100p_data *data)
{
	unsigned long timeout = jiffies +
				msecs_to_jiffies(QMA6100P_NVM_TIMEOUT_MS);
	int ret;

	do {
		ret = qma6100p_read_reg(data, QMA6100P_REG_NVM_STATUS);
		if (ret < 0)
			return ret;
		if ((ret & QMA6100P_NVM_READY_MASK) == QMA6100P_NVM_READY_VAL)
			return 0;
		usleep_range(500, 1000);
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

/* BW/ODR frequency lookup table */
static const struct {
	int freq_hz;
	u8 reg_val;
} qma6100p_bw_table[] = {
	{ 1600,	QMA6100P_BW_1600HZ },
	{ 800,	QMA6100P_BW_800HZ },
	{ 400,	QMA6100P_BW_400HZ },
	{ 200,	QMA6100P_BW_200HZ },
	{ 100,	QMA6100P_BW_100HZ },
	{ 50,	QMA6100P_BW_50HZ },
	{ 25,	QMA6100P_BW_25HZ },
	{ 12,	QMA6100P_BW_12_5HZ },	/* 12.5 Hz */
};

static int qma6100p_set_range(struct qma6100p_data *data, int range_g)
{
	switch (range_g) {
	case 2:
		data->range_reg = QMA6100P_RANGE_2G;
		data->scale_mg = QMA6100P_MG_SCALE_2G;
		break;
	case 4:
		data->range_reg = QMA6100P_RANGE_4G;
		data->scale_mg = QMA6100P_MG_SCALE_4G;
		break;
	case 8:
		data->range_reg = QMA6100P_RANGE_8G;
		data->scale_mg = QMA6100P_MG_SCALE_8G;
		break;
	case 16:
		data->range_reg = QMA6100P_RANGE_16G;
		data->scale_mg = QMA6100P_MG_SCALE_16G;
		break;
	case 32:
		data->range_reg = QMA6100P_RANGE_32G;
		data->scale_mg = QMA6100P_MG_SCALE_32G;
		break;
	default:
		return -EINVAL;
	}

	data->range_g = range_g;
	return 0;
}

static int qma6100p_set_bw(struct qma6100p_data *data, int freq_hz)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qma6100p_bw_table); i++) {
		if (qma6100p_bw_table[i].freq_hz == freq_hz) {
			data->bw_reg = qma6100p_bw_table[i].reg_val;
			data->sampling_freq = freq_hz;
			return qma6100p_write_reg(data, QMA6100P_REG_BW_ODR,
						  data->bw_reg);
		}
	}

	return -EINVAL;
}

static int qma6100p_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct qma6100p_data *data = iio_priv(indio_dev);
	u8 lo_reg, hi_reg;
	s64 tmp;
	int ret;

	switch (chan->channel2) {
	case IIO_MOD_X:
		lo_reg = QMA6100P_REG_XOUTL;
		hi_reg = QMA6100P_REG_XOUTH;
		break;
	case IIO_MOD_Y:
		lo_reg = QMA6100P_REG_YOUTL;
		hi_reg = QMA6100P_REG_YOUTH;
		break;
	case IIO_MOD_Z:
		lo_reg = QMA6100P_REG_ZOUTL;
		hi_reg = QMA6100P_REG_ZOUTH;
		break;
	default:
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = qma6100p_read_accel_raw(data, lo_reg, hi_reg, val);
		if (ret < 0)
			return ret;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		ret = qma6100p_read_accel_raw(data, lo_reg, hi_reg, val);
		if (ret < 0)
			return ret;
		/* Convert to milli-g (1000 = 1g) */
		tmp = (s64)(*val) * data->scale_mg;
		*val = div_s64(tmp, 1000);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = data->scale_mg * 1000;	/* micro-g per LSB */
		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = data->sampling_freq;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int qma6100p_write_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int val, int val2, long mask)
{
	struct qma6100p_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		return qma6100p_set_bw(data, val);
	default:
		return -EINVAL;
	}
}

#define QMA6100P_CHANNEL(axis) {					\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
			      BIT(IIO_CHAN_INFO_PROCESSED),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |		\
				    BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
}

static const struct iio_chan_spec qma6100p_channels[] = {
	QMA6100P_CHANNEL(X),
	QMA6100P_CHANNEL(Y),
	QMA6100P_CHANNEL(Z),
};

static ssize_t qma6100p_read_range(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct qma6100p_data *data = iio_priv(indio_dev);

	return sysfs_emit(buf, "%d\n", data->range_g);
}

static ssize_t qma6100p_write_range(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct qma6100p_data *data = iio_priv(indio_dev);
	int range_g, ret;

	ret = kstrtoint(buf, 10, &range_g);
	if (ret < 0)
		return ret;

	ret = qma6100p_set_range(data, range_g);
	if (ret < 0)
		return ret;

	ret = qma6100p_write_reg(data, QMA6100P_REG_RANGE, data->range_reg);
	if (ret < 0)
		return ret;

	return len;
}

static IIO_DEVICE_ATTR(in_accel_range, 0644,
		       qma6100p_read_range, qma6100p_write_range, 0);

static struct attribute *qma6100p_attributes[] = {
	&iio_dev_attr_in_accel_range.dev_attr.attr,
	NULL,
};

static const struct attribute_group qma6100p_attribute_group = {
	.attrs = qma6100p_attributes,
};

static const struct iio_info qma6100p_info = {
	.read_raw = qma6100p_read_raw,
	.write_raw = qma6100p_write_raw,
	.attrs = &qma6100p_attribute_group,
};

static int qma6100p_init_device(struct qma6100p_data *data)
{
	struct i2c_client *client = data->client;
	int ret;

	/* Default: 2G range, 100 Hz sampling */
	ret = qma6100p_set_range(data, 2);
	if (ret < 0)
		return ret;

	data->bw_reg = QMA6100P_BW_100HZ;
	data->sampling_freq = 100;

	/* Soft reset */
	ret = qma6100p_write_reg(data, QMA6100P_REG_RESET, QMA6100P_RESET_KEY);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);

	ret = qma6100p_write_reg(data, QMA6100P_REG_RESET, 0x00);
	if (ret < 0)
		return ret;
	msleep(1000);

	/* Wait for NVM to finish loading */
	ret = qma6100p_wait_nvm_ready(data);
	if (ret < 0) {
		dev_err(&client->dev, "NVM ready timeout\n");
		return ret;
	}

	/* Analog trim registers (vendor recommended) */
	ret = qma6100p_write_reg(data, QMA6100P_REG_TRIM1, 0x20);
	if (ret < 0)
		return ret;
	ret = qma6100p_write_reg(data, QMA6100P_REG_TRIM2, 0x01);
	if (ret < 0)
		return ret;
	ret = qma6100p_write_reg(data, QMA6100P_REG_TRIM3, 0x80);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);
	ret = qma6100p_write_reg(data, QMA6100P_REG_TRIM3, 0x00);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);

	/* Set measurement range (default 2G) */
	ret = qma6100p_write_reg(data, QMA6100P_REG_RANGE, data->range_reg);
	if (ret < 0)
		return ret;

	/* Set bandwidth / output data rate (default 100 Hz) */
	ret = qma6100p_write_reg(data, QMA6100P_REG_BW_ODR, data->bw_reg);
	if (ret < 0)
		return ret;

	/* Enter active mode */
	ret = qma6100p_write_reg(data, QMA6100P_REG_POWER_MANAGE,
				 QMA6100P_ACTIVE_MODE);
	if (ret < 0)
		return ret;

	dev_info(&client->dev, "QMA6100P initialized, range=%dG, freq=%dHz\n",
		 data->range_g, data->sampling_freq);
	return 0;
}

static void qma6100p_disable(void *data)
{
	struct qma6100p_data *priv = data;

	qma6100p_write_reg(priv, QMA6100P_REG_POWER_MANAGE, 0x00);
}

static void qma6100p_remove_sysfs_link(void *data)
{
	struct qma6100p_data *priv = data;

	if (priv->fixed_kobj) {
		kobject_put(priv->fixed_kobj);
	}
}

/*
 * IIO device naming for multiple instances:
 *   single QMA6100P:  /sys/kernel/qma6100p/device/  (简写)
 *   multiple:         /sys/kernel/qma6100p-3-0012/device/  (带I2C地址)
 */
#define QMA6100P_FIXED_DIR	"qma6100p"
#define QMA6100P_FIXED_DIR_MULTI	"qma6100p-%s"

static int qma6100p_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct qma6100p_data *data;
	int ret;

	/* Verify chip ID */
	ret = i2c_smbus_read_byte_data(client, QMA6100P_REG_CHIP_ID);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "failed to read chip id\n");
	if (ret != QMA6100P_CHIP_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "invalid chip id 0x%02x, expected 0x%02x\n",
				     ret, QMA6100P_CHIP_ID);

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	i2c_set_clientdata(client, indio_dev);

	indio_dev->info = &qma6100p_info;
	indio_dev->name = "qma6100p";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = qma6100p_channels;
	indio_dev->num_channels = ARRAY_SIZE(qma6100p_channels);

	ret = qma6100p_init_device(data);
	if (ret < 0)
		return ret;

	ret = devm_add_action_or_reset(&client->dev, qma6100p_disable, data);
	if (ret < 0)
		return ret;

	ret = devm_iio_device_register(&client->dev, indio_dev);
	if (ret < 0)
		return ret;

	/*
	 * Create a fixed sysfs path using the I2C address as unique key.
	 * First instance:  /sys/kernel/qma6100p/device/   (简写)
	 * Second+:         /sys/kernel/qma6100p-3-0012/device/  (带地址)
	 */
	{
		char dir_name[32];
		struct kobject *parent = kernel_kobj;

		/* Try simple name first; if exists, append I2C address */
		scnprintf(dir_name, sizeof(dir_name), QMA6100P_FIXED_DIR);
		data->fixed_kobj = kobject_create_and_add(dir_name, parent);
		if (!data->fixed_kobj) {
			/* Name taken → use I2C address as unique suffix */
			scnprintf(dir_name, sizeof(dir_name),
				  QMA6100P_FIXED_DIR_MULTI,
				  dev_name(&client->dev));
			data->fixed_kobj = kobject_create_and_add(dir_name,
								parent);
		}

		if (data->fixed_kobj) {
			ret = sysfs_create_link(data->fixed_kobj,
						&indio_dev->dev.kobj, "device");
			if (ret)
				dev_warn(&client->dev,
					 "failed to create sysfs link\n");

			ret = devm_add_action_or_reset(&client->dev,
						       qma6100p_remove_sysfs_link,
						       data);
			if (ret)
				return ret;

			dev_info(&client->dev,
				 "Fixed sysfs: /sys/kernel/%s/device/\n",
				 dir_name);
		}
	}

	dev_info(&client->dev, "QMA6100P driver loaded successfully\n");
	return 0;
}

static int qma6100p_suspend(struct device *dev)
{
	struct qma6100p_data *data;
	struct iio_dev *indio_dev;

	indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	data = iio_priv(indio_dev);

	return qma6100p_write_reg(data, QMA6100P_REG_POWER_MANAGE, 0x00);
}

static int qma6100p_resume(struct device *dev)
{
	struct qma6100p_data *data;
	struct iio_dev *indio_dev;

	indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	data = iio_priv(indio_dev);

	return qma6100p_write_reg(data, QMA6100P_REG_POWER_MANAGE,
				  QMA6100P_ACTIVE_MODE);
}

static DEFINE_SIMPLE_DEV_PM_OPS(qma6100p_pm_ops,
				qma6100p_suspend, qma6100p_resume);

static const struct i2c_device_id qma6100p_i2c_id[] = {
	{ "qma6100p", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, qma6100p_i2c_id);

static const struct of_device_id qma6100p_of_match[] = {
	{ .compatible = "qst,qma6100p" },
	{}
};
MODULE_DEVICE_TABLE(of, qma6100p_of_match);

static struct i2c_driver qma6100p_driver = {
	.driver = {
		.name = "qma6100p",
		.of_match_table = qma6100p_of_match,
		.pm = pm_sleep_ptr(&qma6100p_pm_ops),
	},
	.probe = qma6100p_probe,
	.id_table = qma6100p_i2c_id,
};

module_i2c_driver(qma6100p_driver);

MODULE_AUTHOR("sc-bin");
MODULE_DESCRIPTION("QST QMA6100P 3-Axis Accelerometer IIO Driver");
MODULE_LICENSE("GPL");
