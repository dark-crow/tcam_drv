
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A V4L2 driver for COX LWIR cameras.
 *
 * Copyright (C).
 * Copyright (C).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/videodev2.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-image-sizes.h>
#include <media/v4l2-mediabus.h>


#define		TVDODRV_DBG_MSG
#define 	TVDO_SLAVE_ID	0x54

#define		DEFAULT_TVDO_WIDTH	(384)
#define		DEFAULT_TVDO_HEIGHT	(288)


typedef enum __thermal_video_mode_id__ {
	TVDO_MODE_QVGA_384_288 = 0,
	TVDO_NUM_MODES,
} eTVDOMODE_ID;

typedef enum __thermal_video_mode_framerates__ {
	TVDO_08_FPS = 0,
	TVDO_15_FPS,
	TVDO_30_FPS,
	TVDO_60_FPS,
	TVDO_NUM_FRAMERATES,
} eTVDOMODE_FPS;

typedef struct __thermal_video_pixel_format__ {
	u32 		code;
	u32 		colorspace;
} TVDO_PIXFMT_T;

typedef struct __thermal_video_mode_parameter__ {
	eTVDOMODE_ID		id;
	
	u32			hact;
	u32 		htot;
	u32 		vact;
	u32 		vtot;

	u32 		max_fps;
} TVDOMODE_PARAM_T;

typedef struct __thermal_video_controls__ {
	struct v4l2_ctrl_handler	handler;
	
	struct {
		struct v4l2_ctrl*	auto_gain;
		struct v4l2_ctrl*	gain;
	};

	struct v4l2_ctrl*	pixel_rate;
	struct v4l2_ctrl*	brightness;
	struct v4l2_ctrl*	saturation;
	struct v4l2_ctrl*	contrast;
	struct v4l2_ctrl*	hue;
	struct v4l2_ctrl*	hflip;
	struct v4l2_ctrl*	vflip;
} TVDO_CTRLS_T;

/* regulator supplies */
static const char * const	g_tvdo_supply_name[] = {
	"DOVDD", /* Digital I/O (1.8V) supply */
	"AVDD",  /* Analog (2.8V) supply */
	"DVDD",  /* Digital Core (1.5V) supply */
};

#define TVDO_NUM_SUPPLIES ARRAY_SIZE(g_tvdo_supply_name)

typedef struct __thermal_video_device__ {
	struct i2c_client*			i2c_client;
	struct v4l2_subdev 			sd;
	struct media_pad 			pad;

	struct v4l2_fwnode_endpoint	ep; /* the parsed DT endpoint info */
			
	struct regulator_bulk_data	supplies[TVDO_NUM_SUPPLIES];
	
	
	/* lock to protect all members below */
	struct mutex				lock;

	int							power_count;


	struct v4l2_mbus_framefmt	fmt;
	
	TVDOMODE_PARAM_T			curr_mode;
	TVDOMODE_PARAM_T			last_mode;
	eTVDOMODE_FPS				curr_fr;
	struct v4l2_fract			frame_interval;

	TVDO_CTRLS_T				ctrls;

	bool						streaming;

	eTVDOMODE_ID				curr_id;
} TVDO_DEV_T;


static	u8			g_f_fpga_det = 0;	//	FPGA 감지 플래그
static	u8			g_probe_cnt = 0;	//	프로브 시도 횟수

static	u16			g_res_w = 0;		//	센서 수평 해상도
static	u16			g_res_h = 0;		//	센서 수직 해상도

eTVDOMODE_ID		g_tvdo_id = TVDO_NUM_MODES;

static const int	g_tvdo_fps[] = {
	[TVDO_08_FPS] = 8,
	[TVDO_15_FPS] = 15,
	[TVDO_30_FPS] = 30,
	[TVDO_60_FPS] = 60,
};

//	COX Pixel Formats
static const TVDO_PIXFMT_T g_tvdo_pixfmt[] = {
	//{ MEDIA_BUS_FMT_YUYV8_2X8, V4L2_COLORSPACE_RAW, },
	//{ MEDIA_BUS_FMT_UYVY8_2X8, V4L2_COLORSPACE_RAW, },
	//{ MEDIA_BUS_FMT_RGB888_1X24, V4L2_COLORSPACE_SRGB, },
	//{ MEDIA_BUS_FMT_SBGGR8_1X8, V4L2_COLORSPACE_SRGB, },
	//{ MEDIA_BUS_FMT_RGB888_1X24, V4L2_COLORSPACE_RAW, },
	//{ MEDIA_BUS_FMT_UYVY8_2X8, V4L2_COLORSPACE_RAW, },
	{ MEDIA_BUS_FMT_YUYV8_1X16, V4L2_COLORSPACE_RAW, },
	//{ MEDIA_BUS_FMT_UYVY8_1X16, V4L2_COLORSPACE_RAW, },
	//{ MEDIA_BUS_FMT_UYVY8_1X16, V4L2_COLORSPACE_RAW, },
};

