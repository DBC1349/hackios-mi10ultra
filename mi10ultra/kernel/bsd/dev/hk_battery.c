/*
 * Battery monitor for Xiaomi redwood (POCO X5 Pro).
 *
 * Redwood selects PM7325B and qcom,battery-charger over PMIC GLINK/ADSP, not
 * the local PM7250B/QG layout.  The resident UEFI contains the matching
 * Qualcomm charger drivers.  xnuloader owns that firmware context on core 0,
 * polls it, and publishes a read-only cache; XNU only consumes that cache.
 * Charger limits and JEITA protection therefore remain firmware-owned.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hk_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hk_tl(const char * format, ...);
#define printf hk_tl
extern void HKBatteryReport(const void *state, unsigned size);

#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)
#define LOADER_PAGE_SIZE                0x4000UL

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

static volatile struct loader_battery *shared_battery;
static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates, battery_failures;

/* ---- the phone's own fuel gauge: TI BQ28Z610 on I2C (QUP SE15, GPIO 44/45) -------------------- */
/*
 * cas uses an external TI BQ28Z610 (I2C 0x55) and a BQ25790 charger (0x6B); the PM8150B's own
 * FG-gen4 is disabled in the device tree. The gauge answers the standard SMBus word reads:
 * SOC 0x2C (%), voltage 0x08 (mV), average current 0x14 (signed mA, positive while charging),
 * temperature 0x06 (0.1 K), flags 0x0A (bit 6 discharging, bit 5 full), remaining 0x10 (mAh),
 * full 0x12 (mAh), cycles 0x2A, design 0x3C (mAh). The bus is bit-banged on the TLMM south
 * tile, the same way the touch driver drives its own bus.
 */
#define BQ_GPIO_PHYS    0x0f500000UL
#define BQ_GPIO_SIZE    0x80000UL
#define BQ_ADDR         0x55u
#define BQ_REG_TEMP     0x06u
#define BQ_REG_VOLT     0x08u
#define BQ_REG_FLAGS    0x0au
#define BQ_REG_RC       0x10u
#define BQ_REG_FCC      0x12u
#define BQ_REG_CURR     0x14u
#define BQ_REG_CYCLES   0x2au
#define BQ_REG_SOC      0x2cu
#define BQ_REG_DESIGN   0x3cu

static vm_offset_t bq_gpio;
static unsigned bq_sda = 44, bq_scl = 45;
static bool bq_ready;

static uint32_t
bq_rd(uint32_t off)
{
	return *(volatile uint32_t *)(bq_gpio + off);
}

static void
bq_wr(uint32_t off, uint32_t value)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(bq_gpio + off) = value;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static void
bq_delay(void)
{
	delay(5);       /* about 100 kHz */
}

/* Open drain: drive low, or release the line (pull-up). */
static void
bq_pin(unsigned pin, bool high)
{
	uint32_t off = pin * 0x1000u;
	uint32_t cfg = bq_rd(off);

	if (!high) {
		bq_wr(off + 4, bq_rd(off + 4) & ~2u);
	}
	cfg &= ~0x3ffu;
	cfg |= (high ? 3u : 0u) | (!high ? 0x200u : 0u);
	if (cfg & (1u << 11)) {
		cfg |= 1u << 12;
	}
	bq_wr(off, cfg);
	bq_delay();
}

static bool
bq_sample(unsigned pin)
{
	return (bq_rd(pin * 0x1000u + 4) & 1u) != 0;
}

static bool
bq_scl_high(void)
{
	bq_pin(bq_scl, true);
	for (unsigned i = 0; i < 1000; ++i) {
		if (bq_sample(bq_scl)) {
			return true;
		}
		delay(1);
	}
	return false;
}

static bool
bq_start(void)
{
	bq_pin(bq_sda, true);
	if (!bq_scl_high() || !bq_sample(bq_sda)) {
		return false;
	}
	bq_pin(bq_sda, false);
	bq_pin(bq_scl, false);
	return true;
}

