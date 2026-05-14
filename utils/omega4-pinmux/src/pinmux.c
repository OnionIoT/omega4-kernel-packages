#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <ctype.h>

/*
 * Omega4 pinmux helper (EVB + module pins).
 *
 * Default:
 *   pinmux                 # list all pins
 *
 * Specific pin:
 *   pinmux gpio1_b3        # show mux for that pin
 *   pinmux gpio1_b3 sdmmc0_d3
 *
 * Examples:
 *   pinmux
 *   pinmux gpio0_a1
 *   pinmux gpio0_a1 pwm0_ch0_m0
 *   pinmux gpio1_b3 sdmmc0_d3
 */

static void str_to_lower(char *s)
{
    for (; *s; ++s)
        *s = (char)tolower((unsigned char)*s);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s                      # list all pins\n"
        "  %s <pin>                # show mux for one pin\n"
        "  %s <pin> <function>     # set mux for one pin\n"
        "\n"
        "Examples:\n"
        "  %s                      # show all pins\n"
        "  %s gpio0_a1             # show current function of gpio0_a1\n"
        "  %s gpio0_a1 pwm0_ch0_m0 # set gpio0_a1 to PWM0_CH0_M0\n"
        "  %s gpio1_b3 sdmmc0_d3   # set GPIO1_B3 to SDMMC0_D3\n",
        prog, prog, prog, prog, prog, prog, prog);
}

/* ------------ pin/function description tables ------------ */

struct func_desc {
    const char *name;   /* e.g. "gpio", "pwm0_ch0_m0" */
    uint32_t    value;  /* raw field value (0..15) */
};

struct pin_desc {
    const char            *name;   /* e.g. "gpio0_a1" */
    uint32_t               addr;   /* IOmux register physical address */
    uint32_t               mask;   /* bits for this pin (e.g. 0xF0) */
    unsigned               shift;  /* shift for the field (e.g. 4) */
    const struct func_desc *funcs;
    size_t                 num_funcs;
    int                    module_pin; /* module pad/pin number (0 if N/A) */
    int                    evb_pin;    /* EVB header pin number (0 if N/A) */
};

/*
 * NOTE:
 * These tables cover all EVB-visible pins plus additional
 * module-only GPIO pins and the CLI always lists the full set.
 */

/* ---- GPIO0_A – 0x201B0000 / 0x201B0004 ---- */

static const struct func_desc gpio0_a0_funcs[] = {
    { "gpio",          0 },
    { "rtc_32k_out",   1 },
    { "clk_32k",       2 },
    { "clk_24m_out",   3 },
    { "psram_spi_clk", 4 },
    { "test_clock",    5 },
};

static const struct func_desc gpio0_a1_funcs[] = {
    { "gpio",         0 },
    { "pwm0_ch0_m0",  1 },
    { "cpu_avs",      2 },
    { "psram_spi_d1", 4 },
};

static const struct func_desc gpio0_a2_funcs[] = {
    { "gpio",         0 },
    { "pwm0_ch3_m0",  1 },
    { "psram_spi_d0", 4 },
};

/* GPIO0_A3 – PWR_CTRL_M0 (WiFi/BT power control) */
static const struct func_desc gpio0_a3_funcs[] = {
    { "gpio",        0 },  /* GPIO0_A3 */
    { "pwr_ctrl_m0", 1 },  /* PWR_CTRL_M0 */
};

/* GPIO0_A4 – PWR_CTRL_M1 / PSRAM_CS */
static const struct func_desc gpio0_a4_funcs[] = {
    { "gpio",           0 },  /* GPIO0_A4 */
    { "pwr_ctrl_m1",    1 },  /* PWR_CTRL_M1 (PWR_CTRL1) */
    { "psram_spi_cs0n", 4 },  /* PSRAM_SPI_CS0n */
};

static const struct func_desc gpio0_a5_funcs[] = {
    { "gpio",          0 },
    { "uart0_tx_m0",   1 },
    { "pwm0_ch1_m0",   2 },
    { "i2c0_scl_m0",   3 },
    { "psram_spi_d2",  4 },
    { "jtag_tck_m0",   5 },
};

static const struct func_desc gpio0_a6_funcs[] = {
    { "gpio",          0 },
    { "uart0_rx_m0",   1 },
    { "pwm0_ch2_m0",   2 },
    { "i2c0_sda_m0",   3 },
    { "psram_spi_d3",  4 },
    { "jtag_tms_m0",   5 },
};