static const TVDOMODE_PARAM_T g_tvdo_mode_param = {
	TVDO_MODE_QVGA_384_288,  384,  384,  288,  288,  30
};

/*
 * FIXME: remove this when a subdev API becomes available
 * to set the MIPI CSI-2 virtual channel.
 */
static unsigned int virtual_channel;
module_param(virtual_channel, uint, 0444);
MODULE_PARM_DESC(virtual_channel,
		 "MIPI CSI-2 virtual channel (0..3), default 0");


static inline TVDO_DEV_T*	to_tvdo_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, TVDO_DEV_T, sd);
}

static inline struct v4l2_subdev*	ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, TVDO_DEV_T, ctrls.handler)->sd;
}

static int tvdo_write_reg(TVDO_DEV_T* sensor, u16 reg, u16 val)
{
	struct i2c_client*		client = sensor->i2c_client;

	int			ret;
	u8			buf[4] = { reg >> 8, reg & 0xff, val >> 8, val & 0xFF };
	
    
	ret = i2c_master_send(client, buf, 4);
	/*
	 * Writing the wrong number of bytes also needs to be flagged as an
	 * error. Success needs to produce a 0 return code.
	 */
	if (ret == 4) {
		ret = 0;
	} else {
		dev_err(&client->dev, "%s: i2c write error, reg: %x\n",
				__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
	}

	return ret;
}

static int thermal_read_reg(TVDO_DEV_T* sensor, u16 reg, u16 *val)
{
	struct i2c_client*		client = sensor->i2c_client;
	
	u8		buf[2];
	int		ret;


	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	
	ret = i2c_master_send(client, buf, 2);
	/*
	 * A negative return code, or sending the wrong number of bytes, both
	 * count as an error.
	 */
	if (ret != 2) {
		dev_err(&client->dev, "%s: i2c write error, reg: %x\n",
			__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
		return ret;
	}

	ret = i2c_master_recv(client, buf, 2);
	/*
	 * The only return value indicating success is 1. Anything else, even
	 * a non-negative value, indicates something went wrong.
	 */
	if (ret == 2) {
		ret = 0;
	} else {
		dev_err(&client->dev, "%s: i2c read error, reg: %x\n",
				__func__, reg);
		if (ret >= 0)
			ret = -EINVAL;
	}

	*val = buf[0] << 8;
	*val |= buf[1];

	return ret;
}

static int thermal_comapre_param( TVDOMODE_PARAM_T* mode1, TVDOMODE_PARAM_T* mode2 ) 
{
	if ( 0 != memcmp(mode1, mode2, sizeof(TVDOMODE_PARAM_T)) ) {
		return (-1);
	}

	return (0);
}

static int thermal_copy_param( TVDOMODE_PARAM_T* src, TVDOMODE_PARAM_T* dst ) 
{
	memcpy(dst, src, sizeof(TVDOMODE_PARAM_T));

	return (0);
}

static int thermal_check_valid_mode(TVDO_DEV_T* sensor,
				   const TVDOMODE_PARAM_T* mode,
				   eTVDOMODE_FPS rate)
{
	struct i2c_client *client = sensor->i2c_client;
	

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_check_valid_mode (%d %d)\n", mode->id, rate);
	#endif

	if ( rate >= TVDO_NUM_FRAMERATES ) {
		dev_err(&client->dev, "Invalid thermal_frame_rate (%d)\n", rate);
		return -EINVAL;
	}

	switch (mode->id) {
	case TVDO_MODE_QVGA_384_288		:
		break;	
	default:
		dev_err(&client->dev, "Invalid mode (%d)\n", mode->id);
		return -EINVAL;
	}

	return 0;
}

static int thermal_get_gain(TVDO_DEV_T* sensor)
{
	return 0;
}

static int thermal_set_gain(TVDO_DEV_T* sensor, int gain)
{
	return 0;
}

static int thermal_set_autogain(TVDO_DEV_T* sensor, bool on)
{
	return 0;
}

static const TVDOMODE_PARAM_T*
thermal_find_mode(TVDO_DEV_T* sensor, eTVDOMODE_FPS fr,
		 int width, int height)
{
	const TVDOMODE_PARAM_T* mode;
	
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_find_mode (%d:%d:%d)\n", width, height, fr);
	#endif

	mode = &g_tvdo_mode_param;

	if ( mode->hact != width || mode->vact != height || fr >= TVDO_NUM_FRAMERATES ) {
		printk(KERN_INFO "[E] thermal_find_mode\n");
		return NULL;
	}

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_find_mode\n");
	#endif

	return mode;
}

static u64 thermal_calc_pixel_rate(TVDO_DEV_T* sensor)
{
	u64 rate;

	rate = sensor->curr_mode.vtot * sensor->curr_mode.htot;
	rate *= g_tvdo_fps[sensor->curr_fr];

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_calc_pixel_rate (%llu)\n", rate);
	#endif

	return rate;
}

static void thermal_reset(TVDO_DEV_T* sensor)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_reset\n");
	#endif
}

static int thermal_set_power_on(TVDO_DEV_T* sensor)
{
	//struct i2c_client *client = sensor->i2c_client;
	//int ret;

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_set_power_on\n");
	#endif

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_power_on reset\n");
	#endif

	thermal_reset(sensor);
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O]thermal_set_power_on complete\n");
	#endif
	
	return 0;
}

