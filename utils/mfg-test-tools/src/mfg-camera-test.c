#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef VIDEO_MAX_PLANES
#define VIDEO_MAX_PLANES 8
#endif

#define DEFAULT_FRAMES 5
#define DEFAULT_BUFFERS 4
#define DEFAULT_MIN_MEAN 2.0
#define DEFAULT_MAX_MEAN 253.0
#define DEFAULT_MIN_VARIANCE 4.0
#define DEFAULT_MIN_DELTA_PERCENT 0.01

struct options {
	const char *device;
	unsigned int frames;
	double min_mean;
	double max_mean;
	double min_variance;
	double min_delta_percent;
	int verbose;
};

struct plane_map {
	void *addr;
	size_t length;
};

struct buffer {
	struct plane_map planes[VIDEO_MAX_PLANES];
	unsigned int nplanes;
};

struct stats {
	uint64_t bytes;
	uint64_t zero_count;
	uint64_t ff_count;
	unsigned int min;
	unsigned int max;
	double sum;
	double sumsq;
	double max_delta_percent;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && errno == EINTR);

	return ret;
}

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: mfg-camera-test [options]\n"
		"\n"
		"Options:\n"
		"  -d, --device PATH          Video device to test (default: auto)\n"
		"  -n, --frames COUNT         Frames to capture (default: %u)\n"
		"      --min-mean VALUE       Minimum mean byte value (default: %.2f)\n"
		"      --max-mean VALUE       Maximum mean byte value (default: %.2f)\n"
		"      --min-variance VALUE   Minimum byte variance (default: %.2f)\n"
		"      --min-delta VALUE      Minimum frame delta percent (default: %.4f)\n"
		"  -v, --verbose              Print selected device details\n"
		"  -h, --help                 Show this help\n",
		DEFAULT_FRAMES, DEFAULT_MIN_MEAN, DEFAULT_MAX_MEAN,
		DEFAULT_MIN_VARIANCE, DEFAULT_MIN_DELTA_PERCENT);
}

static int parse_uint(const char *arg, unsigned int *out)
{
	char *end = NULL;
	unsigned long val;

	errno = 0;
	val = strtoul(arg, &end, 0);
	if (errno || !end || *end || val == 0 || val > UINT_MAX)
		return -1;

	*out = (unsigned int)val;
	return 0;
}

static int parse_double(const char *arg, double *out)
{
	char *end = NULL;
	double val;

	errno = 0;
	val = strtod(arg, &end);
	if (errno || !end || *end)
		return -1;

	*out = val;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *opts)
{
	int i;

	opts->frames = DEFAULT_FRAMES;
	opts->min_mean = DEFAULT_MIN_MEAN;
	opts->max_mean = DEFAULT_MAX_MEAN;
	opts->min_variance = DEFAULT_MIN_VARIANCE;
	opts->min_delta_percent = DEFAULT_MIN_DELTA_PERCENT;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(stdout);
			exit(0);
		} else if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			opts->verbose = 1;
		} else if (!strcmp(arg, "-d") || !strcmp(arg, "--device")) {
			if (++i >= argc)
				return -1;
			opts->device = argv[i];
		} else if (!strcmp(arg, "-n") || !strcmp(arg, "--frames")) {
			if (++i >= argc || parse_uint(argv[i], &opts->frames))
				return -1;
		} else if (!strcmp(arg, "--min-mean")) {
			if (++i >= argc || parse_double(argv[i], &opts->min_mean))
				return -1;
		} else if (!strcmp(arg, "--max-mean")) {
			if (++i >= argc || parse_double(argv[i], &opts->max_mean))
				return -1;
		} else if (!strcmp(arg, "--min-variance")) {
			if (++i >= argc || parse_double(argv[i], &opts->min_variance))
				return -1;
		} else if (!strcmp(arg, "--min-delta")) {
			if (++i >= argc || parse_double(argv[i], &opts->min_delta_percent))
				return -1;
		} else {
			return -1;
		}
	}

	if (opts->min_mean < 0 || opts->max_mean > 255 ||
	    opts->min_mean >= opts->max_mean || opts->min_variance < 0 ||
	    opts->min_delta_percent < 0)
		return -1;

	return 0;
}

static int device_is_capture(int fd, enum v4l2_buf_type *type,
			     struct v4l2_capability *cap)
{
	uint32_t caps;

	memset(cap, 0, sizeof(*cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, cap) == -1)
		return 0;

	caps = cap->capabilities;
	if (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
		caps = cap->device_caps;

	if (!(caps & V4L2_CAP_STREAMING))
		return 0;

	if (caps & V4L2_CAP_VIDEO_CAPTURE) {
		*type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		return 1;
	}

	if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
		*type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		return 1;
	}

	return 0;
}

