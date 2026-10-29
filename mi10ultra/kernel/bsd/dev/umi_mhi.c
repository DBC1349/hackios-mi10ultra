/* QCA6390's BHI/BHIE firmware transfer, with UMI's bounded DMA aperture.
 * Register layout was checked against the running UMI Android MHI controller.
 * This is a transport component: it does not turn on rails, initialize PCIe,
 * select an ath11k chip or expose a network interface by itself.
 */
#include <sys/umi_mhi.h>
#include <sys/errno.h>
#include <libkern/libkern.h>

#define SEGMENT_SIZE (512u * 1024u)
#define IMAGE_LIMIT (16u * 1024u * 1024u)
#define DMA_START 0xa0000000ull
#define DMA_END   0xb0000000ull
#define POLLS 20000u
#define MHI_CHANNELS 128u
#define IPCR_TX_CHANNEL 20u
#define IPCR_RX_CHANNEL 21u
#define IPCR_EVENT_RING 1u
#define IPCR_RING_ELEMENTS 32u
#define IPCR_RX_BUFFERS (IPCR_RING_ELEMENTS - 1u)
#define IPCR_BUFFER_SIZE 8192u
#define QRTR_HELLO_SIZE 52u
#define MHI_COMMAND_POLLS 5000u

static void
dma_write_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb oshst" ::: "memory");
#else
	/* Host software model; the kernel target always uses the outer-shareable barrier. */
	__atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

static void
dma_read_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb oshld" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

static bool
span(uint32_t base, uint32_t length, uint32_t limit)
{
	return !(base & 3) && base <= limit && length <= limit - base;
}

static uint32_t
read_reg(const struct umi_mhi *host, uint32_t offset)
{
	return host->io->read32(host->io->context, offset);
}

static void
write_reg(struct umi_mhi *host, uint32_t offset, uint32_t value)
{
	host->io->write32(host->io->context, offset, value);
}

static void
write_address(struct umi_mhi *host, uint32_t offset, uint64_t address)
{
	/* No doorbell is rung until both halves and the size have been written. */
	write_reg(host, offset + 4, (uint32_t)(address >> 32));
	write_reg(host, offset, (uint32_t)address);
}

static uint64_t
words_address(const uint32_t *words, unsigned index)
{
	return words[index] | (uint64_t)words[index + 1] << 32;
}

static void
words_set_address(uint32_t *words, unsigned index, uint64_t address)
{
	words[index] = (uint32_t)address;
	words[index + 1] = (uint32_t)(address >> 32);
}

static uint64_t
ring_next(uint64_t base, size_t length, uint64_t pointer)
{
	pointer += 16;
	return pointer == base + length ? base : pointer;
}

static bool
ring_pointer_valid(uint64_t base, size_t length, uint64_t pointer)
{
	return pointer >= base && pointer < base + length && !((pointer - base) & 15);
}

static void
report(struct umi_mhi *host, const char *stage, uint32_t value)
{
	if (host->io->report) host->io->report(host->io->context, stage, value);
}

int
umi_mhi_probe(struct umi_mhi *host, const struct umi_mhi_io *io)
{
	if (!host || !io || !io->read32 || !io->write32 || !io->allocate ||
	    !io->sync_for_device || !io->delay_us || io->bar_size < 4096) return EINVAL;
	if (host->io) return EBUSY;
	uint32_t version = io->read32(io->context, 8);
	uint32_t length = io->read32(io->context, 0);
	if (length < 0xa8 || length > io->bar_size) return ENODEV;
	uint32_t bhi = io->read32(io->context, 0x28);
	uint32_t bhie = io->read32(io->context, 0x2c);
	uint32_t channels = io->read32(io->context, 0x18);
	uint32_t events = io->read32(io->context, 0x20);
	/* Check every aperture before a caller can issue any firmware transfer. */
	if (bhi < length || bhie < length || channels < length || events < length ||
	    !span(bhi, 0x34, io->bar_size) || !span(bhie, 0x7c, io->bar_size) ||
	    !span(channels, 128 * 8, io->bar_size) || !span(events, 3 * 8, io->bar_size)) return EINVAL;
	if (io->read32(io->context, bhi + 4) != 1) return ENODEV;
	/* Real UMI PBL leaves MHIVER at the reserved-register sentinel. Its
	 * complete BHI layout is readable before the mission-mode version appears. */
	bool cold_pbl = version == 0xdeadbeef && length == 0x100 &&
	    bhi == 0x100 && bhie == 0x224 && channels == 0x300 && events == 0x700 &&
	    io->read32(io->context, bhi + 0x28) == 0;
	if (version != 0x01000000 && !cold_pbl) return ENODEV;
	host->io = io;
	host->bhi = bhi;
	host->bhie = bhie;
	host->channel_db = channels;
	host->event_db = events;
	host->sequence = 1;
	report(host, "MHI version", version);
	return 0;
}