static void thermal_set_power_off(TVDO_DEV_T* sensor)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_power_off\n");
	#endif

	sensor->streaming = false;
}

static int thermal_set_power(TVDO_DEV_T* sensor, bool on)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_power(%x)\n", on);
	#endif

	if ( on ) {
		thermal_set_power_on(sensor);
	}

	if (sensor->ep.bus_type == V4L2_MBUS_CSI2_DPHY) {
	}
	else {
		printk(KERN_INFO "[E] thermal_set_power(%d)\n", sensor->ep.bus_type);
	}

	return 0;
}

/* --------------- Subdev Operations --------------- */
static int thermal_s_power(struct v4l2_subdev *sd, int on)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	int ret = 0;


	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_s_power (%d)\n", on);
	#endif

	mutex_lock(&sensor->lock);

	if (sensor->power_count == !on) {
		#ifdef TVDODRV_DBG_MSG
		printk(KERN_INFO "thermal_s_power (%x:%x)\n", sensor->power_count, on);
		#endif
		ret = thermal_set_power(sensor, !!on);
	}

	/* Update the power count. */
	sensor->power_count += on ? 1 : -1;
	WARN_ON(sensor->power_count < 0);

	mutex_unlock(&sensor->lock);

	if (on && !ret && sensor->power_count == 1) {
		#ifdef TVDODRV_DBG_MSG
		printk(KERN_INFO "thermal_s_power (complete)\n");
		#endif
		/* restore controls */
		//ret = v4l2_ctrl_handler_setup(&sensor->ctrls.handler);
	}

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_s_power(%d)\n", ret);
	#endif

	return ret;
}

static int thermal_try_frame_interval(TVDO_DEV_T* sensor,
				     struct v4l2_fract *fi,
				     u32 width, u32 height)
{
	const TVDOMODE_PARAM_T* mode;
	eTVDOMODE_FPS rate = TVDO_08_FPS;
	int minfps, maxfps, best_fps, fps;
	int i;


	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_try_frame_interval (%x)\n", fi->numerator);
	#endif

	minfps = g_tvdo_fps[TVDO_08_FPS];
	maxfps = g_tvdo_fps[TVDO_30_FPS];

	if (fi->numerator == 0) {
		fi->denominator = maxfps;
		fi->numerator = 1;
		rate = TVDO_30_FPS;
		goto find_mode;
	}

	fps = clamp_val(DIV_ROUND_CLOSEST(fi->denominator, fi->numerator), minfps, maxfps);

	best_fps = minfps;
	for (i = 0; i < ARRAY_SIZE(g_tvdo_fps); i++) {
		int curr_fps = g_tvdo_fps[i];

		if (abs(curr_fps - fps) < abs(best_fps - fps)) {
			best_fps = curr_fps;
			rate = i;
		}
	}

	fi->numerator = 1;
	fi->denominator = best_fps;

find_mode:
	mode = thermal_find_mode(sensor, rate, width, height);

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_try_frame_interval (%p %x)\n", mode, rate);
	#endif

	return mode ? rate : -EINVAL;
}

static int thermal_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	struct v4l2_mbus_framefmt *fmt;

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_get_fmt (%x:%x)\n", format->pad, format->which);
	#endif

	#if 1
	if (format->pad != 0)
		return -EINVAL;
	#endif

	mutex_lock(&sensor->lock);

	if ( format->which == V4L2_SUBDEV_FORMAT_TRY ) {
		//	v4l2_subdev_get_try_format  더 이상 지원 안함.
		//fmt = v4l2_subdev_get_try_format(&sensor->sd, sd_state, format->pad);
		fmt = v4l2_subdev_state_get_format(sd_state, format->pad);
	}
	else {
		fmt = &sensor->fmt;
	}

	fmt->reserved[1] = (sensor->curr_fr == TVDO_30_FPS) ? 30 : 15;
	
	format->format = *fmt;

	printk(KERN_INFO "[O] thermal_get_fmt %08X %08X\n", format->format.code, format->format.colorspace);
	
	mutex_unlock(&sensor->lock);
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_get_fmt\n");
	#endif

	return 0;
}

