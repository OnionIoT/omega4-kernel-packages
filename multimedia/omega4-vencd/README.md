# omega4-vencd

`omega4-vencd` is the Omega4-facing hardware H.264 encoder command and
OpenWrt service.

The daemon uses the Rockchip MPP userspace API directly. It captures frames from
the V4L2 camera device, imports camera DMA-BUF buffers into MPP, and writes
Annex-B H.264 to a file, FIFO, stdout, or native RTP/H.264 over UDP. H.264
headers are emitted at startup and on every IDR frame so late-joining RTP
clients can recover at the next GOP.

## CLI Examples

Write a short H.264 elementary stream:

```sh
omega4-vencd \
	--device /dev/video7 \
	--width 640 \
	--height 480 \
	--frames 120 \
	--bitrate 1200000 \
	--output /tmp/capture.h264
```

Send RTP/H.264 directly without ffmpeg on the device:

```sh
omega4-vencd \
	--device /dev/video7 \
	--width 640 \
	--height 480 \
	--fps 60 \
	--gop 15 \
	--rc cbr \
	--bitrate 800000 \
	--buffers 2 \
	--rtp-dest 127.0.0.1:5601 \
	--rtp-payload-type 96 \
	--rtp-mtu 1200
```

For a remote receiver, replace `127.0.0.1` with the receiver host address. A
matching SDP file for ffplay or ffmpeg looks like:

```sdp
v=0
o=- 0 0 IN IP4 127.0.0.1
s=Omega4 VENC RTP
c=IN IP4 127.0.0.1
t=0 0
m=video 5601 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1
```

Then play it from the receiver:

```sh
ffplay -protocol_whitelist file,udp,rtp -i omega4-vencd.sdp
```

Pipe to ffmpeg for RTP multicast:

```sh
omega4-vencd \
	--device /dev/video7 \
	--width 640 \
	--height 480 \
	--gop 30 \
	--bitrate 1200000 \
	--output - | \
ffmpeg -f h264 -i pipe:0 -c:v copy -f rtp \
	'rtp://239.255.4.4:5004?ttl=16&pkt_size=1200'
```

Pipe to GStreamer for RTP multicast:

```sh
omega4-vencd \
	--device /dev/video7 \
	--width 640 \
	--height 480 \
	--gop 30 \
	--bitrate 1200000 \
	--output - | \
gst-launch-1.0 fdsrc fd=0 ! \
	h264parse config-interval=1 ! \
	rtph264pay pt=96 config-interval=1 ! \
	udpsink host=239.255.4.4 port=5004 auto-multicast=true
```

## UCI Service

The service is disabled by default. Enable it with:

```sh
uci set omega4-venc.main.enabled='1'
uci set omega4-venc.main.device='/dev/video7'
uci set omega4-venc.main.width='640'
uci set omega4-venc.main.height='480'
uci set omega4-venc.main.bitrate='1200000'
uci set omega4-venc.main.output='/run/omega4-venc/main.h264'
uci set omega4-venc.main.output_type='fifo'
uci commit omega4-venc
/etc/init.d/omega4-vencd restart
```

Then consume the FIFO from ffmpeg or GStreamer. The encoder process blocks until
a reader opens the FIFO.

For native RTP service output, set `rtp_dest`. When this is set, the service
does not create or write the configured file/FIFO output:

```sh
uci set omega4-venc.main.enabled='1'
uci set omega4-venc.main.device='/dev/video7'
uci set omega4-venc.main.width='640'
uci set omega4-venc.main.height='480'
uci set omega4-venc.main.fps='60'
uci set omega4-venc.main.gop='15'
uci set omega4-venc.main.rc='cbr'
uci set omega4-venc.main.bitrate='800000'
uci set omega4-venc.main.buffers='2'
uci set omega4-venc.main.rtp_dest='192.168.1.10:5601'
uci set omega4-venc.main.rtp_payload_type='96'
uci set omega4-venc.main.rtp_mtu='1200'
uci commit omega4-venc
/etc/init.d/omega4-vencd restart
```

`buffers` controls how many V4L2 capture buffers `omega4-vencd` queues. The
default is `4`; use `2` for lower latency after the camera path is stable.
Allowed values are `2..10`.

## Validation

Validated on Lab4 with `/dev/video7`:

- CLI file output produced a 640x480 H.264 elementary stream.
- UCI/procd service file output produced a 640x480 H.264 elementary stream.
- CLI stdout output piped into ffmpeg and remuxed as RTP.
- RTP multicast through ffmpeg was received on the host and probed as H.264
  640x480 with decoded frame count.
- Native RTP output with `--rtp-dest 10.0.0.23:5601 --rtp-mtu 1200` was
  received on the host as H.264 640x480 and decoded to PNG.
- Captured evidence:
  - `http://10.0.0.23:5000/native-omega4-vencd-rtp-check.h264`
  - `http://10.0.0.23:5000/native-omega4-vencd-rtp-frame.png`
  - `http://10.0.0.23:5000/omega4-direct-rtp.h264`
  - `http://10.0.0.23:5000/omega4-direct-rtp-frame.png`
