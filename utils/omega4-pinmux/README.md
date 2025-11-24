# omega4-pinmux OpenWrt Package

## Overview

`omega4-pinmux` is a small helper CLI that inspects and switches pinmux on
Omega4 platforms using symbolic pin/function names.

## Build & Install (inside OpenWrt tree)

1. Copy this directory into your OpenWrt `package/` tree, e.g.

   ```sh
   cp -a omega4-pinmux <openwrt>/package/omega4-pinmux
   ```

2. From the OpenWrt root run menuconfig:

   ```
   make menuconfig
     -> Utilities
        -> [*] omega4-pinmux
   ```

3. Build the package:

   ```sh
   make package/omega4-pinmux/compile V=s
   ```

4. After flashing, on the target:

   ```sh
   root@device:~# omega4-pinmux
   gpio0_a0    : gpio          (...)
   gpio0_a1    : pwm0_ch0_m0   (...)
   ...
   ```

## Usage

```sh
omega4-pinmux                      # list EVB pins (default view)
omega4-pinmux -f|--full            # list every module pin
omega4-pinmux <pin>                # show mux for one pin
omega4-pinmux <pin> <function>     # set mux for one pin
omega4-pinmux -h|--help            # print usage / option summary
omega4-pinmux -h|--help <pin>      # list valid functions for <pin>
```

## Examples

```sh
omega4-pinmux                      # EVB subset dump
omega4-pinmux --full               # list all module pins
omega4-pinmux gpio0_a1             # read mux state for one pin
omega4-pinmux gpio0_a1 pwm0_ch0_m0 # set gpio0_a1 to PWM0_CH0_M0
omega4-pinmux -h gpio1_b3          # print every valid mux for GPIO1_B3
omega4-pinmux gpio2_a5 i2c1_sda_m1 # configure another pin/function
```

## Notes

- The default `omega4-pinmux` output shows the EVB-relevant subset; add
  `-f/--full` to dump every module pin. The flag is ignored when you specify
  a pin explicitly.
- Requires `/dev/mem` access and must be run as root.
- Only a subset of pins/functions is compiled in; extend `pinmux.c` as needed
  for your board.