static int thermal_try_fmt_internal(struct v4l2_subdev *sd,
				   struct v4l2_mbus_framefmt *fmt,
				   eTVDOMODE_FPS fr,
				   TVDOMODE_PARAM_T** new_mode)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	const TVDOMODE_PARAM_T* mode;


	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_try_fmt_internal\n");
	#endif
	
	mode = thermal_find_mode(sensor, fr, fmt->width, fmt->height);
	if ( !mode ) {
		return -EINVAL;
	}

	fmt->width = mode->hact;
	fmt->height = mode->vact;

	memset(fmt->reserved, 0, sizeof(fmt->reserved));

	if ( new_mode ) {
		*new_mode = (TVDOMODE_PARAM_T*)mode;
	}

	printk(KERN_INFO "[O] thermal_try_fmt_internal %08X\n", fmt->code);

	fmt->code		= g_tvdo_pixfmt[0].code;
	fmt->colorspace	= g_tvdo_pixfmt[0].colorspace;

	
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
	
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_try_fmt_internal\n");
	#endif

	return 0;
}

static int thermal_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	TVDO_DEV_T*					sensor = to_tvdo_dev(sd);
	struct v4l2_mbus_framefmt	*mbus_fmt = &format->format;
	TVDOMODE_PARAM_T* 			new_mode;
	int		ret;

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_set_fmt %08X %08X\n", mbus_fmt->code, mbus_fmt->colorspace);
	#endif

	if ( 0 != format->pad ) {
		return -EINVAL;
	}

	mutex_lock(&sensor->lock);

	ret = 0;
	ret = thermal_try_fmt_internal(sd, mbus_fmt, sensor->curr_fr, &new_mode);
	if ( ret ) {
		goto set_fmt_out;
	}

	if ( V4L2_SUBDEV_FORMAT_TRY == format->which ) {
		*v4l2_subdev_state_get_format(sd_state, 0) = *mbus_fmt;
		goto set_fmt_out;
	}

	if ( thermal_comapre_param(new_mode, &(sensor->curr_mode)) ) {
		thermal_copy_param(new_mode, &(sensor->curr_mode));
	}

	sensor->fmt = *mbus_fmt;

set_fmt_out:
	mutex_unlock(&sensor->lock);

	printk(KERN_INFO "[O] thermal_set_fmt return %d\n", ret);

	return ret;
}

static int thermal_set_framefmt(TVDO_DEV_T* sensor,
			       struct v4l2_mbus_framefmt *format)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_framefmt\n");
	#endif
	return 0;
}

/*
 * Sensor Controls.
 */
static int thermal_set_ctrl_hue(TVDO_DEV_T* sensor, int value)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_hue\n");
	#endif
	return 0;
}

static int thermal_set_ctrl_contrast(TVDO_DEV_T* sensor, int value)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_contrast\n");	
	#endif
	return 0;
}

static int thermal_set_ctrl_saturation(TVDO_DEV_T* sensor, int value)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_saturation\n");	
	#endif
	return 0;
}

static int thermal_set_ctrl_white_balance(TVDO_DEV_T* sensor, int awb)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_white_balance\n");	
	#endif
	return 0;
}

static int thermal_set_ctrl_exposure(TVDO_DEV_T* sensor,
				    enum v4l2_exposure_auto_type auto_exposure)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_exposure\n");	
	#endif
	return 0;
}

static int thermal_set_ctrl_gain(TVDO_DEV_T* sensor, bool auto_gain)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_gain\n");
	#endif
	return 0;
}

static int thermal_set_ctrl_hflip(TVDO_DEV_T* sensor, int value)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_hflip\n");
	#endif
	return 0;
}

static int thermal_set_ctrl_vflip(TVDO_DEV_T* sensor, int value)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_set_ctrl_vflip\n" );
	#endif
	return 0;
}

static int thermal_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	//struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	//TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	//int val;

	/* v4l2_ctrl_lock() locks our own mutex */
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_g_volatile_ctrl\n");
	#endif

	switch (ctrl->id) {
	case V4L2_CID_AUTOGAIN:
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		break;
	}

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_g_volatile_ctrl\n");
	#endif

	return 0;
}

static int thermal_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	int ret;


	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_s_ctrl (%x)\n", ctrl->id);
	#endif

	/* v4l2_ctrl_lock() locks our own mutex */

	/*
	 * If the device is not powered up by the host driver do
	 * not apply any controls to H/W at this time. Instead
	 * the controls will be restored right after power-up.
	 */
	if (sensor->power_count == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_AUTOGAIN:
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		break;
	case V4L2_CID_HUE:
		break;
	case V4L2_CID_CONTRAST:
		break;
	case V4L2_CID_SATURATION:
		break;
	case V4L2_CID_TEST_PATTERN:
		break;
	case V4L2_CID_POWER_LINE_FREQUENCY:
		break;
	case V4L2_CID_HFLIP:
		break;
	case V4L2_CID_VFLIP:
		break;
	default:
		ret = -EINVAL;
		break;
	}

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_s_ctrl (%x)\n", ret);
	#endif

	return ret;
}

static const struct v4l2_ctrl_ops thermal_ctrl_ops = {	
	.g_volatile_ctrl = thermal_g_volatile_ctrl,
	.s_ctrl = thermal_s_ctrl,
};

