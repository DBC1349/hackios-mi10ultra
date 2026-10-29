/*
 * The data volume of the system image, kept in the phone's own storage.
 *
 * iOS's /private/var is a memory filesystem (bsd/miscfs/ramfs): what it holds lives in RAM, and
 * the whole of it is packed into darwin's writable region when a run ends and unpacked out of it
 * at the next boot (userland/src/var_store.c). That copy is a copy: the RAM is the volume, so the
 * volume can be no larger than the RAM left over for it.
 *
 * Here the flash is the volume instead. A file's pages live in darwin -- the one partition of the
 * storage this port owns and the one place a write may reach (bsd/dev/hk_ufs.c, ufs_write, from
 * UFS_WRITE_FIRST on) -- and only the pages a run is using are in RAM. What that costs is this
 * file and the ramfs change beside it (bsd/miscfs/ramfs/ramfs.c, the UFS-backed page); what it
 * buys is a volume as large as the flash region and a RAM footprint that follows the working set
 * rather than the volume.
 *
 * This file, in the order the work went: where the area is; proof that the kernel can write to it
 * and read it back; and then the block map -- which of the area's blocks are taken -- kept in the
 * metadata and written back, so that the next boot starts from the map this one ended with.
 *
 *   [ UFS_WRITE_FIRST .. +UFSVAR_META_BLOCKS )        the metadata
 *       +0                                            the header: geometry, counts, generation
 *       +1 .. +1+map_blocks-1                         the block map, one bit a data block
 *       ...                                           the tree's room (the next step of the work)
 *   [ +UFSVAR_META_BLOCKS .. the partition's end )    the data area itself
 *
 * Nothing here is written by anything else, and nothing else of darwin -- the iOS volume at its
 * start, the two cryptexes behind it -- is ever written at all: ufs_write refuses those by
 * construction, and this file only ever asks for blocks at or past UFS_WRITE_FIRST.
 *
 * A note on one set of blocks this file does not hand out: the two slots of the var store's
 * archive (userland/src/var_store.c, STORE_AT and SLOT_BYTES) sit in the middle of the data area,
 * because that archive is where /private/var goes today. They are marked taken when the map is
 * made, so that a run of this step cannot allocate what the saver is about to overwrite. When the
 * tree and the files themselves move onto darwin -- the step after the page cache -- the archive
 * goes, and so does this reservation.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/hk_input.h>

#define UFSVAR_BLOCK        4096ULL

/*
 * The metadata's room, in blocks, at the start of the writable region: the block map (one bit per
 * data block: 12 GB of data is 385 KB of map, 128 GB is 3.8 MB) and the tree, both small beside what
 * they describe. The data area is everything after it. 2048 blocks is 8 MiB: enough for a 128 GiB
 * volume's map (962 blocks) with room for the tree, and a 256 GiB volume's map would still not fit,
 * so a volume past ~200 GiB would need this raised again.
 */
#define UFSVAR_META_BLOCKS  2048ULL             /* 8 MiB */

/* The header, the first block of the metadata. */
#define UFSVAR_MAGIC        0x31535655ULL       /* "UVS1" */
#define UFSVAR_VERSION      1

/*
 * userland/src/var_store.c keeps /private/var's archive here: STORE_AT / 4096 for the first slot,
 * two slots of SLOT_BYTES. The archive is metadata now (names, attributes, block lists), not the
 * data, so a slot of 256 MiB is six times a full 120 GiB volume's block lists. The map marks the
 * two slots taken (see the note at the top).
 */
#define UFSVAR_STORE_AT     2097152ULL          /* darwin block of the var store's first slot */
#define UFSVAR_STORE_BLOCKS (2ULL * 1048576ULL) /* two slots of 4 GiB, in blocks of 4096 */
#define UFSVAR_MARKER_BLOCK 2091521ULL          /* the loader's identity marker (darwinfile.c): keep it */

