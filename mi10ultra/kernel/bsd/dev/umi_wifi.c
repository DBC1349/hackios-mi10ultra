/* Xiaomi Mi 10 QCA6390 / PCIe0 bring-up. Addresses are from UMI's DTB.
 * umi_wifi_step=1 is a read-only survey of always-accessible GCC and TLMM.
 * It deliberately does not read the powered-down PCIe or radio aperture.
 * umi_wifi_step=2 explicitly powers and enumerates PCIe0, without enabling DMA.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/umi_pcie.h>
#include <sys/umi_mhi.h>
#include <sys/hk_input.h>
#include <sys/hk_lx.h>
#include <sys/hk_qrtr.h>
#include <sys/hk_qmi.h>
#include <mach/vm_param.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hk_tl(const char *, ...) __printflike(1, 2);
extern void loader_return(void);
extern int umi_rpmh_wifi_power(void);
extern int hk_wifi_umi_smmu_test(void);
bool umi_wifi_init(void);

#define UMI_QRTR_SERVICE_MAX 16u
#define UMI_QRTR_LOOKUP_MAX 4u
#define UMI_QRTR_PORT_FIRST 0x400u
#define UMI_QRTR_HEADER_SIZE 32u
#define UMI_QRTR_PACKET_MAX 8192u
#define UMI_QRTR_NODE_APPS 1u
#define UMI_QRTR_PORT_CTRL 0xfffffffeu
#define UMI_QRTR_NODE_BCAST 0xffffffffu

struct umi_qrtr_service {
	bool valid;
	uint32_t service, version, instance, node, port;
};

struct umi_qrtr_lookup {
	bool valid;
	uint32_t service, version, instance;
};

struct hk_qrtr_port {
	uint32_t number;
	bool active;
	hk_qrtr_receive_t receive;
	hk_qrtr_server_t server;
	void *context;
	struct umi_qrtr_lookup lookups[UMI_QRTR_LOOKUP_MAX];
};

struct umi_qrtr_state {
	struct umi_mhi *mhi;
	uint32_t next_port;
	bool resume_pending;
	uint32_t resume_local_node, resume_local_port, resume_remote_node, resume_remote_port;
	struct hk_qrtr_port port;
	struct umi_qrtr_service services[UMI_QRTR_SERVICE_MAX];
};

static struct umi_qrtr_state umi_qrtr;
static uint8_t umi_qrtr_tx_packet[UMI_QRTR_PACKET_MAX];
static LCK_GRP_DECLARE(umi_wifi_lck_grp, "umi-wifi");
static LCK_MTX_DECLARE(umi_mhi_lock, &umi_wifi_lck_grp);
static bool umi_poll_wanted, umi_poll_finished;
static struct umi_pcie *umi_active_pcie;
static bool umi_normal_session, umi_service_wanted, umi_service_finished = true;
static struct {
	unsigned type;
	unsigned long size;
	unsigned long long address;
	void *memory;
} umi_firmware_memory[] = {
	{ 1, 0x5e0000, 0xa1000000, NULL },
	{ 4, 0xd8000, 0xa1600000, NULL },
	{ 3, 0x411bc, 0xa1880000, NULL },
};

int
lx_os_radio_firmware_memory(unsigned type, unsigned long size, void **memory, unsigned long long *address)
{
	for (unsigned i = 0; i < sizeof(umi_firmware_memory) / sizeof(umi_firmware_memory[0]); i++) {
		if (umi_firmware_memory[i].type != type) continue;
		if (umi_firmware_memory[i].size != size || !umi_firmware_memory[i].memory) return EINVAL;
		*memory = umi_firmware_memory[i].memory;
		*address = umi_firmware_memory[i].address;
		return 0;
	}
	return ENODEV;
}

static uint32_t
umi_qrtr_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
umi_qrtr_put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static bool
umi_qrtr_matches(const struct umi_qrtr_lookup *lookup, const struct umi_qrtr_service *service)
{
	return lookup->valid && lookup->service == service->service &&
	    (!lookup->version || lookup->version == service->version) &&
	    (!lookup->instance || lookup->instance == service->instance);
}

/* RX callbacks only record confirmations: sending here would reenter MHI's
 * event-ring consumer before it has retired the received packet. */
static int
umi_qrtr_flush_resume(void)
{
	for (unsigned i = 0; umi_qrtr.resume_pending && i < 4; i++) {
		uint8_t packet[52] = {0};
		umi_qrtr_put_le32(packet, 1);
		umi_qrtr_put_le32(packet + 4, 7); /* RESUME_TX */
		umi_qrtr_put_le32(packet + 8, umi_qrtr.resume_local_node);
		umi_qrtr_put_le32(packet + 12, umi_qrtr.resume_local_port);
		umi_qrtr_put_le32(packet + 20, 20);
		umi_qrtr_put_le32(packet + 24, umi_qrtr.resume_remote_node);
		umi_qrtr_put_le32(packet + 28, umi_qrtr.resume_remote_port);
		umi_qrtr_put_le32(packet + 32, 7);
		umi_qrtr_put_le32(packet + 36, umi_qrtr.resume_local_node);
		umi_qrtr_put_le32(packet + 40, umi_qrtr.resume_local_port);
		umi_qrtr.resume_pending = false;
		int error = umi_mhi_send_qrtr_packet(umi_qrtr.mhi, packet, sizeof(packet),
		    5000, UINT16_MAX);
		if (error) return error;
		hk_tl("UMI Wi-Fi: QRTR RESUME_TX sent\n");
	}
	return umi_qrtr.resume_pending ? ELOOP : 0;
}

