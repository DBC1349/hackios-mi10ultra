/* Xiaomi Mi 10 PCIe0 bootstrap. Register values and PHY data are from the
 * merged UMI stock DT and Kona register definitions. This diagnostic stops
 * with QCA6390 memory decoding enabled and bus mastering disabled.
 */
#include <sys/umi_pcie.h>
#include <sys/errno.h>
#include "umi_pcie_phy.h"

#define GCC  0x00100000u
#define GPIO 0x0f900000u /* Kona NORTH tile: TLMM base + 0x900000. */
#define PARF 0x01c00000u
#define PHY  0x01c06000u
#define DBI  0x60000000u
#define ATU  0x60001000u
#define CONF 0x60100000u
#define BAR  0x60300000u
#define BIT(n) (1u << (n))
#define TRY(expr) do { error = (expr); if (error) goto fail; } while (0)

static uint32_t rd(struct umi_pcie *p, uint32_t addr)
{
	return p->io->read32(p->io->context, addr);
}
static void wr(struct umi_pcie *p, uint32_t addr, uint32_t value)
{
	p->io->write32(p->io->context, addr, value);
}
static void pause_us(struct umi_pcie *p, unsigned us)
{
	p->io->delay_us(p->io->context, us);
}
static void report(struct umi_pcie *p, const char *name, uint32_t value)
{
	if (p->io->report) p->io->report(p->io->context, name, value);
}
static int update(struct umi_pcie *p, uint32_t addr, uint32_t clear, uint32_t set)
{
	uint32_t value = rd(p, addr);
	if (value == UINT32_MAX) return EIO;
	wr(p, addr, (value & ~clear) | set);
	return 0;
}
static int wait_bits(struct umi_pcie *p, uint32_t addr, uint32_t mask,
    uint32_t wanted, unsigned timeout_us)
{
	uint32_t value = 0;
	for (unsigned us = 0; us < timeout_us; us += 10) {
		value = rd(p, addr);
		if (value == UINT32_MAX) return EIO;
		if ((value & mask) == wanted) return 0;
		pause_us(p, 10);
	}
	report(p, "poll timeout address", addr);
	report(p, "poll timeout value", value);
	return ETIMEDOUT;
}
static int gpio_output(struct umi_pcie *p, unsigned pin, bool high, uint32_t config)
{
	/* Set output latch before output-enable/mux to avoid a reset glitch. */
	int error = update(p, GPIO + pin * 0x1000 + 4, BIT(1), high ? BIT(1) : 0);
	if (error) return error;
	return update(p, GPIO + pin * 0x1000, 0x3ff, config | BIT(9));
}
static int source_xo(struct umi_pcie *p, uint32_t cmd)
{
	/* 19.2 MHz XO, divide by 1. This is the stock Gen1/Gen2 refgen rate. */
	int error = update(p, cmd + 4, 0x371f, 1);
	if (error) return error;
	error = update(p, cmd, 0, 1);
	return error ? error : wait_bits(p, cmd, 1, 0, 10000);
}
static int branch(struct umi_pcie *p, uint32_t offset)
{
	int error = update(p, GCC + offset, 0, 1);
	return error ? error : wait_bits(p, GCC + offset, BIT(31), 0, 10000);
}
static int clocks(struct umi_pcie *p)
{
	int error;
	/* GDSC status is checked before accessing the PCIe controller/PHY. */
	TRY(update(p, GCC + 0x6b004, 7, 0));
	pause_us(p, 1);
	TRY(wait_bits(p, GCC + 0x6b004, BIT(31), BIT(31), 10000));
	TRY(source_xo(p, GCC + 0x6b038));
	TRY(source_xo(p, GCC + 0x6f014));
	TRY(branch(p, 0x8c004)); /* QCA6390 reference clock/LDO. */
	TRY(branch(p, 0x6f02c));
	TRY(branch(p, 0x9000c));
	TRY(branch(p, 0x8d058));
	/* The AXI and PIPE clocks must not retain memory/peripheral state. */
	TRY(update(p, GCC + 0x6b01c, 0x6000, 0));
	/* Force diagnostic AHB/SLV clocks on. In HWCG mode the halt bit can
	 * assert merely because the bus is idle, so it is not a readiness test. */
	TRY(update(p, GCC + 0x6b014, 0x6002, 0));
	TRY(update(p, GCC + 0x6b024, BIT(1), 0));
	TRY(update(p, GCC + 0x6b02c, 0x6000, 0));
	TRY(update(p, GCC + 0x52008, 0, 0x2f)); /* All PCIe0 votes except PIPE. */
	static const uint32_t gates[] = {0x6b010, 0x6b014, 0x6b01c, 0x6b024, 0x6b028};
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++)
		TRY(wait_bits(p, GCC + gates[i], BIT(31), 0, 10000));
	static const uint32_t resets[] = {0x6b000, 0x6c01c};
	for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); i++) {
		TRY(update(p, GCC + resets[i], 0, 1));
		pause_us(p, 1000);
		TRY(update(p, GCC + resets[i], 1, 0));
	}
	return 0;
