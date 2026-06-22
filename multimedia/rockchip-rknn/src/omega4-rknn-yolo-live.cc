// SPDX-License-Identifier: Apache-2.0
//
// Continuous V4L2 camera -> RKNN YOLOv5 detector for Omega4/RV1103B.
//
// The YOLOv5 postprocess math is adapted from Rockchip rknn_model_zoo
// examples/yolov5, Apache-2.0, V2.3.2.

#include "rknn_api.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <vector>

#define OBJ_CLASS_NUM 80
#define OBJ_NUMB_MAX_SIZE 128
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)

static const char *kDefaultModel =
    "/usr/share/rockchip-rknn/models/RV1106B_RV1103B/yolov5n.rknn";
static const char *kDefaultLabels =
    "/usr/share/rockchip-rknn/models/coco_80_labels_list.txt";

static volatile sig_atomic_t g_stop;

struct Buffer {
  void *start;
  size_t length;
};

struct Frame {
  struct v4l2_buffer buf;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
};

struct Box {
  int left;
  int top;
  int right;
  int bottom;
};

struct Detection {
  Box box;
  float score;
  int cls_id;
};

struct Letterbox {
  int x_pad;
  int y_pad;
  float scale;
};

struct AppConfig {
  const char *device = "/dev/video7";
  const char *model = kDefaultModel;
  const char *labels = kDefaultLabels;
  int width = 640;
  int height = 480;
  int fps = 30;
  int warmup_frames = 15;
  int infer_every = 3;
  int max_frames = 0;
  float threshold = 0.25f;
  float nms_threshold = 0.45f;
  bool print_empty = true;
  bool zero_copy = false;
};

struct RknnYolo {
  rknn_context ctx = 0;
  rknn_input_output_num io_num;
  rknn_tensor_attr *input_attrs = NULL;
  rknn_tensor_attr *output_attrs = NULL;
  rknn_tensor_mem *input_mem = NULL;
  rknn_tensor_mem *output_mems[8] = {};
  int model_width = 0;
  int model_height = 0;
  int model_channel = 0;
  bool is_quant = false;
  bool zero_copy = false;
};

static const int kAnchors[3][6] = {
    {10, 13, 16, 30, 33, 23},
    {30, 61, 62, 45, 59, 119},
    {116, 90, 156, 198, 373, 326},
};

static char *g_labels[OBJ_CLASS_NUM];

static void on_signal(int)
{
  g_stop = 1;
}

static int64_t now_us()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
  int ret;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && errno == EINTR);
  return ret;
}

static void dump_tensor_attr(const rknn_tensor_attr *attr)
{
  char dims[128] = {0};
  for (int i = 0; i < attr->n_dims; ++i) {
    int off = strlen(dims);
    snprintf(dims + off, sizeof(dims) - off, "%d%s", attr->dims[i],
             i == attr->n_dims - 1 ? "" : ", ");
  }

  fprintf(stderr,
          "  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, "
          "fmt=%s, type=%s, qnt_type=%s, zp=%d, scale=%f\n",
          attr->index, attr->name, attr->n_dims, dims, attr->n_elems,
          attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
          get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static int load_labels(const char *path)
{
  FILE *fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "failed to open labels %s: %s\n", path, strerror(errno));
    return -1;
  }

  char line[128];
  int count = 0;
  while (count < OBJ_CLASS_NUM && fgets(line, sizeof(line), fp)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    g_labels[count] = strdup(line);
    if (!g_labels[count]) {
      fclose(fp);
      return -1;
    }
    count++;
  }
  fclose(fp);

  if (count != OBJ_CLASS_NUM) {
    fprintf(stderr, "labels file has %d labels, expected %d\n", count,
            OBJ_CLASS_NUM);
    return -1;
  }

  return 0;
}

static void free_labels()
{
  for (int i = 0; i < OBJ_CLASS_NUM; ++i) {
    free(g_labels[i]);
    g_labels[i] = NULL;
  }
}

static const char *label_name(int cls_id)
{
  if (cls_id < 0 || cls_id >= OBJ_CLASS_NUM || !g_labels[cls_id])
    return "unknown";
  return g_labels[cls_id];
}

static uint8_t clamp_u8(int value)
{
  if (value < 0)
    return 0;
  if (value > 255)
    return 255;
  return (uint8_t)value;
}

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g,
                       uint8_t *b)
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

