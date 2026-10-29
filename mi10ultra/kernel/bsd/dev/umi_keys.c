/*
 * Xiaomi Mi 10 (UMI) side and volume keys. The PM8150 PON block reports
 * power and volume-down; PM8150 GPIO6 reports volume-up. This is based on
 * the working UMI implementation in ios-sm8250.
 *
 * PMIC access is read-only: the only MMIO write issues an extended read on
 * the PMIC arbiter's observer channel. The driver's only effects are button
 * records posted to /dev/hackios-touch.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hk_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void hk_tl(const char *, ...) __printflike(1, 2);

#define UMI_ARB_CORE_PA       0x0c440000UL
#define UMI_ARB_CNFG_PA       0x0c40a000UL
#define UMI_ARB_OBSERVER_PA   0x0e600000UL
#define UMI_PON_PPID          0x0008u
#define UMI_GPIO6_PPID        0x00c5u
#define UMI_PON_RT_STATUS     0x0810u
#define UMI_GPIO_RT_STATUS    0xc110u
#define UMI_GPIO_MODE         0xc140u
#define UMI_GPIO_ENABLE       0xc146u
#define UMI_GPIO_SUBTYPE      0xc105u
#define UMI_KEY_POLL_MS       20
#define UMI_NO_APID           0xffffu

#define ARB_VERSION           0x000
#define ARB_FEATURES          0x004
#define ARB_CHANNEL(n)        (0x900u + 4u * (n))
#define ARB_OWNER(n)          (0x700u + 4u * (n))
#define OBS_COMMAND           0x00
#define OBS_STATUS            0x08
#define OBS_DATA0             0x18
#define OBS_DONE              0x1u
#define OBS_ERRORS            0x0eu
#define OBS_READ_LONG         (1u << 27)

static vm_offset_t arb_core, arb_cnfg, arb_observer;
static uint16_t pon_apid = UMI_NO_APID, gpio_apid = UMI_NO_APID;
static bool keys_started;
static int keys_sleep_chan;

static uint32_t
keys_reg(vm_offset_t base, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
keys_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint16_t
keys_find_apid(uint16_t ppid, uint32_t channels)
{
	uint16_t found = UMI_NO_APID;

	for (uint32_t channel = 0; channel < channels; channel++) {
		uint32_t map = keys_reg(arb_core, ARB_CHANNEL(channel));
		if (map == 0 || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		uint32_t owner = keys_reg(arb_cnfg, ARB_OWNER(channel)) & 7u;
		if (found == UMI_NO_APID || owner == 0) {
			found = (uint16_t)channel;
		}
	}
	return found;
}

/* Read one byte through this execution environment's observer channel. */
static bool
keys_read(uint16_t apid, uint16_t address, uint8_t *value)
{
	if (apid == UMI_NO_APID) {
		return false;
	}
	uint32_t offset = 0x80u * apid; /* EE 0 observer, one 0x80-byte window per APID */
	uint64_t deadline, now;
	clock_interval_to_deadline(2, NSEC_PER_MSEC, &deadline);
	hk_spmi_lock();
	__asm__ volatile("dmb oshst" ::: "memory");
	keys_write(arb_observer, offset + OBS_COMMAND,
	    OBS_READ_LONG | ((uint32_t)(address & 0xffu) << 4));
	do {
		uint32_t status = keys_reg(arb_observer, offset + OBS_STATUS);
		if (status & OBS_DONE) {
			bool okay = !(status & OBS_ERRORS);
			if (okay) {
				*value = (uint8_t)keys_reg(arb_observer, offset + OBS_DATA0);
			}
			hk_spmi_unlock();
			return okay;
		}
		now = mach_absolute_time();
	} while (now < deadline);
	hk_spmi_unlock();
	return false;
}

