# Omega4 Video Link

`omega4-videod` is the first video-layer helper for the Omega4 OpenHD-style
link. It uses RF stream `1` and keeps video as RTP/UDP packets so the raw link
does not need to understand codecs yet.

## Air Unit

Forward an external RTP/UDP video source into the RF link:

```sh
omega4-videod --role air --setup --freq 5180 --ht-mode HT20 \
	--udp-listen 5601 --fec 8:1
```

An encoder or camera-side process should send RTP packets to
`127.0.0.1:5601`.

Pull an RTSP camera directly with GStreamer:

```sh
omega4-videod --role air --setup --freq 5180 --ht-mode HT20 \
	--rtsp-url rtsp://192.168.1.10/stream1
```

The RTSP mode forwards the camera RTP packets into `omega4-linkd`; it does not
decode or re-encode video. RTSP input requires `gst-launch-1.0` plus the RTSP,
RTP, RTP manager, and UDP GStreamer plugins in the image. The base
`omega4-videod` package does not depend on GStreamer so plain RTP/UDP forwarding
can stay small.

## Ground Unit

Receive RF stream `1` and emit RTP/UDP:

```sh
omega4-videod --role ground --setup --freq 5180 --ht-mode HT20 \
	--udp-dest 192.168.1.100:5600 --fec 8:1
```

Point VLC, QGroundControl, or another RTP-capable receiver at UDP/RTP port
`5600` on the destination host.

## UCI

The package adds disabled `omega4-video` sections to `/etc/config/omega4-link`:

- `video_air`
- `video_ground`

Enable one side at a time with:

```sh
uci set omega4-link.video_air.enabled='1'
uci commit omega4-link
/etc/init.d/omega4-link restart
```

## Current Limits

- FEC is an optional XOR block code enabled with `--fec DATA:PARITY`; only
  `PARITY=1` is supported for now. For example, `--fec 8:1` sends one recovery
  packet for every eight data packets and can repair one lost RF packet per
  block.
- No adaptive bitrate yet.
- No RTP-aware jitter buffer on the Omega4 side. Recovered packets are emitted
  when the parity packet arrives, so downstream players should tolerate minor
  packet reordering.
- Link quality is exported by `omega4-linkd` status JSON. Service-managed video
  sections write `/tmp/omega4-link-video-air.json` or
  `/tmp/omega4-link-video-ground.json` by default.
- RSSI comes from receive radiotap `DBM_ANTSIGNAL`. The current AIC8800D driver
  exports RSSI in monitor mode but does not export antenna noise, so `snr_db`
  remains `null` until a noise or SNR source is added.