static void nv12_to_rgb_letterbox(const uint8_t *nv12, int src_w, int src_h,
                                  int dst_w, int dst_h, uint8_t *dst,
                                  Letterbox *letterbox)
{
  float scale_w = (float)dst_w / (float)src_w;
  float scale_h = (float)dst_h / (float)src_h;
  float scale = scale_w < scale_h ? scale_w : scale_h;
  int resize_w = (int)roundf((float)src_w * scale);
  int resize_h = (int)roundf((float)src_h * scale);
  int x_pad = (dst_w - resize_w) / 2;
  int y_pad = (dst_h - resize_h) / 2;

  memset(dst, 114, (size_t)dst_w * dst_h * 3);

  for (int y = 0; y < resize_h; ++y) {
    int sy = (int)((float)y / scale);
    if (sy >= src_h)
      sy = src_h - 1;
    for (int x = 0; x < resize_w; ++x) {
      int sx = (int)((float)x / scale);
      if (sx >= src_w)
        sx = src_w - 1;
      size_t y_index = (size_t)sy * src_w + sx;
      size_t uv_index = (size_t)src_w * src_h + (size_t)(sy / 2) * src_w +
                        (sx & ~1);
      uint8_t *out = dst + ((size_t)(y + y_pad) * dst_w + (x + x_pad)) * 3;
      yuv_to_rgb(nv12[y_index], nv12[uv_index], nv12[uv_index + 1], &out[0],
                 &out[1], &out[2]);
    }
  }

  letterbox->x_pad = x_pad;
  letterbox->y_pad = y_pad;
  letterbox->scale = scale;
}

static void copy_quantized_row(RknnYolo *app, uint8_t *dst,
                               const uint8_t *src, size_t pixels)
{
  if (app->input_attrs[0].type == RKNN_TENSOR_INT8) {
    int8_t *qdst = (int8_t *)dst;
    int zp = app->input_attrs[0].zp;
    float scale = app->input_attrs[0].scale;

    for (size_t i = 0; i < pixels; ++i) {
      float normalized = (float)src[i] / 255.0f;
      int q = (int)roundf(normalized / scale) + zp;
      if (q < -128)
        q = -128;
      if (q > 127)
        q = 127;
      qdst[i] = (int8_t)q;
    }
    return;
  }

  memcpy(dst, src, pixels);
}

static void copy_input_with_stride(RknnYolo *app, const uint8_t *rgb)
{
  int width = app->model_width;
  int height = app->model_height;
  int channel = app->model_channel;
  int stride = app->input_attrs[0].w_stride > 0 ? app->input_attrs[0].w_stride
                                                : width;
  uint8_t *dst = (uint8_t *)app->input_mem->virt_addr;

  if (stride == width) {
    copy_quantized_row(app, dst, rgb, (size_t)width * height * channel);
    return;
  }

  size_t src_row = (size_t)width * channel;
  size_t dst_row = (size_t)stride * channel;
  for (int y = 0; y < height; ++y) {
    copy_quantized_row(app, dst + (size_t)y * dst_row,
                       rgb + (size_t)y * src_row, src_row);
  }
}

