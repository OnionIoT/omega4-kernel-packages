# mfg-test-tools

Manufacturing test tools and scripts for Omega4 production.

## Package

Build from the OpenWrt tree:

```sh
make package/mfg-test-tools/compile V=s
```

Install path on target:

```sh
/usr/sbin/mfg-gpio-test
```

## GPIO Test

`mfg-gpio-test` drives available GPIOs high one by one, then drives all tested
GPIOs low, high, and low.

By default, the script uses `/etc/mfg-test-tools/gpio-list`, an Omega4 EVB
header GPIO allowlist. This avoids driving module-only pins that may be used
for board functions.

```sh
mfg-gpio-test              # run the configured GPIO list
mfg-gpio-test -l           # list configured GPIOs without driving them
mfg-gpio-test -d 1         # use a 1 second delay
mfg-gpio-test 32 33 34     # test an explicit GPIO list
mfg-gpio-test -a -l        # development only: scan exportable GPIOs
```

For production fixtures, update `/etc/mfg-test-tools/gpio-list` or pass
explicit GPIO arguments so only fixture-safe pins are driven.