static void
umi_qrtr_notify(const struct umi_qrtr_service *service, bool arrived)
{
	struct hk_qrtr_port *port = &umi_qrtr.port;
	if (!port->active || port->server == NULL) return;
	for (unsigned i = 0; i < UMI_QRTR_LOOKUP_MAX; i++) {
		if (umi_qrtr_matches(&port->lookups[i], service)) {
			port->server(port->context, arrived, service->service, service->version,
			    service->instance, service->node, service->port);
		}
	}
}

static void
umi_qrtr_packet(void *context, const void *packet_data, size_t length)
{
	struct umi_qrtr_state *state = context;
	const uint8_t *packet = packet_data;
	if (state != &umi_qrtr || length < UMI_QRTR_HEADER_SIZE ||
	    umi_qrtr_get_le32(packet) != 1) return;
	uint32_t type = umi_qrtr_get_le32(packet + 4);
	uint32_t src_node = umi_qrtr_get_le32(packet + 8);
	uint32_t src_port = umi_qrtr_get_le32(packet + 12);
	uint32_t size = umi_qrtr_get_le32(packet + 20);
	uint32_t dst_port = umi_qrtr_get_le32(packet + 28);
	if (size > length - UMI_QRTR_HEADER_SIZE) return;
	const uint8_t *payload = packet + UMI_QRTR_HEADER_SIZE;
	if (type == 1 && size >= 7) {
		uint16_t message = (uint16_t)payload[3] | (uint16_t)payload[4] << 8;
		uint16_t qmi_size = (uint16_t)payload[5] | (uint16_t)payload[6] << 8;
		if (qmi_size <= size - 7 && (payload[0] == 2 || payload[0] == 4 ||
		    message == 0x20 || message == 0x34))
			hk_tl("UMI Wi-Fi: QMI RX type=%u message=%#x transaction=%u length=%u\n",
			    payload[0], message, (unsigned)payload[1] | (unsigned)payload[2] << 8,
			    qmi_size);
	}
	if (type == 4 && size >= 20 && umi_qrtr_get_le32(payload) == 4) {
		uint32_t instance = umi_qrtr_get_le32(payload + 8);
		struct umi_qrtr_service service = {
			.valid = true,
			.service = umi_qrtr_get_le32(payload + 4),
			.version = instance & 0xff,
			.instance = instance >> 8,
			.node = umi_qrtr_get_le32(payload + 12),
			.port = umi_qrtr_get_le32(payload + 16),
		};
		unsigned slot = UMI_QRTR_SERVICE_MAX;
		for (unsigned i = 0; i < UMI_QRTR_SERVICE_MAX; i++) {
			if (umi_qrtr.services[i].valid &&
			    umi_qrtr.services[i].service == service.service &&
			    umi_qrtr.services[i].instance == service.instance &&
			    umi_qrtr.services[i].node == service.node &&
			    umi_qrtr.services[i].port == service.port) {
				slot = i;
				break;
			}
			if (slot == UMI_QRTR_SERVICE_MAX && !umi_qrtr.services[i].valid) slot = i;
		}
		if (slot < UMI_QRTR_SERVICE_MAX) {
			umi_qrtr.services[slot] = service;
			umi_qrtr_notify(&service, true);
		}
		return;
	}
	if (type == 5 && size >= 20 && umi_qrtr_get_le32(payload) == 5) {
		uint32_t service_id = umi_qrtr_get_le32(payload + 4);
		uint32_t instance = umi_qrtr_get_le32(payload + 8);
		uint32_t node = umi_qrtr_get_le32(payload + 12);
		uint32_t port_id = umi_qrtr_get_le32(payload + 16);
		for (unsigned i = 0; i < UMI_QRTR_SERVICE_MAX; i++) {
			struct umi_qrtr_service *service = &umi_qrtr.services[i];
			if (service->valid && service->service == service_id &&
			    service->instance == (instance >> 8) && service->node == node &&
			    service->port == port_id) {
				struct umi_qrtr_service gone = *service;
				service->valid = false;
				umi_qrtr_notify(&gone, false);
			}
		}
		return;
	}
	if (type == 3) {
		for (unsigned i = 0; i < UMI_QRTR_SERVICE_MAX; i++) {
			if (umi_qrtr.services[i].valid && umi_qrtr.services[i].node == src_node) {
				struct umi_qrtr_service gone = umi_qrtr.services[i];
				umi_qrtr.services[i].valid = false;
				umi_qrtr_notify(&gone, false);
			}
		}
		return;
	}
	if (type == 1 && dst_port == umi_qrtr.port.number &&
	    umi_qrtr.port.active && umi_qrtr.port.receive != NULL) {
		umi_qrtr.port.receive(umi_qrtr.port.context, src_node, src_port,
		    payload, size);
		if (umi_qrtr_get_le32(packet + 16)) {
			umi_qrtr.resume_local_node = umi_qrtr_get_le32(packet + 24);
			umi_qrtr.resume_local_port = dst_port;
			umi_qrtr.resume_remote_node = src_node;
			umi_qrtr.resume_remote_port = src_port;
			umi_qrtr.resume_pending = true;
		}
	}
}