struct ufsvar_header {
	uint64_t magic;
	uint32_t version;
	uint32_t sum;                   /* FNV-32 of the block with these four bytes read as zero */
	uint64_t data_first;            /* the darwin block the data area starts at */
	uint64_t data_blocks;           /* how many blocks the data area has */
	uint64_t map_bytes;             /* bytes of the block map that are in use */
	uint64_t map_blocks;            /* metadata blocks the map takes */
	uint64_t used;                  /* blocks marked in use when this was written */
	uint64_t generation;            /* one more each time it is written */
	uint64_t tests;                 /* self-tests that have passed so far */
	uint8_t  pad[UFSVAR_BLOCK - 72];
};

static uint32_t ufsvar_part = 0xffffffff;       /* the partition writes go to */
static uint64_t ufsvar_first;                   /* the data area's first block, darwin's numbering */
static uint64_t ufsvar_blocks;                  /* how many blocks the data area has */
static uint64_t ufsvar_meta;                    /* the metadata's first block */

static uint8_t *ufsvar_map;                     /* the map: bit i, data-area block i is taken */
static uint64_t ufsvar_map_bytes;               /* how much of it is in use */
static uint64_t ufsvar_map_blocks;              /* how many metadata blocks it takes */
static uint64_t ufsvar_used;                    /* blocks marked in use */
static uint64_t ufsvar_generation = 1;          /* the header's, for the next write */
static uint64_t ufsvar_tests;                   /* self-tests that have passed */
static uint64_t ufsvar_next;                    /* where the search for a free block starts */
static uint64_t ufsvar_kept;                    /* the var store's blocks that are inside the area */
static bool ufsvar_dirty;                       /* the map on the flash is behind this one */
static bool ufsvar_up;                          /* there is an area, and a map over it */

static LCK_GRP_DECLARE(ufsvar_lck_grp, "hk-ufsvar");
static LCK_MTX_DECLARE(ufsvar_lock, &ufsvar_lck_grp);

SYSCTL_DECL(_kern_hackios);
SYSCTL_QUAD(_kern_hackios, OID_AUTO, ufsvar_blocks, CTLFLAG_RD | CTLFLAG_LOCKED, &ufsvar_blocks,
    "blocks of darwin the data volume may use");
SYSCTL_QUAD(_kern_hackios, OID_AUTO, ufsvar_used, CTLFLAG_RD | CTLFLAG_LOCKED, &ufsvar_used,
    "blocks of the data volume marked in use");
SYSCTL_QUAD(_kern_hackios, OID_AUTO, ufsvar_generation, CTLFLAG_RD | CTLFLAG_LOCKED, &ufsvar_generation,
    "times the block map has been written to darwin");

static uint64_t
ufsvar_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}

	return h;
}

/* The header's own sum: over the whole block, with the sum's four bytes skipped. */
static uint32_t
ufsvar_sum(const uint8_t *block)
{
	uint64_t h = ufsvar_fnv(0xcbf29ce484222325ULL, block, 12);

	h = ufsvar_fnv(h, block + 16, UFSVAR_BLOCK - 16);
	return (uint32_t)(h ^ (h >> 32));
}

/* ---- the map: one bit a block of the data area, the low bit of a byte first ---- */

static bool
ufsvar_taken(uint64_t index)
{
	return (ufsvar_map[index >> 3] >> (index & 7)) & 1;
}

static void
ufsvar_take(uint64_t index)
{
	ufsvar_map[index >> 3] |= (uint8_t)(1u << (index & 7));
}

static void
ufsvar_give(uint64_t index)
{
	ufsvar_map[index >> 3] &= (uint8_t)~(1u << (index & 7));
}

/* Whether a darwin block is one of the area's, and which of them if so. */
static bool
ufsvar_inside(uint64_t block, uint64_t *index)
{
	if (block < ufsvar_first || block >= ufsvar_first + ufsvar_blocks) {
		return false;
	}
	*index = block - ufsvar_first;
	return true;
}

/*
 * The map written into the metadata: the map first and the header after it, because a header that
 * points at a map which is not there yet is worse than a map whose header is one generation behind.
 * Called with the lock held, or from the single thread that brings the volume up.
 */
