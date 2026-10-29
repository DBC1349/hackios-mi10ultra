/*
 * The speakers of the POCO X5 Pro (redwood): their two amplifiers. Every address below belongs
 * to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * Each speaker has an amplifier of its own, an Awinic AW88261 (chip ID 0x2113, a "smart PA"),
 * set up over I2C -- the bus of the SoC's serial engine 3, GPIO 12 data and 13 clock, on which
 * Android's device tree and its running kernel have these two and nothing else -- and fed by
 * the SoC's first TDM interface ("MI2S 0": GPIO 97 bit clock, 100 frame sync, 99 data out),
 * which the audio DSP drives (hk_sound.c). In Android's tree the amplifier at 0x35 is
 * "sound-channel 0" and takes slot 0 of the frame (the left channel), the one at 0x34 is
 * channel 1 and takes slot 1; neither has a reset pin.
 *
 * Two uses:
 *
 *   speaker_probe=1    reads registers and nothing else: the pins, the power names (below)
 *   speaker_probe=2    ... and asks the amplifiers who they are (I2C, below)
 *
 *   hk_speaker_prepare() and the rest (sys/hk_audio.h), for boot-arg sound=3 of
 *   hk_sound.c: the amplifiers set up and run exactly as Android's own driver does it
 *   (Xiaomi's redwood-s-oss kernel, techpack/audio/asoc/codecs/aw88263s_m20: aw882xx_device.c,
 *   aw882xx_init.c and aw882xx_pid_2113_reg.h; the phone runs its version 2.0.0), with the
 *   registers' values from Android's own configuration file -- /vendor/etc/aw882xx_acf.bin of
 *   the phone, which userland/build.sh puts into /fw of the system image from fw-audio/ and
 *   which is not in this repository. Its "Music" profile is what Android plays the speakers
 *   with (tinymix: aw_dev_0_prof, aw_dev_1_prof); 55 of its 57 registers read back from the
 *   idle chips under Android exactly as the file has them, and the other two as the driver
 *   leaves them (0x04 powered down and muted, 0x08 with the feedback off).
 *
 * What the driver does and this does the same way, per amplifier:
 *
 *   prepare  reads the chip ID and the four fuse registers; a soft reset (0x55aa into
 *            register 0); the profile's registers, every one but three as the file has them:
 *            0x04 keeps the power-down, amplifier power-down and mute bits as they are and
 *            sets the mute of the power stage ("ULS"), 0x08 has the feedback off, and 0x05 (the
 *            volume) is turned down to the lowest, -90 dB, right after; 0x50 gets the value the
 *            driver works out from the fuses (8192 * (1000 + icalk) / (1000 + vcalk)), which for
 *            this phone's two chips is the value Android left in them
 *   start    only while the TDM clock runs: power-down off, 2 ms, the PLL locked (status bits 0
 *            and 4); amplifier power-down off, 1 ms, the status as the driver wants it (bits 0, 4,
 *            8, 9); the feedback on; the two mutes off; the interrupts read away; the volume
 *            stays at the lowest
 *   volume   stepwise, 6 dB a step (the driver takes one every 100 us), to `below' eighths of a dB
 *            under Android's own level (the profile's 0x05, -2.1 dB, with Android's
 *            aw_dev_N_volume 0) and never above it
 *   stop     the power stage muted, the volume stepped down to the lowest, muted, the feedback
 *            off, 1 ms, amplifier power-down, power-down
 *
 * Every write is said in the log and read back; a value that does not read back as written, a
 * byte nobody acknowledges, or a fault in the status register (under-voltage, over-current of
 * the boost or the output, over-temperature, no clock, the watchdog, over-voltage) has both
 * amplifiers powered down at once (amplifier power-down and power-down, bits 1 and 0 of 0x04)
 * and nothing more done. What is not done, compared with Android: the DSP's Awinic algorithm,
 * which protects the speakers with their calibration (aw882xx_rx_switch); in its place the
 * samples go through hk_guard.c on their way here. The driver's monitor is done
 * (hk_speaker_monitor(), below): once a second the chips' temperature and the battery's
 * voltage, and the reduction that the file's table asks for at either.
 *
 * I2C is done by hand: a pin is either pulled low or let go (and the pull-ups raise it), at 100
 * kHz at most. For each register read or written the two pins are taken from the serial engine
 * (function qup03) as GPIOs and given back as they were found right after.
 *
 * Register layouts: the files above, and Linux's sound/soc/codecs/aw88261.c; pins:
 * drivers/pinctrl/qcom/pinctrl-yupik.c of the same kernel and the phone's own device tree.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hk_audio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static vm_offset_t tlmm, gcc, cmd_db;
static uint32_t probe_level;
static bool leaving;                    /* on a panic's way out: nothing sleeps, nothing is said but to the loader */

/* Every line to the loader's log as well: what is written to the amplifiers should be known even if the kernel dies. */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* Waits without sleeping (also with interrupts off, on a panic's way out). */
static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 12      /* the serial engine's first pin is the data line, the second the clock */
#define PIN_SCL                 13

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 12, "I2C data" }, { 13, "I2C clock" }, { 97, "MI2S 0 bit clock" }, { 100, "MI2S 0 word select" },
	{ 98, "MI2S 0 data 0" }, { 99, "MI2S 0 data 1" }, { 102, "the amplifier 0x35's interrupt" },
	{ 45, "the amplifier 0x34's interrupt" },
};

static void
show_pins(void)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("speaker: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") :
		    "input", io & 1);
	}
}

/* ---- I2C by hand ----------------------------------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most, I2C's standard mode; the bus does not mind slower */
}

/* A line is pulled low (the pin drives 0) or let go (the pin is an input and the pull-up raises it). */
static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

/* One bit out (the other side's bit in, if we let the data line go): the clock goes up and down once. */
static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();             /* (a device may hold the clock low until it is ready) */
	}
	half_bit();
	in = level(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

/* Returns true if the byte was acknowledged. */
static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);                 /* acknowledge all but the last byte */
	return byte;
}

/*
 * The two pins taken from the serial engine for one transaction, and given back as found. False
 * if the bus is not at rest: then nothing is driven.
 */
