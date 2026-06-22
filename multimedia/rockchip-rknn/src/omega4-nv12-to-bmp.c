// SPDX-License-Identifier: MIT

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t clamp_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (uint8_t)value;
}

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v,
		       uint8_t *r, uint8_t *g, uint8_t *b)
{
	int c = (int)y - 16;
	int d = (int)u - 128;
	int e = (int)v - 128;

	if (c < 0)
		c = 0;

	*r = clamp_u8((298 * c + 409 * e + 128) >> 8);
	*g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
	*b = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

static int write_bmp(const char *path, const uint8_t *rgb, int width, int height)
{
	FILE *fp;
	uint8_t header[54] = {0};
	int row_stride = (width * 3 + 3) & ~3;
	uint32_t pixel_size = (uint32_t)(row_stride * height);
	uint32_t file_size = 54 + pixel_size;
	uint8_t *row;
	int y;

	fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
		return 1;
	}

	header[0] = 'B';
	header[1] = 'M';
	put_le32(&header[2], file_size);
	put_le32(&header[10], 54);
	put_le32(&header[14], 40);
	put_le32(&header[18], (uint32_t)width);
	put_le32(&header[22], (uint32_t)height);
	put_le16(&header[26], 1);
	put_le16(&header[28], 24);
	put_le32(&header[34], pixel_size);

	if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
		fclose(fp);
		return 1;
	}

	row = calloc(1, (size_t)row_stride);
	if (!row) {
		fclose(fp);
		return 1;
	}

	for (y = height - 1; y >= 0; y--) {
		int x;
		const uint8_t *src = rgb + (size_t)y * width * 3;

		memset(row, 0, (size_t)row_stride);
		for (x = 0; x < width; x++) {
			row[x * 3 + 0] = src[x * 3 + 2];
			row[x * 3 + 1] = src[x * 3 + 1];
			row[x * 3 + 2] = src[x * 3 + 0];
		}

		if (fwrite(row, 1, (size_t)row_stride, fp) != (size_t)row_stride) {
			free(row);
			fclose(fp);
			return 1;
		}
	}

	free(row);
	fclose(fp);
	return 0;
}

int main(int argc, char **argv)
{
	const char *input_path;
	const char *output_path;
	int src_w, src_h, dst_w, dst_h;
	size_t nv12_size;
	uint8_t *nv12;
	uint8_t *rgb;
	FILE *fp;
	long file_size;
	long frame_count;
	long frame_offset;
	size_t got;
	int y;

	if (argc != 7) {
		fprintf(stderr,
			"Usage: %s input.nv12 output.bmp src_w src_h dst_w dst_h\n",
			argv[0]);
		return 2;
	}

	input_path = argv[1];
	output_path = argv[2];
	src_w = atoi(argv[3]);
	src_h = atoi(argv[4]);
	dst_w = atoi(argv[5]);
	dst_h = atoi(argv[6]);

	if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
	    (src_w & 1) || (src_h & 1)) {
		fprintf(stderr, "invalid dimensions\n");
		return 2;
	}

	nv12_size = (size_t)src_w * src_h * 3 / 2;
	nv12 = malloc(nv12_size);
	rgb = malloc((size_t)dst_w * dst_h * 3);
	if (!nv12 || !rgb) {
		fprintf(stderr, "allocation failed\n");
		free(nv12);
		free(rgb);
		return 1;
	}

	fp = fopen(input_path, "rb");
	if (!fp) {
		fprintf(stderr, "failed to open %s: %s\n", input_path, strerror(errno));
		free(nv12);
		free(rgb);
		return 1;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		fprintf(stderr, "failed to seek %s: %s\n", input_path, strerror(errno));
		fclose(fp);
		free(nv12);
		free(rgb);
		return 1;
	}

	file_size = ftell(fp);
	if (file_size < 0) {
		fprintf(stderr, "failed to size %s: %s\n", input_path, strerror(errno));
		fclose(fp);
		free(nv12);
		free(rgb);
		return 1;
	}

	frame_count = file_size / (long)nv12_size;
	if (frame_count < 1) {
		fprintf(stderr, "short read: got %ld, expected at least %zu\n",
			file_size, nv12_size);
		fclose(fp);
		free(nv12);
		free(rgb);
		return 1;
	}

	frame_offset = (frame_count - 1) * (long)nv12_size;
	if (fseek(fp, frame_offset, SEEK_SET) != 0) {
		fprintf(stderr, "failed to seek frame %ld in %s: %s\n",
			frame_count, input_path, strerror(errno));
		fclose(fp);
		free(nv12);
		free(rgb);
		return 1;
	}

	got = fread(nv12, 1, nv12_size, fp);
	fclose(fp);
	if (got != nv12_size) {
		fprintf(stderr, "short read: got %zu, expected %zu\n", got, nv12_size);
		free(nv12);
		free(rgb);
		return 1;
	}

	{
		size_t y_size = (size_t)src_w * src_h;
		unsigned long long y_sum = 0;
		uint8_t y_min = 255;
		uint8_t y_max = 0;
		size_t i;

		for (i = 0; i < y_size; i++) {
			uint8_t value = nv12[i];

			if (value < y_min)
				y_min = value;
			if (value > y_max)
				y_max = value;
			y_sum += value;
		}

		fprintf(stderr,
			"selected NV12 frame %ld/%ld from %s: y_min=%u y_avg=%.2f y_max=%u\n",
			frame_count, frame_count, input_path, y_min,
			(double)y_sum / (double)y_size, y_max);
	}

	for (y = 0; y < dst_h; y++) {
		int x;
		int sy = y * src_h / dst_h;

		for (x = 0; x < dst_w; x++) {
			int sx = x * src_w / dst_w;
			size_t y_index = (size_t)sy * src_w + sx;
			size_t uv_index = (size_t)src_w * src_h +
				(size_t)(sy / 2) * src_w + (sx & ~1);
			uint8_t *out = rgb + ((size_t)y * dst_w + x) * 3;

			yuv_to_rgb(nv12[y_index], nv12[uv_index],
				   nv12[uv_index + 1], &out[0], &out[1], &out[2]);
		}
	}

	if (write_bmp(output_path, rgb, dst_w, dst_h)) {
		free(nv12);
		free(rgb);
		return 1;
	}

	free(nv12);
	free(rgb);
	return 0;
}
