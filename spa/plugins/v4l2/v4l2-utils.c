/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

static void v4l2_on_fd_events(struct spa_source *source);

static int xioctl(int fd, int request, void *arg)
{
	int err;

	do {
		err = ioctl(fd, request, arg);
	} while (err == -1 && errno == EINTR);

	return err;
}


static int spa_v4l2_open(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct stat st;
	struct props *props = &this->props;
	int err;

	if (port->opened)
		return 0;

	if (props->device[0] == '\0') {
		spa_log_error(this->log, "v4l2: Device property not set");
		return -EIO;
	}

	spa_log_info(this->log, "v4l2: Playback device is '%s'", props->device);

	if (stat(props->device, &st) < 0) {
		err = errno;
		spa_log_error(this->log, "v4l2: Cannot identify '%s': %d, %s",
			      props->device, err, strerror(err));
		return -err;
	}

	if (!S_ISCHR(st.st_mode)) {
		spa_log_error(this->log, "v4l2: %s is no device", props->device);
		return -ENODEV;
	}

	port->fd = open(props->device, O_RDWR | O_NONBLOCK, 0);

	if (port->fd == -1) {
		err = errno;
		spa_log_error(this->log, "v4l2: Cannot open '%s': %d, %s",
			      props->device, err, strerror(err));
		return -err;
	}

	if (xioctl(port->fd, VIDIOC_QUERYCAP, &port->cap) < 0) {
		err = errno;
		spa_log_error(this->log, "QUERYCAP: %m");
		return -err;
	}

	if ((port->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
	    ((port->cap.capabilities & V4L2_CAP_DEVICE_CAPS) &&
	     (port->cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) == 0)) {
		spa_log_error(this->log, "v4l2: %s is no video capture device", props->device);
		close(port->fd);
		port->fd = -1;
		return -ENODEV;
	}

	port->source.func = v4l2_on_fd_events;
	port->source.data = this;
	port->source.fd = port->fd;
	port->source.mask = SPA_IO_IN | SPA_IO_ERR;
	port->source.rmask = 0;

	port->opened = true;

	return 0;
}

static int spa_v4l2_buffer_recycle(struct impl *this, uint32_t buffer_id)
{
	struct port *port = &this->out_ports[0];
	struct buffer *b = &port->buffers[buffer_id];
	int err;

	if (!SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUTSTANDING))
		return 0;

	SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUTSTANDING);
	spa_log_trace(this->log, "v4l2 %p: recycle buffer %d", this, buffer_id);

	if (xioctl(port->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0) {
		err = errno;
		spa_log_error(this->log, "VIDIOC_QBUF: %m");
		return -err;
	}

	return 0;
}

static int spa_v4l2_clear_buffers(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct v4l2_requestbuffers reqbuf;
	int i;

	if (port->n_buffers == 0)
		return 0;

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d;

		b = &port->buffers[i];
		d = b->outbuf->datas;

		if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUTSTANDING)) {
			spa_log_info(this->log, "v4l2: queueing outstanding buffer %p", b);
			spa_v4l2_buffer_recycle(this, i);
		}
		if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_MAPPED)) {
			munmap(SPA_MEMBER(b->ptr, -d[0].mapoffset, void),
					d[0].maxsize - d[0].mapoffset);
		}
		if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_ALLOCATED)) {
			close(d[0].fd);
		}
		d[0].type = SPA_ID_INVALID;
	}

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = port->memtype;
	reqbuf.count = 0;

	if (xioctl(port->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		spa_log_warn(this->log, "VIDIOC_REQBUFS: %m");
	}
	port->n_buffers = 0;

	return 0;
}

static int spa_v4l2_close(struct impl *this)
{
	struct port *port = &this->out_ports[0];

	if (!port->opened)
		return 0;

	if (port->have_format)
		return 0;

	spa_log_info(this->log, "v4l2: close");

	if (close(port->fd))
		spa_log_warn(this->log, "close: %m");

	port->fd = -1;
	port->opened = false;

	return 0;
}

struct format_info {
	uint32_t fourcc;
	uint32_t format;
	uint32_t media_type;
	uint32_t media_subtype;
};

#define VIDEO   SPA_MEDIA_TYPE_video
#define IMAGE   SPA_MEDIA_TYPE_image

#define RAW     SPA_MEDIA_SUBTYPE_raw

#define BAYER   SPA_MEDIA_SUBTYPE_bayer
#define MJPG    SPA_MEDIA_SUBTYPE_mjpg
#define JPEG    SPA_MEDIA_SUBTYPE_jpeg
#define DV      SPA_MEDIA_SUBTYPE_dv
#define MPEGTS  SPA_MEDIA_SUBTYPE_mpegts
#define H264    SPA_MEDIA_SUBTYPE_h264
#define H263    SPA_MEDIA_SUBTYPE_h263
#define MPEG1   SPA_MEDIA_SUBTYPE_mpeg1
#define MPEG2   SPA_MEDIA_SUBTYPE_mpeg2
#define MPEG4   SPA_MEDIA_SUBTYPE_mpeg4
#define XVID    SPA_MEDIA_SUBTYPE_xvid
#define VC1     SPA_MEDIA_SUBTYPE_vc1
#define VP8     SPA_MEDIA_SUBTYPE_vp8

