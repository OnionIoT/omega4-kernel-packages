# Omega4 MAVLink Telemetry

`omega4-mavlinkd` is a transparent serial/UDP bridge for MAVLink bytes. It does
not parse, route, sign, or modify MAVLink messages.

Telemetry uses two RF streams:

- `2`: air-to-ground MAVLink
- `3`: ground-to-air MAVLink

## Air Unit

Bridge the flight-controller UART to local UDP:

```sh
omega4-mavlinkd --serial /dev/ttyS1 --baud 57600 \
  --udp-listen 14552 --udp-dest 127.0.0.1:14551
```

Send FC bytes to the ground unit:

```sh
omega4-linkd --mode tx --setup --stream 2 --udp-listen 14551
```

Receive ground-to-air bytes and write them to the local bridge:

```sh
omega4-linkd --mode rx --stream 3 --udp-dest 127.0.0.1:14552
```

## Ground Unit

Expose air-to-ground MAVLink to QGroundControl on UDP `14550`:

```sh
omega4-linkd --mode rx --setup --stream 2 --udp-dest 127.0.0.1:14550
```

Accept QGroundControl uplink packets on UDP `14551`:

```sh
omega4-linkd --mode tx --stream 3 --udp-listen 14551
```

## UCI Profiles

`/etc/config/omega4-link` includes disabled sample sections:

- `mav_air_tx`
- `mav_air_rx`
- `air_serial`
- `mav_ground_rx`
- `mav_ground_tx`

Enable the sections matching the board role and restart the service:

```sh
uci set omega4-link.mav_ground_rx.enabled='1'
uci commit omega4-link
/etc/init.d/omega4-link restart
```

## FEC

Telemetry does not enable FEC by default. If needed, add the same FEC setting
to the TX and RX sections for a stream:

```sh
uci set omega4-link.mav_air_tx.fec='8:1'
uci set omega4-link.mav_ground_rx.fec='8:1'
uci commit omega4-link
/etc/init.d/omega4-link restart
```