int
umi_mhi_execution_environment(const struct umi_mhi *host, uint32_t *environment)
{
	if (!host || !host->io || !environment) return EINVAL;
	uint32_t ee = read_reg(host, host->bhi + 0x28);
	if (ee > 7) return EIO;
	*environment = ee;
	return 0;
}

static int
allocate(struct umi_mhi *host, size_t size, struct umi_mhi_dma *dma)
{
	int error = host->io->allocate(host->io->context, size, dma);
	if (error) return error;
	if (!dma->bytes || ((uintptr_t)dma->bytes & 7) || dma->size < size || (dma->address & 4095) ||
	    dma->address < DMA_START || dma->address >= DMA_END ||
	    dma->size > DMA_END - dma->address) return EINVAL;
	return 0;
}

static int
wait_transfer(struct umi_mhi *host, uint32_t status_offset, uint32_t sequence, bool check_sequence)
{
	uint32_t status = 0;
	for (unsigned i = 0; i < POLLS; i++) {
		status = read_reg(host, status_offset);
		if (status == UINT32_MAX || (status >> 30) == 3) break;
		if ((status >> 30) == 2 && (!check_sequence || (status & 0x3fffffff) == sequence)) {
			report(host, "firmware transfer complete", status);
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	host->poisoned = true;
	report(host, "firmware transfer failed; DMA retained until endpoint reset", status);
	return status == UINT32_MAX || (status >> 30) == 3 ? EIO : ETIMEDOUT;
}

/* MHI context fields are 4-byte aligned, including the 64-bit ring pointers.
 * Explicit words avoid unaligned 64-bit accesses through an uncached mapping.
 */
static void
context_address(uint32_t *context, unsigned word, uint64_t value)
{
	context[word] = (uint32_t)value;
	context[word + 1] = (uint32_t)(value >> 32);
}

static void
setup_stock_channel_contexts(struct umi_mhi *host)
{
	/* Match Linux mhi_init_ctxt(): publish the static UMI channel metadata
	 * before the controller enters M0 and starts AMSS. Dynamic ring addresses
	 * and ENABLED state are filled when the IPCR client starts after mission.
	 */
	static const struct { unsigned channel, direction; } stock_channels[] = {
		{ 0, 1 }, { 1, 2 }, { 4, 1 }, { 5, 2 },
		{ IPCR_TX_CHANNEL, 1 }, { IPCR_RX_CHANNEL, 2 },
	};
	for (unsigned i = 0; i < sizeof(stock_channels) / sizeof(stock_channels[0]); i++) {
		uint32_t *context = (uint32_t *)host->channels.bytes +
		    stock_channels[i].channel * 11;
		context[0] = 2u << 8; /* disabled, stock burst mode 2, pollcfg 0 */
		context[1] = stock_channels[i].direction;
		context[2] = IPCR_EVENT_RING;
	}
}

int
umi_mhi_prepare_ready(struct umi_mhi *host)
{
	if (!host || !host->io) return EINVAL;
	if (host->poisoned) return EIO;
	if (host->channels.bytes || host->contexts_ready) return EALREADY;
	if (!host->sbl_sent) return EBUSY;
	/* BHI completion can precede both the SBL EE and READY/reset transition. */
	uint32_t status = 0, control = 0, ee = 0;
	bool ready = false;
	int error;
	for (unsigned i = 0; i < POLLS; i++) {
		status = read_reg(host, 0x48);
		control = read_reg(host, 0x38);
		error = umi_mhi_execution_environment(host, &ee);
		if (error || status == UINT32_MAX || control == UINT32_MAX || (status & 4)) {
			host->poisoned = true;
			return EIO;
		}
		if ((ee == 0 || ee == 1) && (status & 1) && !(control & 2) &&
		    ((status >> 8) & 0xff) == 1) {
			ready = true;
			break;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	if (!ready) {
		host->poisoned = true;
		report(host, "READY timeout; SBL DMA retained until endpoint reset", status);
		return ETIMEDOUT;
	}
	uint32_t config = read_reg(host, 0x10);
	if (config == UINT32_MAX) { host->poisoned = true; return EIO; }
	struct umi_mhi_dma *tables[] = { &host->channels, &host->events, &host->commands,
	    &host->command_ring, &host->event_ring[0], &host->event_ring[1], &host->event_ring[2] };
	const size_t sizes[] = { 128 * 44, 3 * 44, 44, 128 * 16, 32 * 16, 256 * 16, 32 * 16 };
	for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
		error = allocate(host, sizes[i], tables[i]);
		if (error) return error;
		memset(tables[i]->bytes, 0, tables[i]->size);
	}
	setup_stock_channel_contexts(host);
	const unsigned elements[] = { 32, 256, 32 }, vectors[] = { 1, 2, 0 };
	for (unsigned i = 0; i < 3; i++) {
		uint32_t *context = (uint32_t *)host->events.bytes + i * 11;
		uint64_t base = host->event_ring[i].address;
		uint64_t length = elements[i] * 16;
		context[0] = i == 2 ? 1u << 16 : 0; /* stock interrupt moderation */
		context[1] = 1;                   /* valid software event ring */
		context[2] = vectors[i];
		context_address(context, 3, base);
		context_address(context, 5, length);
		context_address(context, 7, base);
		context_address(context, 9, base + length - 16);
	}
	uint32_t *command = host->commands.bytes;
	context_address(command, 3, host->command_ring.address);
	context_address(command, 5, 128 * 16);
	context_address(command, 7, host->command_ring.address);
	context_address(command, 9, host->command_ring.address);
	for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++)
		host->io->sync_for_device(host->io->context, tables[i]);
	for (unsigned i = 0; i < 3; i++) {
		host->event_consumer[i] = host->event_ring[i].address;
		host->event_wp[i] = host->event_ring[i].address +
		    sizes[4 + i] - 16;
	}
	host->command_wp = host->command_ring.address;
	write_address(host, 0x58, host->channels.address);
	write_address(host, 0x60, host->events.address);
	write_address(host, 0x68, host->commands.address);
	write_address(host, 0x80, DMA_START);
	write_address(host, 0x88, DMA_END);
	write_address(host, 0x98, DMA_START);
	write_address(host, 0xa0, DMA_END);
	write_reg(host, 0x10, (config & 0xffff) | (3u << 16));
	dma_write_barrier();
	/* Publish free event entries while READY, before requesting M0. */
	for (unsigned e = 0; e < 3; e++)
		write_address(host, host->event_db + e * 8,
		    host->event_ring[e].address + elements[e] * 16 - 16);
	write_reg(host, 0x38, (control & ~0xff00u) | 0x200); /* request M0 */
	for (unsigned i = 0; i < POLLS; i++) {
		status = read_reg(host, 0x48);
		if (status == UINT32_MAX || (status & 4)) break;
		if (((status >> 8) & 0xff) == 2) {
			/* The stock host initializes the device-wake doorbell to zero and
			 * only asserts it to leave M2. We are already in M0, so keep it clear. */
			host->contexts_ready = true;
			report(host, "MHI contexts installed; M0", status);
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	host->poisoned = true;
	report(host, "M0 failed; DMA retained until endpoint reset", status);
	return status == UINT32_MAX || (status & 4) ? EIO : ETIMEDOUT;
}

static int
check_download(struct umi_mhi *host, const void *image, size_t size, uint32_t wanted_ee)
{
	if (!host || !host->io || !image || !size || size > IMAGE_LIMIT) return EINVAL;
	if (host->poisoned) return EIO;
	uint32_t ee;
	int error = umi_mhi_execution_environment(host, &ee);
	if (error) return error;
	if (ee == 2 || ee == 4) return EALREADY;
	return ee == wanted_ee ? 0 : EBUSY;
}

int
umi_mhi_download_sbl(struct umi_mhi *host, const void *image, size_t size)
{
	int error = check_download(host, image, size, 0); /* PBL */
	if (error) return error;
	if (host->sbl.bytes || host->sbl_sent) return EALREADY;
	size_t length = size < SEGMENT_SIZE ? size : SEGMENT_SIZE;
	error = allocate(host, length, &host->sbl);
	if (error) return error;
	memcpy(host->sbl.bytes, image, length);
	host->io->sync_for_device(host->io->context, &host->sbl);
	uint32_t sequence = host->sequence++;
	write_reg(host, host->bhi + 0x20, 0); /* polling, vector zero */
	write_reg(host, host->bhi + 0x2c, 0); /* discard previous completion */
	write_address(host, host->bhi + 8, host->sbl.address);
	write_reg(host, host->bhi + 0x10, (uint32_t)length);
	dma_write_barrier();
	write_reg(host, host->bhi + 0x18, sequence);
	error = wait_transfer(host, host->bhi + 0x2c, sequence, false);
	if (!error) host->sbl_sent = true;
	return error;
}

int
umi_mhi_download_amss(struct umi_mhi *host, const void *image, size_t size)
{
	int error = check_download(host, image, size, 1); /* SBL */
	if (error) return error;
	if (!host->sbl_sent || !host->contexts_ready) return EBUSY;
	/* SBL firmware download needs the host's READY/context/M0 setup first. */
	if (((read_reg(host, 0x48) >> 8) & 0xff) != 2) return EBUSY;
	if (host->image.bytes || host->vectors.bytes || host->amss_sent) return EALREADY;
	/* The full image is segmented, not just the bytes after the initial SBL. */
	uint32_t count = (uint32_t)((size + SEGMENT_SIZE - 1) / SEGMENT_SIZE);
	error = allocate(host, size, &host->image);
	if (error) return error;
	error = allocate(host, count * 16u, &host->vectors);
	if (error) return error;
	memcpy(host->image.bytes, image, size);
	uint64_t *vectors = host->vectors.bytes;
	for (uint32_t i = 0; i < count; i++) {
		size_t offset = (size_t)i * SEGMENT_SIZE;
		size_t length = size - offset;
		if (length > SEGMENT_SIZE) length = SEGMENT_SIZE;
		/* ARM64 UMI is little endian, as are the MHI vector entries. */
		vectors[2 * i] = host->image.address + offset;
		vectors[2 * i + 1] = length;
	}
	host->io->sync_for_device(host->io->context, &host->image);
	host->io->sync_for_device(host->io->context, &host->vectors);
	uint32_t sequence = host->sequence++;
	write_address(host, host->bhie + 0x2c, host->vectors.address);
	write_reg(host, host->bhie + 0x34, count * 16u);
	dma_write_barrier();
	write_reg(host, host->bhie + 0x3c, sequence);
	error = wait_transfer(host, host->bhie + 0x44, sequence, true);
	if (!error) host->amss_sent = true;
	return error;
}

int
umi_mhi_wait_mission(struct umi_mhi *host)
{
	if (!host || !host->io) return EINVAL;
	if (host->poisoned) return EIO;
	if (!host->amss_sent) return EBUSY;
	uint32_t ee = 0;
	for (unsigned i = 0; i < POLLS; i++) {
		int error = umi_mhi_execution_environment(host, &ee);
		if (error || (ee != 1 && ee != 2 && ee != 4)) {
			host->poisoned = true;
			return EIO;
		}
		if (ee == 2 || ee == 4) {
			report(host, "firmware mission environment", ee);
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	host->poisoned = true;
	report(host, "mission environment timeout; DMA retained until endpoint reset", ee);
	return ETIMEDOUT;
}

/* Recycle event entries in the same format as the Linux MHI host. Device RP
 * lives in each event context; host WP is advanced after consuming entries.
 */
static int
service_event_ring(struct umi_mhi *host, unsigned ring, uint64_t command,
	int *command_cc, unsigned *received)
{
	if (ring >= 3 || !host->event_ring[ring].bytes) return EINVAL;
	static const size_t ring_lengths[] = { 32u * 16u, 256u * 16u, 32u * 16u };
	uint32_t *context = (uint32_t *)host->events.bytes + ring * 11;
	uint64_t base = host->event_ring[ring].address;
	size_t length = ring_lengths[ring];
	uint64_t producer = words_address(context, 7);
	if (!ring_pointer_valid(base, length, producer)) return EIO;
	unsigned processed = 0;
	while (host->event_consumer[ring] != producer && processed < length / 16) {
		uint64_t current = host->event_consumer[ring];
		size_t offset = (size_t)(current - base);
		volatile uint32_t *event = (volatile uint32_t *)
		    ((volatile uint8_t *)host->event_ring[ring].bytes + offset);
		dma_read_barrier();
		uint64_t pointer = event[0] | (uint64_t)event[1] << 32;
		uint32_t dword0 = event[2], dword1 = event[3];
		unsigned type = (dword1 >> 16) & 0xff;
		unsigned code = dword0 >> 24;
		if (ring == 0 && type == 0x21 && pointer == command && command_cc)
			*command_cc = (int)code;
		if (type == 0x20 || type == 0x21 || type == 0x40 ||
		    (type != 0x22 && ring == 0)) {
			report(host, "MHI control event type/code", (type << 24) | code);
			report(host, "MHI control event data", dword1);
		}
		if (ring == IPCR_EVENT_RING && type == 0x22 &&
		    ((dword1 >> 24) & 0xff) == IPCR_TX_CHANNEL) {
			if (!host->ipcr_tx_pending || pointer !=
			    host->ipcr_tx_ring.address + (uint64_t)host->ipcr_tx_slot * 16u) {
				report(host, "IPCR TX completion descriptor mismatch", (uint32_t)pointer);
				return EIO;
			}
			report(host, "IPCR TX completion code/bytes", (code << 24) | (dword0 & 0xffff));
			host->ipcr_tx_pending = false;
			host->ipcr_tx_done = code == 2; /* transfer EOT, unlike command CC_SUCCESS=1 */
			if (code != 2) return EIO;
		}
		if (ring == IPCR_EVENT_RING && type == 0x22 &&
		    ((dword1 >> 24) & 0xff) == IPCR_RX_CHANNEL) {
			uint32_t length_done = dword0 & 0xffff;
			if (pointer < host->ipcr_rx_ring.address ||
			    pointer >= host->ipcr_rx_ring.address + host->ipcr_rx_ring.size ||
			    ((pointer - host->ipcr_rx_ring.address) & 15)) {
				report(host, "IPCR RX completion descriptor out of range",
				    (uint32_t)pointer);
				return EIO;
			}
			unsigned tre_slot = (unsigned)((pointer - host->ipcr_rx_ring.address) / 16);
			const volatile uint32_t *completed_tre = (const volatile uint32_t *)
			    ((const volatile uint8_t *)host->ipcr_rx_ring.bytes + tre_slot * 16u);
			dma_read_barrier();
			uint64_t buffer = completed_tre[0] |
			    (uint64_t)completed_tre[1] << 32;
			if (buffer < host->ipcr_rx_buffers.address ||
			    buffer >= host->ipcr_rx_buffers.address + host->ipcr_rx_buffers.size ||
			    ((buffer - host->ipcr_rx_buffers.address) % IPCR_BUFFER_SIZE) ||
			    length_done > IPCR_BUFFER_SIZE) return EIO;
			unsigned slot = (unsigned)((buffer - host->ipcr_rx_buffers.address) /
			    IPCR_BUFFER_SIZE);
			const volatile uint8_t *packet_bytes =
			    (const volatile uint8_t *)host->ipcr_rx_buffers.bytes +
			    (size_t)slot * IPCR_BUFFER_SIZE;
			if (host->ipcr_waiting_response && length_done >= 35 &&
			    packet_bytes[0] == 1 && packet_bytes[4] == 1 &&
			    packet_bytes[32] == 2 &&
			    ((uint16_t)packet_bytes[33] | (uint16_t)packet_bytes[34] << 8) ==
			    host->ipcr_wait_transaction)
				host->ipcr_response_done = true;
			if (host->io->qrtr_packet)
				host->io->qrtr_packet(host->io->context,
				    (const void *)(uintptr_t)packet_bytes, length_done);
			if (received) ++*received;
			report(host, "IPCR RX completion code/bytes", (code << 24) | length_done);
			/* Repost the completed payload buffer at the producer end of the RX ring. */
			uint64_t next = host->ipcr_rx_wp;
			size_t rx_offset = (size_t)(next - host->ipcr_rx_ring.address);
			volatile uint32_t *tre = (volatile uint32_t *)
			    ((volatile uint8_t *)host->ipcr_rx_ring.bytes + rx_offset);
			tre[0] = (uint32_t)buffer;
			tre[1] = (uint32_t)(buffer >> 32);
			tre[2] = IPCR_BUFFER_SIZE;
			tre[3] = (2u << 16) | (1u << 9); /* transfer, interrupt on EOT */
			host->ipcr_rx_wp = ring_next(host->ipcr_rx_ring.address,
			    IPCR_RING_ELEMENTS * 16u, next);
			uint32_t *channel = (uint32_t *)host->channels.bytes + IPCR_RX_CHANNEL * 11;
			words_set_address(channel, 9, host->ipcr_rx_wp);
			host->io->sync_for_device(host->io->context, &host->ipcr_rx_ring);
			write_address(host, host->channel_db + IPCR_RX_CHANNEL * 8,
			    host->ipcr_rx_wp);
		}
		host->event_consumer[ring] = ring_next(base, length, current);
		host->event_wp[ring] = ring_next(base, length, host->event_wp[ring]);
		processed++;
	}
	if (processed == length / 16 && host->event_consumer[ring] != producer)
		return ELOOP;
	if (processed) {
		words_set_address(context, 9, host->event_wp[ring]);
		host->io->sync_for_device(host->io->context, &host->events);
		write_address(host, host->event_db + ring * 8, host->event_wp[ring]);
	}
	return 0;
}

static int
send_nop(struct umi_mhi *host)
{
	uint64_t base = host->command_ring.address;
	size_t length = 128u * 16u;
	uint64_t command = host->command_wp;
	if (!ring_pointer_valid(base, length, command)) return EINVAL;
	volatile uint32_t *tre = (volatile uint32_t *)
	    ((volatile uint8_t *)host->command_ring.bytes + (size_t)(command - base));
	tre[0] = tre[1] = tre[2] = 0;
	tre[3] = 1u << 16; /* MHI_CMD_NOP */
	uint64_t next = ring_next(base, length, command);
	uint32_t *context = host->commands.bytes;
	words_set_address(context, 9, next);
	host->io->sync_for_device(host->io->context, &host->command_ring);
	host->io->sync_for_device(host->io->context, &host->commands);
	dma_write_barrier();
	write_address(host, 0x70, next);
	int completion = -1;
	for (unsigned i = 0; i < MHI_COMMAND_POLLS; i++) {
		for (unsigned ring = 0; ring < 3; ring++) {
			int error = service_event_ring(host, ring, command, &completion, NULL);
			if (error) { host->poisoned = true; return error; }
		}
		if (completion >= 0) {
			host->command_wp = next;
			report(host, "MHI NOP completion code", completion);
			if (completion == 1) return 0;
			host->poisoned = true;
			return EIO;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	report(host, "MHI NOP timed out", read_reg(host, 0x48));
	host->poisoned = true;
	return ETIMEDOUT;
}

static int
start_channel(struct umi_mhi *host, unsigned channel)
{
	uint64_t base = host->command_ring.address;
	size_t length = 128u * 16u;
	uint64_t command = host->command_wp;
	if (!ring_pointer_valid(base, length, command)) return EINVAL;
	volatile uint32_t *tre = (volatile uint32_t *)
	    ((volatile uint8_t *)host->command_ring.bytes + (size_t)(command - base));
	tre[0] = tre[1] = tre[2] = 0;
	tre[3] = (channel << 24) | (18u << 16); /* START_CHAN */
	uint64_t next = ring_next(base, length, command);
	uint32_t *context = host->commands.bytes;
	words_set_address(context, 9, next);
	host->io->sync_for_device(host->io->context, &host->command_ring);
	host->io->sync_for_device(host->io->context, &host->commands);
	dma_write_barrier();
	report(host, "MHI status before channel start", read_reg(host, 0x48));
	report(host, "MHI control before channel start", read_reg(host, 0x38));
	report(host, "MHI EE before channel start", read_reg(host, host->bhi + 0x28));
	report(host, "MHI command ring base low", (uint32_t)base);
	report(host, "MHI command ring new WP low", (uint32_t)next);
	write_address(host, 0x70, next);
	int completion = -1;
	for (unsigned i = 0; i < MHI_COMMAND_POLLS; i++) {
		for (unsigned ring = 0; ring < 3; ring++) {
			int error = service_event_ring(host, ring, command, &completion, NULL);
			if (error) { host->poisoned = true; return error; }
		}
		if (completion >= 0) {
			host->command_wp = next;
			if (completion == 1) {
				report(host, "MHI channel started", channel);
				return 0;
			}
			report(host, "MHI channel start completion code", (channel << 16) | completion);
			host->poisoned = true;
			return EIO;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	host->poisoned = true;
	report(host, "MHI channel start timed out", channel);
	uint32_t *command_context = host->commands.bytes;
	uint32_t *channel_context = (uint32_t *)host->channels.bytes + channel * 11;
	uint32_t *control_event = host->events.bytes;
	report(host, "MHI status after channel timeout", read_reg(host, 0x48));
	report(host, "MHI control after channel timeout", read_reg(host, 0x38));
	report(host, "MHI command RP low", command_context[7]);
	report(host, "MHI command WP low", command_context[9]);
	report(host, "MHI channel state/type/ring", (channel_context[0] & 0xffff) |
	    (channel_context[1] << 16));
	report(host, "MHI channel WP low", channel_context[9]);
	report(host, "MHI control event RP low", control_event[7]);
	uint64_t event_base = host->event_ring[0].address;
	uint64_t event_current = host->event_consumer[0];
	if (event_current >= event_base && event_current < event_base + 32u * 16u) {
		volatile uint32_t *pending = (volatile uint32_t *)
		    ((volatile uint8_t *)host->event_ring[0].bytes +
		    (size_t)(event_current - event_base));
		dma_read_barrier();
		report(host, "MHI pending command event pointer low", pending[0]);
		report(host, "MHI pending command event dword0", pending[2]);
		report(host, "MHI pending command event dword1", pending[3]);
	}
	return ETIMEDOUT;
}

static void
setup_ipcr_channel(struct umi_mhi *host, unsigned channel, unsigned direction,
	const struct umi_mhi_dma *ring, uint64_t write_pointer)
{
	uint32_t *context = (uint32_t *)host->channels.bytes + channel * 11;
	memset(context, 0, 11 * sizeof(*context));
	context[0] = 1u | (2u << 8); /* ENABLED, burst mode disabled */
	context[1] = direction;
	context[2] = IPCR_EVENT_RING;
	words_set_address(context, 3, ring->address);
	words_set_address(context, 5, IPCR_RING_ELEMENTS * 16u);
	words_set_address(context, 7, ring->address);
	words_set_address(context, 9, write_pointer);
}

int
umi_mhi_start_ipcr(struct umi_mhi *host)
{
	if (!host || !host->io) return EINVAL;
	if (host->poisoned) return EIO;
	if (!host->contexts_ready || !host->amss_sent) return EBUSY;
	if (host->ipcr_started || host->ipcr_tx_ring.bytes || host->ipcr_tx_buffers.bytes ||
	    host->ipcr_rx_ring.bytes || host->ipcr_rx_buffers.bytes) return EALREADY;
	uint32_t ee;
	int error = umi_mhi_execution_environment(host, &ee);
	if (error || (ee != 2 && ee != 4)) return error ? error : EBUSY;
	error = allocate(host, IPCR_RING_ELEMENTS * 16u, &host->ipcr_tx_ring);
	if (!error) error = allocate(host, IPCR_RING_ELEMENTS * IPCR_BUFFER_SIZE,
	    &host->ipcr_tx_buffers);
	if (!error) error = allocate(host, IPCR_RING_ELEMENTS * 16u, &host->ipcr_rx_ring);
	if (!error) error = allocate(host, IPCR_RX_BUFFERS * IPCR_BUFFER_SIZE,
	    &host->ipcr_rx_buffers);
	if (error) return error;
	memset(host->ipcr_tx_ring.bytes, 0, host->ipcr_tx_ring.size);
	memset(host->ipcr_tx_buffers.bytes, 0, host->ipcr_tx_buffers.size);
	memset(host->ipcr_rx_ring.bytes, 0, host->ipcr_rx_ring.size);
	memset(host->ipcr_rx_buffers.bytes, 0, host->ipcr_rx_buffers.size);
	setup_ipcr_channel(host, IPCR_TX_CHANNEL, 1, &host->ipcr_tx_ring,
	    host->ipcr_tx_ring.address);
	host->ipcr_tx_wp = host->ipcr_tx_ring.address;
	host->ipcr_rx_wp = host->ipcr_rx_ring.address;
	setup_ipcr_channel(host, IPCR_RX_CHANNEL, 2, &host->ipcr_rx_ring,
	    host->ipcr_rx_wp);
	host->io->sync_for_device(host->io->context, &host->ipcr_tx_ring);
	host->io->sync_for_device(host->io->context, &host->ipcr_tx_buffers);
	host->io->sync_for_device(host->io->context, &host->ipcr_rx_ring);
	host->io->sync_for_device(host->io->context, &host->ipcr_rx_buffers);
	host->io->sync_for_device(host->io->context, &host->channels);
	dma_write_barrier();
	/* Retire M0 and EE notifications before the channel client starts, as the
	 * stock MHI control-event worker does before it creates the IPCR device.
	 */
	for (unsigned ring = 0; ring < 3; ring++) {
		error = service_event_ring(host, ring, UINT64_MAX, NULL, NULL);
		if (error) { host->poisoned = true; return error; }
	}
	error = send_nop(host);
	if (error) return error;
	/* Match the stock driver: start UL first, then DL, and queue DL buffers
	 * after START completion so the device sees an empty channel initially.
	 */
	error = start_channel(host, IPCR_TX_CHANNEL);
	if (!error) error = start_channel(host, IPCR_RX_CHANNEL);
	if (error) { host->poisoned = true; return error; }
	for (unsigned i = 0; i < IPCR_RX_BUFFERS; i++) {
		uint32_t *tre = (uint32_t *)host->ipcr_rx_ring.bytes + i * 4;
		uint64_t buffer = host->ipcr_rx_buffers.address + (uint64_t)i * IPCR_BUFFER_SIZE;
		tre[0] = (uint32_t)buffer;
		tre[1] = (uint32_t)(buffer >> 32);
		tre[2] = IPCR_BUFFER_SIZE;
		tre[3] = (2u << 16) | (1u << 9); /* transfer, interrupt on EOT */
	}
	host->ipcr_rx_wp = host->ipcr_rx_ring.address + IPCR_RX_BUFFERS * 16u;
	uint32_t *rx_context = (uint32_t *)host->channels.bytes + IPCR_RX_CHANNEL * 11;
	words_set_address(rx_context, 9, host->ipcr_rx_wp);
	host->io->sync_for_device(host->io->context, &host->ipcr_rx_ring);
	host->io->sync_for_device(host->io->context, &host->channels);
	dma_write_barrier();
	write_address(host, host->channel_db + IPCR_RX_CHANNEL * 8,
	    host->ipcr_rx_wp);
	host->ipcr_started = true;
	return 0;
}

int
umi_mhi_poll_ipcr(struct umi_mhi *host, unsigned timeout_ms, unsigned *received)
{
	if (!host || !host->io || !received) return EINVAL;
	if (host->poisoned) return EIO;
	if (!host->ipcr_started) return EBUSY;
	if (!timeout_ms || timeout_ms > 10000) return EINVAL;
	*received = 0;
	for (unsigned i = 0; i < timeout_ms; i++) {
		for (unsigned ring = 0; ring < 3; ring++) {
			int error = service_event_ring(host, ring, UINT64_MAX, NULL, received);
			if (error) { host->poisoned = true; return error; }
		}
		host->io->delay_us(host->io->context, 1000);
	}
	if (*received) report(host, "IPCR receive events", *received);
	return 0;
}

int
umi_mhi_send_qrtr_packet(struct umi_mhi *host, const void *packet,
	size_t length, unsigned timeout_ms, uint16_t expected_transaction)
{
	if (!host || !host->io) return EINVAL;
	if (host->poisoned) return EIO;
	if (!host->ipcr_started) return EBUSY;
	if (host->ipcr_tx_pending || !packet || !length ||
	    length > IPCR_BUFFER_SIZE || !timeout_ms || timeout_ms > 15000) return EINVAL;
	uint64_t ring_base = host->ipcr_tx_ring.address;
	uint64_t command = host->ipcr_tx_wp;
	if (!ring_pointer_valid(ring_base, IPCR_RING_ELEMENTS * 16u, command)) return EINVAL;
	unsigned slot = (unsigned)((command - ring_base) / 16u);
	uint64_t buffer_address = host->ipcr_tx_buffers.address +
	    (uint64_t)slot * IPCR_BUFFER_SIZE;
	memcpy((uint8_t *)host->ipcr_tx_buffers.bytes +
	    (size_t)slot * IPCR_BUFFER_SIZE, packet, length);
	volatile uint32_t *tre = (volatile uint32_t *)
	    ((volatile uint8_t *)host->ipcr_tx_ring.bytes + (size_t)(command - ring_base));
	tre[0] = (uint32_t)buffer_address;
	tre[1] = (uint32_t)(buffer_address >> 32);
	tre[2] = (uint32_t)length;
	tre[3] = (2u << 16) | (1u << 9); /* transfer, interrupt on EOT */
	uint64_t next = ring_next(ring_base, IPCR_RING_ELEMENTS * 16u, command);
	uint32_t *context = (uint32_t *)host->channels.bytes + IPCR_TX_CHANNEL * 11;
	words_set_address(context, 9, next);
	host->ipcr_tx_slot = slot;
	host->ipcr_tx_wp = next;
	host->ipcr_tx_pending = true;
	host->ipcr_tx_done = false;
	host->ipcr_waiting_response = expected_transaction != UINT16_MAX;
	host->ipcr_wait_transaction = expected_transaction;
	host->ipcr_response_done = !host->ipcr_waiting_response;
	host->io->sync_for_device(host->io->context, &host->ipcr_tx_buffers);
	host->io->sync_for_device(host->io->context, &host->ipcr_tx_ring);
	host->io->sync_for_device(host->io->context, &host->channels);
	dma_write_barrier();
	write_address(host, host->channel_db + IPCR_TX_CHANNEL * 8u, next);
	unsigned received = 0;
	for (unsigned i = 0; i < timeout_ms; i++) {
		for (unsigned ring = 0; ring < 3; ring++) {
			int error = service_event_ring(host, ring, UINT64_MAX, NULL, &received);
			if (error) { host->poisoned = true; return error; }
		}
		if (host->ipcr_tx_done && host->ipcr_response_done) {
			host->ipcr_waiting_response = false;
			report(host, "QRTR packet transmitted", (uint32_t)buffer_address);
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	host->poisoned = true;
	report(host, "QRTR packet timeout; status", read_reg(host, 0x48));
	return ETIMEDOUT;
}

int
umi_mhi_send_qrtr_hello(struct umi_mhi *host)
{
	uint32_t hello[13] = {
		1u, 2u, 1u, 0xfffffffeu, 0u, 20u, 0xffffffffu, 0xfffffffeu,
		2u, 0u, 0u, 0u, 0u,
	};
	return umi_mhi_send_qrtr_packet(host, hello, QRTR_HELLO_SIZE, 5000,
	    UINT16_MAX);
}