#define FORMAT_UNKNOWN    SPA_VIDEO_FORMAT_UNKNOWN
#define FORMAT_ENCODED    SPA_VIDEO_FORMAT_ENCODED
#define FORMAT_RGB15      SPA_VIDEO_FORMAT_RGB15
#define FORMAT_BGR15      SPA_VIDEO_FORMAT_BGR15
#define FORMAT_RGB16      SPA_VIDEO_FORMAT_RGB16
#define FORMAT_BGR        SPA_VIDEO_FORMAT_BGR
#define FORMAT_RGB        SPA_VIDEO_FORMAT_RGB
#define FORMAT_BGRA       SPA_VIDEO_FORMAT_BGRA
#define FORMAT_BGRx       SPA_VIDEO_FORMAT_BGRx
#define FORMAT_ARGB       SPA_VIDEO_FORMAT_ARGB
#define FORMAT_xRGB       SPA_VIDEO_FORMAT_xRGB
#define FORMAT_GRAY8      SPA_VIDEO_FORMAT_GRAY8
#define FORMAT_GRAY16_LE  SPA_VIDEO_FORMAT_GRAY16_LE
#define FORMAT_GRAY16_BE  SPA_VIDEO_FORMAT_GRAY16_BE
#define FORMAT_YVU9       SPA_VIDEO_FORMAT_YVU9
#define FORMAT_YV12       SPA_VIDEO_FORMAT_YV12
#define FORMAT_YUY2       SPA_VIDEO_FORMAT_YUY2
#define FORMAT_YVYU       SPA_VIDEO_FORMAT_YVYU
#define FORMAT_UYVY       SPA_VIDEO_FORMAT_UYVY
#define FORMAT_Y42B       SPA_VIDEO_FORMAT_Y42B
#define FORMAT_Y41B       SPA_VIDEO_FORMAT_Y41B
#define FORMAT_YUV9       SPA_VIDEO_FORMAT_YUV9
#define FORMAT_I420       SPA_VIDEO_FORMAT_I420
#define FORMAT_NV12       SPA_VIDEO_FORMAT_NV12
#define FORMAT_NV12_64Z32 SPA_VIDEO_FORMAT_NV12_64Z32
#define FORMAT_NV21       SPA_VIDEO_FORMAT_NV21
#define FORMAT_NV16       SPA_VIDEO_FORMAT_NV16
#define FORMAT_NV61       SPA_VIDEO_FORMAT_NV61
#define FORMAT_NV24       SPA_VIDEO_FORMAT_NV24

static const struct format_info format_info[] = {
	/* RGB formats */
	{V4L2_PIX_FMT_RGB332, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_ARGB555, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_XRGB555, FORMAT_RGB15, VIDEO, RAW},
	{V4L2_PIX_FMT_ARGB555X, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_XRGB555X, FORMAT_BGR15, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB565, FORMAT_RGB16, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB565X, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_BGR666, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_BGR24, FORMAT_BGR, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB24, FORMAT_RGB, VIDEO, RAW},
	{V4L2_PIX_FMT_ABGR32, FORMAT_BGRA, VIDEO, RAW},
	{V4L2_PIX_FMT_XBGR32, FORMAT_BGRx, VIDEO, RAW},
	{V4L2_PIX_FMT_ARGB32, FORMAT_ARGB, VIDEO, RAW},
	{V4L2_PIX_FMT_XRGB32, FORMAT_xRGB, VIDEO, RAW},

	/* Deprecated Packed RGB Image Formats (alpha ambiguity) */
	{V4L2_PIX_FMT_RGB444, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB555, FORMAT_RGB15, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB555X, FORMAT_BGR15, VIDEO, RAW},
	{V4L2_PIX_FMT_BGR32, FORMAT_BGRx, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB32, FORMAT_xRGB, VIDEO, RAW},

	/* Grey formats */
	{V4L2_PIX_FMT_GREY, FORMAT_GRAY8, VIDEO, RAW},
	{V4L2_PIX_FMT_Y4, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y6, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y10, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y12, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y16, FORMAT_GRAY16_LE, VIDEO, RAW},
	{V4L2_PIX_FMT_Y16_BE, FORMAT_GRAY16_BE, VIDEO, RAW},
	{V4L2_PIX_FMT_Y10BPACK, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Palette formats */
	{V4L2_PIX_FMT_PAL8, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Chrominance formats */
	{V4L2_PIX_FMT_UV8, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Luminance+Chrominance formats */
	{V4L2_PIX_FMT_YVU410, FORMAT_YVU9, VIDEO, RAW},
	{V4L2_PIX_FMT_YVU420, FORMAT_YV12, VIDEO, RAW},
	{V4L2_PIX_FMT_YVU420M, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUYV, FORMAT_YUY2, VIDEO, RAW},
	{V4L2_PIX_FMT_YYUV, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YVYU, FORMAT_YVYU, VIDEO, RAW},
	{V4L2_PIX_FMT_UYVY, FORMAT_UYVY, VIDEO, RAW},
	{V4L2_PIX_FMT_VYUY, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV422P, FORMAT_Y42B, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV411P, FORMAT_Y41B, VIDEO, RAW},
	{V4L2_PIX_FMT_Y41P, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV444, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV555, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV565, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV32, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV410, FORMAT_YUV9, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV420, FORMAT_I420, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV420M, FORMAT_I420, VIDEO, RAW},
	{V4L2_PIX_FMT_HI240, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_HM12, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_M420, FORMAT_UNKNOWN, VIDEO, RAW},

	/* two planes -- one Y, one Cr + Cb interleaved  */
	{V4L2_PIX_FMT_NV12, FORMAT_NV12, VIDEO, RAW},
	{V4L2_PIX_FMT_NV12M, FORMAT_NV12, VIDEO, RAW},
	{V4L2_PIX_FMT_NV12MT, FORMAT_NV12_64Z32, VIDEO, RAW},
	{V4L2_PIX_FMT_NV12MT_16X16, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_NV21, FORMAT_NV21, VIDEO, RAW},
	{V4L2_PIX_FMT_NV21M, FORMAT_NV21, VIDEO, RAW},
	{V4L2_PIX_FMT_NV16, FORMAT_NV16, VIDEO, RAW},
	{V4L2_PIX_FMT_NV16M, FORMAT_NV16, VIDEO, RAW},
	{V4L2_PIX_FMT_NV61, FORMAT_NV61, VIDEO, RAW},
	{V4L2_PIX_FMT_NV61M, FORMAT_NV61, VIDEO, RAW},
	{V4L2_PIX_FMT_NV24, FORMAT_NV24, VIDEO, RAW},
	{V4L2_PIX_FMT_NV42, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Bayer formats - see http://www.siliconimaging.com/RGB%20Bayer.htm */
	{V4L2_PIX_FMT_SBGGR8, FORMAT_UNKNOWN, VIDEO, BAYER},
	{V4L2_PIX_FMT_SGBRG8, FORMAT_UNKNOWN, VIDEO, BAYER},
	{V4L2_PIX_FMT_SGRBG8, FORMAT_UNKNOWN, VIDEO, BAYER},
	{V4L2_PIX_FMT_SRGGB8, FORMAT_UNKNOWN, VIDEO, BAYER},

	/* compressed formats */
	{V4L2_PIX_FMT_MJPEG, FORMAT_ENCODED, VIDEO, MJPG},
	{V4L2_PIX_FMT_JPEG, FORMAT_ENCODED, IMAGE, JPEG},
	{V4L2_PIX_FMT_PJPG, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_DV, FORMAT_ENCODED, VIDEO, DV},
	{V4L2_PIX_FMT_MPEG, FORMAT_ENCODED, VIDEO, MPEGTS},
	{V4L2_PIX_FMT_H264, FORMAT_ENCODED, VIDEO, H264},
	{V4L2_PIX_FMT_H264_NO_SC, FORMAT_ENCODED, VIDEO, H264},
	{V4L2_PIX_FMT_H264_MVC, FORMAT_ENCODED, VIDEO, H264},
	{V4L2_PIX_FMT_H263, FORMAT_ENCODED, VIDEO, H263},
	{V4L2_PIX_FMT_MPEG1, FORMAT_ENCODED, VIDEO, MPEG1},
	{V4L2_PIX_FMT_MPEG2, FORMAT_ENCODED, VIDEO, MPEG2},
	{V4L2_PIX_FMT_MPEG4, FORMAT_ENCODED, VIDEO, MPEG4},
	{V4L2_PIX_FMT_XVID, FORMAT_ENCODED, VIDEO, XVID},
	{V4L2_PIX_FMT_VC1_ANNEX_G, FORMAT_ENCODED, VIDEO, VC1},
	{V4L2_PIX_FMT_VC1_ANNEX_L, FORMAT_ENCODED, VIDEO, VC1},
	{V4L2_PIX_FMT_VP8, FORMAT_ENCODED, VIDEO, VP8},

	/*  Vendor-specific formats   */
	{V4L2_PIX_FMT_WNVA, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_SN9C10X, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_PWC1, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_PWC2, FORMAT_UNKNOWN, VIDEO, RAW},
};

static const struct format_info *fourcc_to_format_info(uint32_t fourcc)
{
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].fourcc == fourcc)
			return &format_info[i];
	}
	return NULL;
}

#if 0
static const struct format_info *video_format_to_format_info(uint32_t format)
{
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].format == format)
			return &format_info[i];
	}
	return NULL;
}
#endif