/* ---- GPIO1_A – 0x20180024 ---- */

static const struct func_desc gpio1_a6_funcs[] = {
    { "gpio",       0 },
    { "sdmmc0_det", 1 },
};

static const struct func_desc gpio1_a7_funcs[] = {
    { "gpio",           0 },
    { "sdmmc0_d1",      1 },
    { "uart1_rx_m1",    2 },
    { "emmc_testclk",   3 },  /* eMMC test clock out */
    { "pwm2_ch1_m0",    4 },
    { "fspi_testclk",   5 },
};

/* ---- GPIO1_B – 0x20180028 / 0x2018002C ---- */

static const struct func_desc gpio1_b0_funcs[] = {
    { "gpio",              0 },
    { "sdmmc0_d0",         1 },
    { "uart1_tx_m1",       2 },
    { "emmc_testdata_out", 3 },
    { "pwm2_ch0_m0",       4 },
    { "fspi_testdata_out", 5 },
};

static const struct func_desc gpio1_b1_funcs[] = {
    { "gpio",         0 },
    { "sdmmc0_clk",   1 },
    { "uart1_rtsn_m1",2 },
};

static const struct func_desc gpio1_b2_funcs[] = {
    { "gpio",         0 },
    { "sdmmc0_cmd",   1 },
    { "uart1_ctsn_m1",2 },
};

static const struct func_desc gpio1_b3_funcs[] = {
    { "gpio",        0 },
    { "sdmmc0_d3",   1 },
    { "uart0_rx_m2", 2 },
    { "jtag_tms_m2", 3 },
    { "pwm2_ch3_m0", 4 },
    { "i2c0_sda_m1", 5 },
};

static const struct func_desc gpio1_b4_funcs[] = {
    { "gpio",        0 },
    { "sdmmc0_d2",   1 },
    { "uart0_tx_m2", 2 },
    { "jtag_tck_m2", 3 },
    { "pwm2_ch2_m0", 4 },
    { "i2c0_scl_m1", 5 },
};

static const struct func_desc gpio1_b5_funcs[] = {
    { "gpio",         0 },
    { "cam_clk0_out", 1 },
    { "i2c0_scl_m2",  2 },
    { "uart2_tx_m2",  3 },
};

static const struct func_desc gpio1_b6_funcs[] = {
    { "gpio",         0 },
    { "cam_clk1_out", 1 },
    { "i2c0_sda_m2",  2 },
    { "uart2_rx_m2",  3 },
};

static const struct func_desc gpio1_b7_funcs[] = {
    { "gpio",          0 },
    { "pwm0_ch1_m2",   1 },
    { "i2c4_scl_m1",   2 },
    { "uart2_rtsn_m2", 3 },
};

/* ---- GPIO1_C – 0x20180030 / 0x20180034 ---- */

static const struct func_desc gpio1_c0_funcs[] = {
    { "gpio",          0 },
    { "pwm0_ch2_m2",   1 },
    { "i2c4_sda_m1",   2 },
    { "uart2_ctsn_m2", 3 },
};

/* CSI group: simplified to gpio + mipi_csi_* */

static const struct func_desc gpio1_c1_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d3n", 1 },
};

static const struct func_desc gpio1_c2_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d3p", 1 },
};

static const struct func_desc gpio1_c3_funcs[] = {
    { "gpio",          0 },
    { "mipi_csi_ck1n", 1 },
};

static const struct func_desc gpio1_c4_funcs[] = {
    { "gpio",          0 },
    { "mipi_csi_ck1p", 1 },
};

static const struct func_desc gpio1_c5_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d2n", 1 },
};

static const struct func_desc gpio1_c6_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d2p", 1 },
};

/* ---- GPIO1_D – 0x20180038 / 0x2018003C ---- */

static const struct func_desc gpio1_d0_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d1n", 1 },
};

static const struct func_desc gpio1_d1_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d1p", 1 },
};

static const struct func_desc gpio1_d2_funcs[] = {
    { "gpio",          0 },
    { "mipi_csi_ck0n", 1 },
};

static const struct func_desc gpio1_d3_funcs[] = {
    { "gpio",          0 },
    { "mipi_csi_ck0p", 1 },
};

