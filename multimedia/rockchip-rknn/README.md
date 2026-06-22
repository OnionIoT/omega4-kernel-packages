# rockchip-rknn

`rockchip-rknn` packages the Rockchip RKNN userspace pieces needed to validate
the RV1103B NPU from OpenWrt:

- `librknnmrt.so` from Rockchip RKNN Toolkit2 `armhf-uclibc`
- `rknn_server` for RKNN Toolkit2 connected-board debugging
- `rknn_mobilenet_demo`, built with the Rockchip uClibc toolchain
- `mobilenet_v1.rknn` for `RV1106B_RV1103B`
- `omega4-rknn-yolo-live`, a continuous camera-to-YOLOv5n detector
- `yolov5n.rknn`, converted from the Rockchip model-zoo optimized ONNX model
  for `rv1103b`

The package depends on `rockchip-aiq` to reuse the existing uClibc loader and
standard C/C++ runtime libraries already shipped for the camera ISP stack.

## Run

Run the bundled model and bundled test image:

```sh
omega4-rknn-mobilenet
```

Run the bundled model against a camera snapshot:

```sh
omega4-rknn-camera --device /dev/video7
```

The camera wrapper captures a short burst by default and uses the last complete
NV12 frame. This lets RKAIQ/AE settle before the frame is converted to the
224x224 BMP input passed to the RKNN demo.

Keep the captured model input for inspection:

```sh
omega4-rknn-camera \
	--device /dev/video7 \
	--width 640 \
	--height 480 \
	--warmup-frames 15 \
	--raw-image /tmp/camera-rknn-input.nv12 \
	--image /tmp/camera-rknn-input.bmp
```

The demo prints the RKNN SDK/runtime version, model input/output tensors,
inference time, and top classification indexes.

Run continuous YOLOv5n inference on the live camera stream:

```sh
omega4-rknn-yolo-live \
	--device /dev/video7 \
	--model /usr/share/rockchip-rknn/models/RV1106B_RV1103B/yolov5n.rknn \
	--width 640 \
	--height 480 \
	--fps 30 \
	--warmup-frames 15 \
	--infer-every 3 \
	--zero-copy
```

The YOLO tool keeps the V4L2 stream open, skips the configured startup frames
for camera exposure warmup, and prints one JSON object per inference frame.
Each object includes `frame`, `infer_ms`, capture dimensions, and a COCO
`objects` array with class name, confidence score, and source-frame box.

Useful options:

```sh
omega4-rknn-yolo-live --threshold 0.35 --nms 0.45
omega4-rknn-yolo-live --infer-every 1 --max-frames 120
omega4-rknn-yolo-live --no-empty
```

On RV1103B hardware, YOLO RKNN models must be built for the Toolkit2
`rv1103b` or `rv1106b` target. Models built for plain `rv1103` or `rv1106`
can initialize but time out on the first convolution at `rknn_run`.