static const struct format_info *find_format_info_by_media_type(uint32_t type,
								uint32_t subtype,
								uint32_t format,
								int startidx)
{
	int i;

	for (i = startidx; i < SPA_N_ELEMENTS(format_info); i++) {
		if ((format_info[i].media_type == type) &&
		    (format_info[i].media_subtype == subtype) &&
		    (format == 0 || format_info[i].format == format))
			return &format_info[i];
	}
	return NULL;
}

static uint32_t
enum_filter_format(uint32_t media_type, int32_t media_subtype,
		   const struct spa_pod *filter, uint32_t index)
{
	uint32_t video_format = 0;

	switch (media_type) {
	case SPA_MEDIA_TYPE_video:
	case SPA_MEDIA_TYPE_image:
		if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
			struct spa_pod_prop *p;
			const struct spa_pod *val;
			uint32_t n_values, choice;
			const uint32_t *values;

			if (!(p = spa_pod_find_prop(filter, SPA_FORMAT_VIDEO_format)))
				return SPA_VIDEO_FORMAT_UNKNOWN;

			val = spa_pod_get_values(&p->value, &n_values, &choice);

			if (val->type != SPA_TYPE_Id)
				return SPA_VIDEO_FORMAT_UNKNOWN;

			values = SPA_POD_BODY(val);

			if (choice == SPA_CHOICE_None) {
				if (index == 0)
					video_format = values[0];
			} else {
				if (index + 1 < n_values)
					video_format = values[index + 1];
			}
		} else {
			if (index == 0)
				video_format = SPA_VIDEO_FORMAT_ENCODED;
		}
	}
	return video_format;
}

static bool
filter_framesize(struct v4l2_frmsizeenum *frmsize,
		 const struct spa_rectangle *min,
		 const struct spa_rectangle *max,
		 const struct spa_rectangle *step)
{
	if (frmsize->type == V4L2_FRMSIZE_TYPE_DISCRETE) {
		if (frmsize->discrete.width < min->width ||
		    frmsize->discrete.height < min->height ||
		    frmsize->discrete.width > max->width ||
		    frmsize->discrete.height > max->height) {
			return false;
		}
	} else if (frmsize->type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
		   frmsize->type == V4L2_FRMSIZE_TYPE_STEPWISE) {
		/* FIXME, use LCM */
		frmsize->stepwise.step_width *= step->width;
		frmsize->stepwise.step_height *= step->height;

		if (frmsize->stepwise.max_width < min->width ||
		    frmsize->stepwise.max_height < min->height ||
		    frmsize->stepwise.min_width > max->width ||
		    frmsize->stepwise.min_height > max->height)
			return false;

		frmsize->stepwise.min_width = SPA_MAX(frmsize->stepwise.min_width, min->width);
		frmsize->stepwise.min_height = SPA_MAX(frmsize->stepwise.min_height, min->height);
		frmsize->stepwise.max_width = SPA_MIN(frmsize->stepwise.max_width, max->width);
		frmsize->stepwise.max_height = SPA_MIN(frmsize->stepwise.max_height, max->height);
	} else
		return false;

	return true;
}

static int compare_fraction(struct v4l2_fract *f1, const struct spa_fraction *f2)
{
	uint64_t n1, n2;

	/* fractions are reduced when set, so we can quickly see if they're equal */
	if (f1->denominator == f2->num && f1->numerator == f2->denom)
		return 0;

	/* extend to 64 bits */
	n1 = ((int64_t) f1->denominator) * f2->denom;
	n2 = ((int64_t) f1->numerator) * f2->num;
	if (n1 < n2)
		return -1;
	return 1;
}

