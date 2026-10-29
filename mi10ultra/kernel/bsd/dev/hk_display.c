/*
 * Where the panel's scan is, on the POCO X5 Pro (redwood): for iokit/Kernel/MiniIOMobileFramebuffer.cpp, which
 * puts every frame into the memory the panel is sent from and, without this, can only guess when the panel
 * reads it -- which is why the picture tore and why frames came at a pace of their own, beside the panel's.
 *
 * The panel is a DSI video panel, sent the framebuffer over and over by the display controller's timing engine
 * at the rate of its timing registers, which nobody touches after the firmware: 2432 lines a frame of which
 * 2400 are shown, 119.93 frames a second (seen: hk_adreno.c, display_probe). The interface the panel hangs
 * on (INTF 1, dt: mdss_dsi0's mdp_intf_base) counts the frames and, within a frame, the line being sent; this
 * reads those two counters. To read them at all two bus clocks have to run, which the firmware leaves off: the
 * one of the display's clock controller (in GCC) and, in that controller, the one of the display block's
 * registers. They are turned on here, as Linux keeps them on and as display_probe did on this phone; that is all
 * that is ever written. The timing itself is only read.
 *
 * Boot-arg fb_scan=1 (on the phone; QEMU has no such block): on. Then the framebuffer driver
 *   - starts copying a frame into the panel's memory only where the scan lets the copy land whole in one
 *     refresh (hk_display_window(): see MiniIOMobileFramebuffer.cpp, show), so that no refresh shows
 *     part of one frame and part of the next -- no tearing;
 *   - runs at the panel's own rate (120 Hz) and tells CoreAnimation that rate, and its refresh notifications
 *     come when the panel's frames begin, not on a clock of their own that drifts against them.
 *
 * Registers: Linux drivers/gpu/drm/msm/disp/dpu1/dpu_hw_intf.c (INTF), drivers/clk/qcom/dispcc-sc7280.c.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hk_display.h>
#include <sys/hk_board.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
/* Preserve early clock/timing diagnostics in quiet boots and live loader logs. */
extern void hk_tl(const char *, ...) __printflike(1, 2);
#define printf hk_tl

#define GCC_PHYS                hk_board_addr("gcc", 0x00100000UL)
#define GCC_DISP_AHB_CBCR       (umi ? 0x0b00c : 0x27004)
#define DISPCC_PHYS             hk_board_addr("dispcc", 0x0af00000UL)    /* dt: clock-controller@af00000 */
#define DISPCC_MDSS_CORE_GDSCR  (umi ? 0x3000 : 0x1004)
#define DISPCC_MDSS_AHB_CBCR    (umi ? 0x2080 : 0x1050)
#define INTF1_PHYS              hk_board_addr("intf", umi ? 0x0ae6b800UL : 0x0ae36000UL)
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_HSYNC_CTL          0x008           /* clocks a line << 16 | of which the pulse */
#define INTF_VSYNC_PERIOD       0x00c           /* clocks a frame */
#define INTF_DISPLAY_V_START    0x01c
#define INTF_DISPLAY_V_END      0x024
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0

static vm_offset_t intf, gcc_map, dispcc_map;
static struct hk_display_scan scan;
static bool umi;
static bool command_mode;
static uint32_t te_initial, te_idle_start;

int hk_display_command_mode(void) { return scan.ok && command_mode; }


static inline uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

/*
 * Whether the display block's registers can be read right now: its power switch on and the two bus clocks
 * running. On these SoCs a read of a block whose clock is off does not fail, it hangs the bus -- the whole
 * phone, the loader's core with it. The clock controllers themselves always answer (GCC; dispcc once GCC's
 * clock for it runs, which is checked first), so this is asked before every read of the scan's counters,
 * and once the answer is no, the scan is given up for good: frames go back to the driver's own clock.
 */
static int
regs_alive(void)
{
	uint32_t v = rd(gcc_map, GCC_DISP_AHB_CBCR & 0xfffu);
	if (v == UINT32_MAX || !(v & CBCR_ENABLE) || (v & CBCR_OFF)) return 0;
	v = rd(dispcc_map, DISPCC_MDSS_CORE_GDSCR);
	if (v == UINT32_MAX || !(v & GDSC_POWER_ON)) return 0;
	v = rd(dispcc_map, DISPCC_MDSS_AHB_CBCR);
	return v != UINT32_MAX && (v & CBCR_ENABLE) && !(v & CBCR_OFF);
}

