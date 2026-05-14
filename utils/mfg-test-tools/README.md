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
/usr/sbin/mfg-aic-mac
/usr/sbin/mfg-aic-uart
/usr/share/mfg-test-tools/aic8800d80-mfg-testmode-uart.bin
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

## AIC WiFi/BT MAC Programming

`mfg-aic-mac` programs the built-in AIC8800D80 WiFi and Bluetooth MAC
addresses for manufacturing.

The UART path uses Omega4 UART2 on GPIO2_B0/GPIO2_B1 at 921600 8N1 and the AIC
reset line on GPIO0_A3, Linux GPIO 3. The tool uses `omega4-pinmux` to
temporarily mux GPIO2_B0/GPIO2_B1 to UART2 at runtime so SPI0 can keep its DTS
configuration for normal boot. `load` detaches the AIC SDIO driver, resets the
AIC into Boot ROM, sends the test firmware with XMODEM-1K, and jumps to the
documented load address:

```sh
mfg-aic-mac load
```

Read current values from the loaded UART test firmware:

```sh
mfg-aic-mac show
```

Program and verify both MAC addresses through the loaded UART test firmware.
Pass the WiFi/base MAC address; the tool programs BT as base + 2:

```sh
mfg-aic-mac set --mac 0a1c11223344 --yes
```

The AIC UART `setmac` and `setbtmac` commands program efuse slots. The read
commands report the remaining write count as `(remain:N)`. Do not run
`set` for temporary testing or restoration unless those addresses are final
for the module. When `(remain:0)` is reported, the AIC test firmware rejects
additional writes with `no room`.

The lower-level UART helper is also installed for debug:

```sh
mfg-aic-uart probe getmac
mfg-aic-uart load -a 160000 /usr/share/mfg-test-tools/aic8800d80-mfg-testmode-uart.bin
```

The default firmware path is:

```sh
/usr/share/mfg-test-tools/aic8800d80-mfg-testmode-uart.bin
```

Override it when the firmware is stored elsewhere:

```sh
mfg-aic-mac load --fw /path/to/testmode.bin
```

AIC's UART procedure uses 921600 8N1, loads the test firmware with XMODEM using
`x 160000`, starts it with `g 160000`, then runs `setmac`, `getmac`,
`setbtmac`, and `getbtmac`. The `setmac` and `setbtmac` commands are efuse
writes.

Only program finalized, unique production MAC addresses. Lab validation showed
that `setmac` and `setbtmac` consume the AIC efuse write slots; after the write
count reaches zero, the original value cannot be restored with this firmware.
