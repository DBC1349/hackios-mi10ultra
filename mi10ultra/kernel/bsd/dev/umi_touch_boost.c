/* UMI stock always-on boost: PM8150B GPIO5. Only VIN1/output-high
 * are writable, after checking the inherited peripheral and apps ownership.
 * All accesses run on the single UMI touch thread and share the PMIC bus
 * lock with the button and other SPMI drivers. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <sys/hk_input.h>
/* Preserve hardware diagnostics in the USB log even in quiet UI boots. */
extern void hk_tl(const char *, ...) __printflike(1, 2);
#define printf hk_tl
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
int umi_touch_boost_enable(void);
static vm_offset_t core, cfg, obs, channel;
static bool poisoned;
static uint32_t rd(vm_offset_t base, unsigned off)
{
    uint32_t v = *(volatile uint32_t *)(base + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static void wr(vm_offset_t base, unsigned off, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(base + off) = value;
    __asm__ volatile("dsb osh" ::: "memory");
}
static int done(vm_offset_t base)
{
    for (unsigned i = 0; i < 1000; ++i) {
        unsigned status = rd(base, 8);
        if (status & 1) {
            if (!(status & 14)) return 0;
            poisoned = true;
            return EIO;
        }
        delay(1);
    }
    poisoned = true;
    return ETIMEDOUT;
}
static int read_reg(unsigned reg, uint8_t *value)
{
    if (poisoned || (reg != 5 && reg != 0x40 && reg != 0x41 && reg != 0x44 && reg != 0x46)) return EINVAL;
    hk_spmi_lock();
    wr(obs, 0, (1u << 27) | (reg << 4));
    int err = done(obs);
    if (!err) *value = rd(obs, 0x18);
    hk_spmi_unlock();
    return err;
}
static int write_reg(unsigned reg, uint8_t value)
{
    if (poisoned || !((reg == 0x41 && value == 1) || (reg == 0x44 && value == 0x80))) return EINVAL;
    hk_spmi_lock();
    wr(channel, 0x10, value);
    wr(channel, 0, reg << 4);
    int err = done(channel);
    hk_spmi_unlock();
    uint8_t actual = 0;
    if (!err) err = read_reg(reg, &actual);
    if (!err && actual != value) { poisoned = true; err = EIO; }
    return err;
}
int umi_touch_boost_enable(void)
{
    core = ml_io_map(0x0c440000u, 0x2000);
    cfg = ml_io_map(0x0c40a000u, 0x1000);
    if (!core || !cfg) return ENOMEM;
    unsigned version = rd(core, 0), count = rd(core, 4) & 0x7ff;
    if (version < 0x50000000u || version >= 0x70000000u || !count || count > 512) return ENXIO;
    unsigned apid = 0, matches = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned map = rd(core, 0x900 + i * 4);
        if (map && ((map >> 8) & 0xfff) == 0x2c4 && (rd(cfg, 0x700 + i * 4) & 7) == 0) { apid = i; ++matches; }
    }
    if (matches != 1) return ENXIO;
    obs = ml_io_map(0x0e600000u + apid * 0x80u, 0x80);
    channel = ml_io_map(0x0c600000u + apid * 0x10000u, 0x100);
    if (!obs || !channel) return ENOMEM;
    uint8_t subtype = 0, mode = 0, vin = 0, output = 0, enable = 0;
    int err = read_reg(5, &subtype);
    if (!err) err = read_reg(0x40, &mode);
    if (!err) err = read_reg(0x41, &vin);
    if (!err) err = read_reg(0x44, &output);
    if (!err) err = read_reg(0x46, &enable);
    if (err) return err;
    printf("UMI boost: subtype=%#x mode=%#x vin=%u out=%#x en=%#x apid=%u\n", subtype, mode, vin, output, enable, apid);
    /* LV/MV GPIO source 1 is the inherited paired-GPIO source. Select the
     * same direct output-high configuration used by the working cold boot. */
    if (subtype != 0x11 || mode != 1 || vin > 1 || enable != 0x80 ||
        (output != 0 && output != 1 && output != 0x80)) return ENXIO;
    if (vin != 1) err = write_reg(0x41, 1);
    if (!err && output != 0x80) err = write_reg(0x44, 0x80);
    if (!err) delay(5000);
    if (!err) printf("UMI boost: VIN1 and direct output-high verified\n");
    return err;
}