static int thermal_init_controls(TVDO_DEV_T* sensor)
{
	const struct v4l2_ctrl_ops*	ops = &thermal_ctrl_ops;

	TVDO_CTRLS_T*				ctrls	= &sensor->ctrls;
	struct v4l2_ctrl_handler*	hdl		= &ctrls->handler;

	int ret;

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_init_controls\n");
	#endif
	v4l2_ctrl_handler_init(hdl, 32);

	/* we can use our own mutex for the ctrl lock */
	hdl->lock = &sensor->lock;


	/* Clock related controls */
	ctrls->pixel_rate = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_PIXEL_RATE,
					      0, INT_MAX, 1,
					      thermal_calc_pixel_rate(sensor));

	/* Auto/manual gain */
	ctrls->auto_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_AUTOGAIN,
					     0, 1, 1, 1);
	ctrls->gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_GAIN,
					0, 1023, 1, 0);

	ctrls->saturation = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_SATURATION,
					      0, 255, 1, 64);
	ctrls->hue = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HUE,
				       0, 359, 1, 0);
	ctrls->contrast = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_CONTRAST,
					    0, 255, 1, 0);
	
	ctrls->hflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP,
					 0, 1, 1, 0);
	ctrls->vflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP,
					 0, 1, 1, 0);

	if (hdl->error) {
		printk(KERN_INFO "[E] thermal_init_controls\n");
		ret = hdl->error;
		goto free_ctrls;
	}


	ctrls->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrls->gain->flags |= V4L2_CTRL_FLAG_VOLATILE;
	
	v4l2_ctrl_auto_cluster(2, &ctrls->auto_gain, 0, false);

	sensor->sd.ctrl_handler = hdl;
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_init_controls\n");
	#endif

	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static int thermal_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	TVDO_DEV_T*		sensor = to_tvdo_dev(sd);


	//printk(KERN_INFO "[I] thermal_enum_frame_size(%d:%d)\n", fse->pad, fse->index);
	if (fse->pad != 0) {
		#ifdef TVDODRV_DBG_MSG
		printk(KERN_INFO "[E] thermal_enum_frame_size fse->pad EINVAL\n");
		#endif
		return -EINVAL;
	}

	if (fse->index > 0) {
		#ifdef TVDODRV_DBG_MSG
		printk(KERN_INFO "[E] thermal_enum_frame_size fse->index EINVAL\n");
		#endif
		return -EINVAL;
	}

	fse->min_width = sensor->curr_mode.hact;
	fse->max_width = fse->min_width;

	fse->min_height = sensor->curr_mode.vact;
	fse->max_height = fse->min_height;
	
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_enum_frame_size(%d:%d)\n", fse->max_width, fse->max_height);
	#endif
	return 0;
}

static int thermal_enum_frame_interval(
	struct v4l2_subdev *sd,
	struct v4l2_subdev_state *sd_state,
	struct v4l2_subdev_frame_interval_enum *fie)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);

	//int i, j, count;

	//printk(KERN_INFO "[I] thermal_enum_frame_interval(%d:%d)\n", fie->pad, fie->index);
	if (fie->pad != 0) {
		#ifdef TVDODRV_DBG_MSG
		printk(KERN_INFO "[E] thermal_enum_frame_interval\n");
		#endif
		return -EINVAL;
	}

	if (fie->index >= TVDO_NUM_FRAMERATES) {
		#ifdef TVDODRV_DBG_MSG
		printk(KERN_INFO "[E] thermal_enum_frame_interval\n");
		#endif
		return -EINVAL;
	}

	if (fie->width == 0 || fie->height == 0 || fie->code == 0) {
		pr_warn("Please assign pixel format, width and height.\n");
		return -EINVAL;
	}

	fie->interval.numerator = 1;

	if (fie->width  == sensor->curr_mode.hact && 
		fie->height == sensor->curr_mode.vact) {
		fie->interval.denominator = g_tvdo_fps[fie->index];
		#ifdef TVDODRV_DBG_MSG
		printk(KERN_INFO "[O] thermal_enum_frame_interval\n");
		#endif
		return 0;
	}

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[E] thermal_enum_frame_interval\n");
	#endif

	return -EINVAL;
}

static int thermal_g_frame_interval(struct v4l2_subdev *sd,
	 				struct v4l2_subdev_state *sd_state,
				   	struct v4l2_subdev_frame_interval *fi)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_g_frame_interval\n");
	#endif

	mutex_lock(&sensor->lock);
	fi->interval = sensor->frame_interval;
	mutex_unlock(&sensor->lock);

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_g_frame_interval\n");
	#endif

	return 0;
}


static int thermal_s_frame_interval(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *sd_state,
				   	struct v4l2_subdev_frame_interval *fi)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	const TVDOMODE_PARAM_T* mode;
	int frame_rate, ret = 0;

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_s_frame_interval\n");
	#endif

	if (fi->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->streaming) {
		printk(KERN_ERR "[E] thermal_s_frame_interval (running streaming)\n");
		ret = -EBUSY;
		goto out;
	}

	mode = &(sensor->curr_mode);