static void
bq_stop(void)
{
	bq_pin(bq_sda, false);
	(void)bq_scl_high();
	bq_pin(bq_sda, true);
}

static bool
bq_write_byte(uint8_t value)
{
	for (unsigned bit = 0; bit < 8; ++bit) {
		bq_pin(bq_sda, (value & 0x80u) != 0);
		if (!bq_scl_high()) {
			return false;
		}
		bq_pin(bq_scl, false);
		value <<= 1;
	}
	bq_pin(bq_sda, true);
	if (!bq_scl_high()) {
		return false;
	}
	bool ack = !bq_sample(bq_sda);
	bq_pin(bq_scl, false);
	return ack;
}

static uint8_t
bq_read_byte(bool ack)
{
	uint8_t value = 0;

	bq_pin(bq_sda, true);
	for (unsigned bit = 0; bit < 8; ++bit) {
		value <<= 1;
		if (!bq_scl_high()) {
			return 0xff;
		}
		if (bq_sample(bq_sda)) {
			value |= 1u;
		}
		bq_pin(bq_scl, false);
	}
	bq_pin(bq_sda, !ack);
	(void)bq_scl_high();
	bq_pin(bq_scl, false);
	bq_pin(bq_sda, true);
	return value;
}

/* One SMBus word read: register write, repeated start, two bytes little-endian.
 * `addr` is the 7-bit i2c address: the gauge's 0x55, or the charger's 0x6b. */
static bool
bq_read_at(uint8_t addr, uint8_t reg, uint16_t *value)
{
	for (unsigned attempt = 0; attempt < 3; ++attempt) {
		uint8_t lo, hi;

		if (!bq_start() || !bq_write_byte((uint8_t)(addr << 1)) ||
		    !bq_write_byte(reg) || !bq_start() ||
		    !bq_write_byte((uint8_t)((addr << 1) | 1))) {
			bq_stop();
			delay(10);
			continue;
		}
		lo = bq_read_byte(true);
		hi = bq_read_byte(false);
		bq_stop();
		*value = (uint16_t)lo | (uint16_t)hi << 8;
		return true;
	}
	return false;
}

static bool
bq_read(uint8_t reg, uint16_t *value)
{
	return bq_read_at(BQ_ADDR, reg, value);
}

/*
 * The charger, TI BQ25790 at i2c 0x6b on the same two wires. Its word is
 * what "an external supply is present" really means: the gauge's current
 * sign flips with load transients, so iOS showed charging only in the
 * instants the load dipped. CHARGER_STATUS1 bit 2 is VBUS_PRESENT.
 */
#define CHG_ADDR        0x6bu
#define CHG_REG_STATUS1 0x15u
#define CHG_VBUS_BIT    (1u << 2)

static bool
bq_chg_vbus(void)
{
	uint16_t st1 = 0;

	if (!bq_read_at((uint8_t)CHG_ADDR, (uint8_t)CHG_REG_STATUS1, &st1)) {
		return false;
	}
	return (st1 & CHG_VBUS_BIT) != 0;
}

/*
 * The charger's input current limit: TI BQ25790 register 0x06, ten milliamps a
 * step, and the two data bytes go out as (0, value). Proved on this phone with
 * TWRP's forced i2c: writing (0, 0x96) made the port deliver 702 mA where it had
 * delivered 456, and (0, 0x32) brought it back. Nothing else on this board tells
 * the charger how much it may draw, which is why the pack went flat under iOS:
 * Android's framework does this and our kernel did not.
 */
#define CHG_REG_ILIM 0x06u

static unsigned chg_ilim_ma = 1500;   /* the charger's input current limit, mA */
static unsigned chg_ilim_written = 0;
static bool
bq_chg_set_ilim(unsigned ma)
{
	unsigned steps = ma / 10u;

	if (steps > 255u) steps = 255u;
	if (!bq_start()) return false;
	if (!bq_write_byte((uint8_t)(CHG_ADDR << 1)) ||
	    !bq_write_byte((uint8_t)CHG_REG_ILIM) ||
	    !bq_write_byte(0) ||
	    !bq_write_byte((uint8_t)steps)) {
		bq_stop();
		return false;
	}
	bq_stop();
	return true;
}