static int capture_device_score(const char *path,
				const struct v4l2_capability *cap)
{
	if (strstr((const char *)cap->card, "rkisp_mainpath"))
		return 100;
	if (strstr((const char *)cap->card, "mainpath"))
		return 90;
	if (strstr((const char *)cap->driver, "rkisp"))
		return 80;
	if (strstr((const char *)cap->card, "rkcif") ||
	    strstr((const char *)cap->driver, "rkcif"))
		return 10;
	if (strstr(path, "/dev/video"))
		return 20;
	return 1;
}

static int open_capture_device(const struct options *opts, char *path,
			       size_t path_len, enum v4l2_buf_type *type,
			       struct v4l2_capability *cap)
{
	int fd;
	int i;
	int best_fd = -1;
	int best_score = -1;
	enum v4l2_buf_type best_type = 0;
	struct v4l2_capability best_cap;
	char best_path[PATH_MAX] = { 0 };

	if (opts->device) {
		fd = open(opts->device, O_RDWR | O_NONBLOCK);
		if (fd == -1) {
			printf("FAIL open_device path=%s error=%s\n", opts->device,
			       strerror(errno));
			return -1;
		}
		if (!device_is_capture(fd, type, cap)) {
			printf("FAIL not_capture_device path=%s\n", opts->device);
			close(fd);
			return -1;
		}
		snprintf(path, path_len, "%s", opts->device);
		return fd;
	}

	for (i = 0; i < 32; i++) {
		struct v4l2_capability candidate_cap;
		enum v4l2_buf_type candidate_type;
		int score;

		snprintf(path, path_len, "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd == -1)
			continue;
		if (device_is_capture(fd, &candidate_type, &candidate_cap)) {
			score = capture_device_score(path, &candidate_cap);
			if (score > best_score) {
				if (best_fd != -1)
					close(best_fd);
				best_fd = fd;
				best_score = score;
				best_type = candidate_type;
				best_cap = candidate_cap;
				snprintf(best_path, sizeof(best_path), "%s", path);
				continue;
			}
		}
		close(fd);
	}

	if (best_fd != -1) {
		*type = best_type;
		*cap = best_cap;
		snprintf(path, path_len, "%s", best_path);
		return best_fd;
	}

	printf("FAIL no_capture_device\n");
	return -1;
}

static size_t single_plane_size(const struct v4l2_buffer *buf)
{
	if (buf->bytesused)
		return buf->bytesused;
	return buf->length;
}

static size_t mplane_size(const struct v4l2_plane *planes)
{
	if (planes[0].bytesused)
		return planes[0].bytesused;
	return planes[0].length;
}

static void update_stats(struct stats *stats, const uint8_t *data, size_t len,
			 const uint8_t *prev, size_t prev_len)
{
	uint64_t diff = 0;
	size_t cmp_len;
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned int val = data[i];

		stats->sum += val;
		stats->sumsq += (double)val * (double)val;
		stats->zero_count += val == 0;
		stats->ff_count += val == 0xff;
		if (val < stats->min)
			stats->min = val;
		if (val > stats->max)
			stats->max = val;
	}

	if (prev && prev_len) {
		cmp_len = len < prev_len ? len : prev_len;
		for (i = 0; i < cmp_len; i++) {
			if (data[i] != prev[i])
				diff++;
		}
		if (cmp_len) {
			double pct = ((double)diff * 100.0) / (double)cmp_len;
			if (pct > stats->max_delta_percent)
				stats->max_delta_percent = pct;
		}
	}

	stats->bytes += len;
}

static int map_buffers(int fd, enum v4l2_buf_type type, struct buffer **buffers,
		       unsigned int *count)
{
	struct v4l2_requestbuffers req;
	struct buffer *bufs;
	unsigned int i;

	memset(&req, 0, sizeof(req));
	req.count = DEFAULT_BUFFERS;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;

	if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
		printf("FAIL reqbufs error=%s\n", strerror(errno));
		return -1;
	}

	if (req.count < 2) {
		printf("FAIL insufficient_buffers count=%u\n", req.count);
		return -1;
	}

	bufs = calloc(req.count, sizeof(*bufs));
	if (!bufs) {
		printf("FAIL no_memory\n");
		return -1;
	}

	for (i = 0; i < req.count; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		unsigned int p;

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			buf.m.planes = planes;
			buf.length = VIDEO_MAX_PLANES;
		}

		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
			printf("FAIL querybuf index=%u error=%s\n", i, strerror(errno));
			free(bufs);
			return -1;
		}

		if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			bufs[i].nplanes = buf.length;
			if (bufs[i].nplanes > VIDEO_MAX_PLANES)
				bufs[i].nplanes = VIDEO_MAX_PLANES;
			for (p = 0; p < bufs[i].nplanes; p++) {
				bufs[i].planes[p].length = planes[p].length;
				bufs[i].planes[p].addr = mmap(NULL, planes[p].length,
							      PROT_READ | PROT_WRITE,
							      MAP_SHARED, fd,
							      planes[p].m.mem_offset);
				if (bufs[i].planes[p].addr == MAP_FAILED) {
					printf("FAIL mmap index=%u plane=%u error=%s\n",
					       i, p, strerror(errno));
					free(bufs);
					return -1;
				}
			}
		} else {
			bufs[i].nplanes = 1;
			bufs[i].planes[0].length = buf.length;
			bufs[i].planes[0].addr = mmap(NULL, buf.length,
						      PROT_READ | PROT_WRITE,
						      MAP_SHARED, fd, buf.m.offset);
			if (bufs[i].planes[0].addr == MAP_FAILED) {
				printf("FAIL mmap index=%u error=%s\n", i, strerror(errno));
				free(bufs);
				return -1;
			}
		}
	}

	*buffers = bufs;
	*count = req.count;
	return 0;
}