fail:
	return error;
}
static int outbound(struct umi_pcie *p, unsigned index, uint32_t type,
    uint32_t start, uint32_t end, uint32_t target)
{
	uint32_t a = ATU + index * 0x200;
	wr(p, a + 4, 0);
	wr(p, a, type);
	wr(p, a + 8, start);
	wr(p, a + 12, 0);
	wr(p, a + 16, end);
	wr(p, a + 20, target);
	wr(p, a + 24, 0);
	wr(p, a + 4, BIT(31));
	return wait_bits(p, a + 4, BIT(31), BIT(31), 1000);
}

void umi_pcie_quiesce(struct umi_pcie *p)
{
	if (!p || !p->io) return;
	if (p->msi_cap) {
		uint32_t cap = rd(p, CONF + p->msi_cap);
		if (cap != UINT32_MAX) wr(p, CONF + p->msi_cap, cap & ~BIT(16));
		p->msi_cap = 0;
	}
	/* Stop protected firmware DMA before asserting reset. Enumeration alone
	 * never enables bus mastering. Shared power/clock votes remain held. */
	if (p->dma_enabled) {
		uint32_t command = rd(p, CONF + 4);
		if (command != UINT32_MAX) wr(p, CONF + 4, (command & 0xffff) & ~4u);
		wr(p, DBI + 4, 2);
		p->dma_enabled = false;
	}
	(void)gpio_output(p, 79, false, 1);
	pause_us(p, 1000); /* PERST propagation before the SMMU can be restored. */
	(void)gpio_output(p, 20, false, 3 | (7u << 6));
	p->enumerated = false;
}