static int init_rknn_yolo(const char *model_path, bool zero_copy, RknnYolo *app)
{
  app->zero_copy = zero_copy;
  int ret = rknn_init(&app->ctx, (char *)model_path, 0, 0, NULL);
  if (ret < 0) {
    fprintf(stderr, "rknn_init failed: %d (%s)\n", ret, model_path);
    return -1;
  }

  rknn_sdk_version sdk_ver;
  ret = rknn_query(app->ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver));
  if (ret == RKNN_SUCC) {
    fprintf(stderr, "rknn_api/rknnrt version: %s, driver version: %s\n",
            sdk_ver.api_version, sdk_ver.drv_version);
  }

  ret = rknn_query(app->ctx, RKNN_QUERY_IN_OUT_NUM, &app->io_num,
                   sizeof(app->io_num));
  if (ret != RKNN_SUCC) {
    fprintf(stderr, "RKNN_QUERY_IN_OUT_NUM failed: %d\n", ret);
    return -1;
  }
  if (app->io_num.n_input != 1 || app->io_num.n_output != 3 ||
      app->io_num.n_output > 8) {
    fprintf(stderr, "unsupported YOLO model io: input=%u output=%u\n",
            app->io_num.n_input, app->io_num.n_output);
    return -1;
  }

  app->input_attrs =
      (rknn_tensor_attr *)calloc(app->io_num.n_input, sizeof(rknn_tensor_attr));
  app->output_attrs =
      (rknn_tensor_attr *)calloc(app->io_num.n_output, sizeof(rknn_tensor_attr));
  if (!app->input_attrs || !app->output_attrs)
    return -1;

  fprintf(stderr, "input tensors:\n");
  for (uint32_t i = 0; i < app->io_num.n_input; ++i) {
    app->input_attrs[i].index = i;
    ret = rknn_query(app->ctx, RKNN_QUERY_NATIVE_INPUT_ATTR,
                     &app->input_attrs[i], sizeof(rknn_tensor_attr));
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "RKNN_QUERY_NATIVE_INPUT_ATTR failed: %d\n", ret);
      return -1;
    }
    dump_tensor_attr(&app->input_attrs[i]);
  }

  fprintf(stderr, "output tensors:\n");
  for (uint32_t i = 0; i < app->io_num.n_output; ++i) {
    app->output_attrs[i].index = i;
    ret = rknn_query(app->ctx, RKNN_QUERY_OUTPUT_ATTR, &app->output_attrs[i],
                     sizeof(rknn_tensor_attr));
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "RKNN_QUERY_OUTPUT_ATTR failed: %d\n", ret);
      return -1;
    }
    dump_tensor_attr(&app->output_attrs[i]);
  }

  app->model_height = app->input_attrs[0].dims[1];
  app->model_width = app->input_attrs[0].dims[2];
  app->model_channel = app->input_attrs[0].dims[3];
  app->is_quant =
      app->output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;

  if (!app->zero_copy) {
    fprintf(stderr, "rknn io mode: inputs_set/outputs_get\n");
    fprintf(stderr, "model input: %dx%dx%d quant=%s\n", app->model_width,
            app->model_height, app->model_channel,
            app->is_quant ? "yes" : "no");
    return 0;
  }

  app->input_attrs[0].type = RKNN_TENSOR_UINT8;
  app->input_attrs[0].fmt = RKNN_TENSOR_NHWC;

  app->input_mem = rknn_create_mem(app->ctx, app->input_attrs[0].size_with_stride);
  if (!app->input_mem) {
    fprintf(stderr, "rknn_create_mem input failed\n");
    return -1;
  }
  ret = rknn_set_io_mem(app->ctx, app->input_mem, &app->input_attrs[0]);
  if (ret < 0) {
    fprintf(stderr, "rknn_set_io_mem input failed: %d\n", ret);
    return -1;
  }

  for (uint32_t i = 0; i < app->io_num.n_output; ++i) {
    app->output_mems[i] =
        rknn_create_mem(app->ctx, app->output_attrs[i].size_with_stride);
    if (!app->output_mems[i]) {
      fprintf(stderr, "rknn_create_mem output %u failed\n", i);
      return -1;
    }
    ret = rknn_set_io_mem(app->ctx, app->output_mems[i], &app->output_attrs[i]);
    if (ret < 0) {
      fprintf(stderr, "rknn_set_io_mem output %u failed: %d\n", i, ret);
      return -1;
    }
  }

  fprintf(stderr, "rknn io mode: zero-copy\n");
  fprintf(stderr, "model input: %dx%dx%d quant=%s\n", app->model_width,
          app->model_height, app->model_channel, app->is_quant ? "yes" : "no");
  return 0;
}

static void release_rknn_yolo(RknnYolo *app)
{
  if (app->input_mem)
    rknn_destroy_mem(app->ctx, app->input_mem);
  for (uint32_t i = 0; i < app->io_num.n_output; ++i) {
    if (app->output_mems[i])
      rknn_destroy_mem(app->ctx, app->output_mems[i]);
  }
  if (app->ctx)
    rknn_destroy(app->ctx);
  free(app->input_attrs);
  free(app->output_attrs);
}

static int32_t clip_f32(float val, float min, float max)
{
  if (val <= min)
    return (int32_t)min;
  if (val >= max)
    return (int32_t)max;
  return (int32_t)val;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
  float dst_val = (f32 / scale) + zp;
  return (int8_t)clip_f32(dst_val, -128, 127);
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
  return ((float)qnt - (float)zp) * scale;
}