static uint32_t taken_cfg[2], taken_io[2];

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!level(PIN_SDA) || !level(PIN_SCL)) {
		wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
		wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
		wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
		wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

/* A 16-bit register of an Awinic amplifier, most significant byte first. Returns false if nobody answers. */
static bool
register_read(uint8_t address, uint8_t reg, uint16_t *value)
{
	bool answered;

	if (!bus_take()) {
		say("speaker: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    level(PIN_SDA), PIN_SCL, level(PIN_SCL));
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write((uint8_t)(address << 1 | 1));
	}
	if (answered) {
		*value = (uint16_t)(i2c_read(true) << 8);
		*value |= i2c_read(false);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static bool
register_write(uint8_t address, uint8_t reg, uint16_t value)
{
	bool answered;

	if (!bus_take()) {
		say("speaker: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    level(PIN_SDA), PIN_SCL, level(PIN_SCL));
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg) && i2c_write((uint8_t)(value >> 8)) &&
	    i2c_write((uint8_t)value);
	i2c_stop();
	bus_give_back();
	return answered;
}

static const char *
amplifier_name(uint16_t id)
{
	switch (id) {
	case 0x1852: return "AW88194/AW88195";
	case 0x2013: return "AW88264";
	case 0x2032: return "AW88266";
	case 0x2049: return "AW88395";
	case 0x2055: return "AW88256";
	case 0x2066: return "AW88166";
	case 0x2071: return "AW88350";
	case 0x2113: return "AW88261";
	case 0x2183: return "AW88399";
	case 0x2308: return "AW88230";
	}
	return "an amplifier this list does not know";
}

/* ---- the amplifiers, as Android's driver runs them -------------------------------------------------- */

#define AW_ID                   0x00            /* reads the chip ID; 0x55aa written resets the chip */
#define AW_ID_AW88261           0x2113
#define AW_SOFT_RESET           0x55aa
#define AW_SYSST                0x01
#define AW_SYSINT               0x02            /* read: cleared */
#define AW_SYSINTM              0x03
#define AW_SYSCTRL              0x04
#define AW_SYSCTRL2             0x05            /* [9:0] the volume, attenuation: [9:6] 6 dB steps, [5:0] eighths of a dB */
#define AW_I2SCTRL3             0x08
#define AW_VBAT                 0x21            /* [9:0] the battery's voltage as the amplifier sees it: x 6025 / 1023 mV */
#define AW_TEMP                 0x22            /* [9:0] the amplifier's temperature, degrees Celsius, two's complement */
#define AW_PVDD                 0x23            /* [9:0] its boosted supply */
#define AW_ISNDAT               0x24            /* the current and voltage sense, as last measured */
#define AW_VSNDAT               0x25
#define AW_DBGCTRL              0x14
#define AW_VSNTM1               0x50
#define AW_EFRH4                0x78
#define AW_EFRH3                0x79
#define AW_EFRL4                0x7c
#define AW_EFRL3                0x7d

#define SYSCTRL_PWDN            (1u << 0)
#define SYSCTRL_AMPPD           (1u << 1)
#define SYSCTRL_HMUTE           (1u << 8)
#define SYSCTRL_ULS_HMUTE       (1u << 14)
#define I2SCTRL3_TXEN           (1u << 6)
#define DBGCTRL_EF_OR           (1u << 3)       /* the fuses' two copies are or-ed; else and-ed */
#define SYSINTM_DEFAULT         0xffff

#define SYSST_PLLS              (1u << 0)
#define SYSST_OTHS              (1u << 1)
#define SYSST_OCDS              (1u << 3)
#define SYSST_CLKS              (1u << 4)
#define SYSST_NOCLKS            (1u << 5)
#define SYSST_WDS               (1u << 6)
#define SYSST_SWS               (1u << 8)
#define SYSST_BSTS              (1u << 9)
#define SYSST_BSTOCS            (1u << 11)
#define SYSST_UVLS              (1u << 14)
#define SYSST_OVP2S             (1u << 15)
#define SYSST_PLL_CHECK         (SYSST_CLKS | SYSST_PLLS)
#define SYSST_CHECK             (SYSST_BSTS | SYSST_SWS | SYSST_CLKS | SYSST_PLLS)
#define SYSST_FAULTS            (SYSST_OVP2S | SYSST_UVLS | SYSST_BSTOCS | SYSST_WDS | SYSST_NOCLKS | SYSST_OCDS | SYSST_OTHS)

#define VOLUME_LOWEST           (90 * 8)        /* the driver's "mute volume", in eighths of a dB */
#define VOLUME_STEP             (6 * 8)
#define STATUS_TRIES            10              /* the driver's AW_DEV_SYSST_CHECK_MAX, 2 ms apart */

/* Android's configuration file ("ACF", Awinic's format; Linux: sound/soc/codecs/aw88395/aw88395_lib.c) */
#define ACF_PROFILE_MUSIC       0
#define ACF_DATA_REGISTERS      4               /* a block with a header of its own and registers in it */
#define ACF_DATA_MONITOR        10              /* the driver's "monitor": what to do when the chip is cold or the battery low */
#define MONITOR_ROWS            8
#define ACF_BLOCK_HEADER        0x3c

struct amplifier {
	uint8_t address;
	unsigned device;                /* the file's device number for it (Android's sound-channel) */
	const char *where;
	uint16_t profile[64][2];        /* register, value */
	unsigned count;
	uint16_t sysctrl;               /* what the profile has for 0x04, 0x08 and 0x05 */
	uint16_t i2sctrl3;
	uint16_t volume_at;             /* Android's level: from the profile's 0x05, in eighths of a dB of attenuation */
	uint16_t volume;                /* where it is now */
	uint16_t volume_high;           /* the profile's bits 15:10 of 0x05, which the volume leaves as they are */
	bool prepared, started;
	/* the monitor's tables (from the file) and what was last seen */
	struct { int16_t min, max; uint16_t less; } temp_rows[MONITOR_ROWS], vbat_rows[MONITOR_ROWS];
	unsigned temp_count, vbat_count, looks;
	int temps[3], vbats[3];
	int temp_row, vbat_row;
};

static struct amplifier amplifiers[2] = {
	{ .address = 0x35, .device = 0, .where = "0x35, slot 0", .temp_row = -1, .vbat_row = -1 },
	{ .address = 0x34, .device = 1, .where = "0x34, slot 1", .temp_row = -1, .vbat_row = -1 },
};
static bool broken;                     /* something went wrong: both have been powered down, nothing more is done */

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * The driver's monitor for one amplifier (Android has it on: aw_dev_N_monitor), from the file: every second it
 * reads the chip's temperature and the battery's voltage, and a table says what to do for each range of them.
 * This file's: at 7 to 100 degrees nothing; at 2 to 5 degrees 3 dB less, at -2 to 0 4.5 dB, at -50 to -5 8 dB (and
 * a lower current limit for the boost, which is not done here); from 3.85 V nothing, at 3.65 to 3.75 V and up to
 * 3.55 V a lower "vmax" for the DSP's algorithm, which does not run here. In its temperature rows a vmax goes with
 * each reduction of the volume, so the battery rows' vmax is turned into a volume reduction in the same proportion
 * (the first temperature row's: 3 dB for its vmax): 1.6 and 3.1 dB. Between the ranges the last one holds.
 *   block: checksum (of the bytes after it), version, chip name[16], ui version, the time and count of looks,
 *   switches, then for the temperatures and for the voltages each: a value, the number of rows, a row's size and
 *   the rows' offset; a row: int16 lowest, int16 highest, uint16 boost current, uint16 reduction (eighths of a
 *   dB), int32 vmax
 */
static void
acf_monitor(uint16_t device, const uint8_t *block, uint32_t length)
{
	struct amplifier *a = NULL;
	uint32_t sum = 0, temp_count, temp_size, temp_at, vbat_count, vbat_size, vbat_at;
	int64_t per_eighth = 0;

	for (unsigned i = 0; i < 2; i++) {
		if (amplifiers[i].device == device) {
			a = &amplifiers[i];
		}
	}
	if (a == NULL || length < 0x48) {
		return;
	}
	for (uint32_t i = 4; i < length; i++) {
		sum += block[i];
	}
	temp_count = le32(block + 0x2c);
	temp_size = le32(block + 0x30);
	temp_at = le32(block + 0x34);
	vbat_count = le32(block + 0x3c);
	vbat_size = le32(block + 0x40);
	vbat_at = le32(block + 0x44);
	if (sum != le32(block) || temp_size < 12 || vbat_size < 12 || temp_count > MONITOR_ROWS || vbat_count > MONITOR_ROWS ||
	    temp_at + temp_count * temp_size > length || vbat_at + vbat_count * vbat_size > length) {
		say("speaker: device %u's monitor table is not of the shape known: not used\n", device);
		return;
	}
	for (uint32_t i = 0; i < temp_count; i++) {
		const uint8_t *row = block + temp_at + i * temp_size;
		int32_t vmax = (int32_t)le32(row + 8);

		a->temp_rows[i].min = (int16_t)(row[0] | row[1] << 8);
		a->temp_rows[i].max = (int16_t)(row[2] | row[3] << 8);
		a->temp_rows[i].less = (uint16_t)(row[6] | row[7] << 8);
		if (per_eighth == 0 && a->temp_rows[i].less != 0 && vmax < 0) {
			per_eighth = -(int64_t)vmax / a->temp_rows[i].less;
		}
	}
	for (uint32_t i = 0; i < vbat_count; i++) {
		const uint8_t *row = block + vbat_at + i * vbat_size;
		int32_t vmax = (int32_t)le32(row + 8);

		a->vbat_rows[i].min = (int16_t)(row[0] | row[1] << 8);
		a->vbat_rows[i].max = (int16_t)(row[2] | row[3] << 8);
		a->vbat_rows[i].less = (uint16_t)(row[6] | row[7] << 8);
		if (vmax < 0 && per_eighth > 0) {
			a->vbat_rows[i].less += (uint16_t)((-(int64_t)vmax + per_eighth / 2) / per_eighth);
		}
	}
	a->temp_count = temp_count;
	a->vbat_count = vbat_count;
	for (uint32_t i = 0; i < temp_count; i++) {
		say("speaker: the amplifier %s's monitor: at %d to %d degrees, %u.%03u dB less\n", a->where, a->temp_rows[i].min,
		    a->temp_rows[i].max, a->temp_rows[i].less / 8, a->temp_rows[i].less % 8 * 125);
	}
	for (uint32_t i = 0; i < vbat_count; i++) {
		say("speaker: the amplifier %s's monitor: at %d to %d mV, %u.%03u dB less\n", a->where, a->vbat_rows[i].min,
		    a->vbat_rows[i].max, a->vbat_rows[i].less / 8, a->vbat_rows[i].less % 8 * 125);
	}
}

/* The "Music" profile of every amplifier, from the file; false if the file is not of the shape known. */
static bool
acf_profiles(const uint8_t *acf, uint64_t size)
{
	uint32_t entry_size, entries, at;

	if (size < 0x50 || size > 0x100000) {
		say("speaker: the configuration file is %llu bytes: not one of Awinic's\n", size);
		return false;
	}
	entry_size = le32(acf + 0x34);
	entries = le32(acf + 0x38);
	at = le32(acf + 0x3c);
	if (entries == 0 || entries > 64 || entry_size % entries != 0 || entry_size / entries < 0x30 ||
	    (uint64_t)at + entry_size > size) {
		say("speaker: the configuration file's table (%u entries, %u bytes at %#x) is not of a shape known\n", entries,
		    entry_size, at);
		return false;
	}
	for (uint32_t e = 0; e < entries; e++) {
		const uint8_t *entry = acf + at + e * (entry_size / entries);
		char name[17];
		uint16_t device = (uint16_t)(entry[20] | entry[21] << 8), profile = (uint16_t)(entry[26] | entry[27] << 8);
		uint32_t kind = le32(entry + 28), length = le32(entry + 32), offset = le32(entry + 36);
		uint32_t checksum, sum = 0, data_length, count;
		const uint8_t *block = acf + offset;
		struct amplifier *a = NULL;

		memcpy(name, entry + 4, 16);
		name[16] = 0;
		if (kind == ACF_DATA_MONITOR) {
			acf_monitor(device, acf + offset, (uint64_t)offset + length <= size ? length : 0);
			continue;
		}
		if (kind != ACF_DATA_REGISTERS || profile != ACF_PROFILE_MUSIC || strcmp(name, "aw88261") != 0) {
			continue;
		}
		for (unsigned i = 0; i < 2; i++) {
			if (amplifiers[i].device == device) {
				a = &amplifiers[i];
			}
		}
		if (a == NULL || (uint64_t)offset + length > size || length < ACF_BLOCK_HEADER + 4) {
			say("speaker: the configuration file's entry %u (device %u) does not fit\n", e, device);
			return false;
		}
		checksum = le32(block);
		data_length = le32(block + 0x10);
		count = le32(block + ACF_BLOCK_HEADER);
		if (ACF_BLOCK_HEADER + data_length > length || data_length != 4 + 4 * count || count > 64 ||
		    le32(block + 0x20) != 2 || le32(block + 0x24) != 2) {
			say("speaker: device %u's Music profile is not a list of 16-bit registers (%u bytes, %u registers)\n", device,
			    data_length, count);
			return false;
		}
		for (uint32_t i = 4; i < ACF_BLOCK_HEADER + data_length; i++) {
			sum += block[i];
		}
		if (sum != checksum) {
			say("speaker: device %u's Music profile does not add up (%#x, the file says %#x)\n", device, sum, checksum);
			return false;
		}
		a->count = count;
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *pair = block + ACF_BLOCK_HEADER + 4 + 4 * i;

			a->profile[i][0] = (uint16_t)(pair[0] | pair[1] << 8);
			a->profile[i][1] = (uint16_t)(pair[2] | pair[3] << 8);
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		bool have[3] = { false, false, false };

		for (unsigned r = 0; r < a->count; r++) {
			uint16_t reg = a->profile[r][0], value = a->profile[r][1];

			if (reg == AW_SYSCTRL) {
				a->sysctrl = value;
				have[0] = true;
			} else if (reg == AW_I2SCTRL3) {
				a->i2sctrl3 = value;
				have[1] = true;
			} else if (reg == AW_SYSCTRL2) {
				/* the driver's aw_pid_2113_reg_val_to_db() */
				a->volume_at = (uint16_t)(((value & 0x3ff) >> 6) * VOLUME_STEP + (value & 0x3f));
				a->volume_high = value & 0xfc00;
				have[2] = true;
			} else if (reg == AW_ID || reg >= 0x70) {
				say("speaker: device %u's Music profile writes register %#x, which is not one of the configuration's\n",
				    a->device, reg);
				return false;
			}
		}
		if (a->count == 0 || !have[0] || !have[1] || !have[2] || a->volume_at >= VOLUME_LOWEST) {
			say("speaker: no usable Music profile for device %u (the amplifier at %#x) in the configuration file\n",
			    a->device, a->address);
			return false;
		}
		say("speaker: the amplifier %s: Music profile of %u registers; Android's volume there is -%u.%03u dB (%#x)\n", a->where,
		    a->count, a->volume_at / 8, a->volume_at % 8 * 125, a->volume_at);
	}
	return true;
}

/* Both powered down at once, without the driver's ceremony: a fault, or a value that did not stick. */
static void
power_down_now(const char *why)
{
	broken = true;
	say("speaker: %s: BOTH AMPLIFIERS ARE POWERED DOWN AT ONCE (0x04: amplifier power-down and power-down set)\n", why);
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t value = 0, after = 0;

		if (!a->prepared && !a->started) {
			continue;
		}
		if (!register_read(a->address, AW_SYSCTRL, &value)) {
			value = a->sysctrl;
		}
		value |= SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE;
		if (!register_write(a->address, AW_SYSCTRL, value) || !register_read(a->address, AW_SYSCTRL, &after)) {
			say("speaker: the amplifier %s: 0x04 <- %#06x NOT ACKNOWLEDGED\n", a->where, value);
		} else {
			say("speaker: the amplifier %s: 0x04 <- %#06x, reads %#06x\n", a->where, value, after);
		}
		a->started = false;
	}
}

/* One register written, said and read back; false (and both powered down) if it did not stick. */
static bool
put(struct amplifier *a, uint8_t reg, uint16_t value)
{
	uint16_t after = 0;

	if (broken) {
		return false;
	}
	if (!register_write(a->address, reg, value)) {
		say("speaker: the amplifier %s: %#04x <- %#06x NOT ACKNOWLEDGED\n", a->where, reg, value);
		power_down_now("a write was not acknowledged");
		return false;
	}
	if (!register_read(a->address, reg, &after)) {
		say("speaker: the amplifier %s: %#04x <- %#06x, NOT READ BACK\n", a->where, reg, value);
		power_down_now("a register could not be read back");
		return false;
	}
	say("speaker: the amplifier %s: %#04x <- %#06x, reads %#06x%s\n", a->where, reg, value, after, after == value ? "" :
	    ": NOT AS WRITTEN");
	if (after != value) {
		power_down_now("a register did not read back as written");
		return false;
	}
	return true;
}

static bool
get(struct amplifier *a, uint8_t reg, uint16_t *value)
{
	if (!register_read(a->address, reg, value)) {
		say("speaker: the amplifier %s: %#04x NOT READ\n", a->where, reg);
		return false;
	}
	return true;
}

/* The bits of 0x04 named in `bits' set or cleared, the rest as they read. */
static bool
sysctrl(struct amplifier *a, uint16_t bits, bool set)
{
	uint16_t value;

	if (broken || !get(a, AW_SYSCTRL, &value)) {
		return false;
	}
	return put(a, AW_SYSCTRL, set ? (value | bits) : (value & ~bits));
}

/* The volume register at `volume' eighths of a dB of attenuation (the driver's aw_pid_2113_set_volume()). */
static bool
set_volume(struct amplifier *a, uint16_t volume)
{
	uint16_t value;

	if (volume > VOLUME_LOWEST) {
		volume = VOLUME_LOWEST;
	}
	if (volume < a->volume_at) {
		volume = a->volume_at;  /* never louder than Android */
	}
	/* (the other bits as the profile has them and as they were written: not read first, for a quick fade) */
	value = (uint16_t)(a->volume_high | (volume / VOLUME_STEP) << 6 | volume % VOLUME_STEP);
	if (broken || !put(a, AW_SYSCTRL2, value)) {
		return false;
	}
	a->volume = volume;
	return true;
}

/* What the status register says, and whether it says a fault. */
static bool
faulty(struct amplifier *a, uint16_t *status)
{
	if (!get(a, AW_SYSST, status)) {
		return true;
	}
	return (*status & SYSST_FAULTS) != 0;
}

bool
hk_speaker_prepare(const uint8_t *acf, uint64_t size)
{
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	broken = false;
	say("speaker: the amplifiers are prepared as Android's driver prepares them; the I2C pins as found:\n");
	show_pins();
	if (!acf_profiles(acf, size)) {
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t id = 0, h4 = 0, h3 = 0, l4 = 0, l3 = 0, icalk_bits, vcalk_bits;
		int icalk, vcalk;
		uint16_t vcalb, ef_or = 0;

		if (!get(a, AW_ID, &id)) {
			say("speaker: nobody at I2C address %#x: not powered, or not there\n", a->address);
			power_down_now("an amplifier does not answer");
			return false;
		}
		say("speaker: the amplifier %s: chip ID %#06x, %s\n", a->where, id, amplifier_name(id));
		if (id != AW_ID_AW88261) {
			return false;
		}
		if (!get(a, AW_EFRH4, &h4) || !get(a, AW_EFRH3, &h3) || !get(a, AW_EFRL4, &l4) || !get(a, AW_EFRL3, &l3)) {
			return false;
		}
		for (unsigned r = 0; r < a->count; r++) {
			if (a->profile[r][0] == AW_DBGCTRL) {
				ef_or = a->profile[r][1] & DBGCTRL_EF_OR;
			}
		}
		/* the driver's aw_dev_get_icalk(), aw_dev_get_vcalk() and aw_dev_set_vcalb() */
		icalk_bits = ef_or ? ((h4 & 0x3ff) | (l4 & 0x3ff)) : ((h4 & 0x3ff) & (l4 & 0x3ff));
		vcalk_bits = ef_or ? ((h3 & 0x3ff) | (l3 & 0x3ff)) : ((h3 & 0x3ff) & (l3 & 0x3ff));
		icalk = 1000 + ((icalk_bits & 0x200) ? (int)icalk_bits - 0x400 : (int)icalk_bits);
		vcalk = 1000 + ((vcalk_bits & 0x200) ? (int)vcalk_bits - 0x400 : (int)vcalk_bits);
		if (vcalk <= 0 || icalk <= 0) {
			say("speaker: the amplifier %s: fuses %#x %#x %#x %#x make no sense\n", a->where, h4, h3, l4, l3);
			return false;
		}
		vcalb = (uint16_t)(8192 * icalk / vcalk);
		say("speaker: the amplifier %s: fuses %#06x %#06x %#06x %#06x: icalk %d, vcalk %d, so 0x50 is to be %#06x\n", a->where,
		    h4, h3, l4, l3, icalk, vcalk, vcalb);

		say("speaker: the amplifier %s: soft reset (0x00 <- 0x55aa), as the driver does it first\n", a->where);
		a->prepared = true;
		if (!register_write(a->address, AW_ID, AW_SOFT_RESET)) {
			power_down_now("the soft reset was not acknowledged");
			return false;
		}
		wait_ms(2);
		for (unsigned r = 0; r < a->count; r++) {
			uint8_t reg = (uint8_t)a->profile[r][0];
			uint16_t value = a->profile[r][1], now;

			switch (reg) {
			case AW_SYSINTM:
				value = SYSINTM_DEFAULT;
				break;
			case AW_SYSCTRL:
				/* the three power and mute bits stay as they are; the power stage's mute on */
				if (!get(a, AW_SYSCTRL, &now)) {
					power_down_now("0x04 could not be read");
					return false;
				}
				value = (uint16_t)((value & ~(SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE)) |
				    (now & (SYSCTRL_PWDN | SYSCTRL_AMPPD | SYSCTRL_HMUTE)) | SYSCTRL_ULS_HMUTE);
				break;
			case AW_I2SCTRL3:
				value &= (uint16_t)~I2SCTRL3_TXEN;
				break;
			case AW_VSNTM1:
				continue;
			}
			if (!put(a, reg, value)) {
				return false;
			}
		}
		if (!put(a, AW_VSNTM1, vcalb) || !set_volume(a, VOLUME_LOWEST)) {
			return false;
		}
	}
	say("speaker: BOTH AMPLIFIERS ARE CONFIGURED as Android's Music profile has them: powered down, muted, at the lowest volume\n");
	return true;
}

bool
hk_speaker_start(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t status = 0, interrupts = 0;
		unsigned tries;

		if (broken || !a->prepared) {
			return false;
		}
		say("speaker: the amplifier %s: power-down off\n", a->where);
		a->started = true;
		if (!sysctrl(a, SYSCTRL_PWDN, false)) {
			return false;
		}
		wait_ms(2);
		for (tries = 0; tries < STATUS_TRIES; tries++) {
			if (!get(a, AW_SYSST, &status)) {
				power_down_now("the status could not be read");
				return false;
			}
			if ((status & SYSST_PLL_CHECK) == SYSST_PLL_CHECK) {
				break;
			}
			wait_ms(2);
		}
		say("speaker: the amplifier %s: status %#06x: its PLL %s\n", a->where, status, tries < STATUS_TRIES ?
		    "IS LOCKED to the bit clock" : "DOES NOT LOCK: no clock?");
		if (tries == STATUS_TRIES) {
			power_down_now("an amplifier's PLL does not lock");
			return false;
		}
		say("speaker: the amplifier %s: amplifier power-down off\n", a->where);
		if (!sysctrl(a, SYSCTRL_AMPPD, false)) {
			return false;
		}
		wait_ms(1);
		for (tries = 0; tries < STATUS_TRIES; tries++) {
			if (faulty(a, &status)) {
				break;
			}
			if ((status & SYSST_CHECK) == SYSST_CHECK) {
				break;
			}
			wait_ms(2);
		}
		say("speaker: the amplifier %s: status %#06x\n", a->where, status);
		if ((status & SYSST_FAULTS) != 0 || (status & SYSST_CHECK) != SYSST_CHECK) {
			power_down_now((status & SYSST_FAULTS) ? "an amplifier reports a fault" : "an amplifier's status is not as "
			    "the driver wants it");
			return false;
		}
		if (!put(a, AW_I2SCTRL3, a->i2sctrl3) ||        /* the feedback on, as the profile has it */
		    !sysctrl(a, SYSCTRL_ULS_HMUTE, false) || !sysctrl(a, SYSCTRL_HMUTE, false)) {
			return false;
		}
		if (!get(a, AW_SYSINT, &interrupts) || !get(a, AW_SYSINT, &interrupts) || !put(a, AW_SYSINTM, SYSINTM_DEFAULT)) {
			power_down_now("the interrupts could not be read");
			return false;
		}
		if (faulty(a, &status)) {
			say("speaker: the amplifier %s: status %#06x\n", a->where, status);
			power_down_now("an amplifier reports a fault");
			return false;
		}
		say("speaker: THE AMPLIFIER %s RUNS, unmuted at the lowest volume (status %#06x)\n", a->where, status);
	}
	return true;
}

bool
hk_speaker_volume(uint32_t below, uint32_t step_ms)
{
	uint16_t status;

	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint32_t target = a->volume_at + below;

		if (broken || !a->started) {
			return false;
		}
		if (target > VOLUME_LOWEST) {
			target = VOLUME_LOWEST;
		}
		say("speaker: the amplifier %s: the volume to -%u.%03u dB (Android's level less %u.%03u dB), 6 dB a step every %u ms\n",
		    a->where, target / 8, target % 8 * 125, (target - a->volume_at) / 8, (target - a->volume_at) % 8 * 125, step_ms);
	}
	/* both together, a step every step_ms, the status looked at after every step */
	for (;;) {
		bool moved = false;

		for (unsigned i = 0; i < 2; i++) {
			struct amplifier *a = &amplifiers[i];
			uint32_t target = a->volume_at + below, next;

			if (target > VOLUME_LOWEST) {
				target = VOLUME_LOWEST;
			}
			if (a->volume == target) {
				continue;
			}
			next = a->volume > target ? (a->volume - target > VOLUME_STEP ? a->volume - VOLUME_STEP : target) :
			    (target - a->volume > VOLUME_STEP ? a->volume + VOLUME_STEP : target);
			if (!set_volume(a, (uint16_t)next)) {
				return false;
			}
			/* (a slow fade looks at the status after every step, a quick one once it is over) */
			if (step_ms >= 10 && faulty(a, &status)) {
				say("speaker: the amplifier %s: status %#06x\n", a->where, status);
				power_down_now("an amplifier reports a fault");
				return false;
			}
			moved = true;
		}
		if (!moved) {
			break;
		}
		if (step_ms) {
			wait_ms(step_ms);
		}
	}
	return hk_speaker_healthy();
}

bool
hk_speaker_healthy(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t status;

		if (broken) {
			return false;
		}
		if (!a->started) {
			continue;
		}
		if (faulty(a, &status)) {
			say("speaker: the amplifier %s: status %#06x\n", a->where, status);
			power_down_now("an amplifier reports a fault");
			return false;
		}
	}
	return true;
}