static const struct func_desc gpio1_d4_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d0n", 1 },
};

static const struct func_desc gpio1_d5_funcs[] = {
    { "gpio",         0 },
    { "mipi_csi_d0p", 1 },
};

/* ---- GPIO2_B – 0x201A0044 / 0x201A0048 ---- */

static const struct func_desc gpio2_b0_funcs[] = {
    { "gpio",        0 },
    { "uart2_tx_m1", 1 },
};

static const struct func_desc gpio2_b1_funcs[] = {
    { "gpio",        0 },
    { "uart2_rx_m1", 1 },
};

static const struct func_desc gpio2_b4_funcs[] = {
    { "gpio",       0 },
    { "saradc_in0", 1 },  /* recovery key / analog in */
};

/* ------------ Pin table (module pins) ------------ */

static const struct pin_desc pin_table[] = {
    /* GPIO0_A – 0x201B0000 / 0x201B0004 */
    {
        .name      = "gpio0_a0",
        .addr      = 0x201B0000u,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio0_a0_funcs,
        .num_funcs = sizeof(gpio0_a0_funcs)/sizeof(gpio0_a0_funcs[0]),
    },
    {
        .name      = "gpio0_a1",
        .addr      = 0x201B0000u,
        .mask      = 0x000000F0u,
        .shift     = 4,
        .funcs     = gpio0_a1_funcs,
        .num_funcs = sizeof(gpio0_a1_funcs)/sizeof(gpio0_a1_funcs[0]),
        .module_pin = 41,
        .evb_pin    = 4,
    },
    {
        .name      = "gpio0_a2",
        .addr      = 0x201B0000u,
        .mask      = 0x00000F00u,
        .shift     = 8,
        .funcs     = gpio0_a2_funcs,
        .num_funcs = sizeof(gpio0_a2_funcs)/sizeof(gpio0_a2_funcs[0]),
        .module_pin = 42,
        .evb_pin    = 3,
    },
    {
        .name      = "gpio0_a3",
        .addr      = 0x201B0000u,
        .mask      = 0x0000F000u,   /* [15:12] */
        .shift     = 12,
        .funcs     = gpio0_a3_funcs,
        .num_funcs = sizeof(gpio0_a3_funcs)/sizeof(gpio0_a3_funcs[0]),
        .module_pin = 43,
    },
    {
        .name      = "gpio0_a4",
        .addr      = 0x201B0004u,
        .mask      = 0x0000000Fu,   /* [3:0] */
        .shift     = 0,
        .funcs     = gpio0_a4_funcs,
        .num_funcs = sizeof(gpio0_a4_funcs)/sizeof(gpio0_a4_funcs[0]),
        .module_pin = 44,
    },
    {
        .name      = "gpio0_a5",
        .addr      = 0x201B0004u,
        .mask      = 0x000000F0u,
        .shift     = 4,
        .funcs     = gpio0_a5_funcs,
        .num_funcs = sizeof(gpio0_a5_funcs)/sizeof(gpio0_a5_funcs[0]),
        .module_pin = 45,
    },
    {
        .name      = "gpio0_a6",
        .addr      = 0x201B0004u,
        .mask      = 0x00000F00u,
        .shift     = 8,
        .funcs     = gpio0_a6_funcs,
        .num_funcs = sizeof(gpio0_a6_funcs)/sizeof(gpio0_a6_funcs[0]),
        .module_pin = 46,
    },

    /* GPIO1_A – 0x20180024 */
    {
        .name      = "gpio1_a6",
        .addr      = 0x20180024u,
        .mask      = 0x00000F00u,   /* [11:8] */
        .shift     = 8,
        .funcs     = gpio1_a6_funcs,
        .num_funcs = sizeof(gpio1_a6_funcs)/sizeof(gpio1_a6_funcs[0]),
        .module_pin = 7,
        .evb_pin    = 37,
    },
    {
        .name      = "gpio1_a7",
        .addr      = 0x20180024u,
        .mask      = 0x0000F000u,   /* [15:12] */
        .shift     = 12,
        .funcs     = gpio1_a7_funcs,
        .num_funcs = sizeof(gpio1_a7_funcs)/sizeof(gpio1_a7_funcs[0]),
        .module_pin = 8,
        .evb_pin    = 38,
    },

    /* GPIO1_B – 0x20180028 / 0x2018002C */
    {
        .name      = "gpio1_b0",
        .addr      = 0x20180028u,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio1_b0_funcs,
        .num_funcs = sizeof(gpio1_b0_funcs)/sizeof(gpio1_b0_funcs[0]),
        .module_pin = 9,
        .evb_pin    = 39,
    },
    {
        .name      = "gpio1_b1",
        .addr      = 0x20180028u,
        .mask      = 0x000000F0u,
        .shift     = 4,
        .funcs     = gpio1_b1_funcs,
        .num_funcs = sizeof(gpio1_b1_funcs)/sizeof(gpio1_b1_funcs[0]),
        .module_pin = 11,
        .evb_pin    = 41,
    },
    {
        .name      = "gpio1_b2",
        .addr      = 0x20180028u,
        .mask      = 0x00000F00u,
        .shift     = 8,
        .funcs     = gpio1_b2_funcs,
        .num_funcs = sizeof(gpio1_b2_funcs)/sizeof(gpio1_b2_funcs[0]),
        .module_pin = 10,
        .evb_pin    = 40,
    },
    {
        .name      = "gpio1_b3",
        .addr      = 0x20180028u,
        .mask      = 0x0000F000u,
        .shift     = 12,
        .funcs     = gpio1_b3_funcs,
        .num_funcs = sizeof(gpio1_b3_funcs)/sizeof(gpio1_b3_funcs[0]),
        .module_pin = 12,
        .evb_pin    = 42,
    },
    {
        .name      = "gpio1_b4",
        .addr      = 0x2018002Cu,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio1_b4_funcs,
        .num_funcs = sizeof(gpio1_b4_funcs)/sizeof(gpio1_b4_funcs[0]),
        .module_pin = 13,
        .evb_pin    = 43,
    },
    {
        .name      = "gpio1_b5",
        .addr      = 0x2018002Cu,
        .mask      = 0x000000F0u,
        .shift     = 4,
        .funcs     = gpio1_b5_funcs,
        .num_funcs = sizeof(gpio1_b5_funcs)/sizeof(gpio1_b5_funcs[0]),
        .module_pin = 1,
        .evb_pin    = 36,
    },
    {
        .name      = "gpio1_b6",
        .addr      = 0x2018002Cu,
        .mask      = 0x00000F00u,
        .shift     = 8,
        .funcs     = gpio1_b6_funcs,
        .num_funcs = sizeof(gpio1_b6_funcs)/sizeof(gpio1_b6_funcs[0]),
        .module_pin = 91,
        .evb_pin    = 35,
    },
    {
        .name      = "gpio1_b7",
        .addr      = 0x2018002Cu,
        .mask      = 0x0000F000u,
        .shift     = 12,
        .funcs     = gpio1_b7_funcs,
        .num_funcs = sizeof(gpio1_b7_funcs)/sizeof(gpio1_b7_funcs[0]),
        .module_pin = 90,
        .evb_pin    = 34,
    },

    /* GPIO1_C – 0x20180030 / 0x20180034 */
    {
        .name      = "gpio1_c0",
        .addr      = 0x20180030u,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio1_c0_funcs,
        .num_funcs = sizeof(gpio1_c0_funcs)/sizeof(gpio1_c0_funcs[0]),
        .module_pin = 89,
        .evb_pin    = 33,
    },
    {
        .name      = "gpio1_c1",
        .addr      = 0x20180030u,
        .mask      = 0x000000F0u,  /* [7:4] */
        .shift     = 4,
        .funcs     = gpio1_c1_funcs,
        .num_funcs = sizeof(gpio1_c1_funcs)/sizeof(gpio1_c1_funcs[0]),
        .module_pin = 71,
        .evb_pin    = 19,
    },
    {
        .name      = "gpio1_c2",
        .addr      = 0x20180030u,
        .mask      = 0x00000F00u,  /* [11:8] */
        .shift     = 8,
        .funcs     = gpio1_c2_funcs,
        .num_funcs = sizeof(gpio1_c2_funcs)/sizeof(gpio1_c2_funcs[0]),
        .module_pin = 72,
        .evb_pin    = 20,
    },
    {
        .name      = "gpio1_c3",
        .addr      = 0x20180030u,
        .mask      = 0x0000F000u,  /* [15:12] */
        .shift     = 12,
        .funcs     = gpio1_c3_funcs,
        .num_funcs = sizeof(gpio1_c3_funcs)/sizeof(gpio1_c3_funcs[0]),
        .module_pin = 74,
        .evb_pin    = 21,
    },
    {
        .name      = "gpio1_c4",
        .addr      = 0x20180034u,
        .mask      = 0x0000000Fu,  /* [3:0] */
        .shift     = 0,
        .funcs     = gpio1_c4_funcs,
        .num_funcs = sizeof(gpio1_c4_funcs)/sizeof(gpio1_c4_funcs[0]),
        .module_pin = 75,
        .evb_pin    = 22,
    },
    {
        .name      = "gpio1_c5",
        .addr      = 0x20180034u,
        .mask      = 0x000000F0u,  /* [7:4] */
        .shift     = 4,
        .funcs     = gpio1_c5_funcs,
        .num_funcs = sizeof(gpio1_c5_funcs)/sizeof(gpio1_c5_funcs[0]),
        .module_pin = 77,
        .evb_pin    = 23,
    },
    {
        .name      = "gpio1_c6",
        .addr      = 0x20180034u,
        .mask      = 0x00000F00u,  /* [11:8] */
        .shift     = 8,
        .funcs     = gpio1_c6_funcs,
        .num_funcs = sizeof(gpio1_c6_funcs)/sizeof(gpio1_c6_funcs[0]),
        .module_pin = 78,
        .evb_pin    = 24,
    },

    /* GPIO1_D – 0x20180038 / 0x2018003C */
    {
        .name      = "gpio1_d0",
        .addr      = 0x20180038u,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio1_d0_funcs,
        .num_funcs = sizeof(gpio1_d0_funcs)/sizeof(gpio1_d0_funcs[0]),
        .module_pin = 80,
        .evb_pin    = 25,
    },
    {
        .name      = "gpio1_d1",
        .addr      = 0x20180038u,
        .mask      = 0x000000F0u,
        .shift     = 4,
        .funcs     = gpio1_d1_funcs,
        .num_funcs = sizeof(gpio1_d1_funcs)/sizeof(gpio1_d1_funcs[0]),
        .module_pin = 81,
        .evb_pin    = 26,
    },
    {
        .name      = "gpio1_d2",
        .addr      = 0x20180038u,
        .mask      = 0x00000F00u,
        .shift     = 8,
        .funcs     = gpio1_d2_funcs,
        .num_funcs = sizeof(gpio1_d2_funcs)/sizeof(gpio1_d2_funcs[0]),
        .module_pin = 83,
        .evb_pin    = 27,
    },
    {
        .name      = "gpio1_d3",
        .addr      = 0x20180038u,
        .mask      = 0x0000F000u,
        .shift     = 12,
        .funcs     = gpio1_d3_funcs,
        .num_funcs = sizeof(gpio1_d3_funcs)/sizeof(gpio1_d3_funcs[0]),
        .module_pin = 84,
        .evb_pin    = 28,
    },
    {
        .name      = "gpio1_d4",
        .addr      = 0x2018003Cu,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio1_d4_funcs,
        .num_funcs = sizeof(gpio1_d4_funcs)/sizeof(gpio1_d4_funcs[0]),
        .module_pin = 86,
        .evb_pin    = 29,
    },
    {
        .name      = "gpio1_d5",
        .addr      = 0x2018003Cu,
        .mask      = 0x000000F0u,
        .shift     = 4,
        .funcs     = gpio1_d5_funcs,
        .num_funcs = sizeof(gpio1_d5_funcs)/sizeof(gpio1_d5_funcs[0]),
        .module_pin = 87,
        .evb_pin    = 30,
    },

    /* GPIO2_B – 0x201A0044 / 0x201A0048 */
    {
        .name      = "gpio2_b0",
        .addr      = 0x201A0048u,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio2_b0_funcs,
        .num_funcs = sizeof(gpio2_b0_funcs)/sizeof(gpio2_b0_funcs[0]),
    },
    {
        .name      = "gpio2_b1",
        .addr      = 0x201A0048u,
        .mask      = 0x000000F0u,
        .shift     = 4,
        .funcs     = gpio2_b1_funcs,
        .num_funcs = sizeof(gpio2_b1_funcs)/sizeof(gpio2_b1_funcs[0]),
    },
    {
        .name      = "gpio2_b4",
        .addr      = 0x201A004Cu,
        .mask      = 0x0000000Fu,
        .shift     = 0,
        .funcs     = gpio2_b4_funcs,
        .num_funcs = sizeof(gpio2_b4_funcs)/sizeof(gpio2_b4_funcs[0]),
        .module_pin = 63,
        .evb_pin    = 8,
    },
};