#if 0
	frame_rate = thermal_try_frame_interval(sensor, &fi->interval, mode->hact, mode->vact);
	if (frame_rate < 0) {
		/* Always return a valid frame interval value */
		printk(KERN_ERR "<<<<<<<<<<<<<<<<<<thermal_s_frame_interval (invalid framerates)\n");
		fi->interval = sensor->frame_interval;
		goto out;
	}
#endif
	frame_rate = sensor->curr_fr;

	mode = thermal_find_mode(sensor, frame_rate, mode->hact, mode->vact);
	if (!mode) {
		printk(KERN_ERR "[E] thermal_s_frame_interval (invalid mode)\n");
		ret = -EINVAL;
		goto out;
	}

	if ( thermal_comapre_param((TVDOMODE_PARAM_T*)mode, &(sensor->curr_mode)) || frame_rate != sensor->curr_fr ) {
		sensor->curr_fr = frame_rate;
		sensor->frame_interval = fi->interval;

		thermal_copy_param((TVDOMODE_PARAM_T*)mode, &(sensor->curr_mode));

		__v4l2_ctrl_s_ctrl_int64(sensor->ctrls.pixel_rate, thermal_calc_pixel_rate(sensor));
	}
out:
	mutex_unlock(&sensor->lock);
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_s_frame_interval (%d)\n", ret);
	#endif
	return ret;
}

static int thermal_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	//TVDO_DEV_T* sensor = to_tvdo_dev(sd);

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_enum_mbus_code (%x:%x)\n", code->pad, code->index);
	#endif

	if (code->pad != 0) {
		printk(KERN_INFO "[E] thermal_enum_mbus_code\n");
		return -EINVAL;
	}
	if (code->index >= ARRAY_SIZE(g_tvdo_pixfmt)) {
		printk(KERN_INFO "[E] thermal_enum_mbus_code\n");
		return -EINVAL;
	}

	//	출력 포맷
	//code->code = thermal_formats[code->index].code;
	code->code = g_tvdo_pixfmt[0].code;

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_enum_mbus_code (%x)\n", code->code);
	#endif

	return 0;
}

static int thermal_s_stream(struct v4l2_subdev *sd, int enable)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	struct i2c_client *client = sensor->i2c_client;
	int ret = 0;

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[I] thermal_s_stream (%d)\n", enable);
	#endif

	mutex_lock(&sensor->lock);

	if (sensor->streaming == !enable) {
		ret = thermal_check_valid_mode(sensor,
					      &(sensor->curr_mode),
					      sensor->curr_fr);
		if (ret) {
			dev_err(&client->dev, "Not support WxH@fps=%dx%d@%d\n",
				sensor->curr_mode.hact,
				sensor->curr_mode.vact,
				g_tvdo_fps[sensor->curr_fr]);
			goto out;
		}

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "thermal_s_stream (%dx%d@%d)\n", 
				sensor->curr_mode.hact,
				sensor->curr_mode.vact,
				g_tvdo_fps[sensor->curr_fr]);
	#endif
		
		if (sensor->ep.bus_type == V4L2_MBUS_CSI2_DPHY) {
			sensor->streaming = enable;
		} 
		else {
			ret = -1;
		}
	}
out:
	mutex_unlock(&sensor->lock);

	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO "[O] thermal_s_stream (%d %d)\n", ret, sensor->streaming);
	#endif

	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int thermal_s_register(struct v4l2_subdev *sd, const struct v4l2_dbg_register *reg)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	int ret = 0;

	ret = tvdo_write_reg(sensor, reg->reg, reg->val);
	reg->size = 2;

	return ret;
}

static int thermal_g_register(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg)
{
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);
	int ret = 0;

	ret = thermal_read_reg(sensor, reg->reg, &reg->val);

	return ret;
}
#endif

static const struct v4l2_subdev_core_ops thermal_core_ops = {
	.s_power = thermal_s_power,
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register	= thermal_g_register,
 	.s_register = thermal_s_register,
#endif
};

static const struct v4l2_subdev_video_ops thermal_video_ops = {
	.s_stream = thermal_s_stream,
};

static const struct v4l2_subdev_pad_ops thermal_pad_ops = {
	.enum_mbus_code = thermal_enum_mbus_code,
	.get_fmt = thermal_get_fmt,
	.set_fmt = thermal_set_fmt,
	.get_frame_interval = thermal_g_frame_interval,
	.set_frame_interval = thermal_s_frame_interval,
	.enum_frame_size = thermal_enum_frame_size,
	.enum_frame_interval = thermal_enum_frame_interval,
};

static const struct v4l2_subdev_ops thermal_subdev_ops = {
	.core = &thermal_core_ops,
	.video = &thermal_video_ops,
	.pad = &thermal_pad_ops,
};