/* the row of a monitor table that a value falls in; between rows, the one before */
static int
monitor_row(const struct amplifier *a, bool temperature, int value, int before)
{
	unsigned count = temperature ? a->temp_count : a->vbat_count;

	for (unsigned i = 0; i < count; i++) {
		int min = temperature ? a->temp_rows[i].min : a->vbat_rows[i].min;
		int max = temperature ? a->temp_rows[i].max : a->vbat_rows[i].max;

		if (value >= min && value <= max) {
			return (int)i;
		}
	}
	return before;
}

/*
 * The driver's monitor, once: the temperature and the battery voltage of every amplifier that runs (the average of
 * the last three looks, as the file's count says), and the reduction their tables ask for, the larger of the two
 * amplifiers'. With `tell', what was read (also the boosted supply and the current and voltage sense, raw).
 */
uint32_t
hk_speaker_monitor(bool tell)
{
	uint32_t wanted = 0;

	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];
		uint16_t vbat = 0, temp = 0, pvdd = 0, isn = 0, vsn = 0;
		int t = 0, v = 0;
		uint32_t less = 0;

		if (broken || !a->started) {
			continue;
		}
		if (!get(a, AW_VBAT, &vbat) || !get(a, AW_TEMP, &temp)) {
			continue;
		}
		a->temps[a->looks % 3] = (temp & 0x200) ? (int)(temp & 0x3ff) - 0x400 : (int)(temp & 0x3ff);
		a->vbats[a->looks % 3] = (int)(vbat & 0x3ff) * 6025 / 1023;
		a->looks++;
		for (unsigned k = 0; k < 3 && k < a->looks; k++) {
			t += a->temps[k];
			v += a->vbats[k];
		}
		t /= (int)(a->looks < 3 ? a->looks : 3);
		v /= (int)(a->looks < 3 ? a->looks : 3);
		a->temp_row = monitor_row(a, true, t, a->temp_row);
		a->vbat_row = monitor_row(a, false, v, a->vbat_row);
		less = (a->temp_row >= 0 ? a->temp_rows[a->temp_row].less : 0) + (a->vbat_row >= 0 ? a->vbat_rows[a->vbat_row].less : 0);
		if (less > wanted) {
			wanted = less;
		}
		if (tell) {
			(void)get(a, AW_PVDD, &pvdd);
			(void)get(a, AW_ISNDAT, &isn);
			(void)get(a, AW_VSNDAT, &vsn);
			say("speaker: the amplifier %s: %d degrees, the battery %d mV (%#05x, %#05x; supply %#05x, sense %#06x %#06x): "
			    "%u.%03u dB less\n", a->where, t, v, temp, vbat, pvdd, isn, vsn, less / 8, less % 8 * 125);
		}
	}
	return wanted;
}