int umi_pcie_enumerate(struct umi_pcie *p, const struct umi_pcie_io *io)
{
	if (!p || !io || !io->read32 || !io->write32 || !io->power_on || !io->delay_us)
		return EINVAL;
	if (p->io) return EBUSY;
	p->io = io;
	int error;
	TRY(gpio_output(p, 79, false, 1));
	pause_us(p, 1000);
	TRY(io->power_on(io->context));
	/* UMI stock pinctrl: CLKREQ mux1/pull-up; WAKE input/pull-up. */
	TRY(update(p, GPIO + 80 * 0x1000, 0x3ff, 7));
	TRY(update(p, GPIO + 81 * 0x1000, 0x3ff, 3));
	TRY(gpio_output(p, 20, true, 3 | (7u << 6)));
	pause_us(p, 1000);
	TRY(clocks(p));
	report(p, "power and clocks ready", rd(p, GCC + 0x6b004));
	wr(p, PARF + 0x1000, 4); /* Root complex. */
	TRY(update(p, PARF + 0x40, 1, 0));
	wr(p, PARF + 0x350, 0);
	wr(p, PARF, 0x365e);
	TRY(update(p, PARF + 0x174, 0, BIT(4)));
	wr(p, PARF + 0x22c, 0); /* No unhandled PCIe interrupts during enumeration. */
	wr(p, PARF + 0x358, 0x04000000);
	TRY(update(p, PARF + 0x1a8, 0, BIT(31)));
	for (unsigned i = 0; i < sizeof(umi_pcie_phy) / sizeof(umi_pcie_phy[0]); i++) {
		wr(p, PHY + umi_pcie_phy[i].offset, umi_pcie_phy[i].value);
		if (umi_pcie_phy[i].delay_us) pause_us(p, umi_pcie_phy[i].delay_us);
	}
	pause_us(p, 1000);
	TRY(update(p, GCC + 0x52008, 0, BIT(4))); /* PIPE: vendor skips halt polling. */
	TRY(wait_bits(p, PHY + 0x814, BIT(6), 0, 100000));
	report(p, "PHY ready", rd(p, PHY + 0x814));
	pause_us(p, 10000); /* UMI DT ep-latency=10 ms. */
	TRY(gpio_output(p, 79, true, 1));
	pause_us(p, 1000);
	TRY(update(p, DBI + 0x80c, 0x1f00, 0x100));
	wr(p, DBI + 0x8a8, 0x20);
	TRY(update(p, DBI + 0x890, BIT(0), 0));
	TRY(update(p, DBI + 0x8bc, 0, BIT(0)));
	wr(p, DBI + 0x154, 0x55555555);
	TRY(update(p, DBI + 0x8bc, BIT(0), 0));
	TRY(update(p, DBI + 0xa0, 0xf, 2)); /* Gen2 x1: stock UMI link rate. */
	/* Write only the control half of the PCIe capability (upper half is W1C). */
	uint32_t control = rd(p, DBI + 0x78);
	if (control == UINT32_MAX) { error = EIO; goto fail; }
	wr(p, DBI + 0x78, (control & 0xffff) & ~0x7000u);
	TRY(update(p, PARF + 0x1b0, 0, BIT(8)));
	TRY(wait_bits(p, DBI + 0xf28, BIT(10), BIT(10), 100000));
	TRY(wait_bits(p, DBI + 0x80, BIT(29), BIT(29), 100000));
	report(p, "link status", rd(p, DBI + 0x80));
	wr(p, DBI + 0x18, 0x00010100); /* Primary 0, secondary/subordinate 1. */
	wr(p, DBI + 0x20, 0x60306030); /* 1 MiB non-prefetchable bridge window. */
	wr(p, DBI + 0xb40, 0x14); /* AUX 19.2 MHz. */
	TRY(outbound(p, 0, 4, CONF, CONF + 0xfff, 0x01000000)); /* CFG0, 01:00.0 */
	uint32_t id = 0;
	for (unsigned i = 0; i < 1000; i++) {
		id = rd(p, CONF);
		if (id == 0x110117cb) break;
		pause_us(p, 1000);
	}
	report(p, "endpoint PCI ID", id);
	if (id != 0x110117cb) { error = ENODEV; goto fail; }
	uint32_t command = rd(p, CONF + 4), bar = rd(p, CONF + 0x10);
	if (command == UINT32_MAX || bar == UINT32_MAX || (bar & 1) ||
	    ((bar >> 1) & 3) == 1 || ((bar >> 1) & 3) == 3) {
		error = EIO; goto fail;
	}
	/* QCA6390's known 1 MiB BAR; never enable DMA before installing its SMMU. */
	wr(p, CONF + 4, ((command & 0xffff) & ~7u) | BIT(10));
	if (rd(p, CONF + 4) & 4) { error = EIO; goto fail; }
	/* The SID table can contain uninitialized values after reset.
	 * Initialize this owned, single-endpoint controller like the stock driver,
	 * after checking QCA6390 identity and disabling endpoint bus mastering. */
	for (unsigned i = 0; i < 256; i++) {
		wr(p, PARF + 0x2000 + 4 * i, 0);
		if (rd(p, PARF + 0x2000 + 4 * i) != 0) { error = EIO; goto fail; }
	}
	report(p, "BDF-to-SID table initialized, DMA disabled", 256);
	wr(p, CONF + 0x10, BAR | (bar & 15));
	if ((bar & 6) == 4) wr(p, CONF + 0x14, 0);
	TRY(outbound(p, 1, 0, BAR, BAR + 0xfffff, BAR));
	wr(p, DBI + 4, 2); /* Root bridge memory decode only. */
	wr(p, CONF + 4, ((command & 0xffff) & ~7u) | 2 | BIT(10));
	report(p, "endpoint command", rd(p, CONF + 4));
	report(p, "endpoint BAR0", rd(p, CONF + 0x10));
	if ((rd(p, CONF + 4) & 7) != 2 || (rd(p, CONF + 0x10) & ~15u) != BAR) {
		error = EIO; goto fail;
	}
	uint32_t version = rd(p, BAR + 8);
	report(p, "MHI version (DMA disabled)", version);
	if (version != 0x01000000) {
		report(p, "MHI register length", rd(p, BAR));
		report(p, "MHI control", rd(p, BAR + 0x38));
		report(p, "MHICFG", rd(p, BAR + 0x10));
		report(p, "channel doorbells", rd(p, BAR + 0x18));
		report(p, "event doorbells", rd(p, BAR + 0x20));
		report(p, "BHI offset", rd(p, BAR + 0x28));
		report(p, "BHIE offset", rd(p, BAR + 0x2c));
		if (rd(p, BAR + 0x28) == 0x100) {
			report(p, "BHI major version", rd(p, BAR + 0x104));
			report(p, "BHI execution environment", rd(p, BAR + 0x128));
		}
	}
	/* Captured on real UMI PBL: MHIVER has a placeholder, BHI is readable. */
	bool cold_pbl = version == 0xdeadbeef && rd(p, BAR) == 0x100 &&
	    rd(p, BAR + 0x18) == 0x300 && rd(p, BAR + 0x20) == 0x700 &&
	    rd(p, BAR + 0x28) == 0x100 && rd(p, BAR + 0x2c) == 0x224 &&
	    rd(p, BAR + 0x104) == 1 && rd(p, BAR + 0x128) == 0;
	if (version != 0x01000000 && !cold_pbl) { error = ENODEV; goto fail; }
	if (cold_pbl) report(p, "QCA6390 cold PBL signature validated", 1);
	p->enumerated = true;
	return 0;
fail:
	p->failed = true;
	report(p, "enumeration failed errno", (uint32_t)error);
	umi_pcie_quiesce(p);
	return error;
}