static int process_i8(int8_t *input, const int *anchor, int grid_h, int grid_w,
                      int stride, std::vector<float> &boxes,
                      std::vector<float> &scores, std::vector<int> &class_ids,
                      float threshold, int32_t zp, float scale)
{
  int valid_count = 0;
  int grid_len = grid_h * grid_w;
  int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale);

  for (int a = 0; a < 3; ++a) {
    for (int i = 0; i < grid_h; ++i) {
      for (int j = 0; j < grid_w; ++j) {
        int8_t box_confidence =
            input[(PROP_BOX_SIZE * a + 4) * grid_len + i * grid_w + j];
        if (box_confidence < thres_i8)
          continue;

        int offset = (PROP_BOX_SIZE * a) * grid_len + i * grid_w + j;
        int8_t *in_ptr = input + offset;
        int8_t max_class_prob = in_ptr[5 * grid_len];
        int max_class_id = 0;
        for (int k = 1; k < OBJ_CLASS_NUM; ++k) {
          int8_t prob = in_ptr[(5 + k) * grid_len];
          if (prob > max_class_prob) {
            max_class_id = k;
            max_class_prob = prob;
          }
        }

        float score = deqnt_affine_to_f32(max_class_prob, zp, scale) *
                      deqnt_affine_to_f32(box_confidence, zp, scale);
        if (score < threshold)
          continue;

        float box_x = deqnt_affine_to_f32(*in_ptr, zp, scale) * 2.0f - 0.5f;
        float box_y =
            deqnt_affine_to_f32(in_ptr[grid_len], zp, scale) * 2.0f - 0.5f;
        float box_w =
            deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale) * 2.0f;
        float box_h =
            deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale) * 2.0f;

        box_x = (box_x + j) * (float)stride;
        box_y = (box_y + i) * (float)stride;
        box_w = box_w * box_w * (float)anchor[a * 2];
        box_h = box_h * box_h * (float)anchor[a * 2 + 1];
        box_x -= box_w / 2.0f;
        box_y -= box_h / 2.0f;

        boxes.push_back(box_x);
        boxes.push_back(box_y);
        boxes.push_back(box_w);
        boxes.push_back(box_h);
        scores.push_back(score);
        class_ids.push_back(max_class_id);
        valid_count++;
      }
    }
  }

  return valid_count;
}