void
hk_speaker_stop(void)
{
	if (broken) {
		return;                 /* (already powered down) */
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amplifier *a = &amplifiers[i];

		if (!a->started) {
			continue;
		}
		say("speaker: the amplifier %s is stopped as Android's driver stops it\n", a->where);
		if (!sysctrl(a, SYSCTRL_ULS_HMUTE, true)) {
			return;
		}
		while (a->volume < VOLUME_LOWEST) {
			if (!set_volume(a, (uint16_t)(a->volume + VOLUME_STEP > VOLUME_LOWEST ? VOLUME_LOWEST : a->volume +
			    VOLUME_STEP))) {
				return;
			}
			wait_ms(1);
		}
		if (!sysctrl(a, SYSCTRL_HMUTE, true) || !put(a, AW_I2SCTRL3, (uint16_t)(a->i2sctrl3 & ~I2SCTRL3_TXEN))) {
			return;
		}
		wait_ms(1);
		if (!sysctrl(a, SYSCTRL_AMPPD, true) || !sysctrl(a, SYSCTRL_PWDN, true)) {
			return;
		}
		a->started = false;
		say("speaker: THE AMPLIFIER %s IS POWERED DOWN\n", a->where);
	}
}

void
hk_speaker_quiesce(void)
{
	bool any = false;

	for (unsigned i = 0; i < 2; i++) {
		any |= amplifiers[i].started;
	}
	if (!any || tlmm == 0) {
		return;
	}
	leaving = true;
	power_down_now("the kernel is on its way out");
}

/* ---- speaker_probe=N: a look ------------------------------------------------------------------------ */

static void
show_amplifiers(void)
{
	/* the chip ID, then registers that every chip of the family has at the same place and that reading does not change */
	static const struct {
		uint8_t reg;
		const char *name;
	} more[] = { { 0x01, "status" }, { 0x04, "system control" }, { 0x06, "I2S control" } };

	for (unsigned i = 0; i < 2; i++) {
		uint16_t id = 0, value = 0;

		if (!register_read(amplifiers[i].address, AW_ID, &id)) {
			say("speaker: nobody at I2C address %#x: not powered, or not there\n", amplifiers[i].address);
			continue;
		}
		say("speaker: the amplifier %s is there: chip ID %#06x, %s\n", amplifiers[i].where, id, amplifier_name(id));
		for (unsigned r = 0; r < sizeof(more) / sizeof(more[0]); r++) {
			if (register_read(amplifiers[i].address, more[r].reg, &value)) {
				say("speaker:   register %#04x (%s) = %#06x\n", more[r].reg, more[r].name, value);
			}
		}
	}
}

/* ---- power names of the audio subsystem ------------------------------------------------------------- */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static void
show_power(void)
{
	/* the subsystem's two supplies and, for the DSP, the processor's own two and the crystal */
	static const char *const supplies[] = { "lcx.lvl", "lmx.lvl", "cx.lvl", "mx.lvl", "xo.lvl" };

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		uint32_t address = rpmh_address(supplies[i]);

		if (address) {
			say("speaker: supply \"%s\" can be asked for at RPMh address %#x\n", supplies[i], address);
		} else {
			say("speaker: no supply \"%s\" in the command database\n", supplies[i]);
		}
	}
	/*
	 * (The audio subsystem's bus clock, GCC_CFG_NOC_LPASS_CBCR, is not read: on the phone that read
	 * stops the kernel with a synchronous external abort -- the register is not the main processors'
	 * to read while the audio subsystem belongs to its DSP.)
	 */
}

static void
probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */
	say("speaker: a look at the phone's sound hardware (level %u; nothing is switched on)\n", probe_level);
	show_pins();
	show_power();
	if (probe_level >= 2) {
		show_amplifiers();
	}
	say("speaker: done\n");
	thread_terminate(current_thread());
}

/*
 * UMI uses Cirrus CS35L41 amplifiers on QUPv3 SE3 (I2C addresses 0x40 and 0x42),
 * not HK's AW88261 devices on GPIOs 12/13. This boot-arg is a read-only
 * staged diagnostics. umi_amp_probe=1 reads TLMM without changing pins,
 * =2 writes back the unchanged GPIO119 config, =3 briefly releases the SE3
 * pins and checks their levels, and =4 uses the stock CS35L41 reset pulse
 * before reading IDs. Pins are restored.
 */
#define UMI_I2C_SDA             119
#define UMI_I2C_SCL             120
#define UMI_AMP_IRQ_RCV         112
#define UMI_AMP_IRQ_SPK         113
#define UMI_AMP_RESET           114
#define UMI_TLMM_NORTH_PHYS     0x0f900000UL /* SM8250 GPIO119/120 are on TLMM NORTH */
#define UMI_CS35L41_DEVID       0x00000000u
#define UMI_CS35L41_REVID       0x00000004u
#define UMI_CS35L41_CHIP_ID     0x00035a40u

static uint32_t umi_saved_cfg[2], umi_saved_io[2];
static uint32_t umi_amp_probe_level;

static void
umi_i2c_line(unsigned pin, bool release)
{
	/* GPIO function, 2 mA, no internal pull; the SE3 bus has board pull-ups. */
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), release ? 0 : CFG_OUTPUT);
}

static bool
umi_i2c_level(unsigned pin)
{
	return (rd(GPIO_IO(pin)) & 1) != 0;
}

static bool
umi_i2c_bus_take(void)
{
	unsigned pins[2] = { UMI_I2C_SDA, UMI_I2C_SCL };
	for (unsigned i = 0; i < 2; i++) {
		umi_saved_cfg[i] = rd(GPIO_CFG(pins[i]));
		umi_saved_io[i] = rd(GPIO_IO(pins[i]));
	}
	umi_i2c_line(UMI_I2C_SDA, true);
	umi_i2c_line(UMI_I2C_SCL, true);
	spin_us(10);
	if (!umi_i2c_level(UMI_I2C_SDA) || !umi_i2c_level(UMI_I2C_SCL)) {
		for (unsigned i = 0; i < 2; i++) {
			wr(GPIO_IO(pins[i]), umi_saved_io[i] & 2);
			wr(GPIO_CFG(pins[i]), umi_saved_cfg[i]);
		}
		return false;
	}
	return true;
}

static void
umi_i2c_bus_give(void)
{
	unsigned pins[2] = { UMI_I2C_SDA, UMI_I2C_SCL };
	for (unsigned i = 0; i < 2; i++) {
		wr(GPIO_IO(pins[i]), umi_saved_io[i] & 2);
		wr(GPIO_CFG(pins[i]), umi_saved_cfg[i]);
	}
}

static void
umi_i2c_start(void)
{
	umi_i2c_line(UMI_I2C_SDA, true);
	umi_i2c_line(UMI_I2C_SCL, true);
	spin_us(5);
	umi_i2c_line(UMI_I2C_SDA, false);
	spin_us(5);
	umi_i2c_line(UMI_I2C_SCL, false);
	spin_us(5);
}

static void
umi_i2c_stop(void)
{
	umi_i2c_line(UMI_I2C_SDA, false);
	spin_us(5);
	umi_i2c_line(UMI_I2C_SCL, true);
	spin_us(5);
	umi_i2c_line(UMI_I2C_SDA, true);
	spin_us(5);
}

static bool
umi_i2c_bit(bool output)
{
	umi_i2c_line(UMI_I2C_SDA, output);
	spin_us(5);
	umi_i2c_line(UMI_I2C_SCL, true);
	for (unsigned waited = 0; !umi_i2c_level(UMI_I2C_SCL) && waited < 100; waited++) {
		spin_us(5);
	}
	spin_us(5);
	bool input = umi_i2c_level(UMI_I2C_SDA);
	umi_i2c_line(UMI_I2C_SCL, false);
	return input;
}

static bool
umi_i2c_write_byte(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		umi_i2c_bit((byte >> bit) & 1);
	}
	return !umi_i2c_bit(true);
}

static uint8_t
umi_i2c_read_byte(bool more)
{
	uint8_t byte = 0;
	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | umi_i2c_bit(true));
	}
	umi_i2c_bit(!more);
	return byte;
}

static bool
umi_cs35l41_read(uint8_t address, uint32_t reg, uint32_t *value)
{
	bool ack = false;
	if (!umi_i2c_bus_take()) {
		say("umi-audio: SE3 bus is not idle (GPIO119=%u, GPIO120=%u); left untouched\n",
		    umi_i2c_level(UMI_I2C_SDA), umi_i2c_level(UMI_I2C_SCL));
		return false;
	}

	umi_i2c_start();
	ack = umi_i2c_write_byte((uint8_t)(address << 1));
	for (int shift = 24; ack && shift >= 0; shift -= 8) {
		ack = umi_i2c_write_byte((uint8_t)(reg >> shift));
	}
	if (ack) {
		umi_i2c_start();
		ack = umi_i2c_write_byte((uint8_t)(address << 1 | 1));
	}
	if (ack) {
		*value = (uint32_t)umi_i2c_read_byte(true) << 24;
		*value |= (uint32_t)umi_i2c_read_byte(true) << 16;
		*value |= (uint32_t)umi_i2c_read_byte(true) << 8;
		*value |= umi_i2c_read_byte(false);
	}
	umi_i2c_stop();
	umi_i2c_bus_give();
	return ack;
}

