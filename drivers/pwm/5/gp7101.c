// SPDX-License-Identifier: GPL-2.0
/*
 * Giantec GP7101 I2C PWM backlight driver
 *
 * The GP7101 is a single-channel I2C-controlled PWM generator commonly
 * used for LCD backlight dimming.  Only one register is used:
 *   0x03: duty cycle, 0x00 = 0%, 0xff = 100%
 *
 * Panels differ in backlight polarity: some light up with a high duty
 * cycle, some with a low duty cycle.  The "inverted-backlight" device
 * tree boolean property flips the mapping so both panel styles get
 * "brightness = max -> panel fully lit" semantics.
 */

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>

#define GP7101_REG_DUTY		0x03

#define GP7101_BRIGHTNESS_MAX	255

struct gp7101_bl {
	struct i2c_client *client;
	bool inverted;
};

static int gp7101_bl_update_status(struct backlight_device *bl)
{
	struct gp7101_bl *gbl = bl_get_data(bl);
	int brightness = bl->props.brightness;
	int ret;

	/* Report dark when suspended / fb-blanked */
	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	if (gbl->inverted)
		brightness = GP7101_BRIGHTNESS_MAX - brightness;

	ret = i2c_smbus_write_byte_data(gbl->client, GP7101_REG_DUTY,
					(u8)brightness);
	if (ret)
		dev_err(&gbl->client->dev, "backlight update failed: %d\n", ret);

	return ret;
}

static int gp7101_bl_get_brightness(struct backlight_device *bl)
{
	struct gp7101_bl *gbl = bl_get_data(bl);
	int duty;

	duty = i2c_smbus_read_byte_data(gbl->client, GP7101_REG_DUTY);
	if (duty < 0)
		return duty;

	if (gbl->inverted)
		duty = GP7101_BRIGHTNESS_MAX - duty;

	return duty;
}

static const struct backlight_ops gp7101_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = gp7101_bl_update_status,
	.get_brightness = gp7101_bl_get_brightness,
};

static int gp7101_bl_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct backlight_properties props;
	struct backlight_device *bl;
	struct gp7101_bl *gbl;
	const char *label = "backlight-screen";
	u32 default_brightness = GP7101_BRIGHTNESS_MAX;

	gbl = devm_kzalloc(&client->dev, sizeof(*gbl), GFP_KERNEL);
	if (!gbl)
		return -ENOMEM;

	gbl->client = client;
	gbl->inverted = of_property_read_bool(client->dev.of_node,
					      "inverted-backlight");
	of_property_read_u32(client->dev.of_node, "default-brightness",
			     &default_brightness);
	default_brightness = min(default_brightness,
				 (u32)GP7101_BRIGHTNESS_MAX);
	of_property_read_string(client->dev.of_node, "label", &label);

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = GP7101_BRIGHTNESS_MAX;
	props.brightness = default_brightness;

	bl = devm_backlight_device_register(&client->dev, label,
					    &client->dev, gbl,
					    &gp7101_bl_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&client->dev, "failed to register backlight\n");
		return PTR_ERR(bl);
	}

	backlight_update_status(bl);

	i2c_set_clientdata(client, bl);
	dev_info(&client->dev, "GP7101 backlight registered%s (duty reg 0x%02x)\n",
		 gbl->inverted ? ", inverted" : "", GP7101_REG_DUTY);

	return 0;
}

static const struct of_device_id gp7101_bl_of_match[] = {
	{ .compatible = "wpi,gp7101-backlight" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gp7101_bl_of_match);

static const struct i2c_device_id gp7101_bl_id[] = {
	{ "gp7101-backlight", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, gp7101_bl_id);

static struct i2c_driver gp7101_bl_driver = {
	.driver = {
		.name		= "gp7101-backlight",
		.of_match_table	= of_match_ptr(gp7101_bl_of_match),
	},
	.probe		= gp7101_bl_probe,
	.id_table	= gp7101_bl_id,
};

module_i2c_driver(gp7101_bl_driver);

MODULE_AUTHOR("WalnutPi");
MODULE_DESCRIPTION("GP7101 I2C PWM backlight driver");
MODULE_LICENSE("GPL");
