/* Xiaomi Mi 10 ST FTS521, adapted from the validated UMI bring-up. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#include <sys/hk_input.h>
#include <pexpert/device_tree.h>
/* Preserve hardware diagnostics in the USB log even in quiet UI boots. */
extern void hk_tl(const char *, ...) __printflike(1, 2);
#define printf hk_tl
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern int umi_rpmh_vreg_enable(const char *, unsigned);
extern int umi_touch_boost_enable(void);
void umi_touch_init(void);
#define TOUCH_GPIO_CFG(n) ((uint32_t)(n) * 0x1000u)
#define TOUCH_GPIO_IO(n) (TOUCH_GPIO_CFG(n) + 4u)
#define TOUCH_MAX 10u
#define TOUCH_NORTH_PA 0x0f500000u
#define TOUCH_NORTH_SIZE 0x80000u
#define TOUCH_GPIO_SDA 36u
#define TOUCH_GPIO_SCL 37u
#define TOUCH_GPIO_RESET 38u
#define TOUCH_GPIO_IRQ 39u
#define TOUCH_GPIO_POWER 69u
#define TOUCH_MAX_X 1079u
#define TOUCH_MAX_Y 2339u
static struct { vm_offset_t gpio; bool started, dead; uint32_t active; int x[TOUCH_MAX], y[TOUCH_MAX]; } st;
static int touch_sleep_chan;
static uint32_t
touch_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
touch_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
touch_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&touch_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
touch_gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, bool out, bool high)
{
	uint32_t off = TOUCH_GPIO_CFG(pin);
	uint32_t cfg = touch_rd(st.gpio, off);
	if (out) {
		/* Set the desired latch before enabling output (reset is active low). */
		touch_wr(st.gpio, TOUCH_GPIO_IO(pin),
		    (touch_rd(st.gpio, TOUCH_GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	}
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);

	/* pinctrl-kona.c: egpio_present=11, egpio_enable=12. Mirror
	 * msm_pinmux_set_mux() when firmware exposes the extended GPIO gate. */
	if (cfg & (1u << 11)) {
		cfg |= 1u << 12;
	}

	touch_wr(st.gpio, off, cfg);
}

static void
touch_report(uint32_t mask, const int *x, const int *y)
{
	for (unsigned id = 0; id < TOUCH_MAX; ++id) {
		uint32_t bit = 1u << id;
		if (mask & bit) {
			if (!(st.active & bit) || x[id] != st.x[id] || y[id] != st.y[id]) {
				hk_touch_post((st.active & bit) ? HK_TOUCH_MOVE : HK_TOUCH_DOWN, id, (uint32_t)x[id] * 65535u / TOUCH_MAX_X, (uint32_t)y[id] * 65535u / TOUCH_MAX_Y);
			}
			st.x[id] = x[id];
			st.y[id] = y[id];
		} else if (st.active & bit) {
			hk_touch_post(HK_TOUCH_UP, id, (uint32_t)st.x[id] * 65535u / TOUCH_MAX_X, (uint32_t)st.y[id] * 65535u / TOUCH_MAX_Y);
		}
	}
	st.active = mask;
}

#define UMI_FTS_ADDR 0x49u
#define UMI_FTS_FIFO 0x86u

static void
umi_i2c_delay(void)
{
	delay(5); /* approximately 100 kHz */
}

static void
umi_i2c_line(unsigned pin, bool high)
{
	/* Open drain: output-low drives the line; input releases it. */
	touch_gpio(pin, 0, high ? 3u : 0u, 0, !high, false);
	umi_i2c_delay();
}

static bool
umi_i2c_sample(unsigned pin)
{
	return (touch_rd(st.gpio, TOUCH_GPIO_IO(pin)) & 1u) != 0;
}

static bool
umi_i2c_scl_high(void)
{
	umi_i2c_line(TOUCH_GPIO_SCL, true);
	for (unsigned i = 0; i < 1000; ++i) {
		if (umi_i2c_sample(TOUCH_GPIO_SCL)) {
			return true;
		}
		delay(1);
	}
	return false;
}

static bool
umi_i2c_start(void)
{
	umi_i2c_line(TOUCH_GPIO_SDA, true);
	if (!umi_i2c_scl_high() || !umi_i2c_sample(TOUCH_GPIO_SDA)) {
		return false;
	}
	umi_i2c_line(TOUCH_GPIO_SDA, false);
	umi_i2c_line(TOUCH_GPIO_SCL, false);
	return true;
}

static void
umi_i2c_stop(void)
{
	umi_i2c_line(TOUCH_GPIO_SDA, false);
	(void)umi_i2c_scl_high();
	umi_i2c_line(TOUCH_GPIO_SDA, true);
}

static bool
umi_i2c_write_byte(uint8_t value)
{
	for (unsigned bit = 0; bit < 8; ++bit) {
		umi_i2c_line(TOUCH_GPIO_SDA, (value & 0x80u) != 0);
		if (!umi_i2c_scl_high()) {
			return false;
		}
		umi_i2c_line(TOUCH_GPIO_SCL, false);
		value <<= 1;
	}
	umi_i2c_line(TOUCH_GPIO_SDA, true);
	if (!umi_i2c_scl_high()) {
		return false;
	}
	bool ack = !umi_i2c_sample(TOUCH_GPIO_SDA);
	umi_i2c_line(TOUCH_GPIO_SCL, false);
	return ack;
}

static uint8_t
umi_i2c_read_byte(bool ack)
{
	uint8_t value = 0;
	umi_i2c_line(TOUCH_GPIO_SDA, true);
	for (unsigned bit = 0; bit < 8; ++bit) {
		value <<= 1;
		if (!umi_i2c_scl_high()) {
			return 0xff;
		}
		if (umi_i2c_sample(TOUCH_GPIO_SDA)) {
			value |= 1u;
		}
		umi_i2c_line(TOUCH_GPIO_SCL, false);
	}
	umi_i2c_line(TOUCH_GPIO_SDA, !ack);
	(void)umi_i2c_scl_high();
	umi_i2c_line(TOUCH_GPIO_SCL, false);
	umi_i2c_line(TOUCH_GPIO_SDA, true);
	return value;
}

static int
umi_fts_xfer(const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr)
{
	if (!nt && !nr) {
		return EINVAL;
	}
	if (!umi_i2c_start() || !umi_i2c_write_byte((UMI_FTS_ADDR << 1) | 0u)) {
		printf("UMI FTS: write address failed cmd=%#x scl=%d sda=%d\n",
		    nt ? tx[0] : 0, umi_i2c_sample(TOUCH_GPIO_SCL),
		    umi_i2c_sample(TOUCH_GPIO_SDA));
		umi_i2c_stop();
		return ENXIO;
	}
	for (unsigned i = 0; i < nt; ++i) {
		if (!umi_i2c_write_byte(tx[i])) {
			printf("UMI FTS: command %#x byte %u NACK\n", tx[0], i);
			umi_i2c_stop();
			return EIO;
		}
	}
	if (nr) {
		if (!umi_i2c_start() || !umi_i2c_write_byte((UMI_FTS_ADDR << 1) | 1u)) {
			printf("UMI FTS: repeated START/read address failed cmd=%#x\n", tx[0]);
			umi_i2c_stop();
			return EIO;
		}
		for (unsigned i = 0; i < nr; ++i) {
			rx[i] = umi_i2c_read_byte(i + 1u < nr);
		}
	}
	umi_i2c_stop();
	return 0;
}

static int
umi_fts_fifo(uint8_t event[8])
{
	const uint8_t cmd = UMI_FTS_FIFO;
	return umi_fts_xfer(&cmd, 1, event, 8);
}

static void
umi_fts_snapshot(void)
{
	const uint32_t regs[] = {0x20000000,0x20000004,0x20000024,0x20000025,
	    0x20000029,0x2000002d,0x20000030,0x20000032,0x20000034,0x20000078};
	for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); ++i) {
		uint32_t r = regs[i];
		uint8_t cmd[] = {0xfa, r >> 24, r >> 16, r >> 8, r};
		uint8_t b[4] = {0};
		int err = umi_fts_xfer(cmd, sizeof(cmd), b, sizeof(b));
		printf("UMI FTS: reg %#x err=%d %02x %02x %02x %02x\n",
		    r, err, b[0], b[1], b[2], b[3]);
	}
	const uint8_t cmd[] = {0xa6, 0, 0};
	uint8_t info[32] = {0};
	int err = umi_fts_xfer(cmd, sizeof(cmd), info, sizeof(info));
	printf("UMI FTS: sysinfo err=%d hdr=%02x fw=%02x%02x afe=%02x/%02x/%02x\n",
	    err, info[0], info[17], info[16], info[28], info[29], info[30]);
}

