// SPDX-License-Identifier: Apache-2.0 OR MIT
/*
 * Omega4 hardware H.264 encoder daemon.
 *
 * This uses the Rockchip MPP userspace API directly and imports V4L2 camera
 * buffers into MPP through DMA-BUF, matching the validated RV1103B path.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#define DEFAULT_CAMERA_BUFFERS 4
#define MIN_CAMERA_BUFFERS 2
#define MAX_CAMERA_BUFFERS 10
#define FMT_NUM_PLANES 1
#define RTP_CLOCK_RATE 90000U
#define RTP_HEADER_SIZE 12
#define RTP_DEFAULT_PAYLOAD_TYPE 96
#define RTP_DEFAULT_MTU 1200
#define RTP_MIN_MTU 64

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
	unsigned int camera_buffers;
	const char *rtp_dest;
	unsigned int rtp_payload_type;
	unsigned int rtp_mtu;
	bool dry_run;
};

struct rtp_output {
	int fd;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	uint8_t *buf;
	size_t mtu;
	size_t max_payload;
	unsigned int payload_type;
	uint16_t seq;
	uint32_t timestamp;
	uint32_t timestamp_step;
	uint32_t ssrc;
};

struct output_sink {
	FILE *file;
	struct rtp_output *rtp;
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
		"      --buffers COUNT     V4L2 capture buffers %u..%u (default: %u)\n"
		"  -o, --output PATH       output path, or - for stdout (default: -)\n"
		"      --rtp-dest HOST:PORT send RTP/H.264 over UDP instead of writing Annex-B\n"
		"      --rtp-payload-type N RTP payload type 0..127 (default: 96)\n"
		"      --rtp-mtu BYTES      RTP packet MTU including 12-byte header (default: 1200)\n"
		"      --dry-run           print resolved config and exit\n"
		"  -h, --help              show this help\n",
		prog, MIN_CAMERA_BUFFERS, MAX_CAMERA_BUFFERS, DEFAULT_CAMERA_BUFFERS);
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
		.camera_buffers = DEFAULT_CAMERA_BUFFERS,
		.rtp_payload_type = RTP_DEFAULT_PAYLOAD_TYPE,
		.rtp_mtu = RTP_DEFAULT_MTU,
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
		} else if (!strcmp(arg, "--buffers") || !strncmp(arg, "--buffers=", 10)) {
			TAKES_VALUE();
			cfg->camera_buffers = parse_u32("buffers", val);
		} else if (!strcmp(arg, "-o") || !strcmp(arg, "--output") || !strncmp(arg, "--output=", 9)) {
			TAKES_VALUE();
			cfg->output = val;
		} else if (!strcmp(arg, "--rtp-dest") || !strncmp(arg, "--rtp-dest=", 11)) {
			TAKES_VALUE();
			cfg->rtp_dest = val;
		} else if (!strcmp(arg, "--rtp-payload-type") || !strncmp(arg, "--rtp-payload-type=", 19)) {
			TAKES_VALUE();
			cfg->rtp_payload_type = parse_u32("rtp-payload-type", val);
		} else if (!strcmp(arg, "--rtp-mtu") || !strncmp(arg, "--rtp-mtu=", 10)) {
			TAKES_VALUE();
			cfg->rtp_mtu = parse_u32("rtp-mtu", val);
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
	if (cfg->rtp_payload_type > 127) {
		fprintf(stderr, "omega4-vencd: rtp-payload-type must be 0..127\n");
		exit(1);
	}
	if (cfg->rtp_mtu < RTP_MIN_MTU || cfg->rtp_mtu > 65507) {
		fprintf(stderr, "omega4-vencd: rtp-mtu must be %u..65507\n", RTP_MIN_MTU);
		exit(1);
	}
	if (cfg->camera_buffers < MIN_CAMERA_BUFFERS || cfg->camera_buffers > MAX_CAMERA_BUFFERS) {
		fprintf(stderr, "omega4-vencd: buffers must be %u..%u\n",
			MIN_CAMERA_BUFFERS, MAX_CAMERA_BUFFERS);
		exit(1);
	}
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
					 unsigned int height, MppFrameFormat fmt,
					 unsigned int buffer_count)
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
	req.count = buffer_count;
	req.type = cam->type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0)
		goto fail;
	if (req.count < buffer_count) {
		errno = ENOSPC;
		goto fail;
	}
	cam->bufcnt = buffer_count;

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

static uint32_t random_u32(void)
{
	struct timespec ts;
	uint32_t v;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	v = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^ ((uint32_t)getpid() << 16);
	v ^= v << 13;
	v ^= v >> 17;
	v ^= v << 5;

	return v ? v : 1;
}

static char *parse_rtp_dest(const char *dest, char **service)
{
	char *host = NULL;
	char *end = NULL;
	char *colon = NULL;

	if (!dest || !dest[0])
		return NULL;

	if (dest[0] == '[') {
		end = strchr(dest, ']');
		if (!end || end[1] != ':' || !end[2])
			return NULL;
		host = strndup(dest + 1, (size_t)(end - dest - 1));
		*service = strdup(end + 2);
	} else {
		colon = strrchr(dest, ':');
		if (!colon || colon == dest || !colon[1])
			return NULL;
		host = strndup(dest, (size_t)(colon - dest));
		*service = strdup(colon + 1);
	}

	if (!host || !*service) {
		free(host);
		free(*service);
		*service = NULL;
		return NULL;
	}

	return host;
}

static struct rtp_output *rtp_open(const struct venc_config *cfg)
{
	struct rtp_output *rtp = calloc(1, sizeof(*rtp));
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *ai;
	char *service = NULL;
	char *host = NULL;
	int ret;

	if (!rtp)
		return NULL;

	rtp->fd = -1;
	rtp->mtu = cfg->rtp_mtu;
	rtp->max_payload = cfg->rtp_mtu - RTP_HEADER_SIZE;
	rtp->payload_type = cfg->rtp_payload_type;
	rtp->timestamp_step = RTP_CLOCK_RATE / cfg->fps;
	rtp->seq = (uint16_t)random_u32();
	rtp->timestamp = random_u32();
	rtp->ssrc = random_u32();
	rtp->buf = malloc(rtp->mtu);
	if (!rtp->buf)
		goto fail;

	host = parse_rtp_dest(cfg->rtp_dest, &service);
	if (!host) {
		fprintf(stderr, "omega4-vencd: invalid rtp-dest '%s', expected HOST:PORT\n",
			cfg->rtp_dest);
		goto fail;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_DGRAM;
	ret = getaddrinfo(host, service, &hints, &res);
	if (ret) {
		fprintf(stderr, "omega4-vencd: getaddrinfo(%s:%s): %s\n",
			host, service, gai_strerror(ret));
		goto fail;
	}

	for (ai = res; ai; ai = ai->ai_next) {
		rtp->fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (rtp->fd < 0)
			continue;
		memcpy(&rtp->addr, ai->ai_addr, ai->ai_addrlen);
		rtp->addrlen = ai->ai_addrlen;
		break;
	}

	if (rtp->fd < 0) {
		fprintf(stderr, "omega4-vencd: failed to create RTP socket: %s\n", strerror(errno));
		goto fail;
	}

	freeaddrinfo(res);
	free(host);
	free(service);
	return rtp;

fail:
	if (res)
		freeaddrinfo(res);
	free(host);
	free(service);
	if (rtp) {
		if (rtp->fd >= 0)
			close(rtp->fd);
		free(rtp->buf);
		free(rtp);
	}
	return NULL;
}

static void rtp_close(struct rtp_output *rtp)
{
	if (!rtp)
		return;
	if (rtp->fd >= 0)
		close(rtp->fd);
	free(rtp->buf);
	free(rtp);
}

static int rtp_send_packet(struct rtp_output *rtp, const uint8_t *payload,
			   size_t payload_len, bool marker)
{
	size_t len = RTP_HEADER_SIZE + payload_len;

	if (len > rtp->mtu)
		return -1;

	rtp->buf[0] = 0x80;
	rtp->buf[1] = (uint8_t)(rtp->payload_type & 0x7f);
	if (marker)
		rtp->buf[1] |= 0x80;
	rtp->buf[2] = (uint8_t)(rtp->seq >> 8);
	rtp->buf[3] = (uint8_t)(rtp->seq);
	rtp->buf[4] = (uint8_t)(rtp->timestamp >> 24);
	rtp->buf[5] = (uint8_t)(rtp->timestamp >> 16);
	rtp->buf[6] = (uint8_t)(rtp->timestamp >> 8);
	rtp->buf[7] = (uint8_t)(rtp->timestamp);
	rtp->buf[8] = (uint8_t)(rtp->ssrc >> 24);
	rtp->buf[9] = (uint8_t)(rtp->ssrc >> 16);
	rtp->buf[10] = (uint8_t)(rtp->ssrc >> 8);
	rtp->buf[11] = (uint8_t)(rtp->ssrc);
	if (payload != rtp->buf + RTP_HEADER_SIZE)
		memcpy(rtp->buf + RTP_HEADER_SIZE, payload, payload_len);

	if (sendto(rtp->fd, rtp->buf, len, 0,
		   (struct sockaddr *)&rtp->addr, rtp->addrlen) != (ssize_t)len)
		return -1;

	rtp->seq++;
	return 0;
}

static int rtp_send_nal(struct rtp_output *rtp, const uint8_t *nal,
			size_t nal_len, bool marker)
{
	size_t fu_payload;
	size_t off;

	if (!nal_len)
		return 0;

	if (nal_len <= rtp->max_payload)
		return rtp_send_packet(rtp, nal, nal_len, marker);

	if (rtp->max_payload <= 2)
		return -1;

	fu_payload = rtp->max_payload - 2;
	off = 1;
	while (off < nal_len) {
		size_t chunk = nal_len - off;
		bool start = off == 1;
		bool end;

		if (chunk > fu_payload)
			chunk = fu_payload;
		end = (off + chunk) >= nal_len;

		rtp->buf[RTP_HEADER_SIZE] = (uint8_t)((nal[0] & 0xe0) | 28);
		rtp->buf[RTP_HEADER_SIZE + 1] = (uint8_t)(nal[0] & 0x1f);
		if (start)
			rtp->buf[RTP_HEADER_SIZE + 1] |= 0x80;
		if (end)
			rtp->buf[RTP_HEADER_SIZE + 1] |= 0x40;
		memcpy(rtp->buf + RTP_HEADER_SIZE + 2, nal + off, chunk);

		if (rtp_send_packet(rtp, rtp->buf + RTP_HEADER_SIZE,
				    chunk + 2, marker && end))
			return -1;
		off += chunk;
	}

	return 0;
}

static ssize_t h264_find_start_code(const uint8_t *data, size_t len,
				    size_t from, size_t *prefix_len)
{
	size_t i;

	for (i = from; i + 3 <= len; i++) {
		if (data[i] || data[i + 1])
			continue;
		if (data[i + 2] == 1) {
			*prefix_len = 3;
			return (ssize_t)i;
		}
		if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
			*prefix_len = 4;
			return (ssize_t)i;
		}
	}

	return -1;
}

static int rtp_send_annexb(struct rtp_output *rtp, const void *data,
			   size_t len, bool marker)
{
	const uint8_t *bytes = data;
	ssize_t pos;
	size_t prefix = 0;
	unsigned int nal_count = 0;
	unsigned int nal_idx = 0;

	pos = h264_find_start_code(bytes, len, 0, &prefix);
	if (pos < 0)
		return rtp_send_nal(rtp, bytes, len, marker);

	while (pos >= 0) {
		size_t nal_start = (size_t)pos + prefix;
		ssize_t next;
		size_t next_prefix = 0;

		next = h264_find_start_code(bytes, len, nal_start, &next_prefix);
		if ((size_t)nal_start < (next >= 0 ? (size_t)next : len))
			nal_count++;
		pos = next;
		prefix = next_prefix;
	}

	pos = h264_find_start_code(bytes, len, 0, &prefix);
	while (pos >= 0) {
		size_t nal_start = (size_t)pos + prefix;
		ssize_t next;
		size_t next_prefix = 0;
		size_t nal_end;

		next = h264_find_start_code(bytes, len, nal_start, &next_prefix);
		nal_end = next >= 0 ? (size_t)next : len;
		if (nal_start < nal_end) {
			nal_idx++;
			if (rtp_send_nal(rtp, bytes + nal_start, nal_end - nal_start,
					 marker && nal_idx == nal_count))
				return -1;
		}
		pos = next;
		prefix = next_prefix;
	}

	return 0;
}

static int output_write(struct output_sink *sink, const void *ptr, size_t len,
			bool end_of_frame)
{
	if (!len)
		return 0;

	if (sink->rtp) {
		if (rtp_send_annexb(sink->rtp, ptr, len, end_of_frame)) {
			fprintf(stderr, "omega4-vencd: RTP send failed: %s\n", strerror(errno));
			return -1;
		}
		if (end_of_frame)
			sink->rtp->timestamp += sink->rtp->timestamp_step;
		return 0;
	}

	return fwrite(ptr, 1, len, sink->file) == len ? 0 : -1;
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
	struct output_sink sink;
	unsigned int encoded = 0;
	unsigned int captured = 0;
	size_t packet_size = vcfg->width * vcfg->height;
	size_t md_size = (ALIGN_UP(vcfg->width, 64) >> 6) * (ALIGN_UP(vcfg->height, 16) >> 4) * 16;
	int ret = 1;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	memset(&sink, 0, sizeof(sink));
	if (vcfg->rtp_dest) {
		sink.rtp = rtp_open(vcfg);
		if (!sink.rtp)
			goto done;
	} else {
		sink.file = open_output(vcfg->output);
	}

	cam = camera_open(vcfg->device, vcfg->width, vcfg->height,
			  (MppFrameFormat)vcfg->format, vcfg->camera_buffers);
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
			if (len && output_write(&sink, ptr, len, false))
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
				eoi = mpp_packet_is_partition(packet) ? mpp_packet_is_eoi(packet) : 1;
				if (len && output_write(&sink, ptr, len, eoi)) {
					mpp_packet_deinit(&packet);
					camera_put_frame(cam, frame_idx);
					goto done;
				}
				mpp_packet_deinit(&packet);
			}
		} while (!eoi);

		camera_put_frame(cam, frame_idx);
		encoded++;
	}

	if (sink.file)
		fflush(sink.file);
	ret = sink.file && ferror(sink.file) ? 1 : 0;

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
	if (sink.file && sink.file != stdout)
		fclose(sink.file);
	rtp_close(sink.rtp);

	return ret;
}

int main(int argc, char **argv)
{
	struct venc_config cfg;

	parse_args(argc, argv, &cfg);

	if (cfg.dry_run) {
		printf("device=%s width=%u height=%u format=%u frames=%u fps=%u gop=%u "
		       "bitrate=%u:%u:%u rc=%u output=%s skip=%u "
		       "buffers=%u rtp-dest=%s rtp-payload-type=%u rtp-mtu=%u backend=libmpp\n",
		       cfg.device, cfg.width, cfg.height, cfg.format, cfg.frames,
		       cfg.fps, cfg.gop, cfg.bitrate, cfg.bitrate_min, cfg.bitrate_max,
		       cfg.rc_mode, cfg.output, cfg.skip_frames, cfg.camera_buffers,
		       cfg.rtp_dest ? cfg.rtp_dest : "-", cfg.rtp_payload_type, cfg.rtp_mtu);
		return 0;
	}

	return run_encoder(&cfg);
}