static struct hk_qrtr_port *
umi_qrtr_open(hk_qrtr_receive_t receive, hk_qrtr_server_t server, void *context)
{
	struct hk_qrtr_port *port = &umi_qrtr.port;
	lck_mtx_lock(&umi_mhi_lock);
	if (port->active || receive == NULL) {
		lck_mtx_unlock(&umi_mhi_lock);
		return NULL;
	}
	memset(port, 0, sizeof(*port));
	port->active = true;
	port->number = umi_qrtr.next_port++;
	port->receive = receive;
	port->server = server;
	port->context = context;
	lck_mtx_unlock(&umi_mhi_lock);
	return port;
}

static int
umi_qrtr_send_locked(struct hk_qrtr_port *port, uint32_t node, uint32_t dst_port,
	const void *data, size_t length)
{
	if (port != &umi_qrtr.port || !port->active || umi_qrtr.mhi == NULL ||
	    data == NULL || length > UMI_QRTR_PACKET_MAX - UMI_QRTR_HEADER_SIZE) return EMSGSIZE;
	int error = umi_qrtr_flush_resume();
	if (error) return error;
	size_t padded = (length + 3u) & ~(size_t)3u;
	size_t frame_length = UMI_QRTR_HEADER_SIZE + padded;
	memset(umi_qrtr_tx_packet, 0, frame_length);
	umi_qrtr_put_le32(umi_qrtr_tx_packet, 1); /* QRTR v1 */
	umi_qrtr_put_le32(umi_qrtr_tx_packet + 4, 1); /* DATA */
	umi_qrtr_put_le32(umi_qrtr_tx_packet + 8, UMI_QRTR_NODE_APPS);
	umi_qrtr_put_le32(umi_qrtr_tx_packet + 12, port->number);
	umi_qrtr_put_le32(umi_qrtr_tx_packet + 20, (uint32_t)length);
	umi_qrtr_put_le32(umi_qrtr_tx_packet + 24, node);
	umi_qrtr_put_le32(umi_qrtr_tx_packet + 28, dst_port);
	memcpy(umi_qrtr_tx_packet + UMI_QRTR_HEADER_SIZE, data, length);
	if (length >= 7 && ((const uint8_t *)data)[0] == 0) {
		const uint8_t *qmi = data;
		uint16_t message = (uint16_t)qmi[3] | (uint16_t)qmi[4] << 8;
		uint16_t qmi_size = (uint16_t)qmi[5] | (uint16_t)qmi[6] << 8;
		if (qmi_size <= length - 7 && (message == 0x20 || message == 0x34)) {
			hk_tl("UMI Wi-Fi: QMI TX message=%#x transaction=%u length=%u\n",
			    message, (unsigned)qmi[1] | (unsigned)qmi[2] << 8, qmi_size);
			for (size_t at = 7; at + 3 <= 7 + qmi_size;) {
				uint8_t tag = qmi[at];
				uint16_t tlv_size = (uint16_t)qmi[at + 1] | (uint16_t)qmi[at + 2] << 8;
				if (tlv_size > 7 + qmi_size - at - 3) break;
				uint32_t value = 0;
				for (size_t byte = 0; byte < tlv_size && byte < 4; byte++)
					value |= (uint32_t)qmi[at + 3 + byte] << (byte * 8);
				hk_tl("UMI Wi-Fi: QMI TX TLV tag=%#x length=%u value=%#x\n",
				    tag, tlv_size, value);
				at += 3 + tlv_size;
			}
		}
	}
	uint16_t transaction = UINT16_MAX;
	if (length >= 3 && ((const uint8_t *)data)[0] == 0)
		transaction = (uint16_t)((const uint8_t *)data)[1] |
		    (uint16_t)((const uint8_t *)data)[2] << 8;
	error = umi_mhi_send_qrtr_packet(umi_qrtr.mhi, umi_qrtr_tx_packet,
	    frame_length, 10000, transaction);
	return error ? error : umi_qrtr_flush_resume();
}

static int
umi_qrtr_send(struct hk_qrtr_port *port, uint32_t node, uint32_t dst_port,
	const void *data, size_t length)
{
	lck_mtx_lock(&umi_mhi_lock);
	int error = umi_qrtr_send_locked(port, node, dst_port, data, length);
	lck_mtx_unlock(&umi_mhi_lock);
	return error;
}