static bool
filter_framerate(struct v4l2_frmivalenum *frmival,
		 const struct spa_fraction *min,
		 const struct spa_fraction *max,
		 const struct spa_fraction *step)
{
	if (frmival->type == V4L2_FRMIVAL_TYPE_DISCRETE) {
		if (compare_fraction(&frmival->discrete, min) < 0 ||
		    compare_fraction(&frmival->discrete, max) > 0)
			return false;
	} else if (frmival->type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
		   frmival->type == V4L2_FRMIVAL_TYPE_STEPWISE) {
		/* FIXME, use LCM */
		frmival->stepwise.step.denominator *= step->num;
		frmival->stepwise.step.numerator *= step->denom;

		if (compare_fraction(&frmival->stepwise.max, min) < 0 ||
		    compare_fraction(&frmival->stepwise.min, max) > 0)
			return false;

		if (compare_fraction(&frmival->stepwise.min, min) < 0) {
			frmival->stepwise.min.denominator = min->num;
			frmival->stepwise.min.numerator = min->denom;
		}
		if (compare_fraction(&frmival->stepwise.max, max) > 0) {
			frmival->stepwise.max.denominator = max->num;
			frmival->stepwise.max.numerator = max->denom;
		}
	} else
		return false;

	return true;
}

#define FOURCC_ARGS(f) (f)&0x7f,((f)>>8)&0x7f,((f)>>16)&0x7f,((f)>>24)&0x7f

static int
spa_v4l2_enum_format(struct impl *this,
		     uint32_t *index,
		     const struct spa_pod *filter,
		     struct spa_pod **result,
		     struct spa_pod_builder *builder)
{
	struct port *port = &this->out_ports[0];
	int res, n_fractions;
	const struct format_info *info;
	struct spa_pod_choice *choice;
	uint32_t filter_media_type, filter_media_subtype, video_format;

	if ((res = spa_v4l2_open(this)) < 0)
		return res;

	if (*index == 0) {
		spa_zero(port->fmtdesc);
		port->fmtdesc.index = 0;
		port->fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		port->next_fmtdesc = true;
		spa_zero(port->frmsize);
		port->next_frmsize = true;
		spa_zero(port->frmival);
	}

	if (filter) {
		if ((res = spa_format_parse(filter, &filter_media_type, &filter_media_subtype)) < 0)
			return res;
	}

	if (false) {
	      next_fmtdesc:
		port->fmtdesc.index++;
		port->next_fmtdesc = true;
	}

	while (port->next_fmtdesc) {
		if (filter) {
			video_format = enum_filter_format(filter_media_type,
					    filter_media_subtype,
					    filter, port->fmtdesc.index);

			if (video_format == SPA_VIDEO_FORMAT_UNKNOWN)
				goto enum_end;

			info = find_format_info_by_media_type(filter_media_type,
							      filter_media_subtype,
							      video_format, 0);
			if (info == NULL)
				goto next_fmtdesc;

			port->fmtdesc.pixelformat = info->fourcc;
		} else {
			if ((res = xioctl(port->fd, VIDIOC_ENUM_FMT, &port->fmtdesc)) < 0) {
				res = -errno;
				if (errno != EINVAL)
					spa_log_error(this->log, "VIDIOC_ENUM_FMT: %m");
				goto exit;
			}
		}
		port->next_fmtdesc = false;
		port->frmsize.index = 0;
		port->frmsize.pixel_format = port->fmtdesc.pixelformat;
		port->next_frmsize = true;
	}
	if (!(info = fourcc_to_format_info(port->fmtdesc.pixelformat)))
		goto next_fmtdesc;

      next_frmsize:
	while (port->next_frmsize) {
		if (filter) {
			struct spa_pod_prop *p;
			struct spa_pod *val;
			uint32_t n_vals, choice;

			/* check if we have a fixed frame size */
			if (!(p = spa_pod_find_prop(filter, SPA_FORMAT_VIDEO_size)))
				goto do_frmsize;

			val = spa_pod_get_values(&p->value, &n_vals, &choice);
			if (val->type != SPA_TYPE_Rectangle)
				goto enum_end;

			if (choice == SPA_CHOICE_None) {
				const struct spa_rectangle *values = SPA_POD_BODY(val);

				if (port->frmsize.index > 0)
					goto next_fmtdesc;

				port->frmsize.type = V4L2_FRMSIZE_TYPE_DISCRETE;
				port->frmsize.discrete.width = values[0].width;
				port->frmsize.discrete.height = values[0].height;
				goto have_size;
			}
		}
	      do_frmsize:
		if ((res = xioctl(port->fd, VIDIOC_ENUM_FRAMESIZES, &port->frmsize)) < 0) {
			if (errno == EINVAL)
				goto next_fmtdesc;

			res = -errno;
			spa_log_error(this->log, "VIDIOC_ENUM_FRAMESIZES: %m");
			goto exit;
		}
		if (filter) {
			struct spa_pod_prop *p;
			struct spa_pod *val;
			const struct spa_rectangle step = { 1, 1 }, *values;
			uint32_t choice, i, n_values;

			/* check if we have a fixed frame size */
			if (!(p = spa_pod_find_prop(filter, SPA_FORMAT_VIDEO_size)))
				goto have_size;

			val = spa_pod_get_values(&p->value, &n_values, &choice);
			if (val->type != SPA_TYPE_Rectangle)
				goto have_size;

			values = SPA_POD_BODY_CONST(val);

			if (choice == SPA_CHOICE_Range && n_values > 2) {
				if (filter_framesize(&port->frmsize, &values[1], &values[2], &step))
					goto have_size;
			} else if (choice == SPA_CHOICE_Step && n_values > 3) {
				if (filter_framesize(&port->frmsize, &values[1], &values[2], &values[3]))
					goto have_size;
			} else if (choice == SPA_CHOICE_Enum) {
				for (i = 1; i < n_values; i++) {
					if (filter_framesize(&port->frmsize, &values[i], &values[i], &step))
						goto have_size;
				}
			}
			/* nothing matches the filter, get next frame size */
			port->frmsize.index++;
			continue;
		}

	      have_size:
		if (port->frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
			/* we have a fixed size, use this to get the frame intervals */
			port->frmival.index = 0;
			port->frmival.pixel_format = port->frmsize.pixel_format;
			port->frmival.width = port->frmsize.discrete.width;
			port->frmival.height = port->frmsize.discrete.height;
			port->next_frmsize = false;
		} else if (port->frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
			   port->frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
			/* we have a non fixed size, fix to something sensible to get the
			 * framerate */
			port->frmival.index = 0;
			port->frmival.pixel_format = port->frmsize.pixel_format;
			port->frmival.width = port->frmsize.stepwise.min_width;
			port->frmival.height = port->frmsize.stepwise.min_height;
			port->next_frmsize = false;
		} else {
			port->frmsize.index++;
		}
	}