/* UMI firmware staging follows Cirrus wmfw.h/wm_adsp.c: HALO v3 packed
 * memories and WMDR coefficients. Only DSP memory is written in this test;
 * the core and output stages remain disabled after reset. */
extern const uint8_t *hk_image_file(const char *, const char *, uint64_t *, int *);

static uint32_t umi_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t umi_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool
umi_cs35l41_bytes(uint8_t address, uint32_t reg, uint8_t *bytes, uint32_t length, bool write)
{
	if (length == 0 || length > 60 || (length & 3) || (reg & 3) || !umi_i2c_bus_take()) {
		return false;
	}
	umi_i2c_start();
	bool ack = umi_i2c_write_byte(address << 1);
	for (int shift = 24; ack && shift >= 0; shift -= 8) {
		ack = umi_i2c_write_byte(reg >> shift);
	}
	if (!write && ack) {
		umi_i2c_start();
		ack = umi_i2c_write_byte(address << 1 | 1);
	}
	for (uint32_t i = 0; ack && i < length; i++) {
		if (write) {
			ack = umi_i2c_write_byte(bytes[i]);
		} else {
			bytes[i] = umi_i2c_read_byte(i + 1 < length);
		}
	}
	umi_i2c_stop();
	umi_i2c_bus_give();
	return ack;
}

static bool
umi_cs35l41_write(uint8_t address, uint32_t reg, uint32_t value)
{
	uint8_t data[4] = { value >> 24, value >> 16, value >> 8, value };
	bool ok = umi_cs35l41_bytes(address, reg, data, sizeof(data), true);
	if (!ok) say("umi-audio: amp %#x register write failed at %#x (value %#x)\n", address, reg, value);
	return ok;
}

static bool
umi_cs35l41_memory(uint8_t address, uint32_t reg, const uint8_t *data, uint32_t length)
{
	uint8_t chunk[60], readback[60];
	for (uint32_t done = 0; done < length;) {
		uint32_t n = length - done;
		if (n > sizeof(chunk)) n = sizeof(chunk);
		memcpy(chunk, data + done, n);
		if (!umi_cs35l41_bytes(address, reg + done, chunk, n, true) ||
		    !umi_cs35l41_bytes(address, reg + done, readback, n, false) || memcmp(chunk, readback, n)) {
			say("umi-audio: amp %#x memory verification failed at %#x\n", address, reg + done);
			return false;
		}
		done += n;
		/* Give the scheduler a chance between I2C bursts. */
		wait_ms(1);
	}
	return true;
}

/* Explicit memory windows: no firmware record may address amplifier controls. */
static bool
umi_cs35l41_region(uint32_t type, uint32_t offset, uint32_t length, bool coeff, uint32_t *reg)
{
	uint32_t base, limit, scale;
	switch (type) {
	case 0x10: base = 0x03800000; limit = 0x4fec; scale = 5; break;
	case 0x11: base = 0x02000000; limit = 0x2ff4; scale = 3; break;
	case 0x12: base = 0x02c00000; limit = 0x17f4; scale = 3; break;
	case 5: base = 0x02800000; limit = 0x3ff8; scale = 4; break;
	case 6: base = 0x03400000; limit = 0x1ff8; scale = 4; break;
	default: return false;
	}
	uint64_t at = (uint64_t)offset * scale;
	if (type == 0x11 || type == 0x12) at &= ~3ull;
	if ((type == 0x10 && length % 20) || ((type == 0x11 || type == 0x12) && length % 12)) return false;
	if (coeff) at = offset; /* WMDR's offset is already in bytes. */
	if ((at & 3) || (length & 3) || length == 0 || at > limit || length > limit - at) return false;
	*reg = base + (uint32_t)at;
	return true;
}

struct umi_cs_algorithm { uint32_t id, xm, ym; };
static uint32_t umi_halo_xm[2], umi_cspl_xm[2];
static bool umi_runtime_preparing;

static bool
umi_cs35l41_preload(uint8_t address)
{
	uint64_t size = 0, coeff_size = 0;
	const uint8_t *fw = NULL;
	for (unsigned retry = 0; retry < 100 && fw == NULL; retry++) {
		fw = hk_image_file("fw/cs35l41", "cs35l41-dsp1-spk-prot-revb2.wmfw", &size, NULL);
		if (fw == NULL) wait_ms(100);
	}
	const char *coeff_name = address == 0x42 ? "cs35l41-dsp1-spk-prot-RCV-revb2.bin" :
	    "cs35l41-dsp1-spk-prot-revb2.bin";
	const uint8_t *coeff = NULL;
	for (unsigned retry = 0; retry < 100 && coeff == NULL; retry++) {
		coeff = hk_image_file("fw/cs35l41", coeff_name, &coeff_size, NULL);
		if (coeff == NULL) wait_ms(100);
	}
	if (!fw || size < 40 || size > 128 * 1024 || memcmp(fw, "WMFW", 4) || umi_le32(fw + 4) != 40 ||
	    fw[10] != 4 || fw[11] != 3 || !coeff || coeff_size < 16 || coeff_size > 64 * 1024 ||
	    memcmp(coeff, "WMDR", 4) || umi_le32(coeff + 4) != 16 || coeff[11] != 1) {
		say("umi-audio: amp %#x: missing or unsupported HALO v3 / WMDR v1 firmware\n", address);
		return false;
	}
	/* Validate the entire WMFW before touching the chip. */
	unsigned regions = 0;
	for (uint64_t p = 40; p < size;) {
		if (size - p < 8) return false;
		uint32_t hdr = umi_le32(fw + p), length = umi_le32(fw + p + 4), reg;
		uint32_t type = hdr >> 24;
		if (length > size - p - 8) return false;
		if (type == 0xf2 || type == 0xfe || type == 0xff) {
			/* Control descriptions and text, never register writes. */
		} else if (!umi_cs35l41_region(type, hdr & 0xffffff, length, false, &reg)) {
			say("umi-audio: invalid firmware memory record type %#x offset %#x length %u\n", type, hdr & 0xffffff, length);
			return false;
		} else {
			regions++;
		}
		p += 8 + length;
	}
	uint32_t value;
	if (!regions || !umi_cs35l41_read(address, 0x0001001c, &value) || !(value & 2) ||
	    !umi_cs35l41_read(address, 0x00010018, &value) || (value & 0x80000000) ||
	    !umi_cs35l41_read(address, 0x00002014, &value) || (value & 1) ||
	    !umi_cs35l41_write(address, 0x02bc1000, 0) ||
	    !umi_cs35l41_read(address, 0x02bc1000, &value) || value != 0) {
		say("umi-audio: amp %#x: OTP/core/power prerequisites failed\n", address);
		return false;
	}
	say("umi-audio: amp %#x: loading %u HALO memory regions, with full readback; output remains disabled\n", address, regions);
	for (uint64_t p = 40; p < size;) {
		uint32_t hdr = umi_le32(fw + p), length = umi_le32(fw + p + 4), reg;
		uint32_t typ = hdr >> 24;
		bool region_ok = umi_cs35l41_region(typ, hdr & 0xffffff, length, false, &reg);
		if (region_ok) {
			/* XM/YM live in window 0; the algorithm regions (5, 6, 0x10) live in window 1. */
			uint32_t win = (typ == 0x10 || typ == 5 || typ == 6) ? 1 : 0;
			if (!umi_cs35l41_write(address, 0x02bc1000, win)) return false;
			if (!umi_cs35l41_memory(address, reg, fw + p + 8, length)) {
				say("umi-audio: amp %#x: writing %u bytes at %#x (type %#x) failed\n", address,
				    length, reg, typ);
				return false;
			}
			say("umi-audio: amp %#x: verified %u firmware bytes at %#x\n", address, length, reg);
		}
		p += 8 + length;
	}
	uint8_t header[40];
	if (!umi_cs35l41_write(address, 0x02bc1000, 1)) {
		say("umi-audio: amp %#x: window switch to 1 failed\n", address);
		return false;
	}
	wait_ms(30);
	if (!umi_cs35l41_bytes(address, 0x02800000, header, 4, false)) {
		say("umi-audio: amp %#x: read of the HALO header (4 bytes) at 0x02800000 failed\n", address);
		return false;
	}
	if (!umi_cs35l41_bytes(address, 0x02800004, header + 4, 36, false)) {
		say("umi-audio: amp %#x: read of the HALO header tail (36 bytes) at 0x02800004 failed\n", address);
		return false;
	}
	uint32_t algorithms = umi_be32(header + 36), fw_id = umi_be32(header + 12);
	{
		uint8_t probe[40];
		uint32_t w;

		say("umi-audio: amp %#x: ALGS win1 %08x %08x %08x %08x %08x\n", address,
		    umi_be32(header), umi_be32(header + 4), umi_be32(header + 8),
		    umi_be32(header + 12), umi_be32(header + 16));
		if (umi_cs35l41_read(address, 0x02bc1000, &w)) {
			say("umi-audio: amp %#x: window register reads %#x\n", address, w);
		}
		if (umi_cs35l41_write(address, 0x02bc1000, 0)) {
			if (umi_cs35l41_bytes(address, 0x02800000, probe, sizeof(probe), false)) {
				say("umi-audio: amp %#x: ALGS win0 %08x %08x %08x %08x %08x\n", address,
				    umi_be32(probe), umi_be32(probe + 4), umi_be32(probe + 8),
				    umi_be32(probe + 12), umi_be32(probe + 16));
			}
			if (umi_cs35l41_bytes(address, 0x02000000, probe, sizeof(probe), false)) {
				say("umi-audio: amp %#x: XM win0 %08x %08x %08x %08x\n", address,
				    umi_be32(probe), umi_be32(probe + 4), umi_be32(probe + 8),
				    umi_be32(probe + 12));
			}
		}
		if (umi_cs35l41_write(address, 0x02bc1000, 1)) {
			if (umi_cs35l41_bytes(address, 0x02000000, probe, sizeof(probe), false)) {
				say("umi-audio: amp %#x: XM win1 %08x %08x %08x %08x\n", address,
				    umi_be32(probe), umi_be32(probe + 4), umi_be32(probe + 8),
				    umi_be32(probe + 12));
			}
			if (umi_cs35l41_bytes(address, 0x02c00000, probe, 12, false)) {
				say("umi-audio: amp %#x: YM win1 %08x %08x %08x\n", address,
				    umi_be32(probe), umi_be32(probe + 4), umi_be32(probe + 8));
			}
			if (umi_cs35l41_bytes(address, 0x03800000, probe, 12, false)) {
				say("umi-audio: amp %#x: PM win1 %08x %08x %08x\n", address,
				    umi_be32(probe), umi_be32(probe + 4), umi_be32(probe + 8));
			}
		}
	}
	if ((umi_be32(header + 4) >> 16) != 3 || algorithms > 16) {
		say("umi-audio: amp %#x: unexpected HALO algorithm header (block %#x, count %u)\n", address,
		    umi_be32(header + 4), algorithms);
		return false;
	}
	struct umi_cs_algorithm alg[17];
	alg[0] = (struct umi_cs_algorithm){ fw_id, umi_be32(header + 20), umi_be32(header + 28) };
	for (uint32_t i = 0; i < algorithms; i++) {
		uint8_t entry[24];
		if (!umi_cs35l41_bytes(address, 0x02800028 + i * 24, entry, sizeof(entry), false)) {
			say("umi-audio: amp %#x: read of algorithm entry %u at %#x failed\n", address, i,
			    0x02800028 + i * 24);
			return false;
		}
		alg[i + 1] = (struct umi_cs_algorithm){ umi_be32(entry), umi_be32(entry + 8), umi_be32(entry + 16) };
	}
	if (umi_runtime_preparing && (fw_id != 0x400a4 || umi_be32(header + 16) != 0x300 ||
	    algorithms != 1 || alg[1].id != 0xcd)) {
		/* This is the phone's own vendor firmware (fw-audio/cs35l41, out of its vendor image),
		 * so a different id or version is reported but not treated as fatal any more. */
		say("umi-audio: amp %#x: firmware differs from the validated revision (fw %#x ver %#x, "
		    "%u algorithms), going on\n", address, fw_id, umi_be32(header + 16), algorithms);
	}
	for (uint32_t i = 0; i <= algorithms; i++) {
		if (alg[i].xm > 4082 || alg[i].ym > 2046) {
			say("umi-audio: amp %#x: algorithm %u memory out of range (xm %u ym %u)\n", address, i,
			    alg[i].xm, alg[i].ym);
			return false;
		}
	}
	umi_halo_xm[address == 0x42] = 0x02800000 + 4 * alg[0].xm;
	for (uint32_t i = 1; i <= algorithms; i++) {
		if (alg[i].id == 0xcd) umi_cspl_xm[address == 0x42] = 0x02800000 + 4 * alg[i].xm;
	}
	say("umi-audio: amp %#x: HALO firmware %#x version %#x, %u algorithms\n", address, fw_id,
	    umi_be32(header + 16), algorithms);
	/* Two passes also validate every coefficient record before any tuning is applied. */
	if (!umi_cs35l41_write(address, 0x02bc1000, 0)) return false;
	for (unsigned pass = 0; pass < 2; pass++) {
		for (uint64_t p = 16; p < coeff_size;) {
			if (coeff_size - p < 20) return false;
			uint32_t offset = coeff[p] | (uint32_t)coeff[p + 1] << 8;
			uint32_t type = coeff[p + 2] | (uint32_t)coeff[p + 3] << 8;
			uint32_t id = umi_le32(coeff + p + 4), length = umi_le32(coeff + p + 16), reg = 0;
			if (length > coeff_size - p - 20) return false;
			if (type != 0xfe00 && type != 0xff00) {
				uint32_t a;
				for (a = 0; a <= algorithms && alg[a].id != id; a++) {}
				if (a > algorithms) {
					say("umi-audio: amp %#x: coefficient algorithm %#x missing\n", address, id);
					return false;
				}
				uint32_t base = type == 5 || type == 0x11 ? alg[a].xm : alg[a].ym;
				uint64_t at = (uint64_t)base * (type == 5 || type == 6 ? 4 : 3);
				if (type == 0x11 || type == 0x12) at &= ~3ull;
				at += offset;
				if (at > UINT32_MAX || !umi_cs35l41_region(type, (uint32_t)at, length, true, &reg)) return false;
				if (pass) {
					uint32_t win = (type == 5 || type == 6 || type == 0x10) ? 1 : 0;
					if (!umi_cs35l41_write(address, 0x02bc1000, win)) return false;
					if (!umi_cs35l41_memory(address, reg, coeff + p + 20, length)) return false;
					say("umi-audio: amp %#x: verified %u tuning bytes at %#x\n", address, length, reg);
				}
			}
			p = (p + 20 + length + 3) & ~3ull;
			if (p > coeff_size) return false;
		}
	}
	say("umi-audio: amp %#x: protection firmware and %s verified in DSP memory; core stopped, speaker disabled\n",
	    address, coeff_name);
	return true;
}

