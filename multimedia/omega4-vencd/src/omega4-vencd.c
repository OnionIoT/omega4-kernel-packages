// SPDX-License-Identifier: Apache-2.0 OR MIT
/*
 * Omega4 hardware H.264 encoder daemon.
 *
 * This uses the Rockchip MPP userspace API directly and imports V4L2 camera
 * buffers into MPP through DMA-BUF, matching the validated RV1103B path.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <rk_mpi.h>
#include <rk_venc_rc.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_meta.h>
#include <mpp_packet.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define CAMERA_BUFFERS 4
#define MAX_CAMERA_BUFFERS 10
#define FMT_NUM_PLANES 1

struct camera_frame {
	void *start;
	size_t length;
	int export_fd;
	MppBuffer buffer;
};

struct camera_source {
	int fd;
	unsigned int bufcnt;
	enum v4l2_buf_type type;
	struct camera_frame frames[MAX_CAMERA_BUFFERS];
};

struct venc_config {
	const char *device;
	const char *output;
	unsigned int width;
	unsigned int height;
	unsigned int format;
	unsigned int frames;
	unsigned int fps;
	unsigned int gop;
	unsigned int bitrate;
	unsigned int bitrate_min;
	unsigned int bitrate_max;
	unsigned int rc_mode;
	unsigned int skip_frames;
	bool dry_run;
};

static volatile sig_atomic_t stop_requested;

static void camera_close(struct camera_source *cam);

static const unsigned int v4l2_yuv_cfg[] = {
	V4L2_PIX_FMT_NV12,
	0,
	V4L2_PIX_FMT_NV16,
	0,
	V4L2_PIX_FMT_YVU420,
	V4L2_PIX_FMT_NV21,
	V4L2_PIX_FMT_YUV422P,
	V4L2_PIX_FMT_NV61,
	V4L2_PIX_FMT_YUYV,
	V4L2_PIX_FMT_YVYU,
	V4L2_PIX_FMT_UYVY,
	V4L2_PIX_FMT_VYUY,
	V4L2_PIX_FMT_GREY,
};

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void usage(FILE *fp, const char *prog)
{
	fprintf(fp,
		"Usage: %s [options]\n"
		"\n"
		"Encode camera frames with Omega4 hardware VENC and write Annex-B H.264.\n"
		"\n"
		"Options:\n"
		"  -d, --device PATH       V4L2 capture device (default: /dev/video7)\n"
		"  -W, --width PIXELS      frame width (default: 640)\n"
		"  -H, --height PIXELS     frame height (default: 480)\n"
		"  -f, --format FORMAT     MPP frame format number, 0 is NV12 (default: 0)\n"
		"  -c, --codec h264        codec, only h264 is supported for now\n"
		"  -n, --frames COUNT      frames to encode, 0 means continuous (default: 0)\n"
		"  -r, --fps FPS           input/output fps (default: 30)\n"
		"  -g, --gop FRAMES        keyframe interval (default: 60)\n"
		"  -b, --bitrate BPS       target bitrate in bits/sec (default: width*height*fps/8)\n"
		"      --bitrate-min BPS   minimum bitrate override\n"
		"      --bitrate-max BPS   maximum bitrate override\n"
		"      --rc MODE           vbr, cbr, fixqp, avbr, smtrc, or numeric 0..4\n"
		"      --skip COUNT        camera warm-up frames to drop (default: 50)\n"
		"  -o, --output PATH       output path, or - for stdout (default: -)\n"
		"      --dry-run           print resolved config and exit\n"
		"  -h, --help              show this help\n",
		prog);
}

static void fail(const char *msg)
{
	perror(msg);
	exit(1);
}

static unsigned int parse_u32(const char *name, const char *value)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(value, &end, 0);
	if (errno || !value[0] || *end || v > 0xffffffffUL) {
		fprintf(stderr, "omega4-vencd: invalid %s '%s'\n", name, value);
		exit(1);
	}

	return (unsigned int)v;
}

static unsigned int parse_rc_mode(const char *value)
{
	if (!strcmp(value, "vbr") || !strcmp(value, "VBR") || !strcmp(value, "0"))
		return MPP_ENC_RC_MODE_VBR;
	if (!strcmp(value, "cbr") || !strcmp(value, "CBR") || !strcmp(value, "1"))
		return MPP_ENC_RC_MODE_CBR;
	if (!strcmp(value, "fixqp") || !strcmp(value, "FIXQP") || !strcmp(value, "2"))
		return MPP_ENC_RC_MODE_FIXQP;
	if (!strcmp(value, "avbr") || !strcmp(value, "AVBR") || !strcmp(value, "3"))
		return MPP_ENC_RC_MODE_AVBR;
	if (!strcmp(value, "smtrc") || !strcmp(value, "SMTRC") || !strcmp(value, "4"))
		return MPP_ENC_RC_MODE_SMTRC;

	fprintf(stderr, "omega4-vencd: unsupported rc mode '%s'\n", value);
	exit(1);
}

static void parse_args(int argc, char **argv, struct venc_config *cfg)
{
	int i;

	*cfg = (struct venc_config) {
		.device = "/dev/video7",
		.output = "-",
		.width = 640,
		.height = 480,
		.format = MPP_FMT_YUV420SP,
		.frames = 0,
		.fps = 30,
		.gop = 60,
		.rc_mode = MPP_ENC_RC_MODE_VBR,
		.skip_frames = 50,
	};

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *val = NULL;

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(stdout, argv[0]);
			exit(0);
		} else if (!strcmp(arg, "--dry-run")) {
			cfg->dry_run = true;
			continue;
		} else if (!strncmp(arg, "--", 2) && strchr(arg, '=')) {
			val = strchr(arg, '=') + 1;
		} else if (i + 1 < argc) {
			val = argv[i + 1];
		}

#define TAKES_VALUE() do { if (!val) { usage(stderr, argv[0]); exit(1); } if (!strchr(arg, '=')) i++; } while (0)
		if (!strcmp(arg, "-d") || !strcmp(arg, "--device") || !strncmp(arg, "--device=", 9)) {
			TAKES_VALUE();
			cfg->device = val;
		} else if (!strcmp(arg, "-W") || !strcmp(arg, "--width") || !strncmp(arg, "--width=", 8)) {
			TAKES_VALUE();
			cfg->width = parse_u32("width", val);
		} else if (!strcmp(arg, "-H") || !strcmp(arg, "--height") || !strncmp(arg, "--height=", 9)) {
			TAKES_VALUE();
			cfg->height = parse_u32("height", val);
		} else if (!strcmp(arg, "-f") || !strcmp(arg, "--format") || !strncmp(arg, "--format=", 9)) {
			TAKES_VALUE();
			cfg->format = parse_u32("format", val);
		} else if (!strcmp(arg, "-c") || !strcmp(arg, "--codec") || !strncmp(arg, "--codec=", 8)) {
			TAKES_VALUE();
			if (strcmp(val, "h264") && strcmp(val, "H264")) {
				fprintf(stderr, "omega4-vencd: only h264 is supported\n");
				exit(1);
			}
		} else if (!strcmp(arg, "-n") || !strcmp(arg, "--frames") || !strncmp(arg, "--frames=", 9)) {
			TAKES_VALUE();
			cfg->frames = parse_u32("frames", val);
		} else if (!strcmp(arg, "-r") || !strcmp(arg, "--fps") || !strncmp(arg, "--fps=", 6)) {
			TAKES_VALUE();
			cfg->fps = parse_u32("fps", val);
		} else if (!strcmp(arg, "-g") || !strcmp(arg, "--gop") || !strncmp(arg, "--gop=", 6)) {
			TAKES_VALUE();
			cfg->gop = parse_u32("gop", val);
		} else if (!strcmp(arg, "-b") || !strcmp(arg, "--bitrate") || !strncmp(arg, "--bitrate=", 10)) {
			TAKES_VALUE();
			cfg->bitrate = parse_u32("bitrate", val);
		} else if (!strcmp(arg, "--bitrate-min") || !strncmp(arg, "--bitrate-min=", 14)) {
			TAKES_VALUE();
			cfg->bitrate_min = parse_u32("bitrate-min", val);
		} else if (!strcmp(arg, "--bitrate-max") || !strncmp(arg, "--bitrate-max=", 14)) {
			TAKES_VALUE();
			cfg->bitrate_max = parse_u32("bitrate-max", val);
		} else if (!strcmp(arg, "--rc") || !strncmp(arg, "--rc=", 5)) {
			TAKES_VALUE();
			cfg->rc_mode = parse_rc_mode(val);
		} else if (!strcmp(arg, "--skip") || !strncmp(arg, "--skip=", 7)) {
			TAKES_VALUE();
			cfg->skip_frames = parse_u32("skip", val);
		} else if (!strcmp(arg, "-o") || !strcmp(arg, "--output") || !strncmp(arg, "--output=", 9)) {
			TAKES_VALUE();
			cfg->output = val;
		} else {
			fprintf(stderr, "omega4-vencd: unknown option '%s'\n", arg);
			exit(1);
		}
#undef TAKES_VALUE
	}

	if (!cfg->fps)
		cfg->fps = 30;
	if (!cfg->gop)
		cfg->gop = cfg->fps * 2;
	if (!cfg->bitrate)
		cfg->bitrate = cfg->width * cfg->height / 8 * cfg->fps;
	if (!cfg->bitrate_min) {
		if (cfg->rc_mode == MPP_ENC_RC_MODE_VBR || cfg->rc_mode == MPP_ENC_RC_MODE_AVBR ||
		    cfg->rc_mode == MPP_ENC_RC_MODE_SMTRC)
			cfg->bitrate_min = cfg->bitrate / 16;
		else
			cfg->bitrate_min = cfg->bitrate * 15 / 16;
	}
	if (!cfg->bitrate_max)
		cfg->bitrate_max = cfg->bitrate * 17 / 16;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	struct timespec poll_time = { .tv_nsec = 10000000 };
	int ret;

	while ((ret = ioctl(fd, request, arg)) < 0) {
		if (errno != EINTR && errno != EAGAIN)
			return ret;
		nanosleep(&poll_time, NULL);
	}

	return ret;
}

static struct camera_source *camera_open(const char *device, unsigned int width,
					 unsigned int height, MppFrameFormat fmt)
{
	struct camera_source *cam = calloc(1, sizeof(*cam));
	struct v4l2_capability cap;
	struct v4l2_format vfmt;
	struct v4l2_requestbuffers req;
	unsigned int pixfmt = V4L2_PIX_FMT_NV12;
	unsigned int i;

	if (!cam)
		return NULL;

	cam->fd = -1;
	for (i = 0; i < MAX_CAMERA_BUFFERS; i++)
		cam->frames[i].export_fd = -1;

	cam->fd = open(device, O_RDWR | O_CLOEXEC);
	if (cam->fd < 0)
		goto fail;

	memset(&cap, 0, sizeof(cap));
	if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0)
		goto fail;
	if (!(cap.capabilities & V4L2_CAP_STREAMING))
		goto fail;

	memset(&vfmt, 0, sizeof(vfmt));
	vfmt.type = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ?
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cam->type = vfmt.type;

	if (fmt >= MPP_FRAME_FMT_YUV &&
	    (fmt - MPP_FRAME_FMT_YUV) < ARRAY_SIZE(v4l2_yuv_cfg) &&
	    v4l2_yuv_cfg[fmt - MPP_FRAME_FMT_YUV])
		pixfmt = v4l2_yuv_cfg[fmt - MPP_FRAME_FMT_YUV];

	if (vfmt.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		vfmt.fmt.pix_mp.width = width;
		vfmt.fmt.pix_mp.height = height;
		vfmt.fmt.pix_mp.pixelformat = pixfmt;
		vfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
		vfmt.fmt.pix_mp.num_planes = FMT_NUM_PLANES;
	} else {
		vfmt.fmt.pix.width = width;
		vfmt.fmt.pix.height = height;
		vfmt.fmt.pix.pixelformat = pixfmt;
		vfmt.fmt.pix.field = V4L2_FIELD_NONE;
	}

	if (xioctl(cam->fd, VIDIOC_S_FMT, &vfmt) < 0)
		goto fail;
	if (xioctl(cam->fd, VIDIOC_G_FMT, &vfmt) < 0)
		goto fail;

	memset(&req, 0, sizeof(req));
	req.count = CAMERA_BUFFERS;
	req.type = cam->type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0)
		goto fail;
	if (req.count != CAMERA_BUFFERS)
		goto fail;
	cam->bufcnt = req.count;

	for (i = 0; i < cam->bufcnt; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[FMT_NUM_PLANES];
		struct v4l2_exportbuffer expbuf;
		size_t len;

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = cam->type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			buf.m.planes = planes;
			buf.length = FMT_NUM_PLANES;
		}

		if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0)
			goto fail;

		len = (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ?
			buf.m.planes[0].length : buf.length;
		cam->frames[i].start = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
					    cam->fd,
					    (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ?
					    buf.m.planes[0].m.mem_offset : buf.m.offset);
		if (cam->frames[i].start == MAP_FAILED)
			goto fail;
		cam->frames[i].length = len;

		memset(&expbuf, 0, sizeof(expbuf));
		expbuf.type = cam->type;
		expbuf.index = i;
		expbuf.flags = O_CLOEXEC;
		if (xioctl(cam->fd, VIDIOC_EXPBUF, &expbuf) < 0)
			goto fail;
		cam->frames[i].export_fd = expbuf.fd;

		MppBufferInfo info;
		memset(&info, 0, sizeof(info));
		info.type = MPP_BUFFER_TYPE_EXT_DMA;
		info.fd = expbuf.fd;
		info.size = len & 0x07ffffff;
		info.index = (len & 0xf8000000) >> 27;
		if (mpp_buffer_import(&cam->frames[i].buffer, &info))
			goto fail;
	}

	for (i = 0; i < cam->bufcnt; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[FMT_NUM_PLANES];

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = cam->type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			buf.m.planes = planes;
			buf.length = FMT_NUM_PLANES;
		}
		if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0)
			goto fail;
	}

	if (xioctl(cam->fd, VIDIOC_STREAMON, &cam->type) < 0)
		goto fail;

	return cam;

fail:
	fprintf(stderr, "omega4-vencd: failed to open camera %s: %s\n", device, strerror(errno));
	camera_close(cam);
	return NULL;
}

static void camera_close(struct camera_source *cam)
{
	unsigned int i;

	if (!cam)
		return;

	if (cam->fd >= 0) {
		xioctl(cam->fd, VIDIOC_STREAMOFF, &cam->type);
		for (i = 0; i < cam->bufcnt; i++) {
			if (cam->frames[i].buffer)
				mpp_buffer_put(cam->frames[i].buffer);
			if (cam->frames[i].start && cam->frames[i].start != MAP_FAILED)
				munmap(cam->frames[i].start, cam->frames[i].length);
			if (cam->frames[i].export_fd >= 0)
				close(cam->frames[i].export_fd);
		}
		close(cam->fd);
	}

	free(cam);
}

static int camera_get_frame(struct camera_source *cam)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[FMT_NUM_PLANES];

	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.type = cam->type;
	buf.memory = V4L2_MEMORY_MMAP;
	if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		buf.m.planes = planes;
		buf.length = FMT_NUM_PLANES;
	}

	if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0)
		return -1;
	if (buf.index >= cam->bufcnt)
		return -1;
	return (int)buf.index;
}

static void camera_put_frame(struct camera_source *cam, int idx)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[FMT_NUM_PLANES];

	if (idx < 0)
		return;

	memset(&buf, 0, sizeof(buf));
	memset(planes, 0, sizeof(planes));
	buf.type = cam->type;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = (unsigned int)idx;
	if (cam->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		buf.m.planes = planes;
		buf.length = FMT_NUM_PLANES;
	}

	if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0)
		fprintf(stderr, "omega4-vencd: VIDIOC_QBUF failed: %s\n", strerror(errno));
}

static FILE *open_output(const char *path)
{
	char *copy;
	char *slash;
	FILE *fp;

	if (!strcmp(path, "-"))
		return stdout;

	copy = strdup(path);
	if (!copy)
		fail("strdup");
	slash = strrchr(copy, '/');
	if (slash && slash != copy) {
		*slash = '\0';
		mkdir(copy, 0755);
	}
	free(copy);

	fp = fopen(path, "wb");
	if (!fp)
		fail(path);
	return fp;
}

static void setup_encoder_cfg(MppCtx ctx, MppApi *mpi, MppEncCfg cfg,
			      const struct venc_config *vcfg)
{
	MppEncRcMode rc = (MppEncRcMode)vcfg->rc_mode;

	mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
	mpp_enc_cfg_set_s32(cfg, "prep:width", vcfg->width);
	mpp_enc_cfg_set_s32(cfg, "prep:height", vcfg->height);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", ALIGN_UP(vcfg->width, 8));
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ALIGN_UP(vcfg->height, 8));
	mpp_enc_cfg_set_s32(cfg, "prep:format", vcfg->format);
	mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);

	mpp_enc_cfg_set_s32(cfg, "rc:mode", rc);
	mpp_enc_cfg_set_u32(cfg, "rc:max_reenc_times", 0);
	mpp_enc_cfg_set_u32(cfg, "rc:super_mode", 0);
	mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", vcfg->fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", vcfg->fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_target", vcfg->bitrate);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", vcfg->bitrate_min);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", vcfg->bitrate_max);
	mpp_enc_cfg_set_s32(cfg, "rc:gop", vcfg->gop);
	mpp_enc_cfg_set_s32(cfg, "rc:qp_init", -1);
	mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
	mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
	mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 51);
	mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
	mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);

	mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
	mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
	mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
	mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
	mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

	if (mpi->control(ctx, MPP_ENC_SET_CFG, cfg)) {
		fprintf(stderr, "omega4-vencd: MPP_ENC_SET_CFG failed\n");
		exit(1);
	}

	{
		MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;

		if (mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode)) {
			fprintf(stderr, "omega4-vencd: MPP_ENC_SET_HEADER_MODE failed\n");
			exit(1);
		}
	}
}

static int run_encoder(const struct venc_config *vcfg)
{
	struct camera_source *cam = NULL;
	MppCtx ctx = NULL;
	MppApi *mpi = NULL;
	MppEncCfg enc_cfg = NULL;
	MppBufferGroup buf_grp = NULL;
	MppBuffer pkt_buf = NULL;
	MppBuffer md_info = NULL;
	FILE *out = NULL;
	unsigned int encoded = 0;
	unsigned int captured = 0;
	size_t packet_size = vcfg->width * vcfg->height;
	size_t md_size = (ALIGN_UP(vcfg->width, 64) >> 6) * (ALIGN_UP(vcfg->height, 16) >> 4) * 16;
	int ret = 1;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	out = open_output(vcfg->output);
	cam = camera_open(vcfg->device, vcfg->width, vcfg->height, (MppFrameFormat)vcfg->format);
	if (!cam)
		goto done;

	if (mpp_buffer_group_get_internal(&buf_grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE))
		goto done;
	if (mpp_buffer_get(buf_grp, &pkt_buf, packet_size))
		goto done;
	if (mpp_buffer_get(buf_grp, &md_info, md_size))
		goto done;
	if (mpp_create(&ctx, &mpi))
		goto done;

	MppPollType timeout = MPP_POLL_BLOCK;
	mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);

	if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC))
		goto done;
	if (mpp_enc_cfg_init(&enc_cfg))
		goto done;
	if (mpi->control(ctx, MPP_ENC_GET_CFG, enc_cfg))
		goto done;

	setup_encoder_cfg(ctx, mpi, enc_cfg, vcfg);

	{
		MppPacket hdr = NULL;
		mpp_packet_init_with_buffer(&hdr, pkt_buf);
		mpp_packet_set_length(hdr, 0);
		if (mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hdr) == MPP_OK) {
			void *ptr = mpp_packet_get_pos(hdr);
			size_t len = mpp_packet_get_length(hdr);
			if (len && fwrite(ptr, 1, len, out) != len)
				goto done;
		}
		mpp_packet_deinit(&hdr);
	}

	while (!stop_requested && (!vcfg->frames || encoded < vcfg->frames)) {
		MppFrame frame = NULL;
		MppPacket packet = NULL;
		MppMeta meta;
		int frame_idx = camera_get_frame(cam);
		RK_U32 eoi = 1;

		if (frame_idx < 0)
			goto done;

		if (captured++ < vcfg->skip_frames) {
			camera_put_frame(cam, frame_idx);
			continue;
		}

		if (mpp_frame_init(&frame))
			goto done;
		mpp_frame_set_width(frame, vcfg->width);
		mpp_frame_set_height(frame, vcfg->height);
		mpp_frame_set_hor_stride(frame, ALIGN_UP(vcfg->width, 8));
		mpp_frame_set_ver_stride(frame, ALIGN_UP(vcfg->height, 8));
		mpp_frame_set_fmt(frame, (MppFrameFormat)vcfg->format);
		mpp_frame_set_buffer(frame, cam->frames[frame_idx].buffer);

		mpp_packet_init_with_buffer(&packet, pkt_buf);
		mpp_packet_set_length(packet, 0);
		meta = mpp_frame_get_meta(frame);
		mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);
		mpp_meta_set_buffer(meta, KEY_MOTION_INFO, md_info);

		if (mpi->encode_put_frame(ctx, frame)) {
			mpp_frame_deinit(&frame);
			mpp_packet_deinit(&packet);
			camera_put_frame(cam, frame_idx);
			goto done;
		}
		mpp_frame_deinit(&frame);

		do {
			if (mpi->encode_get_packet(ctx, &packet)) {
				mpp_packet_deinit(&packet);
				camera_put_frame(cam, frame_idx);
				goto done;
			}

			if (packet) {
				void *ptr = mpp_packet_get_pos(packet);
				size_t len = mpp_packet_get_length(packet);
				if (len && fwrite(ptr, 1, len, out) != len) {
					mpp_packet_deinit(&packet);
					camera_put_frame(cam, frame_idx);
					goto done;
				}
				eoi = mpp_packet_is_partition(packet) ? mpp_packet_is_eoi(packet) : 1;
				mpp_packet_deinit(&packet);
			}
		} while (!eoi);

		camera_put_frame(cam, frame_idx);
		encoded++;
	}

	fflush(out);
	ret = ferror(out) ? 1 : 0;

done:
	if (ctx && mpi)
		mpi->reset(ctx);
	if (enc_cfg)
		mpp_enc_cfg_deinit(enc_cfg);
	if (ctx)
		mpp_destroy(ctx);
	if (pkt_buf)
		mpp_buffer_put(pkt_buf);
	if (md_info)
		mpp_buffer_put(md_info);
	if (buf_grp)
		mpp_buffer_group_put(buf_grp);
	camera_close(cam);
	if (out && out != stdout)
		fclose(out);

	return ret;
}

int main(int argc, char **argv)
{
	struct venc_config cfg;

	parse_args(argc, argv, &cfg);

	if (cfg.dry_run) {
		printf("device=%s width=%u height=%u format=%u frames=%u fps=%u gop=%u "
		       "bitrate=%u:%u:%u rc=%u output=%s skip=%u backend=libmpp\n",
		       cfg.device, cfg.width, cfg.height, cfg.format, cfg.frames,
		       cfg.fps, cfg.gop, cfg.bitrate, cfg.bitrate_min, cfg.bitrate_max,
		       cfg.rc_mode, cfg.output, cfg.skip_frames);
		return 0;
	}

	return run_encoder(&cfg);
}