static void
keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(UMI_KEY_POLL_MS, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&keys_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
keys_post(unsigned key, bool down)
{
	static const uint32_t button[] = {
	    HK_BUTTON_LOCK, HK_BUTTON_VOLUME_UP, HK_BUTTON_VOLUME_DOWN
	};
	hk_tl("UMI keys: %s %s\n", key == 0 ? "power" : key == 1 ? "volume-up" : "volume-down",
	    down ? "pressed" : "released");
	hk_touch_post(down ? HK_BUTTON_DOWN : HK_BUTTON_UP, button[key], 0, 0);
}

static void
keys_thread(__unused void *arg, __unused wait_result_t result)
{
	/* Unknown initial state is observed silently; only subsequent transitions are events. */
	int state[3] = { -1, -1, -1 };
	bool live[3] = { false, false, false };
	unsigned errors = 0;
	for (;;) {
		uint8_t pon = 0, gpio = 0;
		bool pon_ok = keys_read(pon_apid, UMI_PON_RT_STATUS, &pon);
		bool gpio_ok = keys_read(gpio_apid, UMI_GPIO_RT_STATUS, &gpio);
		int current[3] = { !!(pon & 1u), !(gpio & 1u), !!(pon & 2u) };
		if (!pon_ok || !gpio_ok) {
			if ((errors++ % 250u) == 0) {
				hk_tl("UMI keys: PMIC read error (PON=%d GPIO6=%d)\n", pon_ok, gpio_ok);
			}
		} else {
			errors = 0;
		}
		for (unsigned key = 0; key < 3; key++) {
			bool okay = key == 1 ? gpio_ok : pon_ok;
			if (!okay) {
				continue;
			}
			if (state[key] < 0) {
				state[key] = current[key];
				live[key] = current[key] == 0;
			} else if (!live[key]) {
				state[key] = current[key];
				if (!current[key]) {
					live[key] = true;
				}
			} else if (state[key] != current[key]) {
				state[key] = current[key];
				keys_post(key, current[key]);
			}
		}
		keys_wait();
	}
}

void
umi_keys_init(void)
{
	uint32_t enabled = 0;
	DTEntry chosen;
	const void *board;
	unsigned size;
	thread_t thread;

	if (keys_started || !PE_parse_boot_argn("umi_keys", &enabled, sizeof(enabled)) || enabled != 1) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "hackios-board", &board, &size) != kSuccess ||
	    size != 4 || memcmp(board, "umi", 4) != 0) {
		hk_tl("UMI keys: board mismatch; driver not started\n");
		return;
	}
	keys_started = true;
	arb_core = ml_io_map(UMI_ARB_CORE_PA, 0x2000);
	arb_cnfg = ml_io_map(UMI_ARB_CNFG_PA, 0x1000);
	arb_observer = ml_io_map(UMI_ARB_OBSERVER_PA, 0x10000);
	if (!arb_core || !arb_cnfg || !arb_observer) {
		hk_tl("UMI keys: PMIC arbiter mapping failed\n");
		return;
	}
	uint32_t version = keys_reg(arb_core, ARB_VERSION);
	uint32_t channels = keys_reg(arb_core, ARB_FEATURES) & 0x7ffu;
	if (version < 0x50000000u || version >= 0x70000000u || channels == 0 || channels > 512) {
		hk_tl("UMI keys: unsupported PMIC arbiter version=%#x channels=%u\n", version, channels);
		return;
	}
	pon_apid = keys_find_apid(UMI_PON_PPID, channels);
	gpio_apid = keys_find_apid(UMI_GPIO6_PPID, channels);
	uint8_t mode = 0, enable = 0, subtype = 0;
	bool mode_ok = keys_read(gpio_apid, UMI_GPIO_MODE, &mode);
	bool enable_ok = keys_read(gpio_apid, UMI_GPIO_ENABLE, &enable);
	bool subtype_ok = keys_read(gpio_apid, UMI_GPIO_SUBTYPE, &subtype);
	unsigned direction = subtype >= 0x10u && subtype <= 0x13u ? mode & 3u : (mode >> 4) & 7u;
	if (!mode_ok || !enable_ok || !subtype_ok || subtype < 0x10u || subtype > 0x13u ||
	    !(enable & 0x80u) || (direction != 0 && direction != 2)) {
		hk_tl("UMI keys: GPIO6 is not confirmed enabled as input; volume-up withheld\n");
		gpio_apid = UMI_NO_APID;
	}
	hk_tl("UMI keys: arbiter=%#x channels=%u PON-APID=%u GPIO6-APID=%u\n",
	    version, channels, pon_apid, gpio_apid);
	kern_return_t kr = kernel_thread_start(keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		hk_tl("UMI keys: kernel_thread_start failed %d\n", kr);
	}
}