static bool
battery_gauge_init(void)
{
	uint16_t soc;

	bq_gpio = ml_io_map(BQ_GPIO_PHYS, BQ_GPIO_SIZE);
	if (bq_gpio == 0) {
		printf("battery: cannot map the gauge's i2c pins\n");
		return false;
	}
	bq_pin(bq_sda, true);
	bq_pin(bq_scl, true);
	if (!bq_read(BQ_REG_SOC, &soc)) {
		/* the pin order is not written down anywhere: SDA/SCL, or SCL/SDA */
		unsigned t = bq_sda;
		bq_sda = bq_scl;
		bq_scl = t;
		if (!bq_read(BQ_REG_SOC, &soc)) {
			printf("battery: no answer from a gauge at i2c 0x%x (SDA %u, SCL %u)\n",
			    BQ_ADDR, bq_sda, bq_scl);
			return false;
		}
	}
	bq_ready = true;
	printf("battery: BQ28Z610 answers on i2c 0x%x (SDA %u, SCL %u): SOC %u%%\n",
	    BQ_ADDR, bq_sda, bq_scl, soc);
	return true;
}

/* Returns true when the gauge gave a usable sample (and has published it). */
static bool
battery_gauge_poll(void)
{
	uint16_t soc, volt = 0, curr = 0, temp = 0, flags = 0, rc = 0, fcc = 0, cycles = 0, design = 0;
	int ma, temp_c10, status, external;
	struct hk_battery_state b = { .size = sizeof(b) };

	if (!bq_ready || !bq_read(BQ_REG_SOC, &soc) || soc > 100) {
		return false;
	}
	(void)bq_read(BQ_REG_VOLT, &volt);
	(void)bq_read(BQ_REG_CURR, &curr);
	(void)bq_read(BQ_REG_TEMP, &temp);
	(void)bq_read(BQ_REG_FLAGS, &flags);
	(void)bq_read(BQ_REG_RC, &rc);
	(void)bq_read(BQ_REG_FCC, &fcc);
	(void)bq_read(BQ_REG_CYCLES, &cycles);
	(void)bq_read(BQ_REG_DESIGN, &design);

	ma = (int)(int16_t)curr;
	temp_c10 = (int)temp - 2731;
	{
		bool vbus = bq_chg_vbus();

		if (chg_ilim_written != chg_ilim_ma) {
			unsigned want = chg_ilim_ma > 2550 ? 2550 : chg_ilim_ma;

			chg_ilim_written = chg_ilim_ma;
			if (bq_chg_set_ilim(want)) {
				hk_tl("battery: charger input current limit now %u mA\n", want);
			} else {
				hk_tl("battery: charger did not take the %u mA limit\n", want);
			}
		}
	status = (flags & 0x20) ? 4 : ma > 10 ? 1 : ma < -10 ? 2 : 3;
		/*
		 * "An external supply is present": the honest signal is the sign of
		 * the current the gauge measures. A wall or USB source that is
		 * charging the pack reads positive; on battery the pack drives the
		 * system and reads negative. The BQ25790's VBUS bit cannot be read
		 * back over these two bit-banged wires with the SMBus word protocol
		 * (the write to its current-limit register is acknowledged, the read
		 * of its status is not), so the current carries the truth here.
		 */
		if (status == 2) {
			/* discharging: genuinely on battery */
			external = false;
		} else if (status == 1 || status == 4) {
			/* positive current: the charger is feeding the pack */
			external = true;
		} else {
			/* idle: plugged and full, or resting; keep the last word */
			external = vbus;
		}
	}

	battery_soc_percent = (int)soc;
	battery_voltage_mv = (volt > 2500 && volt < 5000) || (volt > 7000 && volt < 9000) ? (int)volt : -1; /* single cell, or cas's two in series */
	battery_current_ma = ma;
	battery_temperature_c = (temp_c10 + 5) / 10;
	battery_present = 1;
	battery_charger_present = external;
	battery_charging = status == 1;
	battery_soc_estimated = 0;
	battery_updates++;

	b.percent_hundredths = (int)soc * 100;
	b.voltage_uv = bq_ready && volt > 2500 ? (int)volt * 1000 : 0;
	b.current_ua = ma * 1000;
	b.temperature_hundredths = temp_c10 * 10;
	b.present = 1;
	b.external = external;
	b.status = status;
	b.charge_uah = rc * 1000;
	b.full_uah = fcc * 1000;
	b.design_uah = design * 1000;
	b.cycles = cycles;
	hk_tl("battery: BQ28Z610 SOC=%u%% V=%u mV I=%d mA T=%d.%d C flags=%#x rc=%u fcc=%u cycles=%u\n",
	    soc, volt, ma, temp_c10 / 10, temp_c10 % 10, flags, rc, fcc, cycles);
	HKBatteryReport(&b, sizeof(b));
	return true;
}


