// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>

extern void clear_bss(void);

#define OMEGA4_MBOX_BASE 0x20A10000u
#define OMEGA4_MBOX_STATUS_BIT 0x1u
/*
 * Keep cache maintenance disabled by default: early access to cache
 * registers can trap if the HPMCU peripheral window isn't mapped yet.
 */
#define OMEGA4_USE_CACHE_MAINT 0
#define OMEGA4_MBOX_PING_CMD 0x4f4d4355u /* "OMCU" */
#define OMEGA4_MBOX_BOOT_CMD 0x424f4f54u /* "BOOT" */
#define OMEGA4_CACHE_BASE 0x206A0000u
#define OMEGA4_CACHE_BYPASS_BIT (1u << 6)
#ifndef OMEGA4_FW_ID
#define OMEGA4_FW_ID 0x42415245u /* "BARE" */
#endif

struct omega4_mbox_regs {
	volatile uint32_t a2b_inten;
	volatile uint32_t a2b_status;
	volatile uint32_t a2b_cmd;
	volatile uint32_t a2b_data;
	volatile uint32_t b2a_inten;
	volatile uint32_t b2a_status;
	volatile uint32_t b2a_cmd;
	volatile uint32_t b2a_data;
};

static struct omega4_mbox_regs *const omega4_mbox =
	(struct omega4_mbox_regs *)OMEGA4_MBOX_BASE;

#define OMEGA4_CACHE_LINE_SIZE 32u
#define OMEGA4_CACHE_LINE_MASK (OMEGA4_CACHE_LINE_SIZE - 1u)
#define OMEGA4_CACHE_M_VALID (1u << 0)
#define OMEGA4_CACHE_M_CLEAN 0x0u
#define OMEGA4_CACHE_M_ADDR_MASK 0xFFFFFFE0u
#define OMEGA4_CACHE_STATUS_BUSY (1u << 1)

struct omega4_cache_regs {
	volatile uint32_t cache_ctrl;
	volatile uint32_t cache_maintain[2];
	volatile uint32_t stb_timeout_ctrl;
	volatile uint32_t reserved_0x10[4];
	volatile uint32_t cache_int_en;
	volatile uint32_t cache_int_st;
	volatile uint32_t cache_err_haddr;
	volatile uint32_t reserved_0x2c;
	volatile uint32_t cache_status;
};

static struct omega4_cache_regs *const omega4_cache =
	(struct omega4_cache_regs *)OMEGA4_CACHE_BASE;

struct omega4_hpmcu_meta {
	volatile uint32_t fw_id;
	volatile uint32_t heartbeat;
};

static volatile struct omega4_hpmcu_meta omega4_hpmcu_meta
	__attribute__((section(".hpmcu_meta"))) = {
	.fw_id = OMEGA4_FW_ID,
	.heartbeat = 0,
};
void data_section_fixup(void)
{
	clear_bss();
}

static void delay(volatile uint32_t count)
{
	while (count--) {
		__asm__ volatile ("nop");
	}
}

static void omega4_mbox_send(uint32_t cmd, uint32_t data)
{
	omega4_mbox->b2a_cmd = cmd;
	omega4_mbox->b2a_data = data;
	/* Mark response ready for AP side polling. */
	omega4_mbox->b2a_status = OMEGA4_MBOX_STATUS_BIT;
}

static void omega4_cache_bypass(void)
{
#if OMEGA4_USE_CACHE_MAINT
	omega4_cache->cache_ctrl |= OMEGA4_CACHE_BYPASS_BIT;
#endif
}

static void omega4_dcache_clean(uint32_t addr, uint32_t size)
{
#if !OMEGA4_USE_CACHE_MAINT
	(void)addr;
	(void)size;
	return;
#else
	uint32_t offset;
	uint32_t value;

	if (!size)
		return;

	offset = ((addr & OMEGA4_CACHE_LINE_MASK) + size - 1) >> 5;
	value = (addr & OMEGA4_CACHE_M_ADDR_MASK) |
		OMEGA4_CACHE_M_CLEAN |
		OMEGA4_CACHE_M_VALID;

	omega4_cache->cache_maintain[1] = offset;
	omega4_cache->cache_maintain[0] = value;
	while (omega4_cache->cache_status & OMEGA4_CACHE_STATUS_BUSY) {
		;
	}
#endif
}

static void omega4_mbox_init(void)
{
	/*
	 * Enable mailbox interrupts/status with writeable bit set.
	 * Values mirror the AP-side driver expectations.
	 */
	omega4_mbox->a2b_inten = 0x01010101u;
	omega4_mbox->b2a_inten = 0x01010101u;
	omega4_mbox->a2b_status = OMEGA4_MBOX_STATUS_BIT;
	omega4_mbox->b2a_status = OMEGA4_MBOX_STATUS_BIT;
}

static void omega4_mbox_handle(void)
{
	uint32_t cmd;
	uint32_t data;

	if (!(omega4_mbox->a2b_status & OMEGA4_MBOX_STATUS_BIT))
		return;

	cmd = omega4_mbox->a2b_cmd;
	data = omega4_mbox->a2b_data;
	omega4_mbox->a2b_status = OMEGA4_MBOX_STATUS_BIT;

	if (cmd == OMEGA4_MBOX_PING_CMD)
		omega4_mbox_send(cmd, data);
}

int main(void)
{
	omega4_hpmcu_meta.fw_id = OMEGA4_FW_ID;
	omega4_hpmcu_meta.heartbeat = 0x12340000u;
	omega4_dcache_clean((uint32_t)&omega4_hpmcu_meta,
			    sizeof(omega4_hpmcu_meta));
	omega4_cache_bypass();
	omega4_mbox_init();
	omega4_mbox_send(OMEGA4_MBOX_BOOT_CMD, 0x00000001u);

	for (;;) {
		omega4_hpmcu_meta.heartbeat++;
		omega4_dcache_clean((uint32_t)&omega4_hpmcu_meta.heartbeat,
				    sizeof(omega4_hpmcu_meta.heartbeat));
		omega4_mbox_handle();
		delay(500000);
	}
}