static int
ufsvar_save(void)
{
	uint8_t *block;
	struct ufsvar_header *header;
	int error;

	if (!ufsvar_up) {
		return ENXIO;
	}
	block = kalloc_data(UFSVAR_BLOCK, Z_WAITOK);
	if (block == NULL) {
		return ENOMEM;
	}
	error = hk_ufs_write_blocks((int)ufsvar_part, (ufsvar_meta + 1) * UFSVAR_BLOCK,
	    ufsvar_map, ufsvar_map_blocks * UFSVAR_BLOCK);
	if (error == 0) {
		memset(block, 0, UFSVAR_BLOCK);
		header = (struct ufsvar_header *)block;
		header->magic = UFSVAR_MAGIC;
		header->version = UFSVAR_VERSION;
		header->data_first = ufsvar_first;
		header->data_blocks = ufsvar_blocks;
		header->map_bytes = ufsvar_map_bytes;
		header->map_blocks = ufsvar_map_blocks;
		header->used = ufsvar_used;
		header->generation = ufsvar_generation;
		header->tests = ufsvar_tests;
		header->sum = ufsvar_sum(block);
		error = hk_ufs_write_blocks((int)ufsvar_part, ufsvar_meta * UFSVAR_BLOCK, block,
		    UFSVAR_BLOCK);
	}
	if (error == 0) {
		ufsvar_generation++;
		ufsvar_dirty = false;
	}
	kfree_data(block, UFSVAR_BLOCK);
	return error;
}

/*
 * The map out of the metadata: 0 when what came back is one of ours and describes this area, the
 * map in RAM replaced by it; anything else and the map stays as it is, for the caller to make a
 * new one.
 */
static int
ufsvar_load(void)
{
	uint8_t *block = kalloc_data(UFSVAR_BLOCK, Z_WAITOK);
	struct ufsvar_header *header;
	int error, answer = -1;

	if (block == NULL) {
		return ENOMEM;
	}
	error = hk_ufs_read_blocks((int)ufsvar_part, ufsvar_meta * UFSVAR_BLOCK, block, UFSVAR_BLOCK);
	if (error == 0) {
		header = (struct ufsvar_header *)block;
		if (header->magic == UFSVAR_MAGIC && header->version == UFSVAR_VERSION &&
		    header->data_first == ufsvar_first && header->data_blocks == ufsvar_blocks &&
		    header->map_bytes == ufsvar_map_bytes && header->map_blocks == ufsvar_map_blocks &&
		    header->used <= ufsvar_blocks && header->sum == ufsvar_sum(block)) {
			error = hk_ufs_read_blocks((int)ufsvar_part, (ufsvar_meta + 1) * UFSVAR_BLOCK,
			    ufsvar_map, ufsvar_map_blocks * UFSVAR_BLOCK);
			if (error == 0) {
				ufsvar_used = header->used;
				ufsvar_generation = header->generation + 1;
				ufsvar_tests = header->tests;
				ufsvar_dirty = false;
				answer = 0;
			}
		}
	}
	kfree_data(block, UFSVAR_BLOCK);
	return answer;
}

/*
 * The var store's blocks inside the area, marked taken. Idempotent, and run whether the map came
 * off the flash or was just made: a map written by a build before this reservation existed gets it
 * too, and one that already has it is not changed.
 */
static void
ufsvar_keep_store(void)
{
	uint64_t first = UFSVAR_STORE_AT, last = first + UFSVAR_STORE_BLOCKS, index;

	if (last <= ufsvar_first || first >= ufsvar_first + ufsvar_blocks) {
		return;                 /* the store is not in the area: nothing to hold back */
	}
	if (first < ufsvar_first) {
		first = ufsvar_first;
	}
	if (last > ufsvar_first + ufsvar_blocks) {
		last = ufsvar_first + ufsvar_blocks;
	}
	for (uint64_t block = first; block < last; block++) {
		if (ufsvar_inside(block, &index) && !ufsvar_taken(index)) {
			ufsvar_take(index);
			ufsvar_used++;
		}
	}
	ufsvar_kept = last - first;
	/* The loader verifies the whole volume by one marker block (darwinfile.c DF_MARK_BLOCK);
	 * it sits in the data area just before the store, so it must never be handed out. */
	if (ufsvar_inside(UFSVAR_MARKER_BLOCK, &index) && !ufsvar_taken(index)) {
		ufsvar_take(index);
		ufsvar_used++;
	}
}