static int
umi_qrtr_lookup_locked(struct hk_qrtr_port *port, uint32_t service, uint32_t version,
	uint32_t instance)
{
	if (port != &umi_qrtr.port || !port->active) return EINVAL;
	unsigned slot;
	for (slot = 0; slot < UMI_QRTR_LOOKUP_MAX; slot++)
		if (!port->lookups[slot].valid) break;
	if (slot == UMI_QRTR_LOOKUP_MAX) return ENOSPC;
	port->lookups[slot] = (struct umi_qrtr_lookup) {
		.valid = true, .service = service, .version = version, .instance = instance,
	};
	for (unsigned i = 0; i < UMI_QRTR_SERVICE_MAX; i++)
		if (port->server != NULL && umi_qrtr.services[i].valid &&
		    umi_qrtr_matches(&port->lookups[slot],
		    &umi_qrtr.services[i]))
			port->server(port->context, true, umi_qrtr.services[i].service,
			    umi_qrtr.services[i].version, umi_qrtr.services[i].instance,
			    umi_qrtr.services[i].node, umi_qrtr.services[i].port);
	return 0;
}

static int
umi_qrtr_lookup(struct hk_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	lck_mtx_lock(&umi_mhi_lock);
	int error = umi_qrtr_lookup_locked(port, service, version, instance);
	lck_mtx_unlock(&umi_mhi_lock);
	return error;
}

static int
umi_qrtr_poll_locked(struct hk_qrtr_port *port, uint32_t timeout_ms)
{
	unsigned received = 0;
	if (port != &umi_qrtr.port || !port->active || umi_qrtr.mhi == NULL)
		return EINVAL;
	int error = umi_mhi_poll_ipcr(umi_qrtr.mhi, timeout_ms, &received);
	return error ? error : umi_qrtr_flush_resume();
}

static int
umi_qrtr_poll(struct hk_qrtr_port *port, uint32_t timeout_ms)
{
	lck_mtx_lock(&umi_mhi_lock);
	int error = umi_qrtr_poll_locked(port, timeout_ms);
	lck_mtx_unlock(&umi_mhi_lock);
	return error;
}

static void
umi_wifi_poll_thread(__unused void *argument)
{
	while (os_atomic_load(&umi_poll_wanted, acquire)) {
		int error = umi_qrtr_poll(&umi_qrtr.port, 1);
		if (error && error != EINVAL && error != EBUSY) {
			hk_tl("UMI Wi-Fi: background MHI polling failed: %d\n", error);
			break;
		}
		struct timespec pause = { .tv_sec = 0, .tv_nsec = NSEC_PER_MSEC };
		msleep(&umi_poll_wanted, NULL, PZERO, "umi-ipcr", &pause);
	}
	os_atomic_store(&umi_poll_finished, true, release);
}

static void
umi_qrtr_close(struct hk_qrtr_port *port)
{
	lck_mtx_lock(&umi_mhi_lock);
	if (port == &umi_qrtr.port) memset(port, 0, sizeof(*port));
	lck_mtx_unlock(&umi_mhi_lock);
}

static const struct hk_qrtr_ops umi_qrtr_ops = {
	.open = umi_qrtr_open, .send = umi_qrtr_send,
	.lookup = umi_qrtr_lookup, .poll = umi_qrtr_poll, .close = umi_qrtr_close,
};

static uint32_t
umi_wifi_read(vm_offset_t base, uint32_t offset)
{
	uint32_t value;
	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (base + offset) : "memory");
	return value;
}

static bool
umi_wifi_is_umi(void)
{
	DTEntry chosen;
	const void *board;
	unsigned size;

	return SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "hackios-board", &board, &size) == kSuccess &&
	    size == 4 && memcmp(board, "umi", 4) == 0;
}

bool
umi_wifi_is_board(void)
{
	return umi_wifi_is_umi();
}

bool
umi_wifi_normal_session(void)
{
	return umi_normal_session;
}

bool
umi_wifi_radio_running(void)
{
	lck_mtx_lock(&umi_mhi_lock);
	bool running = umi_qrtr.mhi && umi_qrtr.mhi->ipcr_started && !umi_qrtr.mhi->poisoned;
	lck_mtx_unlock(&umi_mhi_lock);
	return running;
}

static bool
umi_wifi_survey(void)
{
	static const struct { const char *name; uint32_t offset; } clocks[] = {
		{ "pcie0_reset", 0x6b000 }, { "pcie0_gdsc", 0x6b004 },
		{ "pcie0_clock_votes", 0x52008 }, { "pcie0_slv_q2a", 0x6b010 },
		{ "pcie0_slv_axi", 0x6b014 }, { "pcie0_mstr_axi", 0x6b01c },
		{ "pcie0_cfg_ahb", 0x6b024 }, { "pcie0_aux", 0x6b028 },
		{ "pcie0_pipe", 0x6b02c }, { "pcie0_aux_cmd", 0x6b038 },
		{ "pcie0_aux_cfg", 0x6b03c }, { "pcie0_phy_reset", 0x6c01c },
		{ "pcie0_phy_refgen", 0x6f02c },
	};
	static const struct { const char *name; uint32_t pin; } pins[] = {
		{ "wlan_enable", 20 }, { "bluetooth_enable", 21 },
		{ "pcie0_perst", 79 }, { "pcie0_clkreq", 80 },
		{ "pcie0_wake", 81 }, { "wlan_sw_ctrl", 124 },
	};
	vm_offset_t gcc = ml_io_map(0x00100000, 0x80000);
	vm_offset_t gpio = ml_io_map(0x0f900000, 0x80000);
	if (!gcc || !gpio) {
		hk_tl("UMI Wi-Fi: GCC/TLMM mapping failed\n");
		return false;
	}
	hk_tl("UMI Wi-Fi: QCA6390 on PCIe0; read-only GCC/TLMM survey\n");
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		hk_tl("UMI Wi-Fi: GCC %s (%#x) = %#x\n", clocks[i].name,
		    0x00100000 + clocks[i].offset, umi_wifi_read(gcc, clocks[i].offset));
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		hk_tl("UMI Wi-Fi: GPIO%u %s cfg=%#x io=%#x\n", pins[i].pin, pins[i].name,
		    umi_wifi_read(gpio, pins[i].pin * 0x1000),
		    umi_wifi_read(gpio, pins[i].pin * 0x1000 + 4));
	}
	return true;
}

