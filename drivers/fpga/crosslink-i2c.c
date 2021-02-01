/*
 * FPGA Manager Driver for Lattice CrossLink.
 *
 *  Copyright (c) 2021 Antmicro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This driver adds support to the FPGA manager for configuring the SRAM of
 * Lattice CrossLink FPGAs through I2C.
 */

#include <linux/fpga/fpga-mgr.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/stringify.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/slab.h>

#define CROSSLINK_IDCODE		0x43002C01
#define CROSSLINK_RESET_RETRY_CNT	2

u8 isc_enable[]          = {0xC6, 0x00, 0x00};
u8 isc_erase[]           = {0x0E, 0x01, 0x00, 0x00};
u8 isc_disable[]         = {0x26, 0x00, 0x00, 0x00};

u8 idcode_pub[]          = {0xE0, 0x00, 0x00, 0x00};
u8 read_usercode[]       = {0xC0, 0x00, 0x00, 0x00};

u8 lsc_init[]            = {0x46, 0x00, 0x00, 0x00};
u8 lsc_bitstream_burst[] = {0x7A, 0x00, 0x00, 0x00};
u8 lsc_read_status[]     = {0x3C, 0x00, 0x00, 0x00};
u8 lsc_refresh[]         = {0x79, 0x00, 0x00};
u8 lsc_check_busy[]      = {0xF0, 0x00, 0x00, 0x00};

u8 activation_msg[] = {0xA4, 0xC6, 0xF4, 0x8A};

#define STATUS_DONE	BIT(16)
#define STATUS_BUSY	BIT(20)
#define STATUS_FAIL	BIT(21)

struct crosslink_fpga_priv {
	struct i2c_client *dev;
	struct gpio_desc *reset;
	bool bistream_loaded;
};

static int crosslink_fpga_reset(struct crosslink_fpga_priv *priv)
{
	struct i2c_client *client = priv->dev;
	struct i2c_msg msg[2];
	u32 idcode;
	int ret;

	gpiod_set_value_cansleep(priv->reset, 1);

	memset(msg, 0, sizeof(msg));
	msg[0].addr = client->addr;
	msg[0].buf = activation_msg;
	msg[0].len = ARRAY_SIZE(activation_msg);

	ret = i2c_transfer(client->adapter, msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "Writing activation code failed! (%d)\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(priv->reset, 0);

	mdelay(10);

	memset(msg, 0, sizeof(msg));
	msg[0].addr = client->addr;
	msg[0].buf = isc_enable;
	msg[0].len = ARRAY_SIZE(isc_enable);

	ret = i2c_transfer(client->adapter, msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "ISC_ENABLE command failed! (%d)\n", ret);
		return ret;
	}

	memset(msg, 0, sizeof(msg));
	msg[0].addr = client->addr;
	msg[0].buf = idcode_pub;
	msg[0].len = ARRAY_SIZE(idcode_pub);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = (u8 *) &idcode;
	msg[1].len = sizeof(idcode);

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_err(&client->dev, "IDCODE command failed! (%d)\n", ret);
		return ret;
	}

	dev_dbg(&client->dev, "IDCODE: 0x%x\n", idcode);

	if (idcode != CROSSLINK_IDCODE)
		return -ENODEV;

	return 0;
}

static enum fpga_mgr_states crosslink_fpga_ops_state(struct fpga_manager *mgr)
{
	return FPGA_MGR_STATE_OPERATING;
}

static int crosslink_fpga_ops_write_init(struct fpga_manager *mgr,
				     struct fpga_image_info *info,
				     const char *buf, size_t count)
{
	struct crosslink_fpga_priv *priv = mgr->priv;
	struct i2c_client *client = priv->dev;
	struct i2c_msg msg[2];
	int ret;
	int i;

	for (i = 0; i < CROSSLINK_RESET_RETRY_CNT; i++) {
		ret = crosslink_fpga_reset(priv);
		if (ret == 0)
			break;
	}

	if (ret) {
		dev_err(&client->dev, "FPGA reset failed! (%d)\n", ret);
		return ret;
	}

	memset(msg, 0, sizeof(msg));
	msg[0].addr = client->addr;
	msg[0].buf = isc_enable;
	msg[0].len = ARRAY_SIZE(isc_enable);
	if (ret < 0) {
		dev_err(&client->dev, "ISC_ENABLE command failed! (%d)\n", ret);
		return ret;
	}

	mdelay(1);

	memset(msg, 0, sizeof(msg));
	msg[0].addr = client->addr;
	msg[0].buf = isc_erase;
	msg[0].len = ARRAY_SIZE(isc_erase);

	ret = i2c_transfer(client->adapter, msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "ISC_ERASE command failed! (%d)\n", ret);
		return ret;
	}

	mdelay(50);

	return 0;
}