static bool
umi_cs35l41_update(uint8_t a, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t old = 0, check = 0;
	bool ok = umi_cs35l41_read(a, reg, &old) && umi_cs35l41_write(a, reg, (old & ~mask) | (value & mask)) &&
	    umi_cs35l41_read(a, reg, &check) && (check & mask) == (value & mask);
	if (!ok) say("umi-audio: amp %#x register update failed at %#x: old %#x, readback %#x, mask %#x, wanted %#x\n",
	    a, reg, old, check, mask, value);
	return ok;
}

#include "umi_cs35l41_otp.h"
#include "umi_cs35l41_audio.h"

static bool umi_unit_calibrated[2];

static bool
umi_cs35l41_trims(uint8_t a)
{
	uint32_t id, words[32];
	if (!umi_cs35l41_read(a, 0x10, &id)) return false;
	const struct umi_cs_otp_entry *map;
	unsigned count;
	if (id == 1 || id == 8) {
		map = umi_otp_map_1;
		count = sizeof(umi_otp_map_1) / sizeof(umi_otp_map_1[0]);
	} else if (id == 2 || id == 6) {
		map = umi_otp_map_2;
		count = sizeof(umi_otp_map_2) / sizeof(umi_otp_map_2[0]);
	} else {
		say("umi-audio: amp %#x unsupported OTP trim map %#x\n", a, id);
		return false;
	}
	for (unsigned i = 0; i < 32; i++) {
		if (!umi_cs35l41_read(a, 0x400 + 4 * i, &words[i])) return false;
	}
	if (!umi_cs35l41_write(a, 0x40, 0x55) || !umi_cs35l41_write(a, 0x40, 0xaa)) return false;
	bool ok = true;
	unsigned bit = 80; /* Maps 1/2/6/8 start at OTP word 2, bit 16. */
	for (unsigned i = 0; i < count; i++) {
		unsigned size = map[i].size, shift = map[i].shift;
		if (!size || size > 32 || shift + size > 32 || bit + size > 1024) { ok = false; break; }
		uint64_t pair = words[bit / 32];
		if (bit % 32 + size > 32) pair |= (uint64_t)words[bit / 32 + 1] << 32;
		uint32_t mask = (uint32_t)((1ull << size) - 1);
		uint32_t value = (uint32_t)(pair >> (bit % 32)) & mask;
		bit += size;
		if (map[i].reg && !umi_cs35l41_update(a, map[i].reg, mask << shift, value << shift)) {
			ok = false;
			break;
		}
	}
	bool lock = umi_cs35l41_write(a, 0x40, 0xcc) && umi_cs35l41_write(a, 0x40, 0x33);
	say("umi-audio: amp %#x OTP map %#x: %u trim fields, %s\n", a, id, count, ok && lock ? "applied" : "failed");
	return ok && lock;
}

static bool
umi_cs35l41_board_setup(uint8_t a)
{
	uint32_t status = 0;
	unsigned waited;
	for (waited = 0; waited < 100; waited++) {
		if (!umi_cs35l41_read(a, 0x1001c, &status)) return false;
		if (status & 2) break;
		wait_ms(1);
	}
	if (waited == 100 || !umi_cs35l41_read(a, 0x10018, &status) || (status & 0x80000000)) return false;
	/* Rev B2 errata sequence from Xiaomi's Cirrus driver. */
	static const struct { uint32_t reg, value; } patch[] = {
		{0x40, 0x5555}, {0x40, 0xaaaa}, {0x4100, 0}, {0x4310, 0}, {0x4400, 0},
		{0x381c, 0x51}, {0x02bc20e0, 0}, {0x02bc2020, 0}, {0x4854, 0x01010000},
		{0x40, 0xcccc}, {0x40, 0x3333},
	};
	for (unsigned i = 0; i < sizeof(patch) / sizeof(patch[0]); i++) {
		if (!umi_cs35l41_write(a, patch[i].reg, patch[i].value)) return false;
	}
	if (!umi_cs35l41_trims(a)) return false;
	/* UMI device tree: 1 uH, 15 uF, 4000 mA peak; brownout defaults from
	 * cs35l41_probe. Configure limits without enabling the output or boost. */
	return umi_cs35l41_update(a, 0x3810, 0xffff, 0x2424) &&
	    umi_cs35l41_update(a, 0x3814, 0xff03, 0x7500) &&
	    umi_cs35l41_update(a, 0x3808, 0x7f, 0x40) &&
	    umi_cs35l41_update(a, 0x201c, 0x1000, 0x1000) &&
	    umi_cs35l41_write(a, 0x6404, 0x0200530e);
}

static bool
umi_cs35l41_mailbox(uint8_t a, uint32_t cmd, uint32_t expected)
{
	uint32_t status = 0;
	if (!umi_cs35l41_write(a, 0x00010814, 1u << 20) || /* IRQ2_STATUS2: clear DSP sticky */
	    !umi_cs35l41_write(a, 0x00010014, 1u << 21) || /* IRQ1_STATUS2: clear AP sticky */
	    !umi_cs35l41_update(a, 0x00010914, 1u << 20, 0) || /* IRQ2_MASK2 */
	    !umi_cs35l41_write(a, 0x00013020, cmd)) return false;
	bool ack = false;
	for (unsigned i = 0; i < 30; i++) {
		wait_ms(1);
		if (!umi_cs35l41_read(a, 0x00010014, &status)) break;
		if (status & (1u << 21)) { ack = true; break; }
	}
	umi_cs35l41_write(a, 0x00010014, 1u << 21);
	umi_cs35l41_update(a, 0x00010914, 1u << 20, 1u << 20);
	if (!umi_cs35l41_read(a, 0x00013004, &status)) return false;
	say("umi-audio: amp %#x HALO mailbox cmd %u: ack=%u state=%u (expected %u)\n", a, cmd, ack, status, expected);
	return ack && status == expected;
}

