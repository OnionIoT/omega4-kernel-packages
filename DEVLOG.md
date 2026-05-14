# DEVLOG

## mfg-test-tools package

- Added `utils/mfg-test-tools` as an OpenWrt utility package maintained by
  Onion.
- Added `mfg-gpio-test`, a dependency-free shell script using GPIO sysfs.
- The GPIO script can discover exportable GPIOs or operate on an explicit GPIO
  list, then runs the requested one-by-one high and all low/high/low sequence.
- Lab 3 validation found target BusyBox `sleep` rejects fractional seconds, so
  all script sleeps use integer seconds and the default delay is zero.
- Lab 3 discovery also showed some GPIO exports can fail to materialize as
  `/sys/class/gpio/gpioN`; discovery now skips those immediately.
- Lab 4 validation showed unrestricted auto-discovery can touch module-only
  board-function pins and disrupt the DUT. The package now installs an Omega4
  EVB header GPIO allowlist at `/etc/mfg-test-tools/gpio-list`; auto-discovery
  requires explicit `-a`.
- Lab 4 allowlist validation found GPIO2 is not available as an output in the
  tested image. The default allowlist excludes GPIO2 and successfully runs
  GPIO1, GPIO38-GPIO61, and GPIO76 through the full high/low/high/low sequence.