static int thermal_link_setup(struct media_entity *entity,
			   const struct media_pad *local,
			   const struct media_pad *remote, u32 flags)
{
	#ifdef TVDODRV_DBG_MSG
	printk(KERN_INFO ">>>>>>>>>>>>>>>>>>thermal_link_setup\n");
	#endif
	return 0;
}

static const struct media_entity_operations thermal_sd_media_ops = {
	.link_setup = thermal_link_setup,
};


static int thermal_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	TVDO_DEV_T* sensor;
	struct v4l2_mbus_framefmt *fmt;
	
	int ret;


	printk(KERN_INFO ">>>>>>>>> THERMAL PROBE IN(%d, %d, %s, %s, %d)\n", g_probe_cnt, client->addr, client->name, client->adapter->name, client->adapter->nr);

	if ( 11 != client->adapter->nr ) {
        dev_err(dev, "fail, invalid i2c channel.\n");
		return -EINVAL;
    }

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if ( NULL == sensor ) {
		dev_err(dev, "fail, memory allocation.\n");
		return -ENOMEM;
	}

	g_probe_cnt++;

	sensor->i2c_client = client;

	{
		/*
		* default init sequence initialize sensor to
		* YUV422 YUYV QVGA@30
		*/	
		fmt = &sensor->fmt;

		//fmt->code			= MEDIA_BUS_FMT_UYVY8_1X16;
		fmt->code			= MEDIA_BUS_FMT_YUYV8_1X16;
		//fmt->code			= MEDIA_BUS_FMT_RGB888_1X24;
		fmt->colorspace		= V4L2_COLORSPACE_RAW;
		//fmt->colorspace		= V4L2_COLORSPACE_SRGB;
		fmt->ycbcr_enc		= V4L2_YCBCR_ENC_DEFAULT;
		fmt->quantization	= V4L2_QUANTIZATION_FULL_RANGE;
		fmt->xfer_func		= V4L2_XFER_FUNC_DEFAULT;
		fmt->width			= DEFAULT_TVDO_WIDTH;
		fmt->height			= DEFAULT_TVDO_HEIGHT;
		fmt->field			= V4L2_FIELD_NONE;

		printk(KERN_INFO ">>>>>>>>> THERMAL PROBE VIDEO\n");
		
		sensor->frame_interval.numerator	= 1;
		sensor->frame_interval.denominator	= g_tvdo_fps[TVDO_30_FPS];

		sensor->curr_fr		= TVDO_30_FPS;
		thermal_copy_param((TVDOMODE_PARAM_T*)&g_tvdo_mode_param, &(sensor->curr_mode));
		thermal_copy_param(&(sensor->curr_mode), &(sensor->last_mode));
	}


	{	//	스터디 필요한 부분
		endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
		if ( NULL == endpoint ) {
			dev_err(dev, "endpoint node not found\n");
			return -EINVAL;
		}

		ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
		fwnode_handle_put(endpoint);
		if ( ret ) {
			dev_err(dev, "Could not parse endpoint\n");
			return ret;
		}

		//printk(KERN_INFO ">>>>>>>>>>>>>>>>>>SETP01 PASS!\n");
	}

	if ( sensor->ep.bus_type != V4L2_MBUS_CSI2_DPHY ) {
		dev_err(dev, "Unsupported bus type %d\n", sensor->ep.bus_type);
		return -EINVAL;
	}
	else {
		printk(KERN_INFO ">>>>>>>>> mipi_csi2 [%d:(%d:%d:%d:%d):%d:%d:(%d:%d:%d:%d)]\n",
							sensor->ep.bus.mipi_csi2.flags,
							sensor->ep.bus.mipi_csi2.data_lanes[0],
							sensor->ep.bus.mipi_csi2.data_lanes[1],
							sensor->ep.bus.mipi_csi2.data_lanes[2],
							sensor->ep.bus.mipi_csi2.data_lanes[3],
							sensor->ep.bus.mipi_csi2.clock_lane,
							sensor->ep.bus.mipi_csi2.num_data_lanes,
							sensor->ep.bus.mipi_csi2.lane_polarities[0],
							sensor->ep.bus.mipi_csi2.lane_polarities[1],
							sensor->ep.bus.mipi_csi2.lane_polarities[2],
							sensor->ep.bus.mipi_csi2.lane_polarities[3]
							);
		#if 0
		sensor->ep.bus.mipi_csi2.clock_lane;
		sensor->ep.bus.mipi_csi2.data_lanes;
		sensor->ep.bus.mipi_csi2.num_data_lanes;
		sensor->ep.bus.mipi_csi2.flags;
		sensor->ep.bus.mipi_csi2.lane_polarities;
		#endif
	}

	//printk(KERN_INFO ">>>>>>>>>>>>>>>>>>BUSTYPE %d\n", sensor->ep.bus_type);

	if ( 0 == g_f_fpga_det ) {	//	FPGA DETECTION
		int			wait_cnt;

		printk(KERN_INFO ">>>>>>>>> FIRST FPGA DETECTION\n");

		msleep(500);

		//	일초 동안 대기
		//usleep_range(1500000, 2000000);
		
		wait_cnt = 0;
		while ( 1 ) {
			u16			img_w, img_h;
                
                
			wait_cnt++;
			if ( 300 <= wait_cnt ) {
				printk(KERN_INFO ">>>>>>>>> FPGA WAIT TIMEOUT\n");
				break;		
			}
                
			ret = 0;
			ret += thermal_read_reg(sensor, 0x0200, &img_w);
			ret += thermal_read_reg(sensor, 0x0201, &img_h);
			
			if ( 0 == ret ) {
				printk(KERN_INFO ">>>>>>>>> FPGA READ %d %d\n", img_w, img_h);
				break;
			}
		
			msleep(5);
		}

		if ( 300 <= wait_cnt ) {
			dev_err(dev, "thermal:Can`t received ready signal!\n");
			//g_f_fpga_det = 0;

			g_tvdo_id = TVDO_MODE_QVGA_384_288;

			g_res_w = 384;
			g_res_h = 288;
		}
		else {
			u16			img_w, img_h;


			//	해상도를 읽어서 V4L2 기본 설정 진행
			ret = thermal_read_reg(sensor, 0x0200, &img_w);
			ret = thermal_read_reg(sensor, 0x0201, &img_h);

			g_res_w = img_w;
			g_res_h = img_h;

			printk(KERN_INFO ">>>>>>>>> FPGA RES (%04d:%04d)\n", g_res_w, g_res_h);

			if ( g_tvdo_mode_param.hact == g_res_w &&
				g_tvdo_mode_param.vact == g_res_h ) {
				g_tvdo_id = TVDO_MODE_QVGA_384_288;				
			}
			else {
				dev_err(dev, "thermal:invalid resolution (%04d:%04d)\n", g_res_w, g_res_h);

				g_tvdo_id = TVDO_MODE_QVGA_384_288;

				g_res_w = 384;
				g_res_h = 288;
			}
		}

		sensor->curr_mode.id = g_tvdo_mode_param.id;

		sensor->curr_mode.hact = g_tvdo_mode_param.hact;
		sensor->curr_mode.htot = g_tvdo_mode_param.htot;
		sensor->curr_mode.vact = g_tvdo_mode_param.vact;
		sensor->curr_mode.vtot = g_tvdo_mode_param.vtot;

		sensor->curr_mode.max_fps = g_tvdo_mode_param.max_fps;

		g_f_fpga_det = 1;
		
		printk(KERN_INFO ">>>>>>>>> FIRST FPGA DETECTION COMPLETION\n");
	}

	printk(KERN_INFO ">>>>>>>>> MODE ID (%d:%d)\n", g_f_fpga_det, g_tvdo_id);

	sensor->curr_id = g_tvdo_id;

	fmt = &sensor->fmt;

	fmt->width	= g_res_w;
	fmt->height	= g_res_h;

	sensor->curr_fr		= TVDO_30_FPS;	
	thermal_copy_param(&(sensor->curr_mode), &(sensor->last_mode));

	mutex_init(&sensor->lock);
	if ( thermal_init_controls(sensor) ) {
		goto mutex_destroy;
	}	
	
	v4l2_i2c_subdev_init(&sensor->sd, client, &thermal_subdev_ops);

	sensor->sd.flags			|= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	sensor->pad.flags			= MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.ops		= &thermal_sd_media_ops;
	sensor->sd.entity.function	= MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if ( ret ) {
		dev_err(dev, "thermal:error media_entity_pads_init\n");
		// return ret;
		goto free_ctrls;		
	}
			
	//mutex_init(&sensor->lock);
	//thermal_init_controls(sensor);
		
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if ( ret )
		goto free_ctrls;

	printk(KERN_INFO "<<<<<<<<<<<<<<<<<< THERMAL VIDEO PROBE OUT\n");
	
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);
	
entity_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);

mutex_destroy:
	mutex_destroy(&sensor->lock);

	return ret;
}

static void thermal_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	TVDO_DEV_T* sensor = to_tvdo_dev(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);
	v4l2_device_unregister_subdev(sd);
	mutex_destroy(&sensor->lock);
}

static const struct i2c_device_id thermal_id[] = {
	{ "tvdo" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, thermal_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id thermal_of_match[] = {
	{ .compatible = "cox,tvdo" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, thermal_of_match);
#endif

static struct i2c_driver thermal_i2c_driver = {
	.driver = {
		.of_match_table	= of_match_ptr(thermal_of_match),
		.name  = "tvdo",
		//.pm = ,		
	},	
	.probe		= thermal_probe,
	.remove		= thermal_remove,
	.id_table	= thermal_id,
};

module_i2c_driver(thermal_i2c_driver);

MODULE_AUTHOR("COX Co.Ltd <csi@coxcamera.com>");
MODULE_DESCRIPTION("THERMAL VIDEO MIPI Camera Subdev Driver");
MODULE_LICENSE("GPL v2");
