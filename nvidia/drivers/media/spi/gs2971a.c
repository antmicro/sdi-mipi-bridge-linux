/*
 * Copyright (c) 2021, Antmicro Ltd. All rights reserved.
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <media/camera_common.h>
#include <media/mc_common.h>
#include <media/tegra-v4l2-camera.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-of.h>

#define GS2971A_DEFAULT_WIDTH	1920
#define GS2971A_DEFAULT_HEIGHT	1080
#define GS2971A_DEFAULT_FMT	MEDIA_BUS_FMT_VYUY8_2X8

struct gs2971a_priv {
	struct v4l2_subdev		*subdev;
	struct media_pad		pad;
	struct v4l2_ctrl_handler	hdl;
	struct camera_common_data	*s_data;
	u32				mbus_fmt_code;
	u32				width;
	u32				height;
};

static int gs2971a_enable_sdi_level_conversion(struct spi_device *spi)
{
	struct spi_message msg;
	struct spi_transfer spi_xfer = {};
	u8 data[4];
	int ret;

	data[0] = 0x00;
	data[1] = 0x01;
	data[2] = 0x00;
	data[3] = 0x00;

	spi_xfer.tx_buf = data;
	spi_xfer.len = 4;

	spi_message_init(&msg);
	spi_message_add_tail(&spi_xfer, &msg);

	ret = spi_sync(spi, &msg);

	if (ret) {
		dev_err(&spi->dev, "unable to set SDI level conversion!\n");
		return ret;
	}

	return 0;
}

static int gs2971a_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
		struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subdev_subscribe(sd, fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subdev_subscribe_event(sd, fh, sub);
	default:
		return -EINVAL;
	}
}

static int gs2971a_g_mbus_config(struct v4l2_subdev *sd,
		struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2;

	cfg->flags = V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	cfg->flags |= V4L2_MBUS_CSI2_2_LANE; /* XXX wierd */

	return 0;
}

static int gs2971a_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct spi_device *spi = v4l2_get_subdevdata(sd);
	int ret;

	if (enable) {
		ret = gs2971a_enable_sdi_level_conversion(spi);
		if (ret) {
			return ret;
		}
	}
	return 0;
}

static int gs2971a_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct spi_device *spi = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&spi->dev);
	struct gs2971a_priv *priv = (struct gs2971a_priv *)s_data->priv;

	format->format.width      = priv->width;
	format->format.height     = priv->height;
	format->format.code       = priv->mbus_fmt_code;
	format->format.field      = V4L2_FIELD_NONE;
	format->format.colorspace = V4L2_COLORSPACE_SRGB;

	return 0;
}

static int gs2971a_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct spi_device *spi = v4l2_get_subdevdata(sd);
	struct camera_common_data *s_data = to_camera_common_data(&spi->dev);
	struct gs2971a_priv *priv = (struct gs2971a_priv *)s_data->priv;

	format->format.colorspace = V4L2_COLORSPACE_SRGB;
	format->format.field = V4L2_FIELD_NONE;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	priv->mbus_fmt_code = format->format.code;
	priv->width         = format->format.width;
	priv->height        = format->format.height;

	return 0;
}

static uint16_t gs2971a_mbus_formats[] = {
	MEDIA_BUS_FMT_UYVY8_2X8,
	MEDIA_BUS_FMT_VYUY8_2X8,
	MEDIA_BUS_FMT_YUYV8_2X8,
	MEDIA_BUS_FMT_YVYU8_2X8,
};

static int gs2971a_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(gs2971a_mbus_formats))
		return -EINVAL;

	code->code = gs2971a_mbus_formats[code->index];

	return 0;
}

static struct v4l2_frmsize_discrete gs2971a_framesizes[] = {
	{1280,  720},
	{1920, 1080},
};

static int gs2971a_enum_framesizes(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(gs2971a_framesizes))
		return -EINVAL;

	fse->min_width  = fse->max_width  = gs2971a_framesizes[fse->index].width;
	fse->min_height = fse->max_height = gs2971a_framesizes[fse->index].height;

	return 0;
}

static int gs2971a_s_ctrl(struct v4l2_ctrl *ctrl)
{
	return 0;
}

static int gs2971a_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	return 0;
}

static const struct v4l2_ctrl_ops gs2971a_ctrl_ops = {
	.g_volatile_ctrl = gs2971a_g_volatile_ctrl,
	.s_ctrl          = gs2971a_s_ctrl,
};