/* for others in the kernel (the flashlight's thermal guard): -273 while it is not known */
int
hk_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_NODE(_kern, OID_AUTO, hackios, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_hackios, OID_AUTO, chg_ilim, CTLFLAG_RW | CTLFLAG_LOCKED,
    &chg_ilim_ma, 0, "charger input current limit in mA; the poll writes it to the BQ25790");

SYSCTL_INT(_kern_hackios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");

static bool
battery_snapshot(struct loader_battery *out)
{
	uint32_t before, after;

	for (unsigned retry = 0; retry < 4; retry++) {
		before = shared_battery->sequence;
		if (before & 1) continue;
		__asm__ volatile ("dmb ishld" ::: "memory");
		memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
		__asm__ volatile ("dmb ishld" ::: "memory");
		after = shared_battery->sequence;
		if (before == after && !(after & 1)) return true;
	}
	return false;
}

extern void HKBatteryReport(const void *state, unsigned size);
extern void hk_tl(const char * format, ...);

static void
battery_poll(void)
{
	struct loader_battery sample;

	if (bq_ready) {
		/* The gauge is the phone's own source: a failed read waits for the next poll
		 * instead of replacing good values with the UEFI cache's (which has no voltage). */
		if (!battery_gauge_poll()) {
			battery_failures++;
		}
		return;
	}
	if (shared_battery == NULL) {
		battery_failures++;
		return;
	}
	if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
	    sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
	    !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
		battery_failures++;
		printf("battery: no valid UEFI sample (flags=%#x soc=%d v=%d polls=%u failures=%u action=%u)\n",
		    sample.flags, sample.soc_percent, sample.voltage_mv, sample.polls, sample.failures,
		    sample.charger_action);
		return;
	}
	battery_soc_percent = sample.soc_percent;
	battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
	battery_current_ma = sample.current_ma;
	battery_temperature_c = sample.temperature_c;
	battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
	battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
	battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
	battery_charger_action = (int)sample.charger_action;
	battery_updates++;
	{
		struct hk_battery_state b = { .size = sizeof(b) };

		b.percent_hundredths = battery_soc_percent * 100;
		b.voltage_uv = battery_voltage_mv > 0 ? battery_voltage_mv * 1000 : 0;
		b.current_ua = battery_current_ma * 1000;
		b.temperature_hundredths = battery_temperature_c > -100 ? battery_temperature_c * 100 : 0;
		b.present = battery_present;
		b.external = battery_charger_present;
		b.status = battery_charger_present ?
		    (battery_soc_percent >= 100 ? 4 : battery_charging ? 1 : 3) : 2;
		hk_tl("battery: UEFI FG says SOC=%d%% V=%d mV I=%d mA T=%d C present=%d external=%d status=%d%s\n",
		    battery_soc_percent, battery_voltage_mv, battery_current_ma, battery_temperature_c,
		    battery_present, battery_charger_present, b.status,
		    battery_soc_estimated ? " (voltage estimate)" : "");
		HKBatteryReport(&b, sizeof(b));
	}
}