/*
 * Where the data area is: darwin's writable region, less the metadata at its start. Read from the
 * kernel's own knowledge of the partition (hk_ufs_part_bytes) and of where a write may begin
 * (hk_ufs_write_first), so that every address the area ever asks for is one ufs_write accepts.
 */
static int
ufsvar_area(void)
{
	int part = hk_ufs_write_part();
	uint64_t first, bytes, blocks, meta;

	if (part < 0) {
		printf("ufsvar: no partition takes writes (ufs_write=1, and a darwin the loader knows): "
		    "the data volume stays in memory\n");
		return -1;
	}
	first = hk_ufs_write_first();
	bytes = hk_ufs_part_bytes(part);
	if (bytes < (first + UFSVAR_META_BLOCKS + 16) * UFSVAR_BLOCK) {
		printf("ufsvar: partition %d is %llu bytes: too small to hold a data area past block %llu\n",
		    part, bytes, first);
		return -1;
	}
	blocks = bytes / UFSVAR_BLOCK;
	meta = first;
	ufsvar_part = (uint32_t)part;
	ufsvar_meta = meta;
	ufsvar_first = meta + UFSVAR_META_BLOCKS;
	ufsvar_blocks = blocks - ufsvar_first;
	printf("ufsvar: partition %d, %llu blocks (%llu MB), writable from block %llu\n",
	    part, blocks, (blocks * UFSVAR_BLOCK) >> 20, first);
	printf("ufsvar: the metadata at block %llu (%llu KB), the data area blocks %llu..%llu "
	    "(%llu blocks, %llu MB)\n", meta, (UFSVAR_META_BLOCKS * UFSVAR_BLOCK) >> 10,
	    ufsvar_first, blocks - 1, ufsvar_blocks, (ufsvar_blocks * UFSVAR_BLOCK) >> 20);
	return 0;
}

/* The map's room: one bit a block, rounded up to whole blocks, inside the metadata's first ones. */
static int
ufsvar_map_make(void)
{
	uint64_t bytes, blocks, size;

	bytes = (ufsvar_blocks + 7) / 8;
	blocks = (bytes + UFSVAR_BLOCK - 1) / UFSVAR_BLOCK;
	if (blocks + 1 > UFSVAR_META_BLOCKS) {
		printf("ufsvar: a map of %llu blocks does not fit in the metadata's %llu\n", blocks,
		    UFSVAR_META_BLOCKS);
		return -1;
	}
	size = blocks * UFSVAR_BLOCK;
	ufsvar_map = kalloc_data(size, Z_WAITOK);
	if (ufsvar_map == NULL) {
		printf("ufsvar: no memory for a map of %llu KB\n", size >> 10);
		return -1;
	}
	memset(ufsvar_map, 0, size);
	ufsvar_map_bytes = bytes;
	ufsvar_map_blocks = blocks;
	ufsvar_up = true;
	printf("ufsvar: the map is %llu KB in %llu blocks of the metadata, one bit a block of the "
	    "data area\n", size >> 10, blocks);
	return 0;
}

/*
 * The area's last block, written with a pattern and read back: the one block of it nothing else
 * has a use for yet, and the shortest proof that a write of ours reaches the flash and comes back.
 * Kept from the first step, which is how the whole thing was known to work at all.
 */