/* one bus clock on (and left on); 1 if it runs */
static int
clock_on(vm_offset_t base, uint32_t cbcr, const char *name)
{
	uint32_t v = rd(base, cbcr);
	if (v == UINT32_MAX) return 0;

	if (!(v & CBCR_ENABLE) || (v & CBCR_OFF)) {
		wr(base, cbcr, v | CBCR_ENABLE);
		delay_for_interval(10, NSEC_PER_MSEC);
		v = rd(base, cbcr);
	}
	if (!(v & CBCR_ENABLE) || (v & CBCR_OFF)) {
		printf("display: the %s clock does not take (%#x): no scan to go by\n", name, v);
		return 0;
	}
	return 1;
}

uint32_t
hk_display_line(uint32_t *frame)
{
	uint32_t f, line;

	if (intf == 0) {
		return 0;
	}
	if (!regs_alive()) {
		printf("display: the display block's clock or power went off: the scan is no longer read\n");
		intf = 0;
		scan.ok = 0;
		return 0;
	}
	if (command_mode) {
		uint32_t raw = rd(intf, 0x298) & 0xffff;
		if (frame != NULL) *frame = 0;
		line = raw >= te_initial ? raw - te_initial : 0;
		return line < scan.lines ? line : scan.lines - 1;
	}
	/* the two counters together: a frame that begins between the two reads is read again */
	for (unsigned retry = 0; retry < 8; retry++) {
		f = rd(intf, INTF_FRAME_COUNT);
		line = rd(intf, INTF_LINE_COUNT);
		if (rd(intf, INTF_FRAME_COUNT) == f) {
			if (frame != NULL) *frame = f;
			return line;
		}
	}
	/* A broken counter must not trap the presenter in an unbounded retry. */
	intf = 0;
	scan.ok = 0;
	if (frame != NULL) {
		*frame = f;
	}
	return line;
}

const struct hk_display_scan *
hk_display_scan(void)
{
	return scan.ok ? &scan : NULL;
}

/* The command-mode write pointer is the display DMA's progress through the
 * framebuffer. Copy only after it finishes, with room before the next TE.
 * Rendering/scaling must already be complete in a separate staging buffer.
 */
int
hk_display_wait_copy(uint64_t copy_ns, uint64_t limit_ns)
{
	uint64_t start = mach_absolute_time(), elapsed;
	if (!hk_display_command_mode()) return 0;
	uint64_t budget = copy_ns + copy_ns / 4 + 500000;
	for (;;) {
		uint32_t phase = hk_display_line(NULL);
		if (!scan.ok) return 0;
		uint32_t written = rd(intf, 0x2b0) & 0xffff;
		uint64_t used = (uint64_t)phase * scan.line_ns;
		if (phase >= te_idle_start && phase < te_idle_start + 128 && written >= scan.shown &&
		    used < scan.frame_ns && budget < scan.frame_ns - used) return 1;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= limit_ns) return 0;
		delay_for_interval(50, NSEC_PER_USEC);
	}
}

