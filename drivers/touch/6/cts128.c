// SPDX-License-Identifier: GPL-2.0
/*
 * CTS128 Touchscreen Driver for Linux
 *
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/ratelimit.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/unaligned.h>

/* CTS128 Register Definitions */
#define CTS128_REG_FW_VERSION       0xA6
#define CTS128_REG_VENDOR_ID        0xA8
#define CTS128_REG_WORK_MODE        0x00
#define CTS128_REG_TOUCH_POINT      0x02
#define CTS128_REG_COORD_BASE       0x03

/* Touch event definitions */
#define CTS128_TOUCH_DOWN           0x00
#define CTS128_TOUCH_UP             0x01
#define CTS128_TOUCH_CONTACT        0x02

/* Device constants */
#define CTS128_MAX_CONTACTS         5
#define CTS128_MAX_COORD_SIZE       6
#define CTS128_POINT_DATA_SIZE      6  /* X(2) + Y(2) + RES(2) as per RTOS version */
#define CTS128_MAX_PACKET_SIZE      (1 + CTS128_MAX_CONTACTS * CTS128_POINT_DATA_SIZE)  /* Status byte + point data */

#define CTS128_NAME                 "cts128"
#define CTS128_DEFAULT_MAX_X        1024
#define CTS128_DEFAULT_MAX_Y        600

struct cts128_ts_data {
    struct i2c_client *client;
    struct input_dev *input;
    struct touchscreen_properties prop;
    u16 max_x;
    u16 max_y;
    struct regulator *vcc;
    struct regulator *iovcc;

    struct gpio_desc *reset_gpio;
    struct gpio_desc *irq_gpio;

    struct regmap *regmap;

#if defined(CONFIG_DEBUG_FS)
    struct dentry *debug_dir;
#endif

    struct mutex mutex;
    int max_support_points;
    u8 fw_version[10];
    u8 vendor_id;
    bool x_flip;  /* X-axis flip flag */
    bool y_flip;  /* Y-axis flip flag */
};

static const struct regmap_config cts128_i2c_regmap_config = {
    .reg_bits = 8,
    .val_bits = 8,
};

static irqreturn_t cts128_ts_interrupt(int irq, void *dev_id)
{
    struct cts128_ts_data *tsdata = dev_id;
    struct device *dev = &tsdata->client->dev;
    unsigned char data[CTS128_MAX_PACKET_SIZE];
    int i, error;
    u8 num_points;
    u16 x, y;
    u8 id, pressure, major_touch;
    u8 event;

    /* Read touch point status - only read 0x02 to 0x1F as per RTOS version */
    error = regmap_raw_read(tsdata->regmap, CTS128_REG_TOUCH_POINT, data, 
                           CTS128_MAX_PACKET_SIZE);
    if (error) {
        dev_err_ratelimited(dev, "Failed to read touch data: %d\n", error);
        goto out;
    }

    num_points = data[0] & 0x0F;
    if (num_points > CTS128_MAX_CONTACTS) {
        num_points = CTS128_MAX_CONTACTS;
    }
    
    /* Process up to max contacts */
    for (i = 0; i < num_points; i++) {
        int offset = 1 + (i * CTS128_POINT_DATA_SIZE);
        
        if (offset + CTS128_POINT_DATA_SIZE > CTS128_MAX_PACKET_SIZE) {
            break;
        }
        
        /* Extract coordinates following RTOS version structure */
        x = ((data[offset + 0] & 0x0F) << 8) | data[offset + 1]; // x from bytes 0,1 at each point
        y = ((data[offset + 2] & 0x0F) << 8) | data[offset + 3]; // y from bytes 2,3 at each point
        
        /* Extract pressure and touch area from bytes 4,5 at each point */
        pressure = data[offset + 4];
        major_touch = data[offset + 5];
        
        id = i; // Use index as track ID
        
        /* Apply X-axis flip if enabled */
        if (tsdata->x_flip) {
            x = tsdata->max_x - x;
        }
        
        /* Apply Y-axis flip if enabled */
        if (tsdata->y_flip) {
            y = tsdata->max_y - y;
        }
        
        input_mt_slot(tsdata->input, id);
        input_mt_report_slot_state(tsdata->input, MT_TOOL_FINGER, true);
        
        input_report_abs(tsdata->input, ABS_MT_POSITION_X, x);
        input_report_abs(tsdata->input, ABS_MT_POSITION_Y, y);
        
        /* Report additional parameters required for proper evtest functionality */
        input_report_abs(tsdata->input, ABS_MT_PRESSURE, pressure);
        input_report_abs(tsdata->input, ABS_MT_TOUCH_MAJOR, major_touch);
    }
    
    /* Clear unused slots */
    for (i = num_points; i < CTS128_MAX_CONTACTS; i++) {
        input_mt_slot(tsdata->input, i);
        input_mt_report_slot_state(tsdata->input, MT_TOOL_FINGER, false);
    }

    input_mt_report_pointer_emulation(tsdata->input, true);
    input_sync(tsdata->input);

out:
    return IRQ_HANDLED;
}

