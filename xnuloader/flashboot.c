/* ------------------------------------------------------------------------ */
/* Standalone boot from the flash, and the dual-boot menu                   */
/*
 * Included by loader.c. An installed phone boots without a computer: the UEFI in the spare A/B slot's
 * boot partition starts this loader with no payload, and the loader
 *
 *   1. shows a menu -- HackiOS (the default, after a few seconds), Android, or developer mode (the USB
 *      link that `xl` talks to, as `xl enter` gives);
 *   2. for HackiOS reads the boot bundle -- the kernel, the boot volume and the command line, the same
 *      "XNUP" container `xl` sends -- from the boot area of darwin into the payload window and runs it
 *      with no time limit: a session lasts until the phone is switched off or restarted;
 *   3. for Android makes the other slot active again and resets.
 *
 * The boot area is the free stretch of darwin between the cryptexes and the var store's slots (README of
 * the userland repository: "Data that survives a reboot"): two bundle slots used in turn, each a header
 * block and the container behind it. An update writes the slot that does not hold the newest good bundle
 * -- zeroed header first, container, header last -- so an update that is cut off loses the update and
 * not the phone: the other slot still boots. The host side is common/flash/mkbundle.py.
 *
 * Android's A/B slots are switched the way Qualcomm's boot control does it: the A/B byte of the GPT
 * attributes of boot_a and boot_b (bits 48..55: priority, active, retry count, successful, unbootable).
 * The loader never invents those bits: going back to Android it exchanges the two entries' A/B bytes,
 * which is exactly the state the phone had before `bootctl set-active-boot-slot` sent it here. It checks
 * the state first (one of the two active, and the other one), writes the backup GPT before the primary
 * one, and reads both back. Nothing else outside darwin is ever written.
 */

#define FB_BLOCK          4096ULL                 /* the boot area is laid out in darwin's 4 KiB blocks */
#define FB_AREA_FIRST     2042368ULL              /* = BLOCK_WRITE_FIRST: right after both cryptexes */
#define FB_SLOT_BLOCKS    24576ULL                /* 96 MiB a slot; two slots end at 2091520 < 2097152 (var store) */
#define FB_SLOTS          2
#define FB_MENU_SECONDS   5

static const char fb_magic[8] = { 'H', 'K', 'B', 'O', 'O', 'T', '0', '1' };

struct fb_header {
	char magic[8];
	uint64_t generation;                    /* the newest good slot wins */
	uint64_t size;                          /* bytes of container behind the header block */
	uint64_t sum;                           /* FNV-1a 64 of the container */
	uint64_t header_sum;                    /* FNV-1a 64 of the four fields above */
};

typedef struct { uint16_t scan, unicode; } efi_input_key;
typedef struct efi_simple_text_in {
	void *reset;
	efi_status (EFIAPI *read_key_stroke)(struct efi_simple_text_in *self, efi_input_key *key);
	void *wait_for_key;
} efi_simple_text_in;

static uint64_t
fb_fnv(const uint8_t *p, uint64_t n, uint64_t h)
{
	for (uint64_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int
fb_name_is(const efi_partition_info *info, const char *want)
{
	char name[40];

	ufs_name(info, name, sizeof(name));
	return str_eq(name, want);
}

/* darwin's BlockIo, wherever the firmware lists it (a phone's UFS unit, a QEMU disk) */
static efi_block_io *
fb_find_darwin(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || !fb_name_is(info, "darwin")) {
			continue;
		}
		return bio;
	}
	return NULL;
}

/* read `bytes` from darwin at 4 KiB block `blk` into `dst` (its own LBA size may be 512 or 4096) */
static int
fb_read(efi_block_io *bio, uint64_t blk, void *dst, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, lba = blk * FB_BLOCK / bs, done = 0;
	const uint64_t chunk = 4ULL << 20;

	bytes = (bytes + bs - 1) / bs * bs;
	while (done < bytes) {
		uint64_t n = bytes - done < chunk ? bytes - done : chunk;
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, lba + done / bs, n, (uint8_t *)dst + done))) {
			return 0;
		}
		done += n;
	}
	return 1;
}

static int
fb_header_ok(const struct fb_header *h)
{
	return memcmp(h->magic, fb_magic, sizeof(fb_magic)) == 0 && h->size > 16 &&
	    h->size <= PAYLOAD_MAX_SIZE - 0x100000ULL && h->size <= (FB_SLOT_BLOCKS - 1) * FB_BLOCK &&
	    fb_fnv((const uint8_t *)h, 32, 0xcbf29ce484222325ULL) == h->header_sum;
}