static float overlap(float xmin0, float ymin0, float xmax0, float ymax0,
                     float xmin1, float ymin1, float xmax1, float ymax1)
{
  float w = fmaxf(0.f, fminf(xmax0, xmax1) - fmaxf(xmin0, xmin1) + 1.0f);
  float h = fmaxf(0.f, fminf(ymax0, ymax1) - fmaxf(ymin0, ymin1) + 1.0f);
  float inter = w * h;
  float uni = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
              (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - inter;
  return uni <= 0.f ? 0.f : inter / uni;
}

static void apply_nms(int valid_count, const std::vector<float> &boxes,
                      const std::vector<int> &class_ids,
                      std::vector<int> &order, int filter_id, float threshold)
{
  for (int i = 0; i < valid_count; ++i) {
    int n = order[i];
    if (n == -1 || class_ids[n] != filter_id)
      continue;
    for (int j = i + 1; j < valid_count; ++j) {
      int m = order[j];
      if (m == -1 || class_ids[m] != filter_id)
        continue;
      float iou = overlap(boxes[n * 4], boxes[n * 4 + 1],
                          boxes[n * 4] + boxes[n * 4 + 2],
                          boxes[n * 4 + 1] + boxes[n * 4 + 3],
                          boxes[m * 4], boxes[m * 4 + 1],
                          boxes[m * 4] + boxes[m * 4 + 2],
                          boxes[m * 4 + 1] + boxes[m * 4 + 3]);
      if (iou > threshold)
        order[j] = -1;
    }
  }
}

static int post_process(RknnYolo *app, const void *output_ptrs[8],
                        const Letterbox *letterbox, float conf_threshold,
                        float nms_threshold,
                        std::vector<Detection> *detections)
{
  std::vector<float> boxes;
  std::vector<float> scores;
  std::vector<int> class_ids;
  int valid_count = 0;

  detections->clear();

  for (uint32_t i = 0; i < app->io_num.n_output; ++i) {
    int grid_h = app->output_attrs[i].dims[2];
    int grid_w = app->output_attrs[i].dims[3];
    int stride = app->model_height / grid_h;

    if (!app->is_quant) {
      fprintf(stderr, "only INT8 YOLOv5 RKNN output is supported\n");
      return -1;
    }

    valid_count += process_i8((int8_t *)output_ptrs[i],
                              kAnchors[i], grid_h, grid_w, stride, boxes,
                              scores, class_ids, conf_threshold,
                              app->output_attrs[i].zp,
                              app->output_attrs[i].scale);
  }

  if (valid_count <= 0)
    return 0;

  std::vector<int> order(valid_count);
  for (int i = 0; i < valid_count; ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(),
            [&scores](int a, int b) { return scores[a] > scores[b]; });

  std::set<int> class_set(class_ids.begin(), class_ids.end());
  for (std::set<int>::iterator it = class_set.begin(); it != class_set.end();
       ++it) {
    apply_nms(valid_count, boxes, class_ids, order, *it, nms_threshold);
  }

  for (int i = 0; i < valid_count && detections->size() < OBJ_NUMB_MAX_SIZE;
       ++i) {
    int n = order[i];
    if (n == -1)
      continue;

    float x1 = boxes[n * 4] - letterbox->x_pad;
    float y1 = boxes[n * 4 + 1] - letterbox->y_pad;
    float x2 = x1 + boxes[n * 4 + 2];
    float y2 = y1 + boxes[n * 4 + 3];

    Detection det;
    det.box.left =
        (int)(std::max(0.f, std::min(x1, (float)app->model_width)) /
              letterbox->scale);
    det.box.top =
        (int)(std::max(0.f, std::min(y1, (float)app->model_height)) /
              letterbox->scale);
    det.box.right =
        (int)(std::max(0.f, std::min(x2, (float)app->model_width)) /
              letterbox->scale);
    det.box.bottom =
        (int)(std::max(0.f, std::min(y2, (float)app->model_height)) /
              letterbox->scale);
    det.score = scores[n];
    det.cls_id = class_ids[n];
    detections->push_back(det);
  }

  return 0;
}

static int run_inference(RknnYolo *app, const uint8_t *nv12, int width,
                         int height, float threshold, float nms_threshold,
                         std::vector<Detection> *detections,
                         int64_t *elapsed_us)
{
  Letterbox letterbox;
  std::vector<uint8_t> rgb((size_t)app->model_width * app->model_height *
                           app->model_channel);
  nv12_to_rgb_letterbox(nv12, width, height, app->model_width,
                        app->model_height, rgb.data(), &letterbox);

  if (!app->zero_copy) {
    rknn_input input;
    memset(&input, 0, sizeof(input));
    input.index = 0;
    input.buf = rgb.data();
    input.size = rgb.size();
    input.pass_through = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;

    int ret = rknn_inputs_set(app->ctx, 1, &input);
    if (ret < 0) {
      fprintf(stderr, "rknn_inputs_set failed: %d\n", ret);
      return -1;
    }

    rknn_output outputs[8];
    memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < app->io_num.n_output; ++i) {
      outputs[i].index = i;
      outputs[i].want_float = 0;
    }

    int64_t start = now_us();
    ret = rknn_run(app->ctx, NULL);
    *elapsed_us = now_us() - start;
    if (ret < 0) {
      fprintf(stderr, "rknn_run failed: %d\n", ret);
      return -1;
    }

    ret = rknn_outputs_get(app->ctx, app->io_num.n_output, outputs, NULL);
    if (ret < 0) {
      fprintf(stderr, "rknn_outputs_get failed: %d\n", ret);
      return -1;
    }

    const void *output_ptrs[8] = {};
    for (uint32_t i = 0; i < app->io_num.n_output; ++i)
      output_ptrs[i] = outputs[i].buf;
    ret = post_process(app, output_ptrs, &letterbox, threshold, nms_threshold,
                       detections);
    rknn_outputs_release(app->ctx, app->io_num.n_output, outputs);
    return ret;
  }

  copy_input_with_stride(app, rgb.data());
  int ret =
      rknn_mem_sync(app->ctx, app->input_mem, RKNN_MEMORY_SYNC_TO_DEVICE);
  if (ret < 0) {
    fprintf(stderr, "rknn_mem_sync input failed: %d\n", ret);
    return -1;
  }

  int64_t start = now_us();
  ret = rknn_run(app->ctx, NULL);
  *elapsed_us = now_us() - start;
  if (ret < 0) {
    fprintf(stderr, "rknn_run failed: %d\n", ret);
    return -1;
  }

  for (uint32_t i = 0; i < app->io_num.n_output; ++i) {
    ret = rknn_mem_sync(app->ctx, app->output_mems[i],
                        RKNN_MEMORY_SYNC_FROM_DEVICE);
    if (ret < 0) {
      fprintf(stderr, "rknn_mem_sync output %u failed: %d\n", i, ret);
      return -1;
    }
  }

  const void *output_ptrs[8] = {};
  for (uint32_t i = 0; i < app->io_num.n_output; ++i)
    output_ptrs[i] = app->output_mems[i]->virt_addr;
  return post_process(app, output_ptrs, &letterbox, threshold, nms_threshold,
                      detections);
}

