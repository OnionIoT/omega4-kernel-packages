# Omega4 Raw RF Link

`omega4-linkd` is the base OpenHD-style RF transport. It puts the Wi-Fi
interface into monitor mode, sends and receives framed 802.11 action frames,
and bridges payloads to or from UDP.

## Streams

- `1`: video downlink
- `2`: MAVLink air-to-ground
- `3`: MAVLink ground-to-air
- `4`: control/status
- `255`: test payloads

## Basic Test

Use the same frequency, HT mode, and stream on both units.

Ground/RX:

```sh
omega4-linkd --mode rx --iface wlan0 --setup --freq 5180 --ht-mode HT20 \
  --stream 255 --udp-dest 127.0.0.1:5602 --verbose
```

Air/TX ping:

```sh
omega4-linkd --mode ping --iface wlan0 --setup --freq 5180 --ht-mode HT20 \
  --stream 255 --interval-ms 1000 --count 10
```

UDP payload TX:

```sh
omega4-linkd --mode tx --iface wlan0 --setup --freq 5180 --ht-mode HT20 \
  --stream 1 --udp-listen 5601
```

## FEC

Enable optional XOR FEC with `--fec DATA:PARITY`. The current implementation
supports `PARITY=1`.

Example:

```sh
omega4-linkd --mode tx --setup --stream 1 --udp-listen 5601 --fec 8:1
omega4-linkd --mode rx --setup --stream 1 --udp-dest 127.0.0.1:5600 --fec 8:1
```

`8:1` sends one parity packet for every eight data packets. It can recover one
missing RF packet per completed block and adds about 12.5% RF overhead.

## Service

The package installs `/etc/config/omega4-link` and `/etc/init.d/omega4-link`.
All sections are disabled by default. Enable only the sections matching the
board role.

```sh
uci set omega4-link.main.enabled='1'
uci set omega4-link.main.mode='rx'
uci commit omega4-link
/etc/init.d/omega4-link enable
/etc/init.d/omega4-link start
```