static void
ufsvar_pattern(void)
{
	uint64_t block = ufsvar_first + ufsvar_blocks - 1;
	uint8_t *out = kalloc_data(UFSVAR_BLOCK, Z_WAITOK);
	uint8_t *back = kalloc_data(UFSVAR_BLOCK, Z_WAITOK);
	uint64_t want, got;
	int error;

	if (out == NULL || back == NULL) {
		printf("ufsvar: no memory for the self-test\n");
		if (out != NULL) {
			kfree_data(out, UFSVAR_BLOCK);
		}
		if (back != NULL) {
			kfree_data(back, UFSVAR_BLOCK);
		}
		return;
	}
	for (uint64_t i = 0; i < UFSVAR_BLOCK; i++) {
		out[i] = (uint8_t)((block >> ((i % 8) * 8)) ^ (i * 31));
	}
	want = ufsvar_fnv(0xcbf29ce484222325ULL, out, UFSVAR_BLOCK);

	memset(back, 0, UFSVAR_BLOCK);
	error = hk_ufs_write_blocks((int)ufsvar_part, block * UFSVAR_BLOCK, out, UFSVAR_BLOCK);
	if (error != 0) {
		printf("ufsvar: the write of block %llu failed: %d\n", block, error);
		goto out;
	}
	error = hk_ufs_read_blocks((int)ufsvar_part, block * UFSVAR_BLOCK, back, UFSVAR_BLOCK);
	if (error != 0) {
		printf("ufsvar: the read back of block %llu failed: %d\n", block, error);
		goto out;
	}
	got = ufsvar_fnv(0xcbf29ce484222325ULL, back, UFSVAR_BLOCK);
	if (got != want) {
		printf("ufsvar: block %llu came back as %llx, not %llx\n", block, got, want);
		goto out;
	}
	printf("ufsvar: block %llu written and read back, %llu bytes, FNV32 0x%llx: PASS\n",
	    block, UFSVAR_BLOCK, got & 0xffffffffULL);

out:
	kfree_data(out, UFSVAR_BLOCK);
	kfree_data(back, UFSVAR_BLOCK);
}

/* ---- the map, as the rest of the kernel asks for it ---- */

/*
 * A block of the data area, in darwin's numbering, or 0 when the area is full (the area begins at
 * block UFS_WRITE_FIRST, so 0 is never one of its own).
 */
uint64_t
hk_ufsvar_alloc(void)
{
	uint64_t index, n;

	if (!ufsvar_up) {
		return 0;
	}
	lck_mtx_lock(&ufsvar_lock);
	for (n = 0; n < ufsvar_blocks; n++) {
		index = ufsvar_next + n;
		if (index >= ufsvar_blocks) {
			index -= ufsvar_blocks;
		}
		if (!ufsvar_taken(index)) {
			ufsvar_take(index);
			ufsvar_used++;
			ufsvar_dirty = true;
			ufsvar_next = index + 1 < ufsvar_blocks ? index + 1 : 0;
			lck_mtx_unlock(&ufsvar_lock);
			return ufsvar_first + index;
		}
	}
	lck_mtx_unlock(&ufsvar_lock);
	return 0;
}

/* The same block back. One that was not taken, or is not the area's, is left alone. */
void
hk_ufsvar_free(uint64_t block)
{
	uint64_t index;

	if (!ufsvar_up || !ufsvar_inside(block, &index)) {
		return;
	}
	lck_mtx_lock(&ufsvar_lock);
	if (ufsvar_taken(index)) {
		ufsvar_give(index);
		ufsvar_used--;
		ufsvar_dirty = true;
		if (index < ufsvar_next) {
			ufsvar_next = index;
		}
	}
	lck_mtx_unlock(&ufsvar_lock);
}

/* Whether a block is taken; false for one that is not the area's at all. */
bool
hk_ufsvar_taken(uint64_t block)
{
	uint64_t index;
	bool taken;

	if (!ufsvar_up || !ufsvar_inside(block, &index)) {
		return false;
	}
	lck_mtx_lock(&ufsvar_lock);
	taken = ufsvar_taken(index);
	lck_mtx_unlock(&ufsvar_lock);
	return taken;
}

/* How many of the area's blocks are free. */
uint64_t
hk_ufsvar_free_blocks(void)
{
	uint64_t free_blocks = 0;

	if (!ufsvar_up) {
		return 0;
	}
	lck_mtx_lock(&ufsvar_lock);
	free_blocks = ufsvar_used < ufsvar_blocks ? ufsvar_blocks - ufsvar_used : 0;
	lck_mtx_unlock(&ufsvar_lock);
	return free_blocks;
}

/*
 * The map into the metadata, if it has changed since the last time. Called at the end of a run
 * (osfmk/arm64/loader_return.c) and by kern.hackios.ufsvar_save when a run wants it sooner.
 */