/*
 * Put the newest good bundle in the payload window. Returns 1 with the container at PAYLOAD_BASE, 0 when
 * there is none (nothing installed, both slots torn): the caller then stays in the menu.
 */
static int
fb_load_bundle(void)
{
	efi_block_io *bio = fb_find_darwin();
	struct fb_header hdr[FB_SLOTS];
	int order[FB_SLOTS] = { 0, 1 };
	uint64_t addr = PAYLOAD_BASE;

	if (bio == NULL) {
		print("flash: no darwin partition: HackiOS is not installed on this device\n");
		return 0;
	}
	for (int s = 0; s < FB_SLOTS; s++) {
		if (!fb_read(bio, FB_AREA_FIRST + s * FB_SLOT_BLOCKS, ufs_buffer, FB_BLOCK)) {
			memset(&hdr[s], 0, sizeof(hdr[s]));
			continue;
		}
		memcpy(&hdr[s], ufs_buffer, sizeof(hdr[s]));
		print("flash: boot slot %d: %s, generation %d, %d bytes\n", (uint64_t)s,
		    fb_header_ok(&hdr[s]) ? "good header" : "empty or torn", hdr[s].generation, hdr[s].size);
	}
	if (fb_header_ok(&hdr[1]) && (!fb_header_ok(&hdr[0]) || hdr[1].generation > hdr[0].generation)) {
		order[0] = 1; order[1] = 0;
	}
	/* the payload window: reserved in the phone's memory map, ordinary RAM in QEMU -- claim it either way */
	if (EFI_ERROR(BS->allocate_pages(2 /* AllocateAddress */, 2 /* EfiLoaderData */, PAYLOAD_MAX_SIZE >> 12, &addr))) {
		print("flash: the payload window is the firmware's (reserved): reading into it as on a USB boot\n");
	}
	for (int k = 0; k < FB_SLOTS; k++) {
		int s = order[k];
		if (!fb_header_ok(&hdr[s])) {
			continue;
		}
		if (!fb_read(bio, FB_AREA_FIRST + s * FB_SLOT_BLOCKS + 1, (void *)PAYLOAD_BASE, hdr[s].size)) {
			print("flash: boot slot %d: read error\n", (uint64_t)s);
			continue;
		}
		if (fb_fnv((const uint8_t *)PAYLOAD_BASE, hdr[s].size, 0xcbf29ce484222325ULL) != hdr[s].sum) {
			print("flash: boot slot %d: contents do not match their sum, trying the other\n", (uint64_t)s);
			continue;
		}
		clean_dcache(PAYLOAD_BASE, PAYLOAD_BASE + hdr[s].size);
		print("flash: booting slot %d, generation %d\n", (uint64_t)s, hdr[s].generation);
		return 1;
	}
	print("flash: no good boot bundle in darwin\n");
	return 0;
}

/* ---- A/B: back to Android ------------------------------------------------ */

#define AB_ACTIVE 0x04                          /* in the A/B byte (attribute bits 48..55): the slot is active */

struct ab_gpt {
	efi_block_io *bio;
	uint64_t bs, entries_lba, alt_lba, count, esize, bytes, read;
	int a, b;                               /* entry indices of boot_a, boot_b */
};

static int
ab_entry_is(const uint8_t *e, const char *want)
{
	for (int i = 0; i < 36; i++) {
		uint16_t c = e[56 + 2 * i] | e[57 + 2 * i] << 8;
		if (c != (uint8_t)want[i]) {
			return 0;
		}
		if (c == 0) {
			return 1;
		}
	}
	return 0;
}