	spa_pod_builder_push_object(builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_props(builder,
			SPA_FORMAT_mediaType,    &SPA_POD_Id(info->media_type),
			SPA_FORMAT_mediaSubtype, &SPA_POD_Id(info->media_subtype),
			0);

	if (info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_format, 0);
		spa_pod_builder_id(builder, info->format);
	}
	spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_size, 0);
	spa_pod_builder_rectangle(builder, port->frmsize.discrete.width, port->frmsize.discrete.height);

	spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_framerate, 0);

	n_fractions = 0;

	choice = spa_pod_builder_deref(builder,
		spa_pod_builder_push_choice(builder, SPA_CHOICE_None, 0));
	port->frmival.index = 0;

	while (true) {
		if ((res = xioctl(port->fd, VIDIOC_ENUM_FRAMEINTERVALS, &port->frmival)) < 0) {
			res = -errno;
			if (errno == EINVAL) {
				port->frmsize.index++;
				port->next_frmsize = true;
				if (port->frmival.index == 0)
					goto next_frmsize;
				break;
			}
			spa_log_error(this->log, "VIDIOC_ENUM_FRAMEINTERVALS: %m");
			goto exit;
		}
		if (filter) {
			struct spa_pod_prop *p;
			struct spa_pod *val;
			uint32_t i, n_values, choice;
			const struct spa_fraction step = { 1, 1 }, *values;

			if (!(p = spa_pod_find_prop(filter, SPA_FORMAT_VIDEO_framerate)))
				goto have_framerate;

			val = spa_pod_get_values(&p->value, &n_values, &choice);

			if (val->type != SPA_TYPE_Fraction)
				goto enum_end;

			values = SPA_POD_BODY(val);

			switch (choice) {
			case SPA_CHOICE_None:
				if (filter_framerate(&port->frmival, &values[0], &values[0], &step))
					goto have_framerate;
				break;

			case SPA_CHOICE_Range:
				if (n_values > 2 && filter_framerate(&port->frmival, &values[1], &values[2], &step))
					goto have_framerate;
				break;

			case SPA_CHOICE_Step:
				if (n_values > 3 && filter_framerate(&port->frmival, &values[1], &values[2], &values[3]))
					goto have_framerate;
				break;

			case SPA_CHOICE_Enum:
				for (i = 1; i < n_values; i++) {
					if (filter_framerate(&port->frmival, &values[i], &values[i], &step))
						goto have_framerate;
				}
				break;
			default:
				break;
			}
			port->frmival.index++;
			continue;
		}

	      have_framerate:

		if (port->frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
			choice->body.type = SPA_CHOICE_Enum;
			if (n_fractions == 0)
				spa_pod_builder_fraction(builder,
							 port->frmival.discrete.denominator,
							 port->frmival.discrete.numerator);
			spa_pod_builder_fraction(builder,
						 port->frmival.discrete.denominator,
						 port->frmival.discrete.numerator);
			port->frmival.index++;
		} else if (port->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
			   port->frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
			if (n_fractions == 0)
				spa_pod_builder_fraction(builder, 25, 1);
			spa_pod_builder_fraction(builder,
						 port->frmival.stepwise.min.denominator,
						 port->frmival.stepwise.min.numerator);
			spa_pod_builder_fraction(builder,
						 port->frmival.stepwise.max.denominator,
						 port->frmival.stepwise.max.numerator);

			if (port->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
				choice->body.type = SPA_CHOICE_Range;
			} else {
				choice->body.type = SPA_CHOICE_Step;
				spa_pod_builder_fraction(builder,
							 port->frmival.stepwise.step.denominator,
							 port->frmival.stepwise.step.numerator);
			}

			port->frmsize.index++;
			port->next_frmsize = true;
			break;
		}
		n_fractions++;
	}
	if (n_fractions <= 1)
		choice->body.type = SPA_CHOICE_None;

	spa_pod_builder_pop(builder);
	*result = spa_pod_builder_pop(builder);

	(*index)++;

	res = 1;

      exit:
	spa_v4l2_close(this);

	return res;

     enum_end:
	res = 0;
	goto exit;
}

static int spa_v4l2_set_format(struct impl *this, struct spa_video_info *format, bool try_only)
{
	struct port *port = &this->out_ports[0];
	int res, cmd;
	struct v4l2_format reqfmt, fmt;
	struct v4l2_streamparm streamparm;
	const struct format_info *info = NULL;
	uint32_t video_format;
	struct spa_rectangle *size = NULL;
	struct spa_fraction *framerate = NULL;

	spa_zero(fmt);
	spa_zero(streamparm);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	switch (format->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		video_format = format->info.raw.format;
		size = &format->info.raw.size;
		framerate = &format->info.raw.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.mjpg.size;
		framerate = &format->info.mjpg.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.h264.size;
		framerate = &format->info.h264.framerate;
		break;
	default:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		break;
	}

	info = find_format_info_by_media_type(format->media_type,
					      format->media_subtype, video_format, 0);
	if (info == NULL || size == NULL || framerate == NULL) {
		spa_log_error(this->log, "v4l2: unknown media type %d %d %d", format->media_type,
			      format->media_subtype, video_format);
		return -EINVAL;
	}


	fmt.fmt.pix.pixelformat = info->fourcc;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	fmt.fmt.pix.width = size->width;
	fmt.fmt.pix.height = size->height;
	streamparm.parm.capture.timeperframe.numerator = framerate->denom;
	streamparm.parm.capture.timeperframe.denominator = framerate->num;

	spa_log_info(this->log, "v4l2: set %08x %dx%d %d/%d", fmt.fmt.pix.pixelformat,
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     streamparm.parm.capture.timeperframe.denominator,
		     streamparm.parm.capture.timeperframe.numerator);

	reqfmt = fmt;

	if ((res = spa_v4l2_open(this)) < 0)
		return res;

	cmd = try_only ? VIDIOC_TRY_FMT : VIDIOC_S_FMT;
	if (xioctl(port->fd, cmd, &fmt) < 0) {
		res = -errno;
		spa_log_error(this->log, "VIDIOC_S_FMT: %m");
		return res;
	}

	/* some cheap USB cam's won't accept any change */
	if (xioctl(port->fd, VIDIOC_S_PARM, &streamparm) < 0)
		spa_log_warn(this->log, "VIDIOC_S_PARM: %m");

	spa_log_info(this->log, "v4l2: got %08x %dx%d %d/%d", fmt.fmt.pix.pixelformat,
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     streamparm.parm.capture.timeperframe.denominator,
		     streamparm.parm.capture.timeperframe.numerator);

	if (reqfmt.fmt.pix.pixelformat != fmt.fmt.pix.pixelformat ||
	    reqfmt.fmt.pix.width != fmt.fmt.pix.width ||
	    reqfmt.fmt.pix.height != fmt.fmt.pix.height)
		return -EINVAL;

	if (try_only)
		return 0;

	size->width = fmt.fmt.pix.width;
	size->height = fmt.fmt.pix.height;
	port->rate.denom = framerate->num = streamparm.parm.capture.timeperframe.denominator;
	port->rate.num = framerate->denom = streamparm.parm.capture.timeperframe.numerator;

	port->fmt = fmt;
	port->info.flags = (port->export_buf ? SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS : 0) |
		SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
		SPA_PORT_INFO_FLAG_LIVE |
		SPA_PORT_INFO_FLAG_PHYSICAL |
		SPA_PORT_INFO_FLAG_TERMINAL;
	port->info.rate = streamparm.parm.capture.timeperframe.denominator;

	return 0;
}

