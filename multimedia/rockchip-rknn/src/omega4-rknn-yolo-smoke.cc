// SPDX-License-Identifier: Apache-2.0

#include "rknn_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static int64_t now_us()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void dump_tensor_attr(const rknn_tensor_attr *attr)
{
  char dims[128] = {0};
  for (uint32_t i = 0; i < attr->n_dims; ++i) {
    int off = strlen(dims);
    snprintf(dims + off, sizeof(dims) - off, "%u%s", attr->dims[i],
             i == attr->n_dims - 1 ? "" : ", ");
  }

  fprintf(stderr,
          "  index=%d, name=%s, n_dims=%u, dims=[%s], n_elems=%u, size=%u, "
          "size_with_stride=%u, w_stride=%u, fmt=%s, type=%s, qnt_type=%s, "
          "zp=%d, scale=%f\n",
          attr->index, attr->name, attr->n_dims, dims, attr->n_elems,
          attr->size, attr->size_with_stride, attr->w_stride,
          get_format_string(attr->fmt), get_type_string(attr->type),
          get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

int main(int argc, char **argv)
{
  const char *model = argc > 1 ? argv[1]
                               : "/usr/share/rockchip-rknn/models/"
                                 "RV1106B_RV1103B/yolov5n.rknn";
  int fill_input = argc > 2 ? atoi(argv[2]) : 0;

  rknn_context ctx = 0;
  int ret = rknn_init(&ctx, (char *)model, 0, 0, NULL);
  if (ret < 0) {
    fprintf(stderr, "rknn_init failed: %d\n", ret);
    return 1;
  }

  rknn_sdk_version sdk_ver;
  ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver));
  if (ret == RKNN_SUCC)
    fprintf(stderr, "rknn_api/rknnrt version: %s, driver version: %s\n",
            sdk_ver.api_version, sdk_ver.drv_version);

  rknn_input_output_num io_num;
  ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
  if (ret != RKNN_SUCC || io_num.n_input != 1 || io_num.n_output > 16) {
    fprintf(stderr, "bad io count ret=%d input=%u output=%u\n", ret,
            io_num.n_input, io_num.n_output);
    return 1;
  }

  rknn_tensor_attr input_attr;
  memset(&input_attr, 0, sizeof(input_attr));
  input_attr.index = 0;
  ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &input_attr,
                   sizeof(input_attr));
  if (ret != RKNN_SUCC) {
    fprintf(stderr, "input attr query failed: %d\n", ret);
    return 1;
  }
  fprintf(stderr, "input tensor:\n");
  dump_tensor_attr(&input_attr);

  rknn_tensor_attr output_attrs[16];
  memset(output_attrs, 0, sizeof(output_attrs));
  fprintf(stderr, "output tensors:\n");
  for (uint32_t i = 0; i < io_num.n_output; ++i) {
    output_attrs[i].index = i;
    ret = rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &output_attrs[i],
                     sizeof(output_attrs[i]));
    if (ret != RKNN_SUCC) {
      fprintf(stderr, "output attr %u query failed: %d\n", i, ret);
      return 1;
    }
    dump_tensor_attr(&output_attrs[i]);
  }

  input_attr.type = RKNN_TENSOR_UINT8;
  input_attr.fmt = RKNN_TENSOR_NHWC;
  rknn_tensor_mem *input_mem = rknn_create_mem(ctx, input_attr.size_with_stride);
  if (!input_mem) {
    fprintf(stderr, "rknn_create_mem input failed\n");
    return 1;
  }
  ret = rknn_set_io_mem(ctx, input_mem, &input_attr);
  if (ret < 0) {
    fprintf(stderr, "rknn_set_io_mem input failed: %d\n", ret);
    return 1;
  }

  if (fill_input) {
    memset(input_mem->virt_addr, 114, input_attr.size_with_stride);
    ret = rknn_mem_sync(ctx, input_mem, RKNN_MEMORY_SYNC_TO_DEVICE);
    if (ret < 0) {
      fprintf(stderr, "rknn_mem_sync input failed: %d\n", ret);
      return 1;
    }
  }

  rknn_tensor_mem *output_mems[16] = {};
  for (uint32_t i = 0; i < io_num.n_output; ++i) {
    output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride);
    if (!output_mems[i]) {
      fprintf(stderr, "rknn_create_mem output %u failed\n", i);
      return 1;
    }
    ret = rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]);
    if (ret < 0) {
      fprintf(stderr, "rknn_set_io_mem output %u failed: %d\n", i, ret);
      return 1;
    }
  }

  int64_t start = now_us();
  ret = rknn_run(ctx, NULL);
  int64_t elapsed = now_us() - start;
  if (ret < 0) {
    fprintf(stderr, "rknn_run failed: %d elapsed_ms=%.2f\n", ret,
            elapsed / 1000.0);
    return 1;
  }

  fprintf(stderr, "rknn_run ok elapsed_ms=%.2f fill_input=%d\n",
          elapsed / 1000.0, fill_input);
  return 0;
}