static const size_t pin_table_count =
    sizeof(pin_table) / sizeof(pin_table[0]);

/* ------------ /dev/mem access ------------ */

struct mem_map {
    int      fd;
    void    *map;
    size_t   size;
    off_t    base;
};

static int mem_map_open(struct mem_map *m, uint32_t addr)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return -1;
    }

    uint32_t page_mask = (uint32_t)(page_size - 1);
    off_t base = (off_t)(addr & ~page_mask);

    m->fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (m->fd < 0) {
        perror("open(/dev/mem)");
        fprintf(stderr,
                "Hint: run as root and make sure CONFIG_DEVMEM is enabled.\n");
        return -1;
    }

    m->size = (size_t)page_size;
    m->base = base;
    m->map = mmap(NULL, m->size, PROT_READ | PROT_WRITE,
                  MAP_SHARED, m->fd, base);
    if (m->map == MAP_FAILED) {
        perror("mmap");
        close(m->fd);
        m->fd = -1;
        return -1;
    }

    return 0;
}

static void mem_map_close(struct mem_map *m)
{
    if (m->map && m->map != MAP_FAILED) {
        munmap(m->map, m->size);
    }
    if (m->fd >= 0) {
        close(m->fd);
    }
    m->map = NULL;
    m->fd = -1;
}