static bool
umi_cs35l41_dsp_boot(uint8_t a)
{
	/* 48 kHz I2S slave, 16-bit RX, BCLK reference 1.536 MHz (PLL code 0x1b).
	 * DSP-only test: GLOBAL_EN and AMP_EN remain zero, DAC is digitally muted. */
	if (!umi_cs35l41_write(a, 0x00006000, 0x2004) ||
	    !umi_cs35l41_update(a, 0x00002c0c, 0x1f, 3) ||
	    !umi_cs35l41_update(a, 0x00004808, 0xff000755, 0x10000200) ||
	    !umi_cs35l41_update(a, 0x00004840, 0x3f, 16) ||
	    !umi_cs35l41_update(a, 0x00004820, 0x3f3f, a == 0x40 ? 1 : 0x100) ||
	    !umi_cs35l41_write(a, 0x00004c40, 8) || !umi_cs35l41_write(a, 0x00004c44, 8) ||
	    !umi_cs35l41_update(a, 0x00004800, 0x30000, 0x10000) ||
	    !umi_cs35l41_update(a, 0x00002c04, 0x800, 0x800) ||
	    !umi_cs35l41_update(a, 0x00002c04, 0x7e0, 0x1b << 5) ||
	    !umi_cs35l41_update(a, 0x00002c04, 0x10, 0) ||
	    !umi_cs35l41_update(a, 0x00002c04, 7, 0) ||
	    !umi_cs35l41_update(a, 0x00002c04, 0x800, 0) ||
	    !umi_cs35l41_update(a, 0x00002c04, 0x10010, 0x10010)) return false;
	uint32_t xm_bank, ym_bank, xm_size, ym_size;
	if (!umi_cs35l41_read(a, 0x025e0038, &xm_bank) || !umi_cs35l41_read(a, 0x025e003c, &ym_bank) ||
	    !umi_cs35l41_read(a, 0x025e0010, &xm_size) || !umi_cs35l41_read(a, 0x025e0018, &ym_size) ||
	    !xm_bank || !ym_bank || xm_size % xm_bank || ym_size % ym_bank ||
	    xm_size / xm_bank == 0 || xm_size / xm_bank > 31 || ym_size / ym_bank == 0 || ym_size / ym_bank > 31) {
		say("umi-audio: amp %#x invalid HALO memory-bank geometry\n", a);
		return false;
	}
	say("umi-audio: amp %#x HALO geometry: XM %u/%u, YM %u/%u\n", a, xm_size, xm_bank, ym_size, ym_bank);
	const uint32_t dsp = 0x02b80000;
	for (unsigned i = 0; i < 8; i++) {
		if (!umi_cs35l41_update(a, dsp + 0x80 + 8 * i, 0x1f, 1) ||
		    !umi_cs35l41_update(a, dsp + 0x280 + 8 * i, 0x1f, 1)) return false;
	}
	/* Clear HALO stream arbitration before its firmware owns it. */
	for (unsigned i = 0; i < 6; i++) {
		for (unsigned j = 0; j < 3; j++) {
			if (!umi_cs35l41_write(a, dsp + 0x45000 + 16 * i + 4 * j, 0)) return false;
		}
	}
	for (unsigned i = 0; i < 8; i++) {
		if (!umi_cs35l41_write(a, dsp + 0x45200 + 8 * i, 0xfc) ||
		    !umi_cs35l41_write(a, dsp + 0x45400 + 8 * i, 0xfc) ||
		    !umi_cs35l41_write(a, dsp + 0x45600 + 16 * i, 0xfc) ||
		    !umi_cs35l41_write(a, dsp + 0x45604 + 16 * i, 0)) return false;
	}
	if (!umi_cs35l41_write(a, dsp + 0x46020, 0xffffff) ||
	    !umi_cs35l41_write(a, dsp + 0x46008, 0) ||
	    !umi_cs35l41_write(a, dsp + 0x43140, 0x5555) ||
	    !umi_cs35l41_write(a, dsp + 0x43140, 0xaaaa) ||
	    !umi_cs35l41_write(a, dsp + 0x43000, (1u << (xm_size / xm_bank)) - 1) ||
	    !umi_cs35l41_write(a, dsp + 0x43004, (1u << (ym_size / ym_bank)) - 1)) return false;
	static const uint32_t restricted[] = {
		0x43008, 0x4300c, 0x43014, 0x43018, 0x4301c, 0x43020, 0x43024, 0x4302c,
		0x43030, 0x43034, 0x43038, 0x4303c, 0x43044, 0x43048, 0x4304c, 0x43050, 0x43054, 0x4305c,
	};
	for (unsigned i = 0; i < sizeof(restricted) / sizeof(restricted[0]); i++) {
		if (!umi_cs35l41_write(a, dsp + restricted[i], 0)) return false;
	}
	if (umi_runtime_preparing) {
		uint32_t xm = umi_cspl_xm[a == 0x42];
		/* Persisted Android resistance is a little-endian uint32 per amp.
		 * Keep CAL_AMBIENT from this amp's stock protection tuning, as Android does. */
		uint64_t cal_size = 0;
		const char *cal_name = a == 0x42 ? "cs35l41_cal_right.bin" : "cs35l41_cal.bin";
		const uint8_t *cal = hk_image_file("fw/cs35l41", cal_name, &cal_size, NULL);
		uint32_t resistance = cal && cal_size == 4 ? umi_le32(cal) : 0;
		umi_unit_calibrated[a == 0x42] = resistance >= 2000 && resistance <= 30000;
		if (!umi_unit_calibrated[a == 0x42]) resistance = 8392;
		say("umi-audio: amp %#x: %s calibration R=%u; stock %s channel\n", a,
		    umi_unit_calibrated[a == 0x42] ? "per-unit" : "fallback", resistance,
		    a == 0x42 ? "top/left" : "bottom/right");
		if (!xm || !umi_cs35l41_update(a, xm + 7 * 4, 0xffffff, resistance) ||
		    !umi_cs35l41_update(a, xm + 9 * 4, 0xffffff, 1) ||
		    !umi_cs35l41_update(a, xm + 10 * 4, 0xffffff, resistance + 1) ||
		    !umi_cs35l41_write(a, 0x4c50, 0x28) || !umi_cs35l41_write(a, 0x4c54, 0x21) ||
		    !umi_cs35l41_write(a, 0x4c58, 0x3a) || !umi_cs35l41_write(a, 0x4c5c, 0x3b)) return false;
	}
	if (!umi_cs35l41_write(a, dsp + 0x43140, 0) || !umi_cs35l41_write(a, 0x02bc1000, 1)) return false;
	wait_ms(30);
	uint32_t power, amp;
	if (!umi_cs35l41_read(a, 0x2014, &power) || !umi_cs35l41_read(a, 0x2018, &amp)) return false;
	say("umi-audio: amp %#x after HALO start: global power %#x, amplifier power %#x\n", a, power, amp);
	/* The protection firmware prepares AMP_EN/ADCs/boost itself (0x3321).
	 * GLOBAL_EN remains the host's output interlock throughout this test. */
	if (power & 1) return false;
	uint32_t heartbeat1 = 0, heartbeat2 = 0, cspl_state = 0, cspl_error = 0;
	uint32_t halo_xm = umi_halo_xm[a == 0x42], cspl_xm = umi_cspl_xm[a == 0x42];
	if (!halo_xm || !cspl_xm || !umi_cs35l41_read(a, halo_xm + 4, &heartbeat1)) return false;
	wait_ms(20);
	if (!umi_cs35l41_read(a, halo_xm + 4, &heartbeat2) ||
	    !umi_cs35l41_read(a, cspl_xm + 8, &cspl_state) || !umi_cs35l41_read(a, cspl_xm + 12, &cspl_error)) return false;
	say("umi-audio: amp %#x HALO heartbeat %u -> %u; CSPL state %u, error %#x\n", a,
	    heartbeat1, heartbeat2, cspl_state, cspl_error);
	/* Audio-frame heartbeat may pause while GLOBAL_EN is zero; mailbox ACK/state changes prove execution. */
	return umi_cs35l41_mailbox(a, 1, 1) && umi_cs35l41_mailbox(a, 2, 0);
}

static bool umi_output_ready;
static bool umi_output_on, umi_reset_asserted;
static uint32_t umi_reset_cfg, umi_reset_io;
static uint32_t umi_volume_attenuation = 320; /* register starts at -40 dB */

void
umi_speaker_quiesce(void)
{
	if (umi_output_ready && tlmm) {
		wr(GPIO_IO(UMI_AMP_RESET), 0);
		__asm__ volatile("dmb oshst" ::: "memory");
		umi_reset_asserted = true;
		umi_output_on = false;
	}
}

/* A preempted bit-banged read can NACK even though the amp is healthy.
 * Retry transport failures only; a reported protection fault is never retried. */
static bool
umi_cs35l41_health_read(uint8_t a, uint32_t reg, uint32_t *value)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (umi_cs35l41_read(a, reg, value)) {
			if (attempt) say("umi-audio: amp %#x recovered I2C read at %#x after %u retries\n", a, reg, attempt);
			return true;
		}
		wait_ms(2);
	}
	say("umi-audio: amp %#x I2C read failed three times at %#x; output stopped\n", a, reg);
	return false;
}

bool
umi_speaker_healthy(void)
{
	if (!umi_output_ready || umi_reset_asserted) return false;
	for (unsigned i = 0; i < 2; i++) {
		uint8_t a = i ? 0x42 : 0x40;
		uint32_t status = 0, error = 0;
		if (!umi_cs35l41_health_read(a, 0x10010, &status)) goto fault;
		if (status & 0x800281c0) {
			say("umi-audio: amp %#x hardware protection: IRQ1_STATUS1=%#x, faults=%#x\n",
			    a, status, status & 0x800281c0);
			goto fault;
		}
		if (!umi_cs35l41_health_read(a, umi_cspl_xm[i] + 12, &error)) goto fault;
		if (error) {
			say("umi-audio: amp %#x protection DSP error=%#x, IRQ1_STATUS1=%#x\n", a, error, status);
			goto fault;
		}
		continue;
	fault:
		say("umi-audio: both chips held in reset following amp %#x health failure\n", a);
		umi_speaker_quiesce();
		return false;
	}
	return true;
}

void
umi_speaker_stop(void)
{
	if (!umi_output_ready || !umi_output_on || umi_reset_asserted) return;
	for (unsigned i = 0; i < 2; i++) {
		uint8_t a = i ? 0x42 : 0x40;
		umi_cs35l41_write(a, 0x6000, 0x2004);
		umi_cs35l41_update(a, 0x2024, 0x10, 0x10);
	}
	wait_ms(5);
	for (unsigned i = 0; i < 2; i++) {
		uint8_t a = i ? 0x42 : 0x40;
		umi_cs35l41_mailbox(a, 1, 1);
		umi_cs35l41_update(a, 0x2014, 1, 0);
	}
	umi_output_on = false;
	say("umi-audio: outputs muted and global power off; HALO protection remains ready\n");
}

/* Keep the amp's DSP and calibration loaded across brief silences. iOS may
 * start a new stream at any time; only the run's final teardown resets chips. */
void
umi_speaker_shutdown(void)
{
	if (!umi_output_ready) return;
	umi_speaker_stop();
	if (!umi_reset_asserted) {
		wr(GPIO_IO(UMI_AMP_RESET), 0);
		__asm__ volatile("dmb oshst" ::: "memory");
		spin_us(2000);
	}
	wr(GPIO_IO(UMI_AMP_RESET), umi_reset_io & 2);
	wr(GPIO_CFG(UMI_AMP_RESET), umi_reset_cfg);
	umi_output_ready = false;
	umi_reset_asserted = false;
	say("umi-audio: shared reset and I2C pins restored\n");
}

bool
umi_speaker_start(void)
{
	if (!umi_output_ready || umi_reset_asserted) return false;
	if (umi_output_on) return umi_speaker_healthy();
	for (unsigned i = 0; i < 2; i++) {
		uint8_t a = i ? 0x42 : 0x40;
		/* Keep the analog mute asserted while power comes up. Firmware and
		 * calibration are already running; the host owns GLOBAL_EN. */
		if (!umi_cs35l41_update(a, 0x2024, 0x10, 0x10) ||
		    !umi_cs35l41_write(a, 0x6000, 0x2004) ||
		    !umi_cs35l41_write(a, 0x4c00, 0x32) || /* DAC from protection DSP */
		    !umi_cs35l41_update(a, 0x6c04, 0x3e0, 18u << 5) ||
		    !umi_cs35l41_update(a, 0x201c, 0x100010, 0x100010) ||
		    !umi_cs35l41_write(a, 0x10010, 0xffffffff) ||
		    !umi_cs35l41_write(a, 0x40, 0x55) || !umi_cs35l41_write(a, 0x40, 0xaa) ||
		    !umi_cs35l41_write(a, 0x2084, 0x002f1aa0) ||
		    !umi_cs35l41_write(a, 0x40, 0xcc) || !umi_cs35l41_write(a, 0x40, 0x33) ||
		    !umi_cs35l41_update(a, 0x2014, 1, 1)) goto fail;
		wait_ms(2);
		if (!umi_cs35l41_mailbox(a, 2, 0)) goto fail;
	}
	if (!umi_speaker_healthy()) goto fail;
	umi_volume_attenuation = 320;
	for (unsigned i = 0; i < 2; i++) {
		uint8_t a = i ? 0x42 : 0x40;
		/* Fixed initial test ceiling: -40 dB digital, 4 ms soft ramp.
		 * The test tone itself is another 20 dB below full scale. */
		if (!umi_cs35l41_update(a, 0x6000, 0x3fff, umi_cs35l41_volume_register(320)) ||
		    !umi_cs35l41_update(a, 0x2024, 0x10, 0)) goto fail;
	}
	umi_output_on = true;
	say("umi-audio: both protected outputs started at fixed -40 dB digital attenuation\n");
	return true;
fail:
	umi_speaker_quiesce();
	return false;
}