int
hk_ufsvar_save(const char *why)
{
	int error;

	if (!ufsvar_up) {
		return ENXIO;
	}
	lck_mtx_lock(&ufsvar_lock);
	if (!ufsvar_dirty) {
		lck_mtx_unlock(&ufsvar_lock);
		return 0;
	}
	error = ufsvar_save();
	printf("ufsvar: %s: the map written, generation %llu, %llu of %llu blocks in use%s\n", why,
	    ufsvar_generation - 1, ufsvar_used, ufsvar_blocks,
	    error ? " (it did not go: the flash refused it)" : "");
	lck_mtx_unlock(&ufsvar_lock);
	return error;
}

/* How the volume stands, in one line, for whoever asks. */
void
hk_ufsvar_report(const char *why)
{
	uint64_t used, blocks, kept;

	if (!ufsvar_up) {
		printf("ufsvar: %s: no data volume this run\n", why);
		return;
	}
	lck_mtx_lock(&ufsvar_lock);
	used = ufsvar_used;
	blocks = ufsvar_blocks;
	kept = ufsvar_kept;
	lck_mtx_unlock(&ufsvar_lock);
	printf("ufsvar: %s: %llu of %llu blocks in use (%llu of them the var store's), %llu free, "
	    "%llu MB free of %llu MB\n", why, used, blocks, kept, blocks - used,
	    ((blocks - used) * UFSVAR_BLOCK) >> 20, (blocks * UFSVAR_BLOCK) >> 20);
}

/* How many blocks the area has altogether. */
uint64_t
hk_ufsvar_blocks(void)
{
	return ufsvar_up ? ufsvar_blocks : 0;
}

/*
 * The data area as the page store asks (bsd/miscfs/ramfs, ufs_var=1): one block, whole UFSVAR_BLOCK
 * bytes, in darwin's numbering. The rest of the map's callers reach for hk_ufs_read_blocks with the
 * partition number in hand; these two spare them that.
 */
int
hk_ufsvar_read(uint64_t block, void *dst, uint64_t len)
{
	if (!ufsvar_up) {
		return ENXIO;
	}
	return hk_ufs_read_blocks((int)ufsvar_part, block * UFSVAR_BLOCK, dst, len);
}

int
hk_ufsvar_write(uint64_t block, const void *src, uint64_t len)
{
	if (!ufsvar_up) {
		return ENXIO;
	}
	return hk_ufs_write_blocks((int)ufsvar_part, block * UFSVAR_BLOCK, src, len);
}

/* The block this whole thing is made of: 4096 bytes, the partition's own. A ramfs page is several
 * of them (PAGE_SIZE / this, 4 on this kernel), and the page store takes a run of that many at a
 * time so its reads and writes stay whole blocks in darwin's numbering. */
uint64_t
hk_ufsvar_block_size(void)
{
	return UFSVAR_BLOCK;
}

/* n consecutive blocks of the data area, in darwin's numbering, or 0 when no run that long is free. */
uint64_t
hk_ufsvar_alloc_run(uint64_t n)
{
	uint64_t start, i;

	if (!ufsvar_up || n == 0 || n > ufsvar_blocks) {
		return 0;
	}
	lck_mtx_lock(&ufsvar_lock);
	for (start = 0; start + n <= ufsvar_blocks; start++) {
		bool free_run = true;

		for (i = 0; i < n; i++) {
			if (ufsvar_taken(start + i)) {
				free_run = false;
				break;
			}
		}
		if (free_run) {
			for (i = 0; i < n; i++) {
				ufsvar_take(start + i);
			}
			ufsvar_used += n;
			ufsvar_dirty = true;
			ufsvar_next = start + n < ufsvar_blocks ? start + n : 0;
			lck_mtx_unlock(&ufsvar_lock);
			return ufsvar_first + start;
		}
	}
	lck_mtx_unlock(&ufsvar_lock);
	return 0;
}