static int reg_read32(uint32_t addr, uint32_t *out)
{
    struct mem_map m = {0};
    if (mem_map_open(&m, addr) < 0)
        return -1;

    long page_size = sysconf(_SC_PAGESIZE);
    uint32_t page_mask = (uint32_t)(page_size - 1);
    off_t offset = (off_t)(addr & page_mask);

    volatile uint32_t *reg =
        (volatile uint32_t *)((uint8_t *)m.map + offset);
    *out = *reg;

    mem_map_close(&m);
    return 0;
}

static int reg_rmw32(uint32_t addr, uint32_t mask, uint32_t value,
                     uint32_t *old_out, uint32_t *new_out)
{
    struct mem_map m = {0};
    if (mem_map_open(&m, addr) < 0)
        return -1;

    long page_size = sysconf(_SC_PAGESIZE);
    uint32_t page_mask = (uint32_t)(page_size - 1);
    off_t offset = (off_t)(addr & page_mask);

    volatile uint32_t *reg =
        (volatile uint32_t *)((uint8_t *)m.map + offset);

    uint32_t masked_value = value & mask;
    uint32_t old = *reg;
    uint32_t newv = (old & ~mask) | masked_value;

    if ((mask & 0xFFFF0000u) == 0) {
        /* Rockchip IO mux regs use hi-word write semantics */
        uint32_t hiword_mask = mask << 16;
        *reg = hiword_mask | masked_value;
    } else {
        *reg = newv;
    }

    if (old_out) *old_out = old;
    if (new_out) *new_out = newv;

    mem_map_close(&m);
    return 0;
}