static int open_camera(const AppConfig *cfg, Buffer **buffers_out,
                       unsigned int *buffer_count_out,
                       enum v4l2_buf_type *type_out)
{
  int fd = open(cfg->device, O_RDWR | O_NONBLOCK, 0);
  if (fd < 0) {
    fprintf(stderr, "failed to open %s: %s\n", cfg->device, strerror(errno));
    return -1;
  }

  struct v4l2_capability cap;
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    fprintf(stderr, "VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
    close(fd);
    return -1;
  }
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) &&
      !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
    fprintf(stderr, "%s is not a capture device\n", cfg->device);
    close(fd);
    return -1;
  }

  enum v4l2_buf_type type =
      (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
          ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
          : V4L2_BUF_TYPE_VIDEO_CAPTURE;

  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = type;
  if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    fmt.fmt.pix_mp.width = cfg->width;
    fmt.fmt.pix_mp.height = cfg->height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = 1;
  } else {
    fmt.fmt.pix.width = cfg->width;
    fmt.fmt.pix.height = cfg->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
  }
  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    fprintf(stderr, "VIDIOC_S_FMT %dx%d/NV12 failed: %s\n", cfg->width,
            cfg->height, strerror(errno));
    close(fd);
    return -1;
  }

  struct v4l2_streamparm parm;
  memset(&parm, 0, sizeof(parm));
  parm.type = type;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = cfg->fps;
  xioctl(fd, VIDIOC_S_PARM, &parm);

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = 4;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    fprintf(stderr, "VIDIOC_REQBUFS failed: %s\n", strerror(errno));
    close(fd);
    return -1;
  }
  if (req.count < 2) {
    fprintf(stderr, "insufficient V4L2 buffers\n");
    close(fd);
    return -1;
  }

  Buffer *buffers = (Buffer *)calloc(req.count, sizeof(Buffer));
  if (!buffers) {
    close(fd);
    return -1;
  }

  for (unsigned int i = 0; i < req.count; ++i) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      buf.length = VIDEO_MAX_PLANES;
      buf.m.planes = planes;
    }
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
      fprintf(stderr, "VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
      close(fd);
      free(buffers);
      return -1;
    }
    buffers[i].length = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                            ? buf.m.planes[0].length
                            : buf.length;
    buffers[i].start =
        mmap(NULL, buffers[i].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
             type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                 ? buf.m.planes[0].m.mem_offset
                 : buf.m.offset);
    if (buffers[i].start == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s\n", strerror(errno));
      close(fd);
      free(buffers);
      return -1;
    }
  }

  for (unsigned int i = 0; i < req.count; ++i) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      buf.length = VIDEO_MAX_PLANES;
      buf.m.planes = planes;
    }
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
      fprintf(stderr, "VIDIOC_QBUF failed: %s\n", strerror(errno));
      close(fd);
      free(buffers);
      return -1;
    }
  }

  if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
    close(fd);
    free(buffers);
    return -1;
  }

  *buffers_out = buffers;
  *buffer_count_out = req.count;
  *type_out = type;
  return fd;
}