static struct {
	uint32_t address, size;
	vm_offset_t mapping;
} umi_pcie_regions[] = {
	{ 0x00100000, 0xa0000, 0 }, /* GCC, including the PCIe TBU clock. */
	{ 0x0f900000, 0x80000, 0 }, /* Kona NORTH GPIO tile. */
	{ 0x01c00000, 0x3000, 0 },  /* PARF */
	{ 0x01c06000, 0x1000, 0 },  /* PHY */
	{ 0x60000000, 0x1000, 0 },  /* DBI + ELBI */
	{ 0x60001000, 0x1000, 0 },  /* unrolled outbound iATU */
	{ 0x60100000, 0x1000, 0 },  /* endpoint configuration */
	{ 0x60300000, 0x100000, 0 }, /* QCA6390 BAR, including its register window */
};

static vm_offset_t
umi_pcie_register(uint32_t address)
{
	if (address & 3) return 0;
	for (unsigned i = 0; i < sizeof(umi_pcie_regions) / sizeof(umi_pcie_regions[0]); i++) {
		uint32_t offset = address - umi_pcie_regions[i].address;
		if (offset <= umi_pcie_regions[i].size - 4)
			return umi_pcie_regions[i].mapping + offset;
	}
	hk_tl("UMI PCIe: unmapped register %#x\n", address);
	return 0;
}