int umi_pcie_enable_dma(struct umi_pcie *p)
{
	if (!p || !p->io || !p->enumerated || p->failed) return ENODEV;
	if (p->dma_enabled) return EALREADY;
	/* Kona hashes the two BDF bytes (bus 1, devfn 0) using CRC-8/0x07.
	 * SID table contains the low byte; PCIe0 supplies the wired 0x1c00 base. */
	uint8_t hash = 0;
	const uint8_t bdf[] = {1, 0};
	for (unsigned i = 0; i < sizeof(bdf); i++) {
		hash ^= bdf[i];
		for (unsigned b = 0; b < 8; b++) hash = (hash << 1) ^ ((hash & 0x80) ? 7 : 0);
	}
	uint32_t route = PARF + 0x2000 + 4 * hash;
	uint32_t old = rd(p, route), wanted = 0x01000100;
	report(p, "BDF-to-SID route address", route);
	report(p, "BDF-to-SID previous route", old);
	if (old != 0 && old != wanted) return EBUSY;
	wr(p, route, wanted);
	if (rd(p, route) != wanted) return EIO;
	uint32_t command = rd(p, CONF + 4);
	if (command == UINT32_MAX || (command & 7) != 2) return EIO;
	/* SM8250 defaults to BDF-to-SID bypass. A table alone does not route
	 * requests through the owned context; explicitly enable translation. */
	report(p, "BDF-to-SID configuration before", rd(p, PARF + 0x2c00));
	int error = update(p, PARF + 0x2c00, 1, 0);
	if (error || (rd(p, PARF + 0x2c00) & 1)) return EIO;
	report(p, "BDF-to-SID configuration after", rd(p, PARF + 0x2c00));
	wr(p, DBI + 4, 6); /* Allow protected endpoint transactions upstream. */
	wr(p, CONF + 4, (command & 0xffff) | 4);
	p->dma_enabled = true; /* Quiesce must clear it even if readback fails. */
	if ((rd(p, CONF + 4) & 7) != 6) return EIO;
	report(p, "protected DMA stream", 0x1c01);
	return 0;
}