static void close_camera(int fd, enum v4l2_buf_type type, Buffer *buffers,
                         unsigned int buffer_count)
{
  if (fd >= 0) {
    xioctl(fd, VIDIOC_STREAMOFF, &type);
  }
  for (unsigned int i = 0; i < buffer_count; ++i) {
    if (buffers[i].start && buffers[i].start != MAP_FAILED)
      munmap(buffers[i].start, buffers[i].length);
  }
  free(buffers);
  if (fd >= 0)
    close(fd);
}

static int dequeue_frame(int fd, enum v4l2_buf_type type, Frame *frame)
{
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);

  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 0;

  int ret = select(fd + 1, &fds, NULL, NULL, &tv);
  if (ret < 0) {
    if (errno == EINTR)
      return 1;
    fprintf(stderr, "select failed: %s\n", strerror(errno));
    return -1;
  }
  if (ret == 0) {
    fprintf(stderr, "camera frame timeout\n");
    return 1;
  }

  memset(frame, 0, sizeof(*frame));
  frame->buf.type = type;
  frame->buf.memory = V4L2_MEMORY_MMAP;
  if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    frame->buf.length = VIDEO_MAX_PLANES;
    frame->buf.m.planes = frame->planes;
  }
  if (xioctl(fd, VIDIOC_DQBUF, &frame->buf) < 0) {
    if (errno == EAGAIN)
      return 1;
    fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
    return -1;
  }

  return 0;
}

static void print_json_detections(uint32_t frame_id, int64_t pts_us,
                                  int64_t infer_us, int width, int height,
                                  const std::vector<Detection> &detections)
{
  printf("{\"frame\":%u,\"pts_us\":%lld,\"infer_ms\":%.2f,\"width\":%d,"
         "\"height\":%d,\"objects\":[",
         frame_id, (long long)pts_us, infer_us / 1000.0, width, height);
  for (size_t i = 0; i < detections.size(); ++i) {
    const Detection &det = detections[i];
    printf("%s{\"class_id\":%d,\"class\":\"%s\",\"score\":%.4f,"
           "\"box\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}}",
           i ? "," : "", det.cls_id, label_name(det.cls_id), det.score,
           det.box.left, det.box.top, det.box.right - det.box.left,
           det.box.bottom - det.box.top);
  }
  printf("]}\n");
  fflush(stdout);
}

static void usage(const char *prog)
{
  fprintf(stderr,
          "Usage: %s [options]\n"
          "  --device PATH          V4L2 device (default: /dev/video7)\n"
          "  --model PATH           YOLOv5 RKNN model\n"
          "  --labels PATH          COCO labels file\n"
          "  --width PIXELS         Capture width (default: 640)\n"
          "  --height PIXELS        Capture height (default: 480)\n"
          "  --fps FPS              Capture FPS hint (default: 30)\n"
          "  --warmup-frames N      Frames to discard once (default: 15)\n"
          "  --infer-every N        Infer every Nth frame (default: 3)\n"
          "  --max-frames N         Stop after N captured frames (default: 0=infinite)\n"
          "  --threshold FLOAT      Confidence threshold (default: 0.25)\n"
          "  --nms FLOAT            NMS IoU threshold (default: 0.45)\n"
          "  --no-empty             Do not print frames with zero detections\n"
          "  --zero-copy            Use RKNN zero-copy IO buffers\n"
          "  -h, --help             Show this help\n",
          prog);
}

static int parse_int(const char *value, int *out)
{
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (!value[0] || *end || parsed < 0 || parsed > 1000000)
    return -1;
  *out = (int)parsed;
  return 0;
}

static int parse_float(const char *value, float *out)
{
  char *end = NULL;
  float parsed = strtof(value, &end);
  if (!value[0] || *end || parsed < 0.0f || parsed > 1.0f)
    return -1;
  *out = parsed;
  return 0;
}