static int
command_scan_start(void)
{
	uint32_t config = rd(intf, 0x288), refresh = rd(intf, 0x2b4);
	te_initial = rd(intf, 0x294) & 0xffff;
	if (!umi || !(rd(intf, 0x284) & 1) || (config & 0x180000) != 0x180000 ||
	    refresh != 0x80000001 || te_initial != 2340) return 0;
	uint64_t start = mach_absolute_time(), previous_time = start, first_wrap = 0, last_wrap = 0;
	uint64_t line_time = 0, line_count = 0, elapsed;
	uint32_t previous = rd(intf, 0x298) & 0xffff, busy_last = 0;
	unsigned wraps = 0, busy_samples = 0, idle_samples = 0;
	while (wraps < 4) {
		delay_for_interval(50, NSEC_PER_USEC);
		uint32_t raw = rd(intf, 0x298) & 0xffff;
		uint32_t written = rd(intf, 0x2b0) & 0xffff;
		uint64_t now = mach_absolute_time(), interval;
		absolutetime_to_nanoseconds(now - start, &elapsed);
		if (elapsed > 150000000 || raw < te_initial) return 0;
		/* Do not pair the previous frame's read phase with a new DMA transfer. */
		if ((rd(intf, 0x298) & 0xffff) < raw) continue;
		if (raw < previous) {
			if (wraps++ == 0) first_wrap = now;
			last_wrap = now;
		} else if (raw > previous) {
			absolutetime_to_nanoseconds(now - previous_time, &interval);
			line_time += interval;
			line_count += raw - previous;
		}
		uint32_t phase = raw - te_initial;
		if (written > 0 && written < te_initial) {
			if (phase > busy_last) busy_last = phase;
			busy_samples++;
		} else if (written >= te_initial) idle_samples++;
		previous = raw;
		previous_time = now;
	}
	if (!line_count || !busy_samples || !idle_samples) return 0;
	absolutetime_to_nanoseconds(last_wrap - first_wrap, &elapsed);
	scan.frame_ns = elapsed / (wraps - 1);
	scan.line_ns = line_time / line_count;
	if (!scan.line_ns || scan.frame_ns < NSEC_PER_SEC / 120 ||
	    scan.frame_ns > NSEC_PER_SEC / 30) return 0;
	scan.lines = (uint32_t)(scan.frame_ns / scan.line_ns);
	scan.shown = te_initial;
	te_idle_start = busy_last + 32; /* margin after the last observed active DMA line */
	if (scan.lines < scan.shown || scan.lines > 8192 || te_idle_start >= scan.lines) return 0;
	scan.millihz = (uint32_t)(1000000000000ULL / scan.frame_ns);
	command_mode = true;
	scan.ok = 1;
	printf("display: UMI command TE: %u.%03u Hz, %u ns/line, %u phase lines; DMA idle after %u; staged presentation enabled\n",
	    scan.millihz / 1000, scan.millihz % 1000, (unsigned)scan.line_ns, scan.lines, te_idle_start);
	return 1;
}

/*
 * Wait until the scan is in [first, last) of a frame (lines, 0 = the first line shown), at most `limit_ns`;
 * the wait is a sleep for most of the way and a short spin for the end. 1 when it got there.
 */
int
hk_display_wait_lines(uint32_t first, uint32_t last, uint64_t limit_ns)
{
	uint64_t start = mach_absolute_time(), now, waited;

	if (!scan.ok || first >= last || last > scan.lines) {
		return 0;
	}
	for (;;) {
		uint32_t line = hk_display_line(NULL);
		uint64_t ahead_ns;

		if (!scan.ok) return 0;

		if (line >= first && line < last) {
			return 1;
		}
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - start, &waited);
		if (waited >= limit_ns) {
			return 0;
		}
		/* lines to go to `first`, as time: sleep all but the last 200 us of it */
		ahead_ns = (uint64_t)((first + scan.lines - line) % scan.lines) * scan.line_ns;
		if (ahead_ns > 400000) {
			uint64_t nap = ahead_ns - 200000;

			if (waited + nap > limit_ns) {
				nap = limit_ns - waited;
			}
			delay_for_interval((uint32_t)(nap / 1000), NSEC_PER_USEC);
		}
	}
}

/* nanoseconds until the next frame begins (its first line shown), from where the scan is now */
uint64_t
hk_display_next_frame_ns(void)
{
	uint32_t line;

	if (!scan.ok) {
		return 0;
	}
	line = hk_display_line(NULL);
	return (uint64_t)(scan.lines - line) * scan.line_ns;
}