int umi_pcie_prepare_radio(struct umi_pcie *p)
{
	if (!p || !p->io || !p->enumerated || p->failed) return ENODEV;
	if (p->dma_enabled) return EBUSY;
	/* UMI DT sets qcom,set-wlaon-pwr-ctrl. CNSS clears SHUTDOWN_EN and
	 * VDD4BLOW_SW_EN in WLAON_QFPROM_PWR_CTRL before the first MHI request.
	 * This disables those bits; it never enables fuse programming. */
	uint32_t saved_window = rd(p, BAR + 0x310c);
	if (saved_window == UINT32_MAX || saved_window == 0xdeadbeef) return EIO;
	wr(p, BAR + 0x310c, 0x4000003f); /* Internal 0x01f80000 window. */
	int error = EIO;
	if ((rd(p, BAR + 0x310c) & 0x4000003f) == 0x4000003f) {
		uint32_t power = rd(p, BAR + 0x8031c);
		report(p, "WLAON power control before", power);
		if (power != UINT32_MAX && power != 0xdeadbeef) {
			wr(p, BAR + 0x8031c, power & ~5u);
			uint32_t now = rd(p, BAR + 0x8031c);
			report(p, "WLAON power control after", now);
			if (now == (power & ~5u)) error = 0;
		}
	}
	wr(p, BAR + 0x310c, saved_window);
	return error;
}

int umi_pcie_setup_mhi_msi(struct umi_pcie *p, uint64_t address, uint32_t data)
{
	if (!p || !p->io || !p->enumerated || p->failed) return ENODEV;
	if (p->dma_enabled || p->msi_cap) return EBUSY;
	if ((address & 3) || data > 0xffff || (data & 31)) return EINVAL;
	uint32_t cap = rd(p, CONF + 0x34) & 0xff;
	uint64_t seen = 0;
	for (unsigned i = 0; i < 48 && cap; i++) {
		if (cap < 0x40 || cap > 0xfc || (cap & 3) || (seen & (1ull << (cap / 4)))) return EINVAL;
		seen |= 1ull << (cap / 4);
		uint32_t header = rd(p, CONF + cap);
		if (header == UINT32_MAX) return EIO;
		if ((header & 0xff) == 5) {
			uint32_t control = header >> 16;
			bool wide = (control & 0x80) != 0;
			if (((control >> 1) & 7) < 5 || !(control & 0x100)) return ENOTSUP;
			if (!wide && address > UINT32_MAX) return ERANGE;
			uint32_t msg = cap + (wide ? 12 : 8), mask = msg + 4;
			if (mask > 0xfc) return EINVAL;
			wr(p, CONF + cap, header & ~BIT(16));
			wr(p, CONF + mask, UINT32_MAX);
			wr(p, CONF + cap + 4, (uint32_t)address);
			if (wide) wr(p, CONF + cap + 8, (uint32_t)(address >> 32));
			wr(p, CONF + msg, data);
			wr(p, CONF + mask, 0xfffffff8); /* UMI MHI owns vectors 0, 1 and 2. */
			wr(p, CONF + cap, (header & ~(0x71u << 16)) | (0x51u << 16));
			p->msi_cap = cap;
			if (!(rd(p, CONF + cap) & BIT(16))) return EIO;
			report(p, "MHI MSI address low", (uint32_t)address);
			report(p, "MHI MSI data", data);
			return 0;
		}
		cap = (header >> 8) & 0xff;
	}
	return ENODEV;
}