static ssize_t cts128_fw_version_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct cts128_ts_data *tsdata = i2c_get_clientdata(client);
    unsigned int val;
    int error;

    error = regmap_read(tsdata->regmap, CTS128_REG_FW_VERSION, &val);
    if (error)
        return error;

    return snprintf(buf, PAGE_SIZE, "0x%02X\n", val);
}

static DEVICE_ATTR_RO(cts128_fw_version);

static ssize_t cts128_vendor_id_show(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct cts128_ts_data *tsdata = i2c_get_clientdata(client);
    unsigned int val;
    int error;

    error = regmap_read(tsdata->regmap, CTS128_REG_VENDOR_ID, &val);
    if (error)
        return error;

    return snprintf(buf, PAGE_SIZE, "0x%02X\n", val);
}

static DEVICE_ATTR_RO(cts128_vendor_id);

static struct attribute *cts128_attrs[] = {
    &dev_attr_cts128_fw_version.attr,
    &dev_attr_cts128_vendor_id.attr,
    NULL
};

ATTRIBUTE_GROUPS(cts128);

static int cts128_parse_dt(struct device *dev, struct cts128_ts_data *tsdata)
{
    u32 val;

    /* Get max coordinates from device tree or use defaults */
    if (!device_property_read_u32(dev, "cts128,max-x", &val))
        tsdata->max_x = val;
    else
        tsdata->max_x = CTS128_DEFAULT_MAX_X;

    if (!device_property_read_u32(dev, "cts128,max-y", &val))
        tsdata->max_y = val;
    else
        tsdata->max_y = CTS128_DEFAULT_MAX_Y;

    /* Check for X-axis flip property - only set if value is 1 */
    if (!device_property_read_u32(dev, "cts128,x-flip", &val))
        tsdata->x_flip = (val == 1);
    else
        tsdata->x_flip = false;

    /* Check for Y-axis flip property - only set if value is 1 */
    if (!device_property_read_u32(dev, "cts128,y-flip", &val))
        tsdata->y_flip = (val == 1);
    else
        tsdata->y_flip = false;

    return 0;
}

