# SPDX-License-Identifier: GPL-2.0-only

RISCV_PREFIX ?= $(RISCV_TOOLCHAIN)/riscv-none-embed-

CC := $(RISCV_PREFIX)gcc
OBJCOPY := $(RISCV_PREFIX)objcopy

SRCS := start_rv1103b_mcu.S mcu_heartbeat.c

CFLAGS := -march=rv32im -mabi=ilp32 -mcmodel=medany -Os -nostdlib -nostartfiles \
	-ffreestanding -I.
LDFLAGS_COMMON := -Wl,-T,link.lds
LDFLAGS_BARE := $(LDFLAGS_COMMON) -Wl,-Map=bare.map
LDFLAGS_ALT := $(LDFLAGS_COMMON) -Wl,-Map=alt.map

all: bare.elf bare.bin alt.elf alt.bin

bare.elf: $(SRCS) link.lds
	$(CC) $(CFLAGS) -DOMEGA4_FW_ID=0x42415245u $(LDFLAGS_BARE) -o $@ $(SRCS)

bare.bin: bare.elf
	$(OBJCOPY) -O binary $< $@

alt.elf: $(SRCS) link.lds
	$(CC) $(CFLAGS) -DOMEGA4_FW_ID=0x414C5430u $(LDFLAGS_ALT) -o $@ $(SRCS)

alt.bin: alt.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f bare.elf bare.bin bare.map alt.elf alt.bin alt.map