/*
 * What the audio DSP's battery manager says (bsd/dev/hk_adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that iOS reads (iokit/Kernel/HKBattery.cpp).
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_hackios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_hackios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

extern void HKBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/HKBattery.cpp */

void
hk_battery_report(const struct hk_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	battery_temperature_c = (b->temperature_hundredths + 50) / 100;
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_source_dsp = 1;
	battery_updates++;
		{
		extern void hk_tl(const char * format, ...);
		hk_tl("battery: DSP says SOC=%d%% V=%d mV I=%d mA T=%d C present=%d external=%d status=%d\n",
		    percent, b->voltage_uv / 1000, b->current_ua / 1000, b->temperature_hundredths / 100,
		    b->present, b->external, b->status);
	}
	HKBatteryReport(b, sizeof(*b));
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned tick = 0;; tick++) {
		battery_poll();
		if (tick % 12 == 0) {
			printf("battery: SOC=%d%%%s, V=%d mV, I=%d mA, T=%d C, battery=%d charger=%d charging=%d "
			    "action=%d (%llu reads, %llu failed)\n", battery_soc_percent,
			    battery_soc_estimated ? " (voltage estimate)" : "", battery_voltage_mv,
			    battery_current_ma, battery_temperature_c, battery_present, battery_charger_present,
			    battery_charging, battery_charger_action, battery_updates, battery_failures);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
hk_battery_init(void)
{
	/*
	 * The gauge is read by bit-banging I2C with busy delays, and umi_wifi's PCIe
	 * power-on polls the same interconnect with tight timeouts. The first read
	 * and the polling thread both wait for the radio service to finish, so the
	 * two can never overlap.
	 */
	{
		bool (*wifi_done)(void) = (bool (*)(void))0;
		extern bool umi_wifi_service_done(void);

		wifi_done = umi_wifi_service_done;
		for (unsigned waited = 0; waited < 60000 && !wifi_done(); waited += 100) {
			delay_for_interval(100, NSEC_PER_MSEC);
		}
	}

	uint32_t enabled = 0;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
	(void)battery_gauge_init();
	if (PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) && shared_pa != 0) {
		page = ml_io_map((vm_offset_t)(shared_pa & ~(LOADER_PAGE_SIZE - 1)), LOADER_PAGE_SIZE);
		shared_battery = (volatile struct loader_battery *)(page + (shared_pa & (LOADER_PAGE_SIZE - 1)));
		if (shared_battery->magic != LOADER_BATTERY_MAGIC || shared_battery->version != LOADER_BATTERY_VERSION ||
		    shared_battery->size < sizeof(struct loader_battery)) {
			printf("battery: bad xnuloader cache at %#llx (magic %#llx version %u size %u)\n", shared_pa,
			    shared_battery->magic, shared_battery->version, shared_battery->size);
			shared_battery = NULL;
		} else {
			printf("battery: Qualcomm UEFI cache at pa %#llx\n", shared_pa);
		}
	} else {
		printf("battery: xnuloader did not provide battery_shared_pa\n");
	}
	if (shared_battery == NULL && !bq_ready) {
		printf("battery: no source for the battery (no BQ28Z610 answer, no UEFI cache)\n");
		return;
	}
	printf("battery: polling every 5 seconds\n");
	/* The charger's input current limit: nobody else sets it, so the pack went
	 * flat under iOS even on a wall charger. Set it here: the pins are
	 * configured and the gauge has answered by this point. */
	{
		unsigned want = 1500;   /* ten milliamps a step; the port self-limits below this */

		if (bq_chg_set_ilim(want)) {
			uint16_t back = 0;

			(void)bq_read_at((uint8_t)CHG_ADDR, (uint8_t)CHG_REG_ILIM, &back);
			hk_tl("battery: charger input current limit set to %u mA (read back %#x)\n", want, back);
		} else {
			hk_tl("battery: charger did not take the input current limit\n");
		}
	}

	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