static int cts128_ts_probe(struct i2c_client *client)
{
    struct device *dev = &client->dev;
    struct cts128_ts_data *tsdata;
    struct input_dev *input;
    int error;
    unsigned int val;

    dev_info(dev, "Probing CTS128 touchscreen\n");

    /* Allocate driver data */
    tsdata = devm_kzalloc(dev, sizeof(*tsdata), GFP_KERNEL);
    if (!tsdata) {
        dev_err(dev, "Failed to allocate driver data\n");
        return -ENOMEM;
    }

    /* Initialize basic fields */
    tsdata->client = client;
    tsdata->max_support_points = CTS128_MAX_CONTACTS;
    tsdata->max_x = CTS128_DEFAULT_MAX_X;
    tsdata->max_y = CTS128_DEFAULT_MAX_Y;
    tsdata->x_flip = false;  /* Default to no flip */
    tsdata->y_flip = false;  /* Default to no flip */
    mutex_init(&tsdata->mutex);

    /* Initialize regmap */
    tsdata->regmap = devm_regmap_init_i2c(client, &cts128_i2c_regmap_config);
    if (IS_ERR(tsdata->regmap)) {
        dev_err(dev, "Failed to initialize regmap\n");
        return PTR_ERR(tsdata->regmap);
    }

    /* Parse device tree properties */
    cts128_parse_dt(dev, tsdata);

    /* Request regulators - make them optional */
    tsdata->vcc = devm_regulator_get_optional(dev, "vcc");
    if (IS_ERR(tsdata->vcc)) {
        error = PTR_ERR(tsdata->vcc);
        if (error == -ENODEV) {
            tsdata->vcc = NULL;
            dev_info(dev, "No vcc regulator found, assuming always powered\n");
        } else if (error != -EPROBE_DEFER) {
            dev_err(dev, "Failed to get vcc regulator: %d\n", error);
            return error;
        }
    }

    tsdata->iovcc = devm_regulator_get_optional(dev, "iovcc");
    if (IS_ERR(tsdata->iovcc)) {
        error = PTR_ERR(tsdata->iovcc);
        if (error == -ENODEV) {
            tsdata->iovcc = NULL;
            dev_info(dev, "No iovcc regulator found, assuming always powered\n");
        } else if (error != -EPROBE_DEFER) {
            dev_err(dev, "Failed to get iovcc regulator: %d\n", error);
            return error;
        }
    }

    /* Enable regulators if present */
    if (tsdata->vcc) {
        error = regulator_enable(tsdata->vcc);
        if (error) {
            dev_err(dev, "Failed to enable vcc: %d\n", error);
            return error;
        }
    }

    if (tsdata->iovcc) {
        error = regulator_enable(tsdata->iovcc);
        if (error) {
            dev_err(dev, "Failed to enable iovcc: %d\n", error);
            if (tsdata->vcc)
                regulator_disable(tsdata->vcc);
            return error;
        }
    }

    /* Request GPIOs */
    tsdata->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(tsdata->reset_gpio)) {
        error = PTR_ERR(tsdata->reset_gpio);
        dev_err(dev, "Failed to request reset GPIO: %d\n", error);
        return error;
    }

    tsdata->irq_gpio = devm_gpiod_get_optional(dev, "cts128,irq", GPIOD_IN);
    if (IS_ERR(tsdata->irq_gpio)) {
        error = PTR_ERR(tsdata->irq_gpio);
        dev_err(dev, "Failed to request IRQ GPIO: %d\n", error);
        return error;
    }

    /* Hardware reset sequence if reset GPIO is available - match RTOS timing */
    if (tsdata->reset_gpio) {
        gpiod_set_value_cansleep(tsdata->reset_gpio, 0);
        msleep(20);  /* Reset low time from RTOS */
        gpiod_set_value_cansleep(tsdata->reset_gpio, 1);
        msleep(10); /* Wait after release from RTOS */
        gpiod_set_value_cansleep(tsdata->reset_gpio, 0);
        msleep(50); /* Final delay from RTOS */
    } else {
        /* Wait for device initialization without reset */
        msleep(50); /* Wait for touch startup as per RTOS */
    }

    /* Verify device presence by reading a register as per RTOS */
    error = regmap_raw_read(tsdata->regmap, 0xA3, &val, 1);
    if (error || val == 0xFF || val == 0x00) {
        dev_err(dev, "Failed to read device ID or invalid ID: 0x%02X\n", val);
        error = -ENODEV;
        goto err_free_regulators;
    }

    /* Allocate input device */
    input = devm_input_allocate_device(dev);
    if (!input) {
        dev_err(dev, "Failed to allocate input device\n");
        error = -ENOMEM;
        goto err_free_regulators;
    }

    tsdata->input = input;
    input->name = CTS128_NAME;
    input->id.bustype = BUS_I2C;
    input->dev.parent = dev;

    /* Set input capabilities */
    __set_bit(EV_ABS, input->evbit);
    __set_bit(EV_KEY, input->evbit);
    __set_bit(BTN_TOUCH, input->keybit);
    __set_bit(INPUT_PROP_DIRECT, input->propbit);

    /* Configure touchscreen dimensions */
    input_set_abs_params(input, ABS_MT_POSITION_X, 0, tsdata->max_x, 0, 0);
    input_set_abs_params(input, ABS_MT_POSITION_Y, 0, tsdata->max_y, 0, 0);
    input_set_abs_params(input, ABS_MT_WIDTH_MAJOR, 0, 15, 0, 0);
    input_set_abs_params(input, ABS_MT_PRESSURE, 0, 255, 0, 0);

    /* Parse touchscreen properties */
    touchscreen_parse_properties(input, true, &tsdata->prop);

    /* Initialize MT slots */
    error = input_mt_init_slots(input, CTS128_MAX_CONTACTS, INPUT_MT_DIRECT);
    if (error) {
        dev_err(dev, "Failed to initialize MT slots: %d\n", error);
        goto err_free_regulators;
    }

    /* Request interrupt */
    error = devm_request_threaded_irq(dev, client->irq, NULL, cts128_ts_interrupt,
                                     IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
                                     client->name, tsdata);
    if (error) {
        dev_err(dev, "Failed to request IRQ: %d\n", error);
        goto err_free_regulators;
    }

    /* Register input device */
    error = input_register_device(tsdata->input);
    if (error) {
        dev_err(dev, "Failed to register input device: %d\n", error);
        goto err_free_regulators;
    }

    /* Store data in client */
    i2c_set_clientdata(client, tsdata);

    dev_info(dev, "CTS128 touchscreen driver initialized successfully with x_flip=%s, y_flip=%s\n", 
             tsdata->x_flip ? "true" : "false", tsdata->y_flip ? "true" : "false");
    return 0;