static void unmap_buffers(struct buffer *buffers, unsigned int count)
{
	unsigned int i;
	unsigned int p;

	if (!buffers)
		return;

	for (i = 0; i < count; i++) {
		for (p = 0; p < buffers[i].nplanes; p++) {
			if (buffers[i].planes[p].addr &&
			    buffers[i].planes[p].addr != MAP_FAILED)
				munmap(buffers[i].planes[p].addr,
				       buffers[i].planes[p].length);
		}
	}
	free(buffers);
}

static int queue_all_buffers(int fd, enum v4l2_buf_type type,
			     unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			buf.m.planes = planes;
			buf.length = VIDEO_MAX_PLANES;
		}

		if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
			printf("FAIL qbuf index=%u error=%s\n", i, strerror(errno));
			return -1;
		}
	}

	return 0;
}

static int wait_for_frame(int fd)
{
	fd_set fds;
	struct timeval tv;
	int ret;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	tv.tv_sec = 5;
	tv.tv_usec = 0;

	ret = select(fd + 1, &fds, NULL, NULL, &tv);
	if (ret == -1) {
		if (errno == EINTR)
			return 0;
		printf("FAIL select error=%s\n", strerror(errno));
		return -1;
	}
	if (ret == 0) {
		printf("FAIL capture_timeout\n");
		return -1;
	}

	return 0;
}

static int capture_frames(int fd, enum v4l2_buf_type type,
			  struct buffer *buffers, unsigned int buffer_count,
			  const struct options *opts, struct stats *stats)
{
	uint8_t *prev = NULL;
	size_t prev_len = 0;
	unsigned int frames = 0;
	int rc = -1;

	stats->min = 255;

	if (queue_all_buffers(fd, type, buffer_count))
		return -1;

	if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
		printf("FAIL streamon error=%s\n", strerror(errno));
		return -1;
	}

	while (frames < opts->frames) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		uint8_t *data;
		size_t len;

		if (wait_for_frame(fd))
			goto out_streamoff;

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			buf.m.planes = planes;
			buf.length = VIDEO_MAX_PLANES;
		}

		if (xioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
			if (errno == EAGAIN)
				continue;
			printf("FAIL dqbuf error=%s\n", strerror(errno));
			goto out_streamoff;
		}

		if (buf.index >= buffer_count) {
			printf("FAIL bad_buffer_index index=%u\n", buf.index);
			goto out_streamoff;
		}

		data = buffers[buf.index].planes[0].addr;
		len = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ?
			mplane_size(planes) : single_plane_size(&buf);
		if (!data || len == 0 || len > buffers[buf.index].planes[0].length) {
			printf("FAIL bad_frame_size frame=%u bytes=%zu\n", frames, len);
			goto out_streamoff;
		}

		update_stats(stats, data, len, prev, prev_len);

		if (prev_len < len) {
			uint8_t *tmp = realloc(prev, len);
			if (!tmp) {
				printf("FAIL no_memory\n");
				goto out_streamoff;
			}
			prev = tmp;
		}
		memcpy(prev, data, len);
		prev_len = len;

		frames++;

		if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
			printf("FAIL requeue error=%s\n", strerror(errno));
			goto out_streamoff;
		}
	}

	rc = 0;

out_streamoff:
	if (xioctl(fd, VIDIOC_STREAMOFF, &type) == -1 && rc == 0) {
		printf("FAIL streamoff error=%s\n", strerror(errno));
		rc = -1;
	}
	free(prev);
	return rc;
}

static int get_format(int fd, enum v4l2_buf_type type, unsigned int *width,
		      unsigned int *height, uint32_t *pixelformat)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) == -1)
		return -1;

	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		*width = fmt.fmt.pix_mp.width;
		*height = fmt.fmt.pix_mp.height;
		*pixelformat = fmt.fmt.pix_mp.pixelformat;
	} else {
		*width = fmt.fmt.pix.width;
		*height = fmt.fmt.pix.height;
		*pixelformat = fmt.fmt.pix.pixelformat;
	}

	return 0;
}