/* read and check one GPT (header at `lba`) into ufs_buffer; returns 1 and fills g on a sound table */
static int
ab_read_gpt(struct ab_gpt *g, uint64_t lba, uint8_t *header)
{
	uint32_t hsize, hcrc;

	if (EFI_ERROR(g->bio->read_blocks(g->bio, g->bio->media->media_id, lba, g->bs, ufs_buffer)) ||
	    memcmp(ufs_buffer, "EFI PART", 8) != 0) {
		return 0;
	}
	memcpy(header, ufs_buffer, 512);
	hsize = *(uint32_t *)(header + 12); hcrc = *(uint32_t *)(header + 16);
	if (hsize < 92 || hsize > 512) {
		return 0;
	}
	*(uint32_t *)(header + 16) = 0;
	if (ufs_crc32(header, hsize) != hcrc) {
		return 0;
	}
	*(uint32_t *)(header + 16) = hcrc;
	g->entries_lba = *(uint64_t *)(header + 72);
	g->alt_lba = *(uint64_t *)(header + 32);
	g->count = *(uint32_t *)(header + 80);
	g->esize = *(uint32_t *)(header + 84);
	g->bytes = g->count * g->esize;
	g->read = (g->bytes + g->bs - 1) / g->bs * g->bs;
	if (g->esize < 128 || g->bytes == 0 || g->read > sizeof(ufs_buffer) / 2 ||
	    EFI_ERROR(g->bio->read_blocks(g->bio, g->bio->media->media_id, g->entries_lba, g->read, ufs_buffer)) ||
	    ufs_crc32(ufs_buffer, g->bytes) != *(uint32_t *)(header + 88)) {
		return 0;
	}
	return 1;
}

/* find the unit whose GPT holds boot_a and boot_b; the primary table is left in ufs_buffer */
static int
ab_find(struct ab_gpt *g, uint8_t *header)
{
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		return 0;
	}
	for (uint64_t i = 0; i < count; i++) {
		memset(g, 0, sizeof(*g));
		if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&g->bio)) || g->bio->media->logical) {
			continue;
		}
		g->bs = g->bio->media->block_size;
		if (g->bs < 512 || !ab_read_gpt(g, 1, header)) {
			continue;
		}
		g->a = g->b = -1;
		for (uint64_t k = 0; k < g->count; k++) {
			const uint8_t *e = ufs_buffer + k * g->esize;
			if (ab_entry_is(e, "boot_a")) g->a = g->a == -1 ? (int)k : -2;
			if (ab_entry_is(e, "boot_b")) g->b = g->b == -1 ? (int)k : -2;
		}
		if (g->a >= 0 && g->b >= 0) {
			return 1;
		}
	}
	return 0;
}

static uint8_t ab_primary[512], ab_backup[512];

/*
 * Make the other slot active: exchange the A/B bytes of boot_a and boot_b, in the backup GPT and then
 * the primary one. Returns 1 when both tables were written and read back right.
 */
static int
ab_switch_back(void)
{
	struct ab_gpt g, gb;
	uint8_t *tab = ufs_buffer, *tab2 = ufs_buffer + sizeof(ufs_buffer) / 2;
	uint8_t ba, bb;

	if (!ab_find(&g, ab_primary)) {
		print("ab: no unit with boot_a and boot_b in a sound GPT: switch with `fastboot --set-active` instead\n");
		return 0;
	}
	ba = tab[g.a * g.esize + 54]; bb = tab[g.b * g.esize + 54];
	print("ab: boot_a A/B byte %x, boot_b %x\n", (uint64_t)ba, (uint64_t)bb);
	if (((ba & AB_ACTIVE) != 0) == ((bb & AB_ACTIVE) != 0)) {
		print("ab: not exactly one slot is active: nothing written\n");
		return 0;
	}
	memcpy(tab2, tab, g.read);                       /* the primary table, kept while the backup is read */
	gb = g;
	if (!ab_read_gpt(&gb, g.alt_lba, ab_backup) || gb.bytes != g.bytes || memcmp(ufs_buffer, tab2, g.bytes) != 0) {
		print("ab: the backup GPT is missing or differs from the primary one: nothing written\n");
		return 0;
	}
	/* the new table: the two bytes exchanged, nothing else */
	tab2[g.a * g.esize + 54] = bb; tab2[g.b * g.esize + 54] = ba;
	uint32_t crc = ufs_crc32(tab2, g.bytes);
	for (int pass = 0; pass < 2; pass++) {               /* 0: backup, 1: primary */
		uint8_t *h = pass ? ab_primary : ab_backup;
		uint64_t hlba = pass ? 1 : g.alt_lba, elba = *(uint64_t *)(h + 72);
		uint32_t hsize = *(uint32_t *)(h + 12);
		*(uint32_t *)(h + 88) = crc;
		*(uint32_t *)(h + 16) = 0;
		*(uint32_t *)(h + 16) = ufs_crc32(h, hsize);
		memset(ufs_buffer, 0, g.bs);
		memcpy(ufs_buffer, h, 512);
		if (EFI_ERROR(g.bio->write_blocks(g.bio, g.bio->media->media_id, elba, g.read, tab2)) ||
		    EFI_ERROR(g.bio->write_blocks(g.bio, g.bio->media->media_id, hlba, g.bs, ufs_buffer))) {
			print("ab: write of the %s GPT failed\n", pass ? "primary" : "backup");
			return 0;
		}
	}
	if (g.bio->flush_blocks) {
		g.bio->flush_blocks(g.bio);
	}
	/* read both back: sound, and the bytes where they should be */
	struct ab_gpt chk = g;
	uint8_t hb[512];
	if (!ab_read_gpt(&chk, 1, hb) || ufs_buffer[g.a * g.esize + 54] != bb || ufs_buffer[g.b * g.esize + 54] != ba ||
	    !ab_read_gpt(&chk, g.alt_lba, hb) || ufs_buffer[g.a * g.esize + 54] != bb || ufs_buffer[g.b * g.esize + 54] != ba) {
		print("ab: read back does not match: the phone may need `fastboot --set-active=b`\n");
		return 0;
	}
	print("ab: slots exchanged: boot_a %x, boot_b %x\n", (uint64_t)bb, (uint64_t)ba);
	return 1;
}