static int parse_args(int argc, char **argv, AppConfig *cfg)
{
  static const struct option long_opts[] = {
      {"device", required_argument, 0, 1},
      {"model", required_argument, 0, 2},
      {"labels", required_argument, 0, 3},
      {"width", required_argument, 0, 4},
      {"height", required_argument, 0, 5},
      {"fps", required_argument, 0, 6},
      {"warmup-frames", required_argument, 0, 7},
      {"infer-every", required_argument, 0, 8},
      {"max-frames", required_argument, 0, 9},
      {"threshold", required_argument, 0, 10},
      {"nms", required_argument, 0, 11},
      {"no-empty", no_argument, 0, 12},
      {"zero-copy", no_argument, 0, 13},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
    switch (opt) {
    case 1:
      cfg->device = optarg;
      break;
    case 2:
      cfg->model = optarg;
      break;
    case 3:
      cfg->labels = optarg;
      break;
    case 4:
      if (parse_int(optarg, &cfg->width))
        return -1;
      break;
    case 5:
      if (parse_int(optarg, &cfg->height))
        return -1;
      break;
    case 6:
      if (parse_int(optarg, &cfg->fps))
        return -1;
      break;
    case 7:
      if (parse_int(optarg, &cfg->warmup_frames))
        return -1;
      break;
    case 8:
      if (parse_int(optarg, &cfg->infer_every))
        return -1;
      break;
    case 9:
      if (parse_int(optarg, &cfg->max_frames))
        return -1;
      break;
    case 10:
      if (parse_float(optarg, &cfg->threshold))
        return -1;
      break;
    case 11:
      if (parse_float(optarg, &cfg->nms_threshold))
        return -1;
      break;
    case 12:
      cfg->print_empty = false;
      break;
    case 13:
      cfg->zero_copy = true;
      break;
    case 'h':
      usage(argv[0]);
      exit(0);
    default:
      return -1;
    }
  }

  if (cfg->width <= 0 || cfg->height <= 0 || cfg->fps <= 0 ||
      cfg->infer_every <= 0) {
    return -1;
  }

  return 0;
}

int main(int argc, char **argv)
{
  AppConfig cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return 2;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (load_labels(cfg.labels) != 0)
    return 1;

  RknnYolo yolo;
  if (init_rknn_yolo(cfg.model, cfg.zero_copy, &yolo) != 0) {
    free_labels();
    return 1;
  }

  Buffer *buffers = NULL;
  unsigned int buffer_count = 0;
  enum v4l2_buf_type capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int cam_fd = open_camera(&cfg, &buffers, &buffer_count, &capture_type);
  if (cam_fd < 0) {
    release_rknn_yolo(&yolo);
    free_labels();
    return 1;
  }

  fprintf(stderr,
          "streaming %s %dx%d fps=%d warmup=%d infer_every=%d threshold=%.2f\n",
          cfg.device, cfg.width, cfg.height, cfg.fps, cfg.warmup_frames,
          cfg.infer_every, cfg.threshold);

  uint32_t frame_id = 0;
  int ret_code = 0;
  while (!g_stop) {
    Frame frame;
    int ret = dequeue_frame(cam_fd, capture_type, &frame);
    if (ret < 0) {
      ret_code = 1;
      break;
    }
    if (ret > 0)
      continue;

    frame_id++;
    if ((int)frame_id > cfg.warmup_frames &&
        ((int)(frame_id - cfg.warmup_frames) % cfg.infer_every) == 0) {
      int64_t elapsed_us = 0;
      std::vector<Detection> detections;
      ret = run_inference(&yolo, (const uint8_t *)buffers[frame.buf.index].start,
                          cfg.width, cfg.height, cfg.threshold,
                          cfg.nms_threshold, &detections, &elapsed_us);
      if (ret != 0) {
        ret_code = 1;
        xioctl(cam_fd, VIDIOC_QBUF, &frame.buf);
        break;
      }
      if (cfg.print_empty || !detections.empty()) {
        int64_t pts_us = (int64_t)frame.buf.timestamp.tv_sec * 1000000 +
                         frame.buf.timestamp.tv_usec;
        print_json_detections(frame_id, pts_us, elapsed_us, cfg.width,
                              cfg.height, detections);
      }
    }

    if (xioctl(cam_fd, VIDIOC_QBUF, &frame.buf) < 0) {
      fprintf(stderr, "VIDIOC_QBUF failed: %s\n", strerror(errno));
      ret_code = 1;
      break;
    }

    if (cfg.max_frames > 0 && (int)frame_id >= cfg.max_frames)
      break;
  }

  close_camera(cam_fd, capture_type, buffers, buffer_count);
  release_rknn_yolo(&yolo);
  free_labels();
  return ret_code;
}