static uint32_t
umi_pcie_read(void *context, uint32_t address)
{
	(void)context;
	vm_offset_t reg = umi_pcie_register(address);
	if (!reg) return UINT32_MAX;
	uint32_t value = *(volatile uint32_t *)reg;
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
umi_pcie_write(void *context, uint32_t address, uint32_t value)
{
	(void)context;
	vm_offset_t reg = umi_pcie_register(address);
	if (!reg) return;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)reg = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static int
umi_pcie_power(void *context)
{
	(void)context;
	return umi_rpmh_wifi_power();
}

static void
umi_pcie_delay(void *context, unsigned us)
{
	(void)context;
	delay(us);
}

static void
umi_pcie_report(void *context, const char *stage, uint32_t value)
{
	(void)context;
	hk_tl("UMI PCIe: %s = %#x\n", stage, value);
}

/* PCIe's TBU clocks are on at this point. Inspect ownership before choosing
 * any DMA context; this survey never changes the global SMMU or its streams. */
static void
umi_wifi_smmu_survey(void)
{
	vm_offset_t smmu = ml_io_map(0x15000000, 0x2000);
	if (!smmu) return;
	uint32_t scr0 = umi_wifi_read(smmu, 0);
	uint32_t idr0 = umi_wifi_read(smmu, 0x20);
	uint32_t idr1 = umi_wifi_read(smmu, 0x24);
	hk_tl("UMI Wi-Fi: SMMU survey SCR0=%#x IDR0=%#x IDR1=%#x\n", scr0, idr0, idr1);
	if (scr0 == UINT32_MAX || idr0 == UINT32_MAX || idr1 == UINT32_MAX) return;
	unsigned count = idr0 & 0xff, free = 0, covering = 0;
	for (unsigned i = 0; i < count && i < 256; i++) {
		uint32_t smr = umi_wifi_read(smmu, 0x800 + 4 * i);
		uint32_t s2cr = umi_wifi_read(smmu, 0xc00 + 4 * i);
		if (!(smr & (1u << 31))) {
			++free;
			continue;
		}
		uint32_t stream = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		if (((stream ^ 0x1c01) & ~mask) == 0) {
			++covering;
			hk_tl("UMI Wi-Fi: SMMU existing endpoint owner SMR%u=%#x S2CR=%#x\n", i, smr, s2cr);
		}
	}
	hk_tl("UMI Wi-Fi: SMMU stream=0x1c01 free matches=%u existing owners=%u contexts=%u\n",
	    free, covering, idr1 & 0xff);
}

static uint32_t
umi_mhi_read(void *context, uint32_t offset)
{
	(void)context;
	return offset <= 0xffc && !(offset & 3) ? umi_pcie_read(NULL, 0x60300000 + offset) : UINT32_MAX;
}

static void
umi_mhi_write(void *context, uint32_t offset, uint32_t value)
{
	(void)context;
	if (offset <= 0xffc && !(offset & 3)) umi_pcie_write(NULL, 0x60300000 + offset, value);
}

static int
umi_mhi_allocate(void *context, size_t size, struct umi_mhi_dma *dma)
{
	(void)context;
	unsigned long long address = 0;
	if (size > 16u * 1024u * 1024u) return EINVAL;
	dma->size = round_page(size);
	dma->bytes = lx_os_radio_memory(dma->size, &address);
	dma->address = address;
	return dma->bytes ? 0 : ENOMEM;
}

static void
umi_mhi_sync(void *context, const struct umi_mhi_dma *dma)
{
	(void)context;
	(void)dma;
	/* lx_os_radio_memory returns the uncached alias after cleaning the
	 * original cached allocation. Publish writes before ringing a doorbell. */
	__asm__ volatile("dsb osh" ::: "memory");
	if (dma->size == 512u * 1024u) {
		const volatile uint8_t *bytes = dma->bytes;
		uint32_t hash = 2166136261u;
		for (size_t i = 0; i < dma->size; i++) hash = (hash ^ bytes[i]) * 16777619u;
		hk_tl("UMI Wi-Fi: uncached SBL buffer FNV32=%#x prefix=%02x%02x%02x%02x\n",
		    hash, bytes[0], bytes[1], bytes[2], bytes[3]);
	}
}

struct umi_fw_test { struct umi_pcie *pcie; unsigned step; };

void
umi_wifi_radio_reset(void)
{
	lck_mtx_lock(&umi_mhi_lock);
	if (umi_active_pcie && umi_qrtr.mhi) {
		umi_pcie_quiesce(umi_active_pcie);
		umi_qrtr.mhi->ipcr_started = false;
		for (unsigned i = 0; i < UMI_QRTR_SERVICE_MAX; i++) {
			if (!umi_qrtr.services[i].valid) continue;
			struct umi_qrtr_service service = umi_qrtr.services[i];
			umi_qrtr.services[i].valid = false;
			umi_qrtr_notify(&service, false);
		}
		umi_qrtr.resume_pending = false;
		hk_tl("UMI Wi-Fi: endpoint reset; firmware and CAL allocations retained\n");
	}
	lck_mtx_unlock(&umi_mhi_lock);
}

int
umi_wifi_radio_boot(void)
{
	lck_mtx_lock(&umi_mhi_lock);
	struct umi_mhi *mhi = umi_qrtr.mhi;
	struct umi_pcie *pcie = umi_active_pcie;
	int error = ENXIO;
	if (!mhi || !pcie) goto out;
	if (mhi->ipcr_started && !mhi->poisoned) { error = 0; goto out; }
	const struct umi_mhi_io *mhi_io = mhi->io;
	const struct umi_pcie_io *pcie_io = pcie->io;
	if (!mhi_io || !pcie_io) goto out;
	memset(pcie, 0, sizeof(*pcie));
	error = umi_pcie_enumerate(pcie, pcie_io);
	if (error) goto out;
	memset(mhi, 0, sizeof(*mhi));
	error = umi_mhi_probe(mhi, mhi_io);
	if (!error) error = umi_pcie_prepare_radio(pcie);
	unsigned long long msi_address = 0;
	uint32_t data = 0, spi = 0;
	if (!error && !hk_wifi_radio_msi(&msi_address, &data, &spi)) error = ENOMEM;
	if (!error) error = umi_pcie_setup_mhi_msi(pcie, msi_address, data);
	if (!error) error = umi_pcie_enable_dma(pcie);
	uint64_t size = 0;
	const uint8_t *firmware = hk_wifi_driver_file("amss.bin", &size);
	if (!error && (!firmware || size < 512u * 1024u)) error = ENOENT;
	if (!error) error = umi_mhi_download_sbl(mhi, firmware, size);
	if (!error) error = umi_mhi_prepare_ready(mhi);
	if (!error) error = umi_mhi_download_amss(mhi, firmware, size);
	if (!error) error = umi_mhi_wait_mission(mhi);
	if (!error) error = umi_mhi_start_ipcr(mhi);
	if (!error) error = umi_mhi_send_qrtr_hello(mhi);
	if (error) umi_pcie_quiesce(pcie);
	hk_tl("UMI Wi-Fi: post-calibration firmware restart returned %d\n", error);
out:
	lck_mtx_unlock(&umi_mhi_lock);
	return error;
}

static int
umi_wifi_firmware_diagnostic(void *context)
{
	struct umi_fw_test *test = context;
	struct umi_pcie *pcie = test->pcie;
	umi_active_pcie = pcie;
	memset(&umi_qrtr, 0, sizeof(umi_qrtr));
	umi_qrtr.next_port = UMI_QRTR_PORT_FIRST;
	const struct umi_mhi_io io = {
		.context = &umi_qrtr,
		.bar_size = 4096, .read32 = umi_mhi_read, .write32 = umi_mhi_write,
		.allocate = umi_mhi_allocate, .sync_for_device = umi_mhi_sync,
		.delay_us = umi_pcie_delay, .report = umi_pcie_report,
		.qrtr_packet = umi_qrtr_packet,
	};
	struct umi_mhi mhi = {0};
	umi_qrtr.mhi = &mhi;
	uint64_t size = 0;
	const uint8_t *firmware = hk_wifi_driver_file("amss.bin", &size);
	if (firmware && size >= 512u * 1024u) {
		uint32_t hash = 2166136261u;
		for (size_t i = 0; i < 512u * 1024u; i++) hash = (hash ^ firmware[i]) * 16777619u;
		hk_tl("UMI Wi-Fi: firmware size=%llu SBL source FNV32=%#x prefix=%02x%02x%02x%02x\n",
		    size, hash, firmware[0], firmware[1], firmware[2], firmware[3]);
	}
	int error = !firmware || size < 512u * 1024u ? ENOENT : umi_mhi_probe(&mhi, &io);
	if (!error) error = umi_pcie_prepare_radio(pcie);
	if (!error) {
		unsigned long long msi_address = 0;
		uint32_t data = 0, spi = 0;
		if (!hk_wifi_radio_msi(&msi_address, &data, &spi)) error = ENOMEM;
		else error = umi_pcie_setup_mhi_msi(pcie, msi_address, data);
	}
	if (!error) error = umi_pcie_enable_dma(pcie);
	if (!error) error = umi_mhi_download_sbl(&mhi, firmware, size);
	if (!error && test->step >= 5) error = umi_mhi_prepare_ready(&mhi);
	if (!error && test->step >= 6) error = umi_mhi_download_amss(&mhi, firmware, size);
	if (!error && test->step >= 6) error = umi_mhi_wait_mission(&mhi);
	if (!error && test->step >= 7) error = umi_mhi_start_ipcr(&mhi);
	unsigned received = 0;
	if (!error && test->step >= 7) error = umi_mhi_send_qrtr_hello(&mhi);
	if (!error && test->step >= 7) error = umi_mhi_poll_ipcr(&mhi, 3000, &received);
	if (test->step >= 7)
		hk_tl("UMI Wi-Fi: QRTR/IPCR channel diagnostic errno=%d receive_events=%u\n",
		    error, received);
	if (!error && test->step >= 7) {
		hk_qmi_set_transport(&umi_qrtr_ops);
		if (test->step >= 10) {
			for (unsigned i = 0; i < sizeof(umi_firmware_memory) / sizeof(umi_firmware_memory[0]); i++) {
				umi_firmware_memory[i].memory = lx_os_radio_memory_at(umi_firmware_memory[i].size,
				    umi_firmware_memory[i].address);
				if (!umi_firmware_memory[i].memory) { error = ENOMEM; break; }
			}
			os_atomic_store(&umi_poll_finished, false, relaxed);
			os_atomic_store(&umi_poll_wanted, true, release);
			if (!error) error = lx_os_thread("umi-ipcr", umi_wifi_poll_thread, NULL);
			if (!error) {
				error = hk_lx_start();
				if (!error && umi_normal_session) {
					hk_tl("UMI Wi-Fi: station ready for iOS Wi-Fi requests\n");
					while (os_atomic_load(&umi_service_wanted, acquire) &&
					    !os_atomic_load(&umi_poll_finished, acquire)) {
						struct timespec pause = { .tv_sec = 0, .tv_nsec = 100 * NSEC_PER_MSEC };
						msleep(&umi_service_wanted, NULL, PZERO, "umi-wifi", &pause);
					}
					hk_lx_stop();
				}
				os_atomic_store(&umi_poll_wanted, false, release);
				while (!os_atomic_load(&umi_poll_finished, acquire)) {
					struct timespec pause = { .tv_sec = 0, .tv_nsec = NSEC_PER_MSEC };
					msleep(&umi_poll_finished, NULL, PZERO, "umi-ipcr-end", &pause);
				}
			}
			hk_tl("UMI Wi-Fi: ath11k diagnostic returned %d\n", error);
		} else {
			bool queried = hk_wlfw_query(test->step);
			hk_tl("UMI Wi-Fi: QCA6390 WLFW step %u %s\n", test->step,
			    queried ? "completed" : "failed");
			if (!queried) error = EIO;
		}
		hk_qmi_set_transport(NULL);
	}
	hk_qmi_set_transport(NULL);
	umi_qrtr.mhi = NULL;
	uint32_t ee = UINT32_MAX;
	if (mhi.io && pcie->enumerated) (void)umi_mhi_execution_environment(&mhi, &ee);
	if (error && mhi.io && pcie->enumerated) {
		hk_tl("UMI Wi-Fi: BHI error=%#x debug=%#x/%#x/%#x\n",
		    umi_mhi_read(NULL, mhi.bhi + 0x30), umi_mhi_read(NULL, mhi.bhi + 0x34),
		    umi_mhi_read(NULL, mhi.bhi + 0x38), umi_mhi_read(NULL, mhi.bhi + 0x3c));
	}
	hk_tl("UMI Wi-Fi: firmware diagnostic step=%u errno=%d EE=%#x (buffers retained until reset)\n",
	    test->step, error, ee);
	/* The SMMU owner requires reset propagation before restoring translation. */
	umi_pcie_quiesce(pcie);
	umi_active_pcie = NULL;
	return error;
}

static int umi_wifi_pcie_diagnostic(unsigned step);

bool
umi_wifi_service_done(void)
{
	return os_atomic_load(&umi_service_finished, acquire);
}

static void
umi_wifi_service_thread(__unused void *argument)
{
	int error = umi_wifi_pcie_diagnostic(10);
	hk_tl("UMI Wi-Fi: service stopped, errno=%d\n", error);
	os_atomic_store(&umi_service_finished, true, release);
}

void
umi_wifi_stop(void)
{
	os_atomic_store(&umi_service_wanted, false, release);
	wakeup(&umi_service_wanted);
	for (unsigned i = 0; i < 400 && !os_atomic_load(&umi_service_finished, acquire); i++) {
		struct timespec pause = { .tv_sec = 0, .tv_nsec = 100 * NSEC_PER_MSEC };
		msleep(&umi_service_finished, NULL, PZERO, "umi-wifi-end", &pause);
	}
	if (!os_atomic_load(&umi_service_finished, acquire) && umi_active_pcie)
		umi_pcie_quiesce(umi_active_pcie); /* SMMU restoration follows after other cores stop. */
}

/* The panic path cannot take a mutex held by a stopped kernel thread. */
void
umi_wifi_quiesce(void)
{
	os_atomic_store(&umi_service_wanted, false, release);
	if (umi_active_pcie) umi_pcie_quiesce(umi_active_pcie);
	hk_wifi_umi_smmu_quiesce();
}

static int
umi_wifi_pcie_diagnostic(unsigned step)
{
	for (unsigned i = 0; i < sizeof(umi_pcie_regions) / sizeof(umi_pcie_regions[0]); i++) {
		umi_pcie_regions[i].mapping = ml_io_map(umi_pcie_regions[i].address, umi_pcie_regions[i].size);
		if (!umi_pcie_regions[i].mapping) return ENOMEM;
	}
	const struct umi_pcie_io io = {
		.read32 = umi_pcie_read, .write32 = umi_pcie_write,
		.power_on = umi_pcie_power, .delay_us = umi_pcie_delay, .report = umi_pcie_report,
	};
	struct umi_pcie host = {0};
	hk_tl("UMI Wi-Fi: PCIe enumeration diagnostic; DMA stays disabled\n");
	int error = umi_pcie_enumerate(&host, &io);
	if (!error) {
		umi_pcie_report(NULL, "MHICFG", umi_pcie_read(NULL, 0x60300010));
		umi_pcie_report(NULL, "BHI offset", umi_pcie_read(NULL, 0x60300028));
		umi_pcie_report(NULL, "BHIE offset", umi_pcie_read(NULL, 0x6030002c));
		umi_wifi_smmu_survey();
		if (step == 3) error = hk_wifi_umi_smmu_test();
		if (step >= 4) {
			struct umi_fw_test test = { .pcie = &host, .step = step };
			error = hk_wifi_umi_smmu_run(umi_wifi_firmware_diagnostic, &test);
		}
	}
	umi_pcie_quiesce(&host);
	return error;
}

/* Return true for UMI even when disabled: never enter HK's WPSS path. */
bool
umi_wifi_init(void)
{
	uint32_t step = 0, back = 0, enabled = 0;
	if (!umi_wifi_is_umi()) {
		return false;
	}
	PE_parse_boot_argn("umi_wifi_step", &step, sizeof(step));
	PE_parse_boot_argn("umi_wifi_return", &back, sizeof(back));
	PE_parse_boot_argn("wifi", &enabled, sizeof(enabled));
	if (!step && enabled == 1) {
		umi_normal_session = true;
		os_atomic_store(&umi_service_finished, false, relaxed);
		os_atomic_store(&umi_service_wanted, true, release);
		hk_lx_net_early();
		int error = lx_os_thread("umi-wifi", umi_wifi_service_thread, NULL);
		if (error) {
			os_atomic_store(&umi_service_finished, true, release);
			os_atomic_store(&umi_service_wanted, false, release);
		}
		hk_tl("UMI Wi-Fi: background startup requested, errno=%d\n", error);
		return true;
	}
	if (step == 1 && back == 1) {
		hk_tl("UMI Wi-Fi: survey %s; returning to loader\n", umi_wifi_survey() ? "passed" : "failed");
		loader_return();
	} else if (step >= 2 && step <= 10 && back == 1) {
		int error = umi_wifi_pcie_diagnostic(step);
		hk_tl("UMI Wi-Fi: PCIe diagnostic errno=%d; endpoint reset; returning to loader\n", error);
		loader_return();
	} else if (step || enabled) {
		hk_tl("UMI Wi-Fi: QCA6390 requires the UMI PCIe/MHI driver; diagnostic: umi_wifi_step=1 umi_wifi_return=1\n");
	}
	return true;
}