/* is there an A/B pair to go back to (without writing anything) */
static int
ab_available(void)
{
	struct ab_gpt g;
	return ab_find(&g, ab_primary);
}

/* ---- the menu ------------------------------------------------------------ */

enum { MENU_HACKIOS, MENU_ANDROID, MENU_DEVELOPER, MENU_ITEMS };
static const char *const menu_names[MENU_ITEMS] = { "HackiOS", "Android", "Developer mode (USB, xl)" };

static void
menu_draw(int sel, int have[MENU_ITEMS], int secs)
{
	print("\n  HACKiOS boot menu        volume: move   power: select\n");
	for (int i = 0; i < MENU_ITEMS; i++) {
		if (!have[i]) {
			continue;
		}
		print("   %s %s\n", i == sel ? ">" : " ", menu_names[i]);
	}
	if (secs >= 0) {
		print("  %s in %d s\n", menu_names[sel], (uint64_t)secs);
	}
}

/*
 * The choice: volume up/down move, any other key (power) takes the highlighted entry; with no key for
 * FB_MENU_SECONDS the default (HackiOS when installed) is taken. Returns a MENU_* value.
 */
static int
fb_menu(int have[MENU_ITEMS])
{
	efi_simple_text_in *in = (efi_simple_text_in *)ST->con_in;
	int sel = have[MENU_HACKIOS] ? MENU_HACKIOS : MENU_DEVELOPER, touched = 0;
	uint64_t ticks = 0;

	menu_draw(sel, have, FB_MENU_SECONDS);
	flush_log();
	for (;;) {
		efi_input_key key;
		if (in != NULL && !EFI_ERROR(in->read_key_stroke(in, &key))) {
			if (key.scan == 1 || key.scan == 2) {               /* up, down */
				touched = 1;
				do {
					sel = (sel + (key.scan == 2 ? 1 : MENU_ITEMS - 1)) % MENU_ITEMS;
				} while (!have[sel]);
				menu_draw(sel, have, -1);
			} else {
				return sel;
			}
		}
		BS->stall(50000);
		if (!touched && ++ticks % 20 == 0) {
			int left = FB_MENU_SECONDS - (int)(ticks / 20);
			if (left <= 0) {
				return sel;
			}
			print("  %s in %d s\n", menu_names[sel], (uint64_t)left);
		}
	}
}

/* The boot of an installed device: the menu, then what was chosen. Does not return. */
static void
fb_standalone(void)
{
	int have[MENU_ITEMS] = { 1, 0, 1 };

	have[MENU_HACKIOS] = fb_find_darwin() != NULL;
	have[MENU_ANDROID] = ab_available();
	for (;;) {
		int choice = fb_menu(have);
		print("menu: %s\n", menu_names[choice]);
		if (choice == MENU_HACKIOS) {
			if (fb_load_bundle()) {
				run_kernel(0, 0);               /* until the phone is switched off; if it comes back, reset */
				reboot();
			}
			have[MENU_HACKIOS] = 0;                 /* nothing bootable there: offer the rest */
		} else if (choice == MENU_ANDROID) {
			if (ab_switch_back()) {
				reboot();
			}
			have[MENU_ANDROID] = 0;
		} else {
			if (usb_start()) {
				flush_log();
				usb_command_loop();
			}
			print("menu: no USB link here\n");
		}
	}
}
