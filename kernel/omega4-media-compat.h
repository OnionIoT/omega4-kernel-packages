/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMEGA4_MEDIA_COMPAT_H
#define OMEGA4_MEDIA_COMPAT_H
#include <linux/version.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#define v4l2_subdev_get_try_format(sd, state, pad) \
	v4l2_subdev_state_get_format(state, pad)
#define v4l2_subdev_get_try_crop(sd, state, pad) \
	v4l2_subdev_state_get_crop(state, pad)
#endif

/* Vendor capture drivers query the active sensor timing, never TRY state. */
static inline int omega4_subdev_get_frame_interval(struct v4l2_subdev *sd,
		struct v4l2_subdev_frame_interval *fi)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	fi->which = V4L2_SUBDEV_FORMAT_ACTIVE;
	return v4l2_subdev_call(sd, pad, get_frame_interval, NULL, fi);
#else
	return v4l2_subdev_call(sd, video, g_frame_interval, fi);
#endif
}

static inline int omega4_subdev_set_frame_interval(struct v4l2_subdev *sd,
		struct v4l2_subdev_frame_interval *fi)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	fi->which = V4L2_SUBDEV_FORMAT_ACTIVE;
	return v4l2_subdev_call(sd, pad, set_frame_interval, NULL, fi);
#else
	return v4l2_subdev_call(sd, video, s_frame_interval, fi);
#endif
}
/* Keep the vendor queue's stream-start threshold on both kernel APIs. */
static inline void omega4_vb2_set_min_buffers(struct vb2_queue *q,
		unsigned int count)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	q->min_queued_buffers = count;
#else
	q->min_buffers_needed = count;
#endif
}
#endif