/* The run of n blocks back. Blocks outside the area, or already free, are left alone. */
void
hk_ufsvar_free_run(uint64_t first, uint64_t n)
{
	uint64_t index, i;

	if (!ufsvar_up || n == 0 || !ufsvar_inside(first, &index)) {
		return;
	}
	lck_mtx_lock(&ufsvar_lock);
	for (i = 0; i < n && index + i < ufsvar_blocks; i++) {
		if (ufsvar_taken(index + i)) {
			ufsvar_give(index + i);
			ufsvar_used--;
		}
	}
	if (index < ufsvar_next) {
		ufsvar_next = index;
	}
	ufsvar_dirty = true;
	lck_mtx_unlock(&ufsvar_lock);
}

/* kern.hackios.ufsvar_save: 1 writes the map now, and says how that went */
static int
ufsvar_sysctl_save(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2,
    struct sysctl_req *req)
{
	int value = 0, error, changed = 0;

	error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error != 0 || !changed || value != 1) {
		return error;
	}
	return hk_ufsvar_save("kern.hackios.ufsvar_save=1");
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, ufsvar_save, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufsvar_sysctl_save, "I", "1: the block map is written into darwin now");

/*
 * The second step, proved: a block is taken, written and read back; the map goes to the flash and
 * comes out of it with that block still taken; then it is given back and the map written again. So
 * what a boot of this step leaves behind is the map it started with, and the next boot is where
 * this one began -- which is also what makes running it twice say the same thing twice.
 */
static void
ufsvar_selftest(void)
{
	uint64_t before, a, b, again, index;
	uint8_t *out = NULL, *back = NULL;
	uint64_t want, got;
	int error;

	before = ufsvar_used;
	a = hk_ufsvar_alloc();
	b = hk_ufsvar_alloc();
	if (a == 0 || b == 0) {
		printf("ufsvar: self-test: the area is full: %llu of %llu blocks in use\n", before,
		    ufsvar_blocks);
		return;
	}
	if (b != a + 1) {
		printf("ufsvar: self-test: two blocks in a row came as %llu and %llu, not a pair\n", a, b);
		goto give;
	}
	hk_ufsvar_free(a);
	again = hk_ufsvar_alloc();
	if (again != a) {
		printf("ufsvar: self-test: block %llu given back came as %llu the second time\n", a, again);
		goto give;
	}

	out = kalloc_data(UFSVAR_BLOCK, Z_WAITOK);
	back = kalloc_data(UFSVAR_BLOCK, Z_WAITOK);
	if (out == NULL || back == NULL) {
		printf("ufsvar: self-test: no memory for the pattern\n");
		goto give;
	}
	for (uint64_t i = 0; i < UFSVAR_BLOCK; i++) {
		out[i] = (uint8_t)((a >> ((i % 8) * 8)) ^ (i * 31));
	}
	want = ufsvar_fnv(0xcbf29ce484222325ULL, out, UFSVAR_BLOCK);
	error = hk_ufs_write_blocks((int)ufsvar_part, a * UFSVAR_BLOCK, out, UFSVAR_BLOCK);
	if (error != 0) {
		printf("ufsvar: self-test: the write of block %llu failed: %d\n", a, error);
		goto give;
	}

	/* the map onto the flash, then out of it again, with a and b in it */
	error = hk_ufsvar_save("self-test");
	if (error != 0) {
		printf("ufsvar: self-test: the map did not go to the flash: %d\n", error);
		goto give;
	}
	memset(ufsvar_map, 0, ufsvar_map_blocks * UFSVAR_BLOCK);
	ufsvar_used = 0;
	if (ufsvar_load() != 0) {
		printf("ufsvar: self-test: the map did not come back from the flash\n");
		ufsvar_keep_store();
		goto give;
	}
	ufsvar_keep_store();
	if (!ufsvar_inside(a, &index) || !ufsvar_taken(index)) {
		printf("ufsvar: self-test: block %llu is not in the map that came back\n", a);
		goto give;
	}
	if (ufsvar_used != before + 2) {
		printf("ufsvar: self-test: the map that came back has %llu in use, not %llu\n",
		    ufsvar_used, before + 2);
		goto give;
	}

	/* and the data in that block is still there, not only the bit that says so */
	memset(back, 0, UFSVAR_BLOCK);
	error = hk_ufs_read_blocks((int)ufsvar_part, a * UFSVAR_BLOCK, back, UFSVAR_BLOCK);
	got = error == 0 ? ufsvar_fnv(0xcbf29ce484222325ULL, back, UFSVAR_BLOCK) : 0;
	if (error != 0 || got != want) {
		printf("ufsvar: self-test: block %llu did not read back after the map went and came: %d, "
		    "%llx not %llx\n", a, error, got, want);
		goto give;
	}

	ufsvar_tests++;
	printf("ufsvar: self-test: blocks %llu and %llu taken, %llu given back and taken again, written "
	    "and read back; the map written (generation %llu), read out of the flash with both still in "
	    "it, and written again: PASS\n", a, b, a, ufsvar_generation - 1);

give:
	if (out != NULL) {
		kfree_data(out, UFSVAR_BLOCK);
	}
	if (back != NULL) {
		kfree_data(back, UFSVAR_BLOCK);
	}
	if (a != 0) {
		hk_ufsvar_free(a);
	}
	if (b != 0) {
		hk_ufsvar_free(b);
	}
	if (ufsvar_used != before) {
		printf("ufsvar: self-test: %llu blocks were in use, %llu are now\n", before, ufsvar_used);
	}
	if (ufsvar_dirty) {
		(void)hk_ufsvar_save("self-test: the map as it was");
	}
	hk_ufsvar_report("after the second step");
}