static void
umi_fts_drain(void)
{
	uint8_t event[8];
	for (unsigned i = 0; i < 32; ++i) {
		if (umi_fts_fifo(event) || event[0] == 0 || !(event[7] & 0x1fu)) {
			break;
		}
	}
}

static int
umi_fts_initialize(void)
{
	/* Scan mode 1 was validated on this UMI. The vendor also supports
	 * 0x20 (no calibration). Do not stop sensing before this command. */
	unsigned scan = 1;
	(void)PE_parse_boot_argn("umifts_scan", &scan, sizeof(scan));
	if (scan != 0x01 && scan != 0x20) {
		return EINVAL;
	}
	const uint8_t sense_on[] = {0xa0, 0x00, (uint8_t)scan};
	static const uint8_t gestures[] = {0xa2, 0x03, 0x00, 0x00, 0x00, 0x03};
	static const uint8_t chipid[] = {0xfa, 0x20, 0x00, 0x00, 0x00};
	uint8_t event[8], id[4] = {0};

	umi_fts_drain();
	/* ftsCore.c fts_system_reset(): when reset_gpio is defined, Xiaomi
	 * pulses the pad low for 10ms instead of writing ADDR_SYSTEM_RESET.
	 * The UMI DT explicitly supplies reset GPIO38. */
	touch_gpio(TOUCH_GPIO_RESET, 0, 3, 3, true, false);
	touch_wait(10);
	touch_gpio(TOUCH_GPIO_RESET, 0, 3, 3, true, true);
	for (unsigned i = 0; i < 60; ++i) {
		touch_wait(2);
		if (!umi_fts_fifo(event) && event[0] == 0x03) {
			break;
		}
		if (i == 59) {
			printf("UMI FTS: controller-ready timeout\n");
			return ETIMEDOUT;
		}
	}
	if (umi_fts_xfer(chipid, sizeof(chipid), id, sizeof(id)) ||
	    id[0] != 0x36 || id[1] != 0x39) {
		printf("UMI FTS: bad chip id %02x %02x %02x %02x\n",
		    id[0], id[1], id[2], id[3]);
		return ENXIO;
	}
	/* Captured on this phone with recovery stm_fts_cmd F1: senseOn(),
	 * then forced mode-handler feature restoration, another senseOn(),
	 * and the gesture mask. Do not assume reset cleared feature state. */
	const uint8_t features[][3] = {{0xa2, 0, 0}, {0xa2, 1, 0}, {0xa2, 2, 0}};
	if (umi_fts_xfer(sense_on, sizeof(sense_on), NULL, 0)) {
		return EIO;
	}
	for (unsigned i = 0; i < sizeof(features)/sizeof(features[0]); ++i) {
		if (umi_fts_xfer(features[i], sizeof(features[i]), NULL, 0)) {
			return EIO;
		}
	}
	if (umi_fts_xfer(sense_on, sizeof(sense_on), NULL, 0) ||
	    umi_fts_xfer(gestures, sizeof(gestures), NULL, 0)) {
		return EIO;
	}
	touch_wait(100);
	/* Leave command/status events for the logged event loop below. */
	printf("UMI FTS: ST FTS521 %02x%02x initialized scan=%#x\n", id[0], id[1], scan);
	umi_fts_snapshot();
	return 0;
}