err_free_regulators:
    if (tsdata->iovcc)
        regulator_disable(tsdata->iovcc);
    if (tsdata->vcc)
        regulator_disable(tsdata->vcc);
    return error;
}

static void cts128_ts_remove(struct i2c_client *client)
{
    struct cts128_ts_data *tsdata = i2c_get_clientdata(client);

    sysfs_remove_groups(&client->dev.kobj, cts128_groups);
    
    if (tsdata->iovcc)
        regulator_disable(tsdata->iovcc);
    if (tsdata->vcc)
        regulator_disable(tsdata->vcc);
}

static int cts128_ts_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct cts128_ts_data *tsdata = i2c_get_clientdata(client);
    int ret = 0;

    if (device_may_wakeup(dev)) {
        enable_irq_wake(client->irq);
    } else {
        /* Put device into sleep mode */
        ret = regmap_write(tsdata->regmap, CTS128_REG_WORK_MODE, 0x03); // Sleep mode
        if (ret)
            dev_warn(dev, "Failed to enter sleep mode: %d\n", ret);
    }

    return ret;
}

static int cts128_ts_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct cts128_ts_data *tsdata = i2c_get_clientdata(client);
    int ret = 0;

    if (device_may_wakeup(dev)) {
        disable_irq_wake(client->irq);
    } else {
        /* Wake up device - match RTOS reset sequence */
        if (tsdata->reset_gpio) {
            gpiod_set_value_cansleep(tsdata->reset_gpio, 0);
            msleep(20);
            gpiod_set_value_cansleep(tsdata->reset_gpio, 1);
            msleep(10);
            gpiod_set_value_cansleep(tsdata->reset_gpio, 0);
            msleep(50);
        } else {
            msleep(50);
        }
    }

    return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(cts128_ts_pm_ops, cts128_ts_suspend, cts128_ts_resume);

static const struct of_device_id cts128_of_match[] = {
    { .compatible = "cts128,touchscreen", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cts128_of_match);

static const struct i2c_device_id cts128_ts_id[] = {
    { CTS128_NAME, 0 },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, cts128_ts_id);

static struct i2c_driver cts128_ts_driver = {
    .driver = {
        .name = CTS128_NAME,
        .of_match_table = cts128_of_match,
        .pm = pm_sleep_ptr(&cts128_ts_pm_ops),
    },
    .id_table = cts128_ts_id,
    .probe = cts128_ts_probe,
    .remove = cts128_ts_remove,
};

module_i2c_driver(cts128_ts_driver);

MODULE_AUTHOR("sc-bin");
MODULE_DESCRIPTION("CTS128 Touchscreen Driver");
MODULE_LICENSE("GPL v2");