/* ------------ lookup & print helpers ------------ */

static const struct pin_desc *find_pin(const char *name)
{
    for (size_t i = 0; i < pin_table_count; ++i) {
        if (strcmp(pin_table[i].name, name) == 0)
            return &pin_table[i];
    }
    return NULL;
}

static const struct func_desc *find_func(const struct pin_desc *pin,
                                         const char *func_name)
{
    for (size_t i = 0; i < pin->num_funcs; ++i) {
        if (strcmp(pin->funcs[i].name, func_name) == 0)
            return &pin->funcs[i];
    }
    return NULL;
}

static const struct func_desc *func_from_field(const struct pin_desc *pin,
                                               uint32_t field_val)
{
    for (size_t i = 0; i < pin->num_funcs; ++i) {
        if (pin->funcs[i].value == field_val)
            return &pin->funcs[i];
    }
    return NULL;
}

static void format_pin_index(int value, char *buf, size_t len)
{
    if (!buf || len == 0)
        return;
    if (value > 0) {
        snprintf(buf, len, "%d", value);
    } else {
        snprintf(buf, len, "--");
    }
}

static int print_pin_status(const struct pin_desc *pin)
{
    uint32_t reg;
    if (reg_read32(pin->addr, &reg) < 0) {
        fprintf(stderr, "Failed to read IOmux register 0x%08X for %s\n",
                pin->addr, pin->name);
        return -1;
    }

    uint32_t field = (reg & pin->mask) >> pin->shift;
    const struct func_desc *f = func_from_field(pin, field);

    char module_buf[8];
    char evb_buf[8];
    format_pin_index(pin->module_pin, module_buf, sizeof(module_buf));
    format_pin_index(pin->evb_pin, evb_buf, sizeof(evb_buf));

    if (f) {
        printf("%-9s (module %3s / evb %3s) : %s\n",
               pin->name, module_buf, evb_buf, f->name);
    } else {
        printf("%-9s (module %3s / evb %3s) : UNKNOWN\n",
               pin->name, module_buf, evb_buf);
    }

    return 0;
}