int
hk_display_scan_start(void)
{
	uint32_t on = 0, hsync, period, v0, v1, f0, f1, l0, l1;
	uint64_t t0, t1, ns, gone;
	vm_offset_t gcc, dispcc;

	if (scan.ok) {
		return 1;
	}
	if (!PE_parse_boot_argn("fb_scan", &on, sizeof(on)) || on != 1) {
		return 0;
	}
	DTEntry chosen;
	const void *board;
	unsigned board_size;
	umi = SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "hackios-board", &board, &board_size) == kSuccess &&
	    board_size == 4 && memcmp(board, "umi", 4) == 0;
	gcc = gcc_map = ml_io_map(GCC_PHYS + (GCC_DISP_AHB_CBCR & ~0xfffu), 0x1000);
	if (!gcc) return 0;
	if (!clock_on(gcc, GCC_DISP_AHB_CBCR & 0xfffu, "display clock controller's bus")) {
		return 0;
	}
	dispcc = dispcc_map = ml_io_map(DISPCC_PHYS, umi ? 0x4000 : 0x2000);
	if (!dispcc) return 0;
	if (!(rd(dispcc, DISPCC_MDSS_CORE_GDSCR) & GDSC_POWER_ON)) {
		printf("display: the display block has no power (the firmware did not leave the panel on?): no scan to go by\n");
		return 0;
	}
	if (!clock_on(dispcc, DISPCC_MDSS_AHB_CBCR, "display registers' bus")) {
		return 0;
	}
	intf = ml_io_map(INTF1_PHYS, 0x1000);
	if (!intf) return 0;
	printf("display: %s scan registers at %#lx, engine=%#x; GCC AHB=%#x DISPCC AHB=%#x GDSC=%#x\n",
	    umi ? "UMI/Kona" : "Yupik", (unsigned long)INTF1_PHYS,
	    rd(intf, INTF_TIMING_ENGINE_EN), rd(gcc, GCC_DISP_AHB_CBCR & 0xfffu),
	    rd(dispcc, DISPCC_MDSS_AHB_CBCR), rd(dispcc, DISPCC_MDSS_CORE_GDSCR));
	if (!(rd(intf, INTF_TIMING_ENGINE_EN) & 1)) {
		printf("display: command mode: TE enable=%#x config=%#x height=%#x initial=%#x start=%#x auto=%#x\n",
		    rd(intf, 0x284), rd(intf, 0x288), rd(intf, 0x28c), rd(intf, 0x294), rd(intf, 0x2a0), rd(intf, 0x2b4));
		if (command_scan_start()) return 1;
		printf("display: command TE calibration failed; scan sync deferred\n");
		intf = 0;
		return 0;
	}
	hsync = rd(intf, INTF_HSYNC_CTL) >> 16;
	period = rd(intf, INTF_VSYNC_PERIOD);
	v0 = rd(intf, INTF_DISPLAY_V_START);
	v1 = rd(intf, INTF_DISPLAY_V_END);
	if (hsync == 0 || period < hsync || v1 <= v0) {
		printf("display: the timing engine's registers make no sense (line %u, frame %u clocks): no scan to go by\n", hsync, period);
		intf = 0;
		return 0;
	}
	scan.lines = period / hsync;
	scan.shown = (v1 - v0 + 1) / hsync;
	/* the rate the panel is really sent at: the counters 100 ms apart */
	l0 = hk_display_line(&f0);
	if (!intf) return 0;
	t0 = mach_absolute_time();
	delay_for_interval(100, NSEC_PER_MSEC);
	l1 = hk_display_line(&f1);
	if (!intf) return 0;
	t1 = mach_absolute_time();
	absolutetime_to_nanoseconds(t1 - t0, &ns);
	gone = (uint64_t)(f1 - f0) * scan.lines + l1 - l0;
	if (f1 == f0 || gone == 0 || ns == 0 || scan.lines < scan.shown || scan.shown == 0 ||
	    scan.lines > 65535 || scan.shown > 8192 || l0 >= scan.lines || l1 >= scan.lines) {
		printf("display: the scan counters do not move (frame %u -> %u, line %u -> %u): no scan to go by\n", f0, f1, l0, l1);
		intf = 0;
		return 0;
	}
	scan.line_ns = ns / gone;
	scan.frame_ns = scan.line_ns * scan.lines;
	if (scan.line_ns == 0 || scan.frame_ns < NSEC_PER_SEC / 240 ||
	    scan.frame_ns > NSEC_PER_SEC / 20) {
		printf("display: implausible scan period %llu ns; scan sync deferred\n", scan.frame_ns);
		intf = 0;
		return 0;
	}
	scan.millihz = (uint32_t)(1000000000000ULL / scan.frame_ns);
	scan.ok = 1;
	printf("display: the panel's scan: %u lines a frame (%u shown), %u.%03u frames a second, %llu ns a line; "
	    "frames follow it (fb_scan=1)\n", scan.lines, scan.shown, scan.millihz / 1000, scan.millihz % 1000, scan.line_ns);
	return 1;
}