/* CS35L41 digital volume is centered at 0 dB in code 2048, in 1/8 dB
 * steps. The kernel's sound_below and the iOS slider are both attenuation;
 * keep the digital stage at or below unity and ramp in 6 dB steps. */
bool
umi_speaker_volume(uint32_t below, uint32_t step_ms)
{
	uint32_t target_attenuation = below > UMI_CS35L41_MAX_ATTENUATION ? UMI_CS35L41_MAX_ATTENUATION : below;
	if (target_attenuation == 0) target_attenuation = 1;

	if (!umi_output_ready || !umi_output_on || umi_reset_asserted) return false;
	for (;;) {
		uint32_t next = umi_volume_attenuation;
		if (next > target_attenuation) {
			next = next - target_attenuation > 48 ? next - 48 : target_attenuation;
		} else if (next < target_attenuation) {
			next = target_attenuation - next > 48 ? next + 48 : target_attenuation;
		} else {
			break;
		}
		for (unsigned i = 0; i < 2; i++) {
			uint8_t a = i ? 0x42 : 0x40;
			if (!umi_cs35l41_update(a, 0x6000, 0x3fff, umi_cs35l41_volume_register(next))) {
				umi_speaker_quiesce();
				return false;
			}
		}
		umi_volume_attenuation = next;
		if (step_ms) wait_ms(step_ms);
		if (step_ms >= 10 && !umi_speaker_healthy()) return false;
	}
	if (!umi_speaker_healthy()) return false;
	say("umi-audio: digital volume at -%u.%03u dB (under unity gain)\n",
	    target_attenuation / 8, target_attenuation % 8 * 125);
	return true;
}

static bool
umi_speaker_begin(bool keep)
{
	if (tlmm == 0) tlmm = ml_io_map(UMI_TLMM_NORTH_PHYS, TLMM_SIZE);
	if (tlmm == 0) return false;
	uint32_t cfg = rd(GPIO_CFG(UMI_AMP_RESET)), io = rd(GPIO_IO(UMI_AMP_RESET));
	umi_runtime_preparing = keep;
	wr(GPIO_IO(UMI_AMP_RESET), 0);
	wr(GPIO_CFG(UMI_AMP_RESET), CFG_OUTPUT);
	spin_us(2000);
	wr(GPIO_IO(UMI_AMP_RESET), 2);
	wait_ms(10);
	bool ok = true;
	for (unsigned i = 0; i < 2; i++) {
		uint8_t a = i ? 0x42 : 0x40;
		uint32_t id, rev;
		if (!umi_cs35l41_read(a, 0, &id) || id != UMI_CS35L41_CHIP_ID ||
		    !umi_cs35l41_read(a, 4, &rev) || rev != 0xb2 ||
		    (keep && !umi_cs35l41_board_setup(a)) || !umi_cs35l41_preload(a) || !umi_cs35l41_dsp_boot(a)) {
			say("umi-audio: amp %#x HALO startup test failed; outputs kept disabled\n", a);
			ok = false;
			break;
		}
	}
	if (keep && ok) {
		umi_reset_cfg = cfg;
		umi_reset_io = io;
		umi_output_on = false;
		umi_reset_asserted = false;
		umi_output_ready = true;
		return true;
	}
	wr(GPIO_IO(UMI_AMP_RESET), 0);
	spin_us(2000);
	wr(GPIO_IO(UMI_AMP_RESET), io & 2);
	wr(GPIO_CFG(UMI_AMP_RESET), cfg);
	say("umi-audio: HALO execution/mailbox test %s; both chips reset, output stages never enabled\n", ok ? "passed" : "failed");
	return ok;
}

bool umi_speaker_dsp_test(void) { return umi_speaker_begin(false); }
bool umi_speaker_prepare(void) { return umi_speaker_begin(true); }
bool umi_speaker_calibrated(void) { return umi_output_ready && umi_unit_calibrated[0] && umi_unit_calibrated[1]; }

static void
umi_amp_probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const struct { uint8_t address; const char *name; } amps[] = {
		{ 0x40, "speaker amp" },
		{ 0x42, "receiver amp" },
	};
	uint32_t devid, revid;

	say("umi-audio: probe stage %u starting; mapping UMI TLMM NORTH at %#llx\n", umi_amp_probe_level,
	    (uint64_t)UMI_TLMM_NORTH_PHYS);
	if (tlmm == 0) {
		tlmm = ml_io_map(UMI_TLMM_NORTH_PHYS, TLMM_SIZE);
	}
	if (tlmm == 0) {
		say("umi-audio: TLMM mapping failed\n");
		thread_terminate(current_thread());
		return;
	}
	static const unsigned pins[] = {
		UMI_AMP_IRQ_RCV, UMI_AMP_IRQ_SPK, UMI_AMP_RESET, UMI_I2C_SDA, UMI_I2C_SCL,
	};
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		unsigned pin = pins[i];
		say("umi-audio: GPIO%u cfg=%#x io=%#x\n", pin, rd(GPIO_CFG(pin)), rd(GPIO_IO(pin)));
	}
	if (umi_amp_probe_level == 1) {
		say("umi-audio: passive GPIO read complete; no pins changed\n");
		thread_terminate(current_thread());
		return;
	}
	if (umi_amp_probe_level == 2) {
		uint32_t before = rd(GPIO_CFG(UMI_I2C_SDA));
		say("umi-audio: harmless same-value GPIO119 config write, value=%#x\n", before);
		wr(GPIO_CFG(UMI_I2C_SDA), before);
		say("umi-audio: GPIO119 config readback=%#x\n", rd(GPIO_CFG(UMI_I2C_SDA)));
		say("umi-audio: same-value write check complete; no pin state changed\n");
		thread_terminate(current_thread());
		return;
	}
	if (umi_amp_probe_level == 3) {
		bool idle = umi_i2c_bus_take();
		bool sda = umi_i2c_level(UMI_I2C_SDA), scl = umi_i2c_level(UMI_I2C_SCL);
		if (idle) {
			say("umi-audio: SE3 lines released high (GPIO119=%u, GPIO120=%u); restoring pins\n", sda, scl);
			umi_i2c_bus_give();
		} else {
			say("umi-audio: SE3 lines stayed low after release (GPIO119=%u, GPIO120=%u); pins restored\n", sda, scl);
		}
		say("umi-audio: pin-release check complete; no I2C transaction sent\n");
		thread_terminate(current_thread());
		return;
	}
	uint32_t reset_cfg = 0, reset_io = 0;
	if (umi_amp_probe_level >= 4) {
		reset_cfg = rd(GPIO_CFG(UMI_AMP_RESET));
		reset_io = rd(GPIO_IO(UMI_AMP_RESET));
		say("umi-audio: pulsing shared CS35L41 reset GPIO%u low 2 ms, then high; saved cfg=%#x io=%#x\n",
		    UMI_AMP_RESET, reset_cfg, reset_io);
		wr(GPIO_IO(UMI_AMP_RESET), 0);
		wr(GPIO_CFG(UMI_AMP_RESET), CFG_OUTPUT);
		__asm__ volatile("dmb oshst" ::: "memory");
		spin_us(2000);
		wr(GPIO_IO(UMI_AMP_RESET), 2);
		__asm__ volatile("dmb oshst" ::: "memory");
		spin_us(10000);
		say("umi-audio: reset released; GPIO%u cfg=%#x io=%#x\n", UMI_AMP_RESET,
		    rd(GPIO_CFG(UMI_AMP_RESET)), rd(GPIO_IO(UMI_AMP_RESET)));
	}
	say("umi-audio: read-only CS35L41 ID probe on SE3; amplifier registers are not written\n");
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		if (!umi_cs35l41_read(amps[i].address, UMI_CS35L41_DEVID, &devid)) {
			say("umi-audio: %s at 0x%02x did not acknowledge the ID read\n", amps[i].name, amps[i].address);
			continue;
		}
		if (!umi_cs35l41_read(amps[i].address, UMI_CS35L41_REVID, &revid)) {
			say("umi-audio: %s at 0x%02x answered, but the revision read failed (ID %#x)\n",
			    amps[i].name, amps[i].address, devid);
			continue;
		}
		say("umi-audio: %s at 0x%02x: DEVID=%#x REVID=%#x%s\n", amps[i].name, amps[i].address,
		    devid, revid, devid == UMI_CS35L41_CHIP_ID ? " (CS35L41 confirmed)" : " (unexpected chip ID)");
	}
	if (umi_amp_probe_level == 5) {
		for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
			if (!umi_cs35l41_read(amps[i].address, UMI_CS35L41_DEVID, &devid) || devid != UMI_CS35L41_CHIP_ID ||
			    !umi_cs35l41_read(amps[i].address, UMI_CS35L41_REVID, &revid) || revid != 0xb2 ||
			    !umi_cs35l41_preload(amps[i].address)) {
				say("umi-audio: amp %#x firmware staging failed; stopping before playback\n", amps[i].address);
				break;
			}
		}
		/* Clear staged DSP memory and leave reset state as found. */
		wr(GPIO_IO(UMI_AMP_RESET), 0);
		spin_us(2000);
	}
	say("umi-audio: probe complete; SE3 pins restored\n");
	if (umi_amp_probe_level >= 4) {
		wr(GPIO_IO(UMI_AMP_RESET), reset_io & 2);
		wr(GPIO_CFG(UMI_AMP_RESET), reset_cfg);
		__asm__ volatile("dmb oshst" ::: "memory");
		say("umi-audio: reset pin restored to cfg=%#x io=%#x\n", rd(GPIO_CFG(UMI_AMP_RESET)),
		    rd(GPIO_IO(UMI_AMP_RESET)));
	}
	thread_terminate(current_thread());
}

void
hk_speaker_init(void)
{
	thread_t thread;
	uint32_t umi_amp_probe = 0;

	if (PE_parse_boot_argn("umi_amp_probe", &umi_amp_probe, sizeof(umi_amp_probe))) {
		if (umi_amp_probe >= 1 && umi_amp_probe <= 5) {
			umi_amp_probe_level = umi_amp_probe;
		}
		if (umi_amp_probe_level != 0) {
			if (kernel_thread_start(umi_amp_probe_thread, NULL, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
		}
		return;
	}

	if (!PE_parse_boot_argn("speaker_probe", &probe_level, sizeof(probe_level)) || probe_level == 0) {
		return;
	}
	if (kernel_thread_start(probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