static void list_pins(void)
{
    for (size_t i = 0; i < pin_table_count; ++i)
        (void)print_pin_status(&pin_table[i]);
}

/* ------------ main ------------ */

int main(int argc, char **argv)
{
    int help = 0;
    int argi = 1;

    /* Handle -h / --help */
    if (argc > argi &&
        (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0)) {
        help = 1;
        argi++;
    }

    int remaining = argc - argi;

    /* help with no pin: just print general usage */
    if (help && remaining == 0) {
        usage(argv[0]);
        return 0;
    }

    /* help with specific pin: list all muxing options */
    if (help && remaining == 1) {
        char *pin_name = argv[argi];
        str_to_lower(pin_name);

        const struct pin_desc *pin = find_pin(pin_name);
        if (!pin) {
            fprintf(stderr, "Unknown pin '%s'\n", pin_name);
            return 1;
        }

        printf("Pin %s supports the following mux functions:\n", pin_name);
        for (size_t i = 0; i < pin->num_funcs; ++i)
            printf("  %-16s (%u)\n", pin->funcs[i].name, pin->funcs[i].value);

        return 0;
    }

    /* Normal behavior ======================================= */

    if (remaining == 0) {
        /* No pin/function: list all pins */
        list_pins();
        return 0;
    }

    if (remaining == 1) {
        /* Single pin: show pin status */
        char *pin_name = argv[argi];
        str_to_lower(pin_name);

        const struct pin_desc *pin = find_pin(pin_name);
        if (!pin) {
            fprintf(stderr, "Unknown pin '%s'\n", pin_name);
            return 1;
        }
        return print_pin_status(pin);
    }

    if (remaining == 2) {
        /* Pin + function: set mux */
        char *pin_name  = argv[argi];
        char *func_name = argv[argi + 1];

        str_to_lower(pin_name);
        str_to_lower(func_name);

        const struct pin_desc *pin = find_pin(pin_name);
        if (!pin) {
            fprintf(stderr, "Unknown pin '%s'\n", pin_name);
            return 1;
        }

        const struct func_desc *func = find_func(pin, func_name);
        if (!func) {
            fprintf(stderr,
                    "Unknown function '%s' for pin '%s'\n",
                    func_name, pin_name);

            printf("Valid functions for %s:\n", pin_name);
            for (size_t i = 0; i < pin->num_funcs; ++i)
                printf("  %s\n", pin->funcs[i].name);
            return 1;
        }

        uint32_t field_val  = func->value << pin->shift;
        uint32_t field_mask = pin->mask;
        uint32_t oldv, newv;

        if (reg_rmw32(pin->addr, field_mask, field_val, &oldv, &newv) < 0) {
            fprintf(stderr, "Failed to update register 0x%08X for %s\n",
                    pin->addr, pin_name);
            return 1;
        }

        printf("Pin %s set to %s\n", pin_name, func_name);
        printf("  addr: 0x%08X\n", pin->addr);
        printf("  mask: 0x%08X\n", field_mask);
        printf("  old : 0x%08X\n", oldv);
        printf("  new : 0x%08X\n", newv);

        return 0;
    }

    usage(argv[0]);
    return 1;
}