static int crosslink_fpga_ops_write(struct fpga_manager *mgr,
				const char *buf, size_t count)
{
	struct crosslink_fpga_priv *priv = mgr->priv;
	struct i2c_client *client = priv->dev;
	struct i2c_msg msg[2];
	u8* msgbuf = kzalloc(count + 4, GFP_KERNEL);
	int msglen = count + 4;
	int maxlen = U16_MAX;
	int msgnum = DIV_ROUND_UP(msglen,maxlen);
	struct i2c_msg *bitstream_msg;
	u32 status;
	int ret;
	int i;

	msgbuf = kzalloc(count + 4, GFP_KERNEL);
	if (!msgbuf)
		return -ENOMEM;

	bitstream_msg = kzalloc(sizeof(struct i2c_msg) * msgnum, GFP_KERNEL);
	if (!bitstream_msg)
		return -ENOMEM;

	memset(msg, 0, sizeof(msg));

	msg[0].addr = client->addr;
	msg[0].buf = lsc_init;
	msg[0].len = ARRAY_SIZE(lsc_init);

	ret = i2c_transfer(client->adapter, msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "LSC_INIT command failed! (%d)\n", ret);
		return ret;
	}

	mdelay(100);

	memcpy(msgbuf, lsc_bitstream_burst, 4);
	memcpy(msgbuf + 4, buf, count);

	for(i = 0; i < msgnum; i++) {
		bitstream_msg[i].addr = client->addr;
		bitstream_msg[i].buf  = msgbuf + (i * maxlen);
		bitstream_msg[i].len  = msglen > maxlen ? maxlen : msglen;
		if (i > 0)
			bitstream_msg[i].flags = I2C_M_NOSTART;

		msglen -= maxlen;
	}

	ret = i2c_transfer(client->adapter, bitstream_msg, msgnum);
	if (ret < 0) {
		dev_err(&client->dev, "BITSTREAM_BURST command failed! (%d)\n", ret);
		return ret;
	}

	memset(msg, 0, sizeof(msg));
	msg[0].addr = client->addr;
	msg[0].buf = lsc_read_status;
	msg[0].len = ARRAY_SIZE(lsc_read_status);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = (u8 *) &status;
	msg[1].len = sizeof(status);

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_err(&client->dev, "LSC_READ_STATUS command failed! (%d)\n", ret);
		return ret;
	}

	kfree(msgbuf);
	kfree(bitstream_msg);

	dev_dbg(&client->dev, "STATUS: 0x%x\n (done: %s busy: %s, fail: %s)", status,
			status & STATUS_DONE ? "yes" : "no",
			status & STATUS_BUSY ? "yes" : "no",
			status & STATUS_FAIL ? "yes" : "no");

	if (!(status & STATUS_DONE)) {
		dev_err(&client->dev, "Bitstream loading failed!\n");
		return -EIO;
	}

	return 0;
}

static int crosslink_fpga_ops_write_complete(struct fpga_manager *mgr,
					 struct fpga_image_info *info)
{
	struct crosslink_fpga_priv *priv = mgr->priv;
	struct i2c_client *client = priv->dev;
	struct i2c_msg msg[2];
	int ret;

	memset(msg, 0, sizeof(msg));
	msg[0].addr = client->addr;
	msg[0].buf = isc_disable;
	msg[0].len = ARRAY_SIZE(isc_disable);

	ret = i2c_transfer(client->adapter, msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "ISC_DISABLE command failed! (%d)\n", ret);
		return ret;
	}

	dev_info(&client->dev, "Bitstream loading successful!\n");

	return 0;
}

static const struct fpga_manager_ops crosslink_fpga_ops = {
	.state = crosslink_fpga_ops_state,
	.write_init = crosslink_fpga_ops_write_init,
	.write = crosslink_fpga_ops_write,
	.write_complete = crosslink_fpga_ops_write_complete,
};

static int crosslink_fpga_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct crosslink_fpga_priv *priv;
	struct i2c_msg msg[2];
	int ret;
	int i;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = client;

	priv->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset)) {
		ret = PTR_ERR(priv->reset);
		dev_err(dev, "Failed to get CRESET_B GPIO: %d\n", ret);
		return ret;
	}

	/*
	 * XXX: every second reset causes device to respond to every I2C
	 * command with 0xFF, so we try 2nd time if 1st time caused this
	 * behavior
	 */
	for (i = 0; i < CROSSLINK_RESET_RETRY_CNT; i++) {
		ret = crosslink_fpga_reset(priv);
		if (ret == 0)
			break;
	}

	if (ret) {
		dev_err(dev, "FPGA reset failed! (%d)\n", ret);
		return ret;
	}

	/* Register with the FPGA manager */
	return fpga_mgr_register(dev, "Lattice CrossLink FPGA Manager",
				 &crosslink_fpga_ops, priv);
}

static int crosslink_fpga_remove(struct i2c_client *client)
{
	fpga_mgr_unregister(&client->dev);
	return 0;
}

static const struct i2c_device_id crosslink_id[] = {
	{ "crosslink-fpga-mgr", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, crosslink_id);

static struct i2c_driver crosslink_fpga_driver = {
	.probe = crosslink_fpga_probe,
	.remove = crosslink_fpga_remove,
	.driver = {
		.name = "crosslink-i2c",
	},
	.id_table = crosslink_id,
};

module_i2c_driver(crosslink_fpga_driver);

MODULE_AUTHOR("Maciej Sobkowski <msobkowski@antmicro.com>");
MODULE_DESCRIPTION("Lattice CrossLink i2c FPGA Manager");
MODULE_LICENSE("GPL v2");