static void
umi_fts_event(const uint8_t event[8])
{
	static unsigned logged;
	if (logged++ < 48) {
		printf("UMI FTS: event %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    event[0], event[1], event[2], event[3], event[4], event[5], event[6], event[7]);
	}
	unsigned type = event[0], id = (event[1] >> 4) & 0x0fu;
	if (id >= TOUCH_MAX) {
		return;
	}
	if (type == 0x13 || type == 0x23) {
		unsigned x = ((unsigned)(event[3] & 0x0fu) << 8) | event[2];
		unsigned y = ((unsigned)event[4] << 4) | (event[3] >> 4);
		if (x <= TOUCH_MAX_X && y <= TOUCH_MAX_Y) {
			int xs[TOUCH_MAX], ys[TOUCH_MAX];
			memcpy(xs, st.x, sizeof(xs));
			memcpy(ys, st.y, sizeof(ys));
			xs[id] = (int)x;
			ys[id] = (int)y;
			touch_report(st.active | (1u << id), xs, ys);
		}
	} else if (type == 0x33) {
		touch_report(st.active & ~(1u << id), st.x, st.y);
	}
}

static void
umi_touch_run(void)
{
	uint8_t event[8];
	st.gpio = ml_io_map(TOUCH_NORTH_PA, TOUCH_NORTH_SIZE);
	if (!st.gpio) {
		printf("UMI FTS: TLMM map failed\n");
		return;
	}
	touch_gpio(TOUCH_GPIO_IRQ, 0, 3, 3, false, false);
	touch_gpio(TOUCH_GPIO_RESET, 0, 3, 3, true, false);
	int boost_error = umi_touch_boost_enable();
	printf("UMI FTS: boost setup err=%d\n", boost_error);
	if (boost_error) return;
	int power_error = umi_rpmh_vreg_enable("ldoc1", 1800);
	printf("UMI FTS: vdd ldoc1 1800mV vote err=%d\n", power_error);
	if (power_error) {
		return;
	}
	touch_wait(15);
	touch_gpio(TOUCH_GPIO_POWER, 0, 0, 0, true, true);
	touch_wait(10);
	touch_gpio(TOUCH_GPIO_RESET, 0, 3, 3, true, true);
	touch_wait(100);
	umi_i2c_line(TOUCH_GPIO_SDA, true);
	umi_i2c_line(TOUCH_GPIO_SCL, true);
	/* An interrupted firmware transaction can leave SDA low. Clock out the
	 * remaining byte and terminate it before attempting the first address. */
	for (unsigned i = 0; i < 9 && !umi_i2c_sample(TOUCH_GPIO_SDA); ++i) {
		umi_i2c_line(TOUCH_GPIO_SCL, false);
		if (!umi_i2c_scl_high()) {
			break;
		}
	}
	umi_i2c_stop();
	printf("UMI FTS: GPIO cfg SDA=%#x SCL=%#x RESET=%#x AVDD=%#x\n",
	    touch_rd(st.gpio, TOUCH_GPIO_CFG(TOUCH_GPIO_SDA)),
	    touch_rd(st.gpio, TOUCH_GPIO_CFG(TOUCH_GPIO_SCL)),
	    touch_rd(st.gpio, TOUCH_GPIO_CFG(TOUCH_GPIO_RESET)),
	    touch_rd(st.gpio, TOUCH_GPIO_CFG(TOUCH_GPIO_POWER)));
	printf("UMI FTS: bus idle scl=%d sda=%d reset=%#x power=%#x\n",
	    umi_i2c_sample(TOUCH_GPIO_SCL), umi_i2c_sample(TOUCH_GPIO_SDA),
	    touch_rd(st.gpio, TOUCH_GPIO_IO(TOUCH_GPIO_RESET)),
	    touch_rd(st.gpio, TOUCH_GPIO_IO(TOUCH_GPIO_POWER)));
	if (umi_fts_initialize()) {
		printf("UMI FTS: initialization failed\n");
		return;
	}
	unsigned ticks = 0, events = 0, errors = 0;
	while (!st.dead) {
		/* Match the working recovery IRQ path: leave an empty FIFO alone. */
		if (touch_rd(st.gpio, TOUCH_GPIO_IO(TOUCH_GPIO_IRQ)) & 1u) {
			/* no pending controller interrupt */
		} else if (!umi_fts_fifo(event)) {
			unsigned count = event[7] & 0x1fu;
			if (event[0]) {
				++events;
				umi_fts_event(event);
			}
			if (count) {
				uint8_t batch[31 * 8];
				const uint8_t cmd = UMI_FTS_FIFO;
				/* Vendor drains all remaining records in one transaction. */
				if (umi_fts_xfer(&cmd, 1, batch, count * 8)) {
					++errors;
				} else for (unsigned i = 0; i < count; ++i) {
					if (batch[i * 8]) {
						++events;
						umi_fts_event(batch + i * 8);
					}
				}
			}
		} else {
			++errors;
		}
		if (++ticks % 625u == 0) {
			printf("UMI FTS: poll=%u events=%u errors=%u irq=%u active=%#x\n",
			    ticks, events, errors,
			    touch_rd(st.gpio, TOUCH_GPIO_IO(TOUCH_GPIO_IRQ)) & 1u, st.active);

		}
		touch_wait(8);
	}
}

static void umi_touch_thread(void *arg, wait_result_t result)
{
    (void)arg; (void)result;
    touch_wait(10000);
    umi_touch_run();
    (void)thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}
void umi_touch_init(void)
{
    uint32_t enabled = 0;
    DTEntry chosen; const void *board; unsigned size;
    if (st.started || !PE_parse_boot_argn("umi_touch", &enabled, sizeof(enabled)) || enabled != 1) return;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "hackios-board", &board, &size) != kSuccess ||
        size != 4 || memcmp(board, "umi", 4)) { printf("UMI FTS: board mismatch\n"); return; }
    st.started = true;
    thread_t thread;
    kern_return_t kr = kernel_thread_start(umi_touch_thread, NULL, &thread);
    if (kr == KERN_SUCCESS) thread_deallocate(thread);
    else printf("UMI FTS: kernel_thread_start failed %d\n", kr);
}