static int query_ext_ctrl_ioctl(struct port *port, struct v4l2_query_ext_ctrl *qctrl)
{
	struct v4l2_queryctrl qc;
	int res;

	if (port->have_query_ext_ctrl) {
		res = ioctl(port->fd, VIDIOC_QUERY_EXT_CTRL, qctrl);
		if (errno != ENOTTY)
			return res;
		port->have_query_ext_ctrl = false;
	}
	qc.id = qctrl->id;
	res = ioctl(port->fd, VIDIOC_QUERYCTRL, &qc);
	if (res == 0) {
		qctrl->type = qc.type;
		memcpy(qctrl->name, qc.name, sizeof(qctrl->name));
		qctrl->minimum = qc.minimum;
		if (qc.type == V4L2_CTRL_TYPE_BITMASK) {
			qctrl->maximum = (__u32)qc.maximum;
			qctrl->default_value = (__u32)qc.default_value;
		} else {
			qctrl->maximum = qc.maximum;
			qctrl->default_value = qc.default_value;
		}
		qctrl->step = qc.step;
		qctrl->flags = qc.flags;
		qctrl->elems = 1;
		qctrl->nr_of_dims = 0;
		memset(qctrl->dims, 0, sizeof(qctrl->dims));
		switch (qctrl->type) {
		case V4L2_CTRL_TYPE_INTEGER64:
			qctrl->elem_size = sizeof(__s64);
			break;
		case V4L2_CTRL_TYPE_STRING:
			qctrl->elem_size = qc.maximum + 1;
			break;
		default:
			qctrl->elem_size = sizeof(__s32);
			break;
		}
		memset(qctrl->reserved, 0, sizeof(qctrl->reserved));
	}
	qctrl->id = qc.id;
	return res;
}

static uint32_t control_to_prop_id(struct impl *impl, uint32_t control_id)
{
	switch (control_id) {
	case V4L2_CID_BRIGHTNESS:
		return SPA_PROP_brightness;
	case V4L2_CID_CONTRAST:
		return SPA_PROP_contrast;
	case V4L2_CID_SATURATION:
		return SPA_PROP_saturation;
	case V4L2_CID_HUE:
		return SPA_PROP_hue;
	case V4L2_CID_GAMMA:
		return SPA_PROP_gamma;
	case V4L2_CID_EXPOSURE:
		return SPA_PROP_exposure;
	case V4L2_CID_GAIN:
		return SPA_PROP_gain;
	case V4L2_CID_SHARPNESS:
		return SPA_PROP_sharpness;
	default:
		return SPA_PROP_unknown;
	}
}

static int
spa_v4l2_enum_controls(struct impl *this,
		       uint32_t *index,
		       const struct spa_pod *filter,
		       struct spa_pod **result,
		       struct spa_pod_builder *builder)
{
	struct port *port = &this->out_ports[0];
	struct v4l2_query_ext_ctrl queryctrl;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint32_t prop_id, ctrl_id;
	uint8_t buffer[1024];
	int res;
        const unsigned next_fl = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;

	if ((res = spa_v4l2_open(this)) < 0)
		return res;

      next:
	spa_zero(queryctrl);

	if (*index == 0) {
		*index |= next_fl;
		port->n_controls = 0;
	}

	queryctrl.id = *index;
	spa_log_debug(this->log, "test control %08x", queryctrl.id);

	if (query_ext_ctrl_ioctl(port, &queryctrl) != 0) {
		if (errno == EINVAL) {
			if (queryctrl.id != next_fl)
				goto enum_end;

			if (*index & next_fl)
				*index = V4L2_CID_USER_BASE;
			else if (*index >= V4L2_CID_USER_BASE && *index < V4L2_CID_LASTP1)
				(*index)++;
			else if (*index >= V4L2_CID_LASTP1)
				*index = V4L2_CID_PRIVATE_BASE;
			else
				goto enum_end;
			goto next;
		}
		res = -errno;
		spa_log_error(this->log, "VIDIOC_QUERYCTRL: %m");
		return res;
	}
	if (*index & next_fl)
		(*index) = queryctrl.id | next_fl;
	else
		(*index)++;

	if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
		goto next;

	if (port->n_controls >= MAX_CONTROLS)
		goto enum_end;

	ctrl_id = queryctrl.id & ~next_fl;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	prop_id = control_to_prop_id(this, ctrl_id);

	port->controls[port->n_controls].id = prop_id;
	port->controls[port->n_controls].ctrl_id = ctrl_id;
	port->controls[port->n_controls].value = queryctrl.default_value;

	spa_log_debug(this->log, "Control %s %d %d", queryctrl.name, prop_id, ctrl_id);

