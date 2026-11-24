# SC3336 camera capture on Linux 6.12

## Problem and changes

On OpenWrt 25.12 / Linux 6.12.112, the SC3336 probes at I2C 4-0030
(ID 0xcc41) but the stock v0.0.16 DPHY async graph can complete without it.
The old CONFIG_NO_GKI-dependent gate skips endpoints whose sensor driver has
not probed yet. Register enabled endpoints unconditionally and let V4L2 async
binding wait for the sensor. Explicitly unregister/clean up the embedded
notifier; do not hand its allocation to the subdevice core as owned storage.

Zero initialize local V4L2 format, selection and interval requests in CIF,
ISP and CSI2. Linux 6.12 validates stream IDs, so uninitialized new fields
can reject ordinary stream-zero format queries with EINVAL.
The four affected kernel package releases increase from 2 to 3.

## Hardware validation — 24 November 2026

User-selected Omega4+ on HWSPD-C1 A29C, SC3336 camera, published v0.0.16
with rebuilt modules overlaid in /lib/modules/6.12.112+ and rebooted.
Original modules are backed up in sibling workspace evidence directory
hardware-validation/2026-11-24-camera. Package compile targets passed.

The sensor now binds to the DPHY and CIF/ISP async notifiers complete.
`mfg-camera-test -v` captures five changing, non-flat NV12 frames at
2304x1296 through /dev/video7 (rkisp_mainpath). Repeated capture passes.
A raw frame was exported using v4l2-ctl and decoded on the host; visual
inspection confirms a real scene, including ceiling lights and framing.
The alternate mipi-rx and rkisp1 packages are not required for this path.

Run tests after the rkaiq service has started; an early boot capture waited
for initial ISP parameters and reported a params stream-off timeout, although
capture passed. The subsequent capture passed. `media-ctl -p` still triggers
CSI2 get_fmt fallback warnings on graph queries; these are separate from the
fixed capture-time EINVAL calls. An ISP params link_validate warning also
remains. HDR, other sensors, all output formats and long-duration streaming
have not been qualified. These are local development fixes, not a new release.

## Reproduce

Build the phy-rockchip-csi2-dphy, video-rockchip-cif, video-rockchip-isp and
video-rockchip-mipi-csi2 targets under package/feeds/omega4ko using the current
board configuration. Install matching modules, reboot and allow boot services
to settle. Check sensor binding and run `mfg-camera-test -v`.
For a frame: `v4l2-ctl -d /dev/video7 --stream-mmap=4 --stream-count=1
--stream-to=/tmp/camera-frame.raw` (one shell command). Decode as raw NV12,
2304x1296. Preserve configuration and environment during firmware updates.