static const struct v4l2_subdev_core_ops gs2971a_core_ops = {
	.subscribe_event   = gs2971a_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops gs2971a_video_ops = {
	.g_mbus_config = gs2971a_g_mbus_config,
	.s_stream      = gs2971a_s_stream,
};

static const struct v4l2_subdev_pad_ops gs2971a_pad_ops = {
	.set_fmt        = gs2971a_set_fmt,
	.get_fmt        = gs2971a_get_fmt,
	.enum_mbus_code = gs2971a_enum_mbus_code,
	.enum_frame_size = gs2971a_enum_framesizes,
};

static const struct v4l2_subdev_ops gs2971a_subdev_ops = {
	.core  = &gs2971a_core_ops,
	.video = &gs2971a_video_ops,
	.pad   = &gs2971a_pad_ops,
};

static const struct media_entity_operations gs2971a_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

const struct of_device_id gs2971a_of_match[] = {
	{ .compatible = "semtech,gs2971a",},
	{ },
};

MODULE_DEVICE_TABLE(of, gs2971a_of_match);

/* Read image format from camera,
 * should be only called once, during initialization
 * */
static int gs2971a_probe(struct spi_device *spi)
{
	struct gs2971a_priv *priv;
	struct device *dev = &spi->dev;
	int ret;

	struct v4l2_of_endpoint *endpoint;
	struct device_node *ep;
	struct camera_common_data *common_data;

	common_data = devm_kzalloc(&spi->dev,
			sizeof(struct camera_common_data), GFP_KERNEL);
	if (!common_data)
		return -ENOMEM;

	priv = devm_kzalloc(&spi->dev, sizeof(struct gs2971a_priv),
			GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi->irq = -1;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "unable to setup SPI!\n");
		return ret;
	}

	priv->subdev = &common_data->subdev;
	priv->subdev->ctrl_handler = &priv->hdl;

	priv->mbus_fmt_code = GS2971A_DEFAULT_FMT;
	priv->width = GS2971A_DEFAULT_WIDTH;
	priv->height = GS2971A_DEFAULT_HEIGHT;
	priv->s_data = common_data;

	ep = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!ep) {
		dev_err(dev, "missing endpoint node\n");
		return -EINVAL;
	}

	endpoint = v4l2_of_alloc_parse_endpoint(ep);
	if (IS_ERR(endpoint)) {
		dev_err(dev, "failed to parse endpoint\n");
		return PTR_ERR(endpoint);
	}

	v4l2_spi_subdev_init(&common_data->subdev, spi, &gs2971a_subdev_ops);

	priv->subdev->dev = &spi->dev;

	common_data->priv = priv;
	common_data->dev = &spi->dev;
	common_data->ctrl_handler = &priv->hdl;

	dev_info(dev, "Probing simple sensor");

	snprintf(priv->subdev->name, sizeof(priv->subdev->name), "simple-sensor");

	v4l2_ctrl_handler_init(&priv->hdl, 0);

	ret = v4l2_ctrl_handler_setup(priv->subdev->ctrl_handler);
	common_data->numctrls = 0;

	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	priv->subdev->entity.ops = &gs2971a_media_ops;
	ret = tegra_media_entity_init(&priv->subdev->entity, 1,
				&priv->pad, true, true);
	if (ret < 0)
		return ret;

	ret = camera_common_initialize(common_data, "gs2971a");
	if (ret) {
		dev_err(&spi->dev, "Failed to initialize tegra common!\n");
		return ret;
	}

	ret = v4l2_async_register_subdev(priv->subdev);
	if (ret < 0)
		return ret;

	dev_info(&spi->dev, "sensor %s registered\n",
			priv->subdev->name);

	return 0;
}

static int gs2971a_remove(struct spi_device *spi)
{
	struct v4l2_subdev *sd = spi_get_drvdata(spi);

	v4l2_async_unregister_subdev(sd);
	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	return 0;
}

static struct spi_device_id gs2971a_id[] = {
	{"gs2971a", 0},
	{}
};

MODULE_DEVICE_TABLE(spi, gs2971a_id);

static struct spi_driver gs2971a_driver = {
	.driver = {
		.name = "gs2971a",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(gs2971a_of_match),
	},
	.probe = gs2971a_probe,
	.remove = gs2971a_remove,
	.id_table = gs2971a_id,
};

module_spi_driver(gs2971a_driver);

MODULE_AUTHOR("Maciej Sobkowski <msobkowski@antmicro.com>");
MODULE_DESCRIPTION("Semtech GS2971A driver");
MODULE_LICENSE("GPL");