	port->n_controls++;

	switch (queryctrl.type) {
	case V4L2_CTRL_TYPE_INTEGER:
		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_id,   &SPA_POD_Id(prop_id),
			SPA_PROP_INFO_type, &SPA_POD_CHOICE_STEP_Int(
						queryctrl.default_value,
						queryctrl.minimum,
						queryctrl.maximum,
						queryctrl.step),
			SPA_PROP_INFO_name, &SPA_POD_Stringv(queryctrl.name),
			0);
		break;
	case V4L2_CTRL_TYPE_BOOLEAN:
		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_id,   &SPA_POD_Id(prop_id),
			SPA_PROP_INFO_type, &SPA_POD_CHOICE_Bool(queryctrl.default_value),
			SPA_PROP_INFO_name, &SPA_POD_Stringv(queryctrl.name),
			0);
		break;
	case V4L2_CTRL_TYPE_MENU:
	{
		struct v4l2_querymenu querymenu;

		spa_pod_builder_push_object(&b, SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
		spa_pod_builder_props(&b,
			SPA_PROP_INFO_id,    &SPA_POD_Id(prop_id),
			SPA_PROP_INFO_type,  &SPA_POD_CHOICE_ENUM_Int(1, queryctrl.default_value),
			SPA_PROP_INFO_name,  &SPA_POD_Stringv(queryctrl.name),
			0);

		spa_zero(querymenu);
		querymenu.id = queryctrl.id;

		spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);
		spa_pod_builder_push_struct(&b);
		for (querymenu.index = queryctrl.minimum;
		    querymenu.index <= queryctrl.maximum;
		    querymenu.index++) {
			if (ioctl(port->fd, VIDIOC_QUERYMENU, &querymenu) == 0) {
				spa_pod_builder_int(&b, querymenu.index);
				spa_pod_builder_string(&b, (const char *)querymenu.name);
			}
		}
		spa_pod_builder_pop(&b);
		param = spa_pod_builder_pop(&b);
		break;
	}
	case V4L2_CTRL_TYPE_INTEGER_MENU:
	case V4L2_CTRL_TYPE_BITMASK:
	case V4L2_CTRL_TYPE_BUTTON:
	case V4L2_CTRL_TYPE_INTEGER64:
	case V4L2_CTRL_TYPE_STRING:
	default:
		goto next;

	}
	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	res = 1;

      exit:
	spa_v4l2_close(this);

	return res;

     enum_end:
	res = 0;
	goto exit;
}

static int mmap_read(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct v4l2_buffer buf;
	struct buffer *b;
	struct spa_data *d;
	int64_t pts;

	spa_zero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = port->memtype;

	if (xioctl(port->fd, VIDIOC_DQBUF, &buf) < 0)
		return -errno;

	pts = SPA_TIMEVAL_TO_TIME(&buf.timestamp);

	if (this->clock) {
		this->clock->nsec = pts;
		this->clock->rate = port->rate;
		this->clock->position = buf.sequence;
	}

	b = &port->buffers[buf.index];
	if (b->h) {
		b->h->flags = 0;
		if (buf.flags & V4L2_BUF_FLAG_ERROR)
			b->h->flags |= SPA_META_HEADER_FLAG_CORRUPTED;
		b->h->seq = buf.sequence;
		b->h->pts = buf.sequence * 1000000000LL / port->rate.denom;
	}

	d = b->outbuf->datas;
	d[0].chunk->offset = 0;
	d[0].chunk->size = buf.bytesused;
	d[0].chunk->stride = port->fmt.fmt.pix.bytesperline;

	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);
	spa_list_append(&port->queue, &b->link);

	spa_log_trace(this->log, "v4l2 %p: have output %d", this, buf.index);
	this->callbacks->process(this->callbacks_data, SPA_STATUS_HAVE_BUFFER);

	return 0;
}

static void v4l2_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;

	if (source->rmask & SPA_IO_ERR) {
		struct port *port = &this->out_ports[0];
		spa_log_error(this->log, "v4l2 %p: error %08x", this, source->rmask);
		if (port->source.loop)
			spa_loop_remove_source(this->data_loop, &port->source);
		return;
	}

	if (!(source->rmask & SPA_IO_IN)) {
		spa_log_warn(this->log, "v4l2 %p: spurious wakeup %d", this, source->rmask);
		return;
	}

	if (mmap_read(this) < 0)
		return;
}