static void fourcc_to_string(uint32_t pixelformat, char fourcc[5])
{
	fourcc[0] = pixelformat & 0xff;
	fourcc[1] = (pixelformat >> 8) & 0xff;
	fourcc[2] = (pixelformat >> 16) & 0xff;
	fourcc[3] = (pixelformat >> 24) & 0xff;
	fourcc[4] = '\0';
}

static void print_format(unsigned int width, unsigned int height,
			 uint32_t pixelformat, enum v4l2_buf_type type)
{
	char fourcc[5];

	fourcc_to_string(pixelformat, fourcc);

	printf("INFO format=%ux%u/%s type=%s\n", width, height, fourcc,
	       type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? "mplane" : "single");
}

int main(int argc, char **argv)
{
	struct options opts = { 0 };
	struct v4l2_capability cap;
	enum v4l2_buf_type type;
	struct buffer *buffers = NULL;
	unsigned int buffer_count = 0;
	struct stats stats = { 0 };
	char path[PATH_MAX];
	uint32_t pixelformat = 0;
	unsigned int width = 0;
	unsigned int height = 0;
	double mean;
	double variance;
	double zero_pct;
	double ff_pct;
	int fd;
	int rc = 1;

	if (parse_options(argc, argv, &opts)) {
		usage(stderr);
		return 2;
	}

	fd = open_capture_device(&opts, path, sizeof(path), &type, &cap);
	if (fd == -1)
		return 1;

	if (opts.verbose) {
		printf("INFO device=%s driver=%s card=%s bus=%s\n", path,
		       cap.driver, cap.card, cap.bus_info);
	}

	if (get_format(fd, type, &width, &height, &pixelformat) == -1) {
		printf("FAIL get_format device=%s error=%s\n", path, strerror(errno));
		goto out;
	}
	if (opts.verbose)
		print_format(width, height, pixelformat, type);
	if (!width || !height || !pixelformat) {
		char fourcc[5];

		fourcc_to_string(pixelformat, fourcc);
		printf("FAIL invalid_format device=%s width=%u height=%u format=%s\n",
		       path, width, height, fourcc);
		goto out;
	}

	if (map_buffers(fd, type, &buffers, &buffer_count))
		goto out;

	if (capture_frames(fd, type, buffers, buffer_count, &opts, &stats))
		goto out;

	if (!stats.bytes) {
		printf("FAIL no_frame_data device=%s\n", path);
		goto out;
	}

	mean = stats.sum / (double)stats.bytes;
	variance = (stats.sumsq / (double)stats.bytes) - (mean * mean);
	if (variance < 0 && variance > -0.0001)
		variance = 0;
	zero_pct = ((double)stats.zero_count * 100.0) / (double)stats.bytes;
	ff_pct = ((double)stats.ff_count * 100.0) / (double)stats.bytes;

	if (stats.zero_count == stats.bytes) {
		printf("FAIL blank_zero device=%s bytes=%llu\n", path,
		       (unsigned long long)stats.bytes);
		goto out;
	}
	if (stats.ff_count == stats.bytes) {
		printf("FAIL blank_ff device=%s bytes=%llu\n", path,
		       (unsigned long long)stats.bytes);
		goto out;
	}
	if (mean < opts.min_mean) {
		printf("FAIL underexposed device=%s mean=%.2f min_mean=%.2f\n",
		       path, mean, opts.min_mean);
		goto out;
	}
	if (mean > opts.max_mean) {
		printf("FAIL overexposed device=%s mean=%.2f max_mean=%.2f\n",
		       path, mean, opts.max_mean);
		goto out;
	}
	if (variance < opts.min_variance) {
		printf("FAIL flat_frame device=%s variance=%.2f min_variance=%.2f\n",
		       path, variance, opts.min_variance);
		goto out;
	}
	if (opts.frames > 1 && stats.max_delta_percent < opts.min_delta_percent) {
		printf("FAIL no_live_delta device=%s delta=%.4f min_delta=%.4f\n",
		       path, stats.max_delta_percent, opts.min_delta_percent);
		goto out;
	}

	printf("PASS camera device=%s frames=%u bytes=%llu mean=%.2f variance=%.2f min=%u max=%u zero=%.2f%% ff=%.2f%% delta=%.4f%%\n",
	       path, opts.frames, (unsigned long long)stats.bytes, mean, variance,
	       stats.min, stats.max, zero_pct, ff_pct, stats.max_delta_percent);
	rc = 0;

out:
	unmap_buffers(buffers, buffer_count);
	close(fd);
	return rc;
}