/*
 * A ramfs page is PAGE_SIZE bytes, which is several of darwin's 4096-byte blocks (4 on this
 * kernel), so the page store takes a run of that many at a time. This proves a run comes back, is
 * given back and taken again as one, and that the next run starts right after it.
 */
static void
ufsvar_run_selftest(void)
{
	uint64_t blks = PAGE_SIZE / UFSVAR_BLOCK;
	uint64_t before, a, b, again;

	if (blks < 1) {
		printf("ufsvar: run: a page (%u bytes) is smaller than a block: no page store\n",
		    (unsigned)PAGE_SIZE);
		return;
	}
	before = ufsvar_used;
	a = hk_ufsvar_alloc_run(blks);
	b = hk_ufsvar_alloc_run(blks);
	if (a == 0 || b == 0) {
		printf("ufsvar: run: no run of %llu blocks is free: %llu of %llu in use\n", blks, before,
		    ufsvar_blocks);
		return;
	}
	if (b != a + blks) {
		printf("ufsvar: run: two runs of %llu came as %llu and %llu, not neighbours\n", blks, a, b);
		goto give;
	}
	hk_ufsvar_free_run(a, blks);
	again = hk_ufsvar_alloc_run(blks);
	if (again != a) {
		printf("ufsvar: run: the run at %llu given back came as %llu the second time\n", a, again);
		goto give;
	}
	printf("ufsvar: run: a page is %llu blocks; runs at %llu and %llu taken, %llu given back and "
	    "taken again: PASS\n", blks, a, b, a);

give:
	if (a != 0) {
		hk_ufsvar_free_run(a, blks);
	}
	if (b != 0) {
		hk_ufsvar_free_run(b, blks);
	}
	if (ufsvar_dirty) {
		(void)hk_ufsvar_save("run self-test: the map as it was");
	}
}

void
hk_ufsvar_init(void)
{
	uint32_t check = 0;

	if (!PE_parse_boot_argn("ufs_write", &check, sizeof(check)) || check != 1) {
		return;                 /* no writes onto the storage: there is no volume to keep there */
	}
	if (ufsvar_area() != 0) {
		return;
	}
	if (ufsvar_map_make() != 0) {
		return;
	}
	if (ufsvar_load() == 0) {
		printf("ufsvar: the map this run starts from came out of the flash: generation %llu, "
		    "%llu blocks in use, %llu self-tests before this one\n", ufsvar_generation - 1,
		    ufsvar_used, ufsvar_tests);
	} else {
		printf("ufsvar: no map of ours in the metadata (nothing there yet, or not this area's): "
		    "one is made, the whole area free\n");
	}
	ufsvar_keep_store();
	ufsvar_pattern();
	ufsvar_selftest();
	ufsvar_run_selftest();
}