static int spa_v4l2_use_buffers(struct impl *this, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct port *port = &this->out_ports[0];
	struct v4l2_requestbuffers reqbuf;
	int i;
	struct spa_data *d;

	if (n_buffers > 0) {
		d = buffers[0]->datas;

		if (d[0].type == SPA_DATA_MemFd ||
		    (d[0].type == SPA_DATA_MemPtr && d[0].data != NULL)) {
			port->memtype = V4L2_MEMORY_USERPTR;
		} else if (d[0].type == SPA_DATA_DmaBuf) {
			port->memtype = V4L2_MEMORY_DMABUF;
		} else {
			spa_log_error(this->log, "v4l2: can't use buffers of type %d", d[0].type);
			return -EINVAL;
		}
	}

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = port->memtype;
	reqbuf.count = n_buffers;

	if (xioctl(port->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		spa_log_error(this->log, "v4l2: VIDIOC_REQBUFS %m");
		return -errno;
	}
	spa_log_info(this->log, "v4l2: got %d buffers", reqbuf.count);
	if (reqbuf.count < n_buffers) {
		spa_log_error(this->log, "v4l2: can't allocate enough buffers");
		return -ENOMEM;
	}

	for (i = 0; i < reqbuf.count; i++) {
		struct buffer *b;

		b = &port->buffers[i];
		b->outbuf = buffers[i];
		b->flags = BUFFER_FLAG_OUTSTANDING;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		spa_log_info(this->log, "v4l2: import buffer %p", buffers[i]);

		if (buffers[i]->n_datas < 1) {
			spa_log_error(this->log, "v4l2: invalid memory on buffer %p", buffers[i]);
			return -EINVAL;
		}
		d = buffers[i]->datas;

		spa_zero(b->v4l2_buffer);
		b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->v4l2_buffer.memory = port->memtype;
		b->v4l2_buffer.index = i;

		if (port->memtype == V4L2_MEMORY_USERPTR) {
			if (d[0].data == NULL) {
				void *data;

				data = mmap(NULL,
					    d[0].maxsize + d[0].mapoffset,
					    PROT_READ | PROT_WRITE, MAP_SHARED,
					    d[0].fd,
					    0);
				if (data == MAP_FAILED)
					return -errno;

				b->ptr = SPA_MEMBER(data, d[0].mapoffset, void);
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
			}
			else
				b->ptr = d[0].data;

			b->v4l2_buffer.m.userptr = (unsigned long) b->ptr;
			b->v4l2_buffer.length = d[0].maxsize;
		}
		else if (port->memtype == V4L2_MEMORY_DMABUF) {
			b->v4l2_buffer.m.fd = d[0].fd;
		}
		else
			return -EIO;

		spa_v4l2_buffer_recycle(this, buffers[i]->id);
	}
	port->n_buffers = reqbuf.count;

	return 0;
}

static int
mmap_init(struct impl *this,
	  struct spa_pod **params,
	  uint32_t n_params,
	  struct spa_buffer **buffers,
	  uint32_t *n_buffers)
{
	struct port *port = &this->out_ports[0];
	struct v4l2_requestbuffers reqbuf;
	int i;

	port->memtype = V4L2_MEMORY_MMAP;

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = port->memtype;
	reqbuf.count = *n_buffers;

	if (xioctl(port->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		spa_log_error(this->log, "VIDIOC_REQBUFS: %m");
		return -errno;
	}

	spa_log_info(this->log, "v4l2: got %d buffers", reqbuf.count);
	*n_buffers = reqbuf.count;

	if (reqbuf.count < 2) {
		spa_log_error(this->log, "v4l2: can't allocate enough buffers");
		return -ENOMEM;
	}
	if (port->export_buf)
		spa_log_info(this->log, "v4l2: using EXPBUF");

	for (i = 0; i < reqbuf.count; i++) {
		struct buffer *b;
		struct spa_data *d;

		if (buffers[i]->n_datas < 1) {
			spa_log_error(this->log, "v4l2: invalid buffer data");
			return -EINVAL;
		}

		b = &port->buffers[i];
		b->outbuf = buffers[i];
		b->flags = BUFFER_FLAG_OUTSTANDING;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		spa_zero(b->v4l2_buffer);
		b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->v4l2_buffer.memory = port->memtype;
		b->v4l2_buffer.index = i;

		if (xioctl(port->fd, VIDIOC_QUERYBUF, &b->v4l2_buffer) < 0) {
			spa_log_error(this->log, "VIDIOC_QUERYBUF: %m");
			return -errno;
		}

		d = buffers[i]->datas;
		d[0].mapoffset = 0;
		d[0].maxsize = b->v4l2_buffer.length;
		d[0].chunk->offset = 0;
		d[0].chunk->size = 0;
		d[0].chunk->stride = port->fmt.fmt.pix.bytesperline;

		if (port->export_buf) {
			struct v4l2_exportbuffer expbuf;

			spa_zero(expbuf);
			expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			expbuf.index = i;
			expbuf.flags = O_CLOEXEC | O_RDONLY;
			if (xioctl(port->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
				spa_log_error(this->log, "VIDIOC_EXPBUF: %m");
				continue;
			}
			d[0].type = SPA_DATA_DmaBuf;
			d[0].fd = expbuf.fd;
			d[0].data = NULL;
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_ALLOCATED);
		} else {
			d[0].type = SPA_DATA_MemPtr;
			d[0].fd = -1;
			d[0].data = mmap(NULL,
					 b->v4l2_buffer.length,
					 PROT_READ, MAP_SHARED,
					 port->fd,
					 b->v4l2_buffer.m.offset);
			if (d[0].data == MAP_FAILED) {
				spa_log_error(this->log, "mmap: %m");
				continue;
			}
			b->ptr = d[0].data;
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
		}
		spa_v4l2_buffer_recycle(this, i);
	}
	port->n_buffers = reqbuf.count;

	return 0;
}

static int userptr_init(struct impl *this)
{
	return -ENOTSUP;
}

static int read_init(struct impl *this)
{
	return -ENOTSUP;
}

static int
spa_v4l2_alloc_buffers(struct impl *this,
		       struct spa_pod **params,
		       uint32_t n_params,
		       struct spa_buffer **buffers,
		       uint32_t *n_buffers)
{
	int res;
	struct port *port = &this->out_ports[0];

	if (port->n_buffers > 0)
		return -EIO;

	if (port->cap.capabilities & V4L2_CAP_STREAMING) {
		if ((res = mmap_init(this, params, n_params, buffers, n_buffers)) < 0)
			if ((res = userptr_init(this)) < 0)
				return res;
	} else if (port->cap.capabilities & V4L2_CAP_READWRITE) {
		if ((res = read_init(this)) < 0)
			return res;
	} else
		return -EIO;

	return 0;
}

static int spa_v4l2_stream_on(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	enum v4l2_buf_type type;

	if (!port->opened)
		return -EIO;

	if (port->started)
		return 0;

	spa_log_debug(this->log, "starting");

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(port->fd, VIDIOC_STREAMON, &type) < 0) {
		spa_log_error(this->log, "VIDIOC_STREAMON: %m");
		return -errno;
	}

	spa_loop_add_source(this->data_loop, &port->source);

	port->started = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct port *port = user_data;
	if (port->source.loop)
		spa_loop_remove_source(loop, &port->source);
	return 0;
}

static int spa_v4l2_stream_off(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	enum v4l2_buf_type type;
	int i;

	if (!port->opened)
		return -EIO;

	if (!port->started)
		return 0;

	spa_log_debug(this->log, "stopping");

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, port);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(port->fd, VIDIOC_STREAMOFF, &type) < 0) {
		spa_log_error(this->log, "VIDIOC_STREAMOFF: %m");
		return -errno;
	}
	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b;

		b = &port->buffers[i];
		if (!SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUTSTANDING)) {
			if (xioctl(port->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0)
				spa_log_warn(this->log, "VIDIOC_QBUF: %s", strerror(errno));
		}
	}
	spa_list_init(&port->queue);
	port->started = false;

	return 0;
}
