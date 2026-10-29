/*
 * A STAND-IN, TO BE THROWN AWAY when ath11k's own qmi.c runs here.
 *
 * Before a WCN6750's driver touches the radio it has an exchange with the Wi-Fi firmware's
 * service ("WLFW", on the DSP that hk_wifi.c starts): it says which indications it wants
 * and what the host can do, asks what the firmware and the chip are, sends the regulatory
 * database and the board's data file in pieces, and is told "init done" and "ready". This file
 * makes that exchange by hand, in the order and with the values of ath11k's qmi.c for this chip
 * (ath11k_qmi_event_server_arrive() and what it calls), to learn from the phone, before the
 * work of running the real qmi.c is spent: whether this firmware build takes ath11k's messages,
 * whether it takes the phone's own board file, and what calibration does.
 *
 * It is step 15 of wifi_step=N (hk_wifi.c), and stops where qmi.c hands over to the rest of the
 * driver. Boot-arg wifi_cal=1 adds what qmi.c does with cold-boot calibration switched on.
 * Boot-arg wifi_mission=1 adds what qmi.c is asked for next (ath11k_qmi_firmware_start()): the
 * firmware's logging, the configuration, the mode "mission", after which the firmware has the
 * radio on and the driver begins to use its registers. The configuration is NOT ALL OF
 * ath11k's: the copy engines' pipes and the map of services to them are its own tables for this
 * chip, taken out with the message tables, but the list of shadow registers, which the driver
 * works out from its rings when it runs, is left out. With that mode the handle stays open
 * until hk_wlfw_stop(), which sends the mode "off" as ath11k does when it stops.
 *
 * What the phone said (13 October, firmware WLAN.MSL.1.0.1-01277): every request is answered
 * with success; the regulatory database goes down in 4 pieces and bd_m20gl.elf in 10; "init
 * done" comes 283 ms after the first request. The firmware gives no board id (ath11k then takes
 * 0xff), chip id 1 of family 0xb, and the radio's registers at 0x61e00000, 2 MB. Left alone
 * after that, it does not say "ready" (10 s waited): that waits for a mode. Asked for cold-boot
 * calibration, it says "calibration done" 6.7 s later and "ready" 0.1 s after that -- with the
 * radio's pins and supply rails as the boot loader leaves them. The mode "off" sent after that
 * (which qmi.c does not send at that point) it refuses, error 48. With wifi_mission=1: logging,
 * the configuration without a list of shadow registers, and the mode "mission" are all taken
 * (3, 3 and 342 ms), the radio's registers then answer (step 16), and the mode "off" is taken.
 *
 * The messages' structures and the tables that describe them are ath11k's own, taken out of its
 * sources when the kernel is built (tools/mk-wlfw.sh) and not kept anywhere: nothing of them is
 * in this file. Without them this compiles to a line that says so. The coding itself and the
 * transactions are ours (hk_qmi.c).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hk_lx.h>
#include <sys/hk_input.h>
#include <sys/hk_qmi.h>
#include <sys/systm.h>

#define say hk_wifi_say

#if !__has_include("hk_wlfw_ath11k.h")

bool
hk_wlfw_registers(__unused uint64_t *address, __unused uint32_t *size, __unused bool *mission)
{
	return false;
}

void
hk_wlfw_stop(__unused uint32_t step)
{
}

bool
hk_wlfw_exchange(uint32_t step)
{
	say("wifi: %u: THE EXCHANGE IS NOT BUILT IN: ath11k's message descriptions were not there when this kernel was built "
	    "(tools/mk-wlfw.sh, then build again)\n", step);
	return false;
}

bool
hk_wlfw_query(__unused uint32_t step)
{
	return false;
}

#else

#define HK_QMI_NO_KERNEL_CONNECT
#include "hk_wlfw_ath11k.h"

#define MODE_COLD_BOOT          7               /* core.h: ATH11K_FIRMWARE_MODE_COLD_BOOT */
#define ATH11K_FIRMWARE_MODE_NORMAL 0           /* core.h: the mode "mission" */
#define UMI_QCA_DDR_IOVA          0xa1000000ull /* stock CNSS starts the main firmware region here */
#define UMI_QCA_CAL_IOVA          0xa1600000ull /* stock CNSS's separate cold-boot calibration region */
#define UMI_QCA_M3_IOVA           0xa1880000ull /* stock CNSS places M3 after the calibration area */

static struct qmi_handle handle;
static bool handle_open, in_mission;
static uint64_t registers_at;           /* where the firmware said the radio's registers are, and ... */
static uint32_t registers_size;         /* ... how many bytes of them; 0: it has not said */
static struct {
	volatile bool server, gone, mem_request, mem_ready, init_done, ready, cal_done;
	uint64_t began, mem_ready_at, init_done_at, ready_at, cal_done_at;
	uint32_t mem_seg_count;
	struct { uint32_t size, type, cfg_count; uint64_t address; void *memory; } mem_seg[ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01];
	uint32_t step;
} wlfw;

static uint64_t
now_ms(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000000;
}

/* As qmi.c's ath11k_qmi_ops_new_server(): what follows goes to the server that has appeared. */
static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = hk_qmi_connect(qmi->sock, service->node, service->port);

	say("wifi: %u: the service is at node %u port %u (service %#x, version %u, instance %u)%s\n", wlfw.step, service->node,
	    service->port, service->service, service->version, service->instance, error ? ": CANNOT BE CONNECTED TO" : "");
	wlfw.server = error == 0;
	return error;
}

static void
wlfw_del_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	say("wifi: %u: THE SERVICE AT NODE %u PORT %u HAS GONE\n", wlfw.step, service->node, service->port);
	wlfw.gone = true;
}

static void
wlfw_indication(volatile bool *flag, uint64_t *when, const char *name, unsigned id)
{
	*when = now_ms();
	*flag = true;
	say("wifi: %u: +%llu ms: THE FIRMWARE INDICATES \"%s\" (%#x)\n", wlfw.step, *when - wlfw.began, name, id);
}

static void
wlfw_init_done(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	wlfw_indication(&wlfw.init_done, &wlfw.init_done_at, "firmware init done", QMI_WLFW_FW_INIT_DONE_IND_V01);
}

static void
wlfw_mem_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
	__unused const void *decoded)
{
	wlfw_indication(&wlfw.mem_ready, &wlfw.mem_ready_at, "firmware memory ready", QMI_WLFW_FW_MEM_READY_IND_V01);
}

static void
wlfw_mem_request(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
	const void *decoded)
{
	const struct qmi_wlanfw_request_mem_ind_msg_v01 *request = decoded;
	wlfw.mem_seg_count = request->mem_seg_len;
	if (wlfw.mem_seg_count > ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01)
		wlfw.mem_seg_count = ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01;
	wlfw.mem_request = true;
	say("wifi: %u: QCA6390 requests %u firmware memory segments; response is deferred until this callback returns\n",
	    wlfw.step, request->mem_seg_len);
	for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
		const struct qmi_wlanfw_mem_seg_s_v01 *segment = &request->mem_seg[i];
		wlfw.mem_seg[i].size = segment->size;
		wlfw.mem_seg[i].type = segment->type;
		wlfw.mem_seg[i].cfg_count = segment->mem_cfg_len;
		say("wifi: %u:   segment %u type %u size %#x configs %u\n", wlfw.step,
		    i, segment->type, segment->size, segment->mem_cfg_len);
	}
}

static void
wlfw_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	wlfw_indication(&wlfw.ready, &wlfw.ready_at, "firmware ready", QMI_WLFW_FW_READY_IND_V01);
}

static void
wlfw_cal_done(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	wlfw_indication(&wlfw.cal_done, &wlfw.cal_done_at, "cold boot calibration done", QMI_WLFW_COLD_BOOT_CAL_DONE_IND_V01);
}

static bool ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
	const struct qmi_elem_info *reply_ei, void *reply, bool quiet);

static const struct qmi_ops wlfw_ops = { .new_server = wlfw_new_server, .del_server = wlfw_del_server };
/* The UMI probe also records the request-memory and memory-ready indications. */
static const struct qmi_msg_handler wlfw_handlers[] = {
	{ QMI_INDICATION, QMI_WLANFW_REQUEST_MEM_IND_V01, qmi_wlanfw_request_mem_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_request_mem_ind_msg_v01), wlfw_mem_request },
	{ QMI_INDICATION, QMI_WLFW_FW_MEM_READY_IND_V01, qmi_wlanfw_mem_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_mem_ready_ind_msg_v01), wlfw_mem_ready },
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, qmi_wlanfw_fw_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_ready_ind_msg_v01), wlfw_ready },
	{ QMI_INDICATION, QMI_WLFW_COLD_BOOT_CAL_DONE_IND_V01, qmi_wlanfw_cold_boot_cal_done_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_cold_cal_done_ind_msg_v01), wlfw_cal_done },
	{ QMI_INDICATION, QMI_WLFW_FW_INIT_DONE_IND_V01, qmi_wlfw_fw_init_done_ind_msg_v01_ei,
	  sizeof(struct qmi_wlfw_fw_init_done_ind_msg_v01), wlfw_init_done },
	{ 0 },
};

/*
 * One request and its reply, the way each of qmi.c's goes; `result` is the reply's first field.
 * Says what was asked, the firmware's result and error, and how long it took; a request that is
 * refused is written out in full. True: the firmware said "success".
 */
static bool
ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply, bool quiet)
{
	const struct qmi_response_type_v01 *result = reply;
	static uint8_t bytes[QMI_HEADER_SIZE + 6400];
	struct qmi_txn txn;
	uint64_t began = now_ms();
	int ret;

	ret = qmi_txn_init(&handle, &txn, reply_ei, reply);
	if (ret >= 0) {
		ret = qmi_send_request(&handle, NULL, &txn, message, largest, request_ei, request);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = hk_qmi_txn_wait_ms(&txn, ATH11K_QMI_WLANFW_TIMEOUT_MS);
		}
	}
	if (ret >= 0 && result->result == QMI_RESULT_SUCCESS_V01) {
		if (!quiet) {
			say("wifi: %u: %s (message %#x): success, after %llu ms\n", wlfw.step, what, message, now_ms() - began);
		}
		return true;
	}
	if (ret < 0) {
		say("wifi: %u: %s (message %#x): NO ANSWER THAT CAN BE READ: %d, after %llu ms\n", wlfw.step, what, message, ret,
		    now_ms() - began);
	} else {
		say("wifi: %u: %s (message %#x): THE FIRMWARE REFUSES: result %u, error %u (%#x), after %llu ms\n", wlfw.step, what,
		    message, result->result, result->error, result->error, now_ms() - began);
	}
	ret = hk_qmi_encode(QMI_REQUEST, (uint16_t)message, 0, request_ei, request, bytes, sizeof(bytes));
	for (int at = 0; at < ret && at < 96; at += 24) {
		char text[3 * 24 + 1] = "";

		for (int i = 0; i < 24 && at + i < ret; i++) {
			snprintf(text + 3 * i, 4, " %02x", bytes[at + i]);
		}
		say("wifi: %u:   the request, %d bytes, from %d:%s\n", wlfw.step, ret, at, text);
	}
	return false;
}

/*
 * A file to the firmware in pieces, as qmi.c's ath11k_qmi_load_file_target_mem() sends one
 * (down to "what is left" as the total, and one structure reused for every piece).
 */
static bool
send_file(const char *what, const char *name, uint8_t type, uint32_t board_id)
{
	static struct qmi_wlanfw_bdf_download_req_msg_v01 request;
	struct qmi_wlanfw_bdf_download_resp_msg_v01 reply;
	uint64_t size = 0, began = now_ms();
	const uint8_t *data = hk_wifi_file(name, &size);
	uint32_t left, pieces = 0;

	if (data == NULL || size == 0 || size > 256 * 1024) {
		say("wifi: %u: %s: fw/%s is not in the small image (or is no such file: %llu bytes): not sent\n", wlfw.step, what, name,
		    size);
		return false;
	}
	say("wifi: %u: %s: fw/%s, %llu bytes, beginning %02x %02x %02x %02x; as type %u for board id %#x\n", wlfw.step, what, name,
	    size, data[0], data[1], data[2], data[3], type, board_id);
	bzero(&request, sizeof(request));
	for (left = (uint32_t)size; left > 0; pieces++) {
		request.valid = 1;
		request.file_id_valid = 1;
		request.file_id = board_id;
		request.total_size_valid = 1;
		request.total_size = left;
		request.seg_id_valid = 1;
		request.data_valid = 1;
		request.bdf_type = type;
		request.bdf_type_valid = 1;
		request.end_valid = 1;
		request.end = left <= QMI_WLANFW_MAX_DATA_SIZE_V01;
		request.data_len = request.end ? left : QMI_WLANFW_MAX_DATA_SIZE_V01;
		memcpy(request.data, data, request.data_len);
		bzero(&reply, sizeof(reply));
		if (!ask(what, QMI_WLANFW_BDF_DOWNLOAD_REQ_V01, QMI_WLANFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_LEN,
		    qmi_wlanfw_bdf_download_req_msg_v01_ei, &request, qmi_wlanfw_bdf_download_resp_msg_v01_ei, &reply, true)) {
			say("wifi: %u: %s: REFUSED AT PIECE %u (%u bytes of it, %u still to go)\n", wlfw.step, what, pieces,
			    request.data_len, left);
			return false;
		}
		left -= request.data_len;
		data += request.data_len;
		request.seg_id++;
	}
	say("wifi: %u: %s: ACCEPTED, %u pieces in %llu ms\n", wlfw.step, what, pieces, now_ms() - began);
	return true;
}

static bool
wait_for(volatile bool *flag, uint32_t ms)
{
	for (uint32_t i = 0; i < ms && !*flag && !wlfw.gone; i++) {
		int error = hk_qmi_poll(&handle, 1);
		if (error) {
			say("wifi: %u: QRTR poll failed while waiting for an indication: %d\n", wlfw.step, error);
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return *flag;
}

static bool
wait_for_memory_event(uint32_t ms)
{
	for (uint32_t i = 0; i < ms && !wlfw.mem_request && !wlfw.mem_ready && !wlfw.gone; i++) {
		int error = hk_qmi_poll(&handle, 1);
		if (error) {
			say("wifi: %u: QRTR poll failed while waiting for QCA6390 memory request: %d\n", wlfw.step, error);
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return wlfw.mem_request || wlfw.mem_ready;
}

/* Stock CNSS uses fixed IOVAs for the two QCA6390 segments. */
static bool
respond_memory_request(void)
{
	struct qmi_wlanfw_respond_mem_req_msg_v01 request;
	struct qmi_wlanfw_respond_mem_resp_msg_v01 reply;
	bool have_ddr = false, have_cal = false;

	if (!wlfw.mem_request || wlfw.mem_seg_count != 2 ||
	    wlfw.mem_seg_count > ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01) return false;
	bzero(&request, sizeof(request));
	bzero(&reply, sizeof(reply));
	request.mem_seg_len = wlfw.mem_seg_count;
	for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
		if (wlfw.mem_seg[i].size == 0) return false;
		if (wlfw.mem_seg[i].type == QMI_WLANFW_MEM_TYPE_DDR_V01) {
			if (have_ddr || wlfw.mem_seg[i].size > UMI_QCA_CAL_IOVA - UMI_QCA_DDR_IOVA) return false;
			have_ddr = true;
			wlfw.mem_seg[i].address = UMI_QCA_DDR_IOVA;
		} else if (wlfw.mem_seg[i].type == QMI_WLANFW_MEM_CAL_V01) {
			if (have_cal || wlfw.mem_seg[i].size > UMI_QCA_M3_IOVA - UMI_QCA_CAL_IOVA) return false;
			have_cal = true;
			wlfw.mem_seg[i].address = UMI_QCA_CAL_IOVA;
		} else {
			say("wifi: %u: refusing unsupported QCA6390 memory type %u\n",
			    wlfw.step, wlfw.mem_seg[i].type);
			return false;
		}
	}
	if (!have_ddr || !have_cal) return false;
	/* Map in ascending IOVA order, independent of the order in the QMI request. */
	for (unsigned type = 0; type < 2; type++) {
		for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
			bool is_ddr = wlfw.mem_seg[i].type == QMI_WLANFW_MEM_TYPE_DDR_V01;
			if (is_ddr != (type == 0)) continue;
			wlfw.mem_seg[i].memory = lx_os_radio_memory_at(wlfw.mem_seg[i].size,
			    wlfw.mem_seg[i].address);
			if (wlfw.mem_seg[i].memory == NULL) {
				say("wifi: %u: could not allocate %u bytes for QCA6390 memory segment %u\n",
				    wlfw.step, wlfw.mem_seg[i].size, i);
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
		request.mem_seg[i].addr = wlfw.mem_seg[i].address;
		request.mem_seg[i].size = wlfw.mem_seg[i].size;
		request.mem_seg[i].type = wlfw.mem_seg[i].type;
		say("wifi: %u: mapped QCA6390 memory segment %u type %u size %#x at %#llx\n",
		    wlfw.step, i, wlfw.mem_seg[i].type, wlfw.mem_seg[i].size,
		    (unsigned long long)wlfw.mem_seg[i].address);
	}
	return ask("UMI WLFW memory response", QMI_WLANFW_RESPOND_MEM_REQ_V01,
	    QMI_WLANFW_RESPOND_MEM_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_respond_mem_req_msg_v01_ei, &request,
	    qmi_wlanfw_respond_mem_resp_msg_v01_ei, &reply, false);
}

bool
hk_wlfw_registers(uint64_t *address, uint32_t *size, bool *mission)
{
	*address = registers_at;
	*size = registers_size;
	*mission = in_mission;
	return registers_size != 0;
}

/* ath11k_qmi_firmware_stop(): the mode "off"; and our handle given up. Before the DSP is stopped, while the link is up. */
void
hk_wlfw_stop(uint32_t step)
{
	struct qmi_wlanfw_wlan_mode_req_msg_v01 mode;
	struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_reply;

	if (!handle_open) {
		return;
	}
	wlfw.step = step;
	if (in_mission) {
		bzero(&mode, sizeof(mode));
		bzero(&mode_reply, sizeof(mode_reply));
		mode.mode = ATH11K_FIRMWARE_MODE_OFF;
		mode.hw_debug_valid = 1;
		ask("the mode: off", QMI_WLANFW_WLAN_MODE_REQ_V01, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
		    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode, qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply, false);
		in_mission = false;
	}
	qmi_handle_release(&handle);
	handle_open = false;
}

/*
 * ath11k_qmi_firmware_start(), mode "mission": the firmware's logging on (this chip's entry
 * says so), the configuration, the mode. True: the firmware took the mode.
 */
static bool
mission(void)
{
	static struct qmi_wlanfw_wlan_cfg_req_msg_v01 cfg;
	struct qmi_wlanfw_wlan_cfg_resp_msg_v01 cfg_reply;
	struct qmi_wlanfw_wlan_ini_req_msg_v01 ini;
	struct qmi_wlanfw_wlan_ini_resp_msg_v01 ini_reply;
	struct qmi_wlanfw_wlan_mode_req_msg_v01 mode;
	struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_reply;

	if (WLFW_SENDS_INI) {
		bzero(&ini, sizeof(ini));
		bzero(&ini_reply, sizeof(ini_reply));
		ini.enablefwlog_valid = 1;
		ini.enablefwlog = 1;
		if (!ask("the firmware's logging: on", QMI_WLANFW_WLAN_INI_REQ_V01, QMI_WLANFW_WLAN_INI_REQ_MSG_V01_MAX_LEN,
		    qmi_wlanfw_wlan_ini_req_msg_v01_ei, &ini, qmi_wlanfw_wlan_ini_resp_msg_v01_ei, &ini_reply, false)) {
			return false;
		}
	}
	/* ath11k_qmi_wlanfw_wlan_cfg_send(), but for the shadow registers (see the top) */
	bzero(&cfg, sizeof(cfg));
	bzero(&cfg_reply, sizeof(cfg_reply));
	cfg.host_version_valid = 1;
	strlcpy(cfg.host_version, ATH11K_HOST_VERSION_STRING, sizeof(cfg.host_version));
	cfg.tgt_cfg_valid = 1;
	cfg.tgt_cfg_len = WLFW_PIPE_COUNT;
	for (uint32_t i = 0; i < cfg.tgt_cfg_len; i++) {
		cfg.tgt_cfg[i].pipe_num = WLFW_PIPES[i].pipenum;
		cfg.tgt_cfg[i].pipe_dir = WLFW_PIPES[i].pipedir;
		cfg.tgt_cfg[i].nentries = WLFW_PIPES[i].nentries;
		cfg.tgt_cfg[i].nbytes_max = WLFW_PIPES[i].nbytes_max;
		cfg.tgt_cfg[i].flags = WLFW_PIPES[i].flags;
	}
	cfg.svc_cfg_valid = 1;
	cfg.svc_cfg_len = WLFW_SERVICE_COUNT;
	for (uint32_t i = 0; i < cfg.svc_cfg_len; i++) {
		cfg.svc_cfg[i].service_id = WLFW_SERVICES[i].service_id;
		cfg.svc_cfg[i].pipe_dir = WLFW_SERVICES[i].pipedir;
		cfg.svc_cfg[i].pipe_num = WLFW_SERVICES[i].pipenum;
	}
	say("wifi: %u: the configuration: host version \"%s\", %u pipes and %u services from ath11k's tables for this chip; "
	    "shadow registers: none%s\n", wlfw.step, cfg.host_version, cfg.tgt_cfg_len, cfg.svc_cfg_len, WLFW_SHADOW_REGISTERS ?
	    " (ath11k SENDS A LIST of up to 36 here)" : "");
	if (!ask("the configuration", QMI_WLANFW_WLAN_CFG_REQ_V01, QMI_WLANFW_WLAN_CFG_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_cfg_req_msg_v01_ei, &cfg, qmi_wlanfw_wlan_cfg_resp_msg_v01_ei, &cfg_reply, false)) {
		return false;
	}
	bzero(&mode, sizeof(mode));
	bzero(&mode_reply, sizeof(mode_reply));
	mode.mode = ATH11K_FIRMWARE_MODE_NORMAL;
	mode.hw_debug_valid = 1;
	mode.hw_debug = 0;
	if (!ask("the mode: mission", QMI_WLANFW_WLAN_MODE_REQ_V01, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode, qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply, false)) {
		return false;
	}
	say("wifi: %u: THE FIRMWARE IS IN MISSION MODE\n", wlfw.step);
	return true;
}

bool
hk_wlfw_exchange(uint32_t step)
{
	struct qmi_wlanfw_ind_register_req_msg_v01 indications;
	struct qmi_wlanfw_ind_register_resp_msg_v01 indications_reply;
	struct qmi_wlanfw_host_cap_req_msg_v01 host;
	struct qmi_wlanfw_host_cap_resp_msg_v01 host_reply;
	struct qmi_wlanfw_cap_req_msg_v01 what = { 0 };
	static struct qmi_wlanfw_cap_resp_msg_v01 target;
	struct qmi_wlanfw_device_info_req_msg_v01 where = { 0 };
	struct qmi_wlanfw_device_info_resp_msg_v01 device;
	struct qmi_wlanfw_wlan_mode_req_msg_v01 mode;
	struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_reply;
	uint32_t calibrate = 0, seconds = 5, to_mission = 0, board_id;
	bool far = false, board = false;
	int ret;

	if (handle_open) {
		return false;
	}
	bzero(&wlfw, sizeof(wlfw));
	wlfw.step = step;
	wlfw.began = now_ms();
	PE_parse_boot_argn("wifi_cal", &calibrate, sizeof(calibrate));
	PE_parse_boot_argn("wifi_ready", &seconds, sizeof(seconds));
	PE_parse_boot_argn("wifi_mission", &to_mission, sizeof(to_mission));
	ret = qmi_handle_init(&handle, ATH11K_QMI_RESP_LEN_MAX, &wlfw_ops, wlfw_handlers);
	if (ret < 0) {
		say("wifi: %u: no handle on the router: %d\n", step, ret);
		return false;
	}
	handle_open = true;
	ret = qmi_add_lookup(&handle, ATH11K_QMI_WLFW_SERVICE_ID_V01, ATH11K_QMI_WLFW_SERVICE_VERS_V01,
	    ATH11K_QMI_WLFW_SERVICE_INS_ID_V01_WCN6750);
	if (ret < 0 || !wait_for(&wlfw.server, 3000)) {
		say("wifi: %u: THE SERVICE (%#x, version %u, instance %u) DOES NOT APPEAR within 3 s (lookup: %d)\n", step,
		    ATH11K_QMI_WLFW_SERVICE_ID_V01, ATH11K_QMI_WLFW_SERVICE_VERS_V01, ATH11K_QMI_WLFW_SERVICE_INS_ID_V01_WCN6750, ret);
		goto out;
	}

	/* ath11k_qmi_fw_ind_register_send(): this chip's firmware has its memory fixed, so nothing is asked about memory */
	bzero(&indications, sizeof(indications));
	bzero(&indications_reply, sizeof(indications_reply));
	indications.client_id_valid = 1;
	indications.client_id = QMI_WLANFW_CLIENT_ID;
	indications.fw_ready_enable_valid = 1;
	indications.fw_ready_enable = 1;
	indications.cal_done_enable_valid = 1;
	indications.cal_done_enable = 1;
	indications.fw_init_done_enable_valid = 1;
	indications.fw_init_done_enable = 1;
	if (!ask("which indications we want", QMI_WLANFW_IND_REGISTER_REQ_V01, QMI_WLANFW_IND_REGISTER_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_ind_register_req_msg_v01_ei, &indications, qmi_wlanfw_ind_register_resp_msg_v01_ei, &indications_reply,
	    false)) {
		goto out;
	}
	if (indications_reply.fw_status_valid) {
		say("wifi: %u:   the firmware's status word: %#llx\n", step, (unsigned long long)indications_reply.fw_status);
	}

	/* ath11k_qmi_host_cap_send(), for a chip with no m3 image and no sleep clock of its own */
	bzero(&host, sizeof(host));
	bzero(&host_reply, sizeof(host_reply));
	host.num_clients_valid = 1;
	host.num_clients = 1;
	host.mem_cfg_mode = 0;
	host.mem_cfg_mode_valid = 1;
	host.bdf_support_valid = 1;
	host.bdf_support = 1;
	host.cal_done_valid = 1;
	host.cal_done = 0;
	if (!ask("what the host can do", QMI_WLANFW_HOST_CAP_REQ_V01, QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_host_cap_req_msg_v01_ei, &host, qmi_wlanfw_host_cap_resp_msg_v01_ei, &host_reply, false)) {
		goto out;
	}

	/* ath11k_qmi_request_target_cap() */
	bzero(&target, sizeof(target));
	if (!ask("what the firmware and the chip are", QMI_WLANFW_CAP_REQ_V01, QMI_WLANFW_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_cap_req_msg_v01_ei, &what, qmi_wlanfw_cap_resp_msg_v01_ei, &target, false)) {
		goto out;
	}
	board_id = target.board_info_valid ? target.board_info.board_id : 0xff;
	say("wifi: %u:   chip id %#x, chip family %#x (%s); BOARD ID %#x (%s); soc id %#x (%s)\n", step, target.chip_info.chip_id,
	    target.chip_info.chip_family, target.chip_info_valid ? "given" : "not given", board_id, target.board_info_valid ?
	    "given" : "not given: ath11k takes 0xff", target.soc_info.soc_id, target.soc_info_valid ? "given" : "not given");
	say("wifi: %u:   firmware version %#x, built \"%s\" (%s); BUILD ID \"%s\" (%s)\n", step, target.fw_version_info.fw_version,
	    target.fw_version_info.fw_build_timestamp, target.fw_version_info_valid ? "given" : "not given", target.fw_build_id,
	    target.fw_build_id_valid ? "given" : "not given");
	say("wifi: %u:   also:%s%s%s%s%s numbers: macs %u, voltage %u mV, time %u Hz, otp %#x, eeprom timeout %u\n", step,
	    target.num_macs_valid ? " macs" : "", target.voltage_mv_valid ? " voltage" : "", target.time_freq_hz_valid ? " time" : "",
	    target.otp_version_valid ? " otp" : "", target.eeprom_read_timeout_valid ? " eeprom" : "", target.num_macs,
	    target.voltage_mv, target.time_freq_hz, target.otp_version, target.eeprom_read_timeout);

	/* ath11k_qmi_request_device_info(): where the radio's registers are (the driver maps them from this) */
	bzero(&device, sizeof(device));
	if (!ask("where the radio's registers are", QMI_WLANFW_DEVICE_INFO_REQ_V01, QMI_WLANFW_DEVICE_INFO_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_device_info_req_msg_v01_ei, &where, qmi_wlfw_device_info_resp_msg_v01_ei, &device, false)) {
		goto out;
	}
	if (device.bar_addr_valid && device.bar_size_valid) {
		registers_at = device.bar_addr;
		registers_size = device.bar_size;
	}
	say("wifi: %u:   registers at %#llx (%s), %#x bytes (%s; ath11k wants exactly %#x)\n", step,
	    (unsigned long long)device.bar_addr, device.bar_addr_valid ? "given" : "not given", device.bar_size,
	    device.bar_size_valid ? "given" : "not given", ATH11K_QMI_DEVICE_BAR_SIZE);

	/*
	 * ath11k_qmi_event_load_bdf(): the regulatory database, whose fate qmi.c does not look at, then
	 * the board's data. ath11k would look in "board-2.bin" for an entry named after the ids above
	 * and fall back to "board.bin"; here it is the file this phone's Android sends.
	 */
	send_file("the regulatory database", "regdb_xiaomi.bin", ATH11K_QMI_BDF_TYPE_REGDB, board_id);
	board = send_file("the board's data", "bd_m20gl.elf", ATH11K_QMI_BDF_TYPE_ELF, board_id);
	if (!board) {
		goto out;
	}
	far = true;

	if (!wait_for(&wlfw.init_done, 10000)) {
		say("wifi: %u: NO \"FIRMWARE INIT DONE\" within 10 s of the board's data\n", step);
		goto out;
	}
	if (!calibrate) {
		/* (with cold-boot calibration off qmi.c goes on to the rest of the driver at this point) */
		if (to_mission) {
			in_mission = mission();
			goto out;
		}
		say("wifi: %u: cold-boot calibration is off: waiting %u s to see whether \"firmware ready\" comes by itself\n", step,
		    seconds);
		wait_for(&wlfw.ready, seconds * 1000);
		say("wifi: %u: \"firmware ready\" %s\n", step, wlfw.ready ? "HAS COME" : "HAS NOT COME BY ITSELF");
		goto out;
	}

	/* ath11k_qmi_process_coldboot_calibration(): the mode, then up to a minute for "calibration done" */
	bzero(&mode, sizeof(mode));
	bzero(&mode_reply, sizeof(mode_reply));
	mode.mode = MODE_COLD_BOOT;
	mode.hw_debug_valid = 1;
	mode.hw_debug = 0;
	if (!ask("the mode: cold-boot calibration", QMI_WLANFW_WLAN_MODE_REQ_V01, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode, qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply, false)) {
		goto out;
	}
	if (!wait_for(&wlfw.cal_done, 60000) && !wlfw.ready) {
		say("wifi: %u: NEITHER \"CALIBRATION DONE\" NOR \"FIRMWARE READY\" within 60 s of the mode\n", step);
	} else {
		wait_for(&wlfw.ready, seconds * 1000);
		say("wifi: %u: calibration: \"done\" %s, \"firmware ready\" %s\n", step, wlfw.cal_done ? "came" : "did not come",
		    wlfw.ready ? "CAME" : "DID NOT COME");
	}
	if (to_mission && wlfw.ready) {
		/* (qmi.c, on "ready": the rest of the driver, which begins with ath11k_qmi_firmware_start()) */
		in_mission = mission();
		goto out;
	}
	/*
	 * Not qmi.c's next message: it goes on to configuration and mission mode from here. This is
	 * what it sends when the driver stops (ath11k_qmi_firmware_stop()), tried here to leave the
	 * firmware as it was found; in this state the firmware refuses it (see the top).
	 */
	bzero(&mode_reply, sizeof(mode_reply));
	mode.mode = ATH11K_FIRMWARE_MODE_OFF;
	ask("the mode: off (not ath11k's next message)", QMI_WLANFW_WLAN_MODE_REQ_V01, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode, qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply, false);
out:
	say("wifi: %u: the exchange ends %llu ms after it began: \"init done\" %s, \"ready\" %s, \"calibration done\" %s, mission "
	    "mode %s\n", step, now_ms() - wlfw.began, wlfw.init_done ? "yes" : "no", wlfw.ready ? "yes" : "no", wlfw.cal_done ? "yes" :
	    "no", in_mission ? "YES" : "no");
	if (!in_mission) {
		qmi_handle_release(&handle);
		handle_open = false;
	}
	return far && wlfw.init_done;
}

/* UMI's gated WLFW startup: step 7 queries capabilities, step 8 supplies
 * board data and M3, and step 9 requests cold-boot calibration.
 * UMI advertises instance 1; WCN6750 uses instance 3.
 */
bool
hk_wlfw_query(uint32_t step)
{
	struct qmi_wlanfw_ind_register_req_msg_v01 indications;
	struct qmi_wlanfw_ind_register_resp_msg_v01 indications_reply;
	struct qmi_wlanfw_host_cap_req_msg_v01 host;
	struct qmi_wlanfw_host_cap_resp_msg_v01 host_reply;
	struct qmi_wlanfw_cap_req_msg_v01 what;
	struct qmi_wlanfw_cap_resp_msg_v01 target;
	bool ok = false;
	int ret;

	if (handle_open) return false;
	bzero(&wlfw, sizeof(wlfw));
	wlfw.step = step;
	wlfw.began = now_ms();
	ret = qmi_handle_init(&handle, ATH11K_QMI_RESP_LEN_MAX, &wlfw_ops, wlfw_handlers);
	if (ret < 0) {
		say("wifi: %u: no handle on UMI's QRTR transport: %d\n", step, ret);
		return false;
	}
	handle_open = true;
	ret = qmi_add_lookup(&handle, ATH11K_QMI_WLFW_SERVICE_ID_V01,
	    ATH11K_QMI_WLFW_SERVICE_VERS_V01, 1);
	if (ret < 0 || !wait_for(&wlfw.server, 3000)) {
		say("wifi: %u: WLFW service instance 1 was not found (lookup: %d)\n", step, ret);
		goto out;
	}

	bzero(&indications, sizeof(indications));
	bzero(&indications_reply, sizeof(indications_reply));
	indications.client_id_valid = 1;
	indications.client_id = QMI_WLANFW_CLIENT_ID;
	indications.request_mem_enable_valid = 1;
	indications.request_mem_enable = 1;
	indications.fw_mem_ready_enable_valid = 1;
	indications.fw_mem_ready_enable = 1;
	indications.fw_ready_enable_valid = 1;
	indications.fw_ready_enable = 1;
	indications.cal_done_enable_valid = 1;
	indications.cal_done_enable = 1;
	indications.fw_init_done_enable_valid = 1;
	indications.fw_init_done_enable = 1;
	if (!ask("UMI WLFW indication registration", QMI_WLANFW_IND_REGISTER_REQ_V01,
	    QMI_WLANFW_IND_REGISTER_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_ind_register_req_msg_v01_ei, &indications,
	    qmi_wlanfw_ind_register_resp_msg_v01_ei, &indications_reply, false)) goto out;

	bzero(&host, sizeof(host));
	bzero(&host_reply, sizeof(host_reply));
	host.num_clients_valid = 1;
	/* Unlike Android CNSS, this probe has no second cnss-daemon client. */
	host.num_clients = 1;
	host.wake_msi_valid = 1;
	host.wake_msi = 813; /* stock CNSS assigns WAKE after the three MHI MSI vectors */
	host.bdf_support_valid = 1;
	host.bdf_support = 1;
	/* Stock UMI CNSS sends the M3 image after the target-capability query. */
	host.m3_support_valid = 1;
	host.m3_support = 1;
	host.m3_cache_support_valid = 1;
	host.m3_cache_support = 1;
	host.cal_done_valid = 1;
	host.cal_done = 0; /* this boot has freshly allocated, uncalibrated CAL memory */
	if (!ask("UMI WLFW host capabilities", QMI_WLANFW_HOST_CAP_REQ_V01,
	    QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_host_cap_req_msg_v01_ei, &host,
	    qmi_wlanfw_host_cap_resp_msg_v01_ei, &host_reply, false)) goto out;
	if (!wait_for_memory_event(5000)) {
		say("wifi: %u: QCA6390 sent neither a memory request nor FW_MEM_READY after host capabilities\n", step);
		goto out;
	}
	if (wlfw.mem_request && !respond_memory_request()) goto out;
	if (!wlfw.mem_ready && !wait_for(&wlfw.mem_ready, 5000)) {
		say("wifi: %u: QCA6390 did not indicate FW_MEM_READY after the memory response\n", step);
		goto out;
	}

	bzero(&what, sizeof(what));
	bzero(&target, sizeof(target));
	if (!ask("UMI WLFW target capabilities", QMI_WLANFW_CAP_REQ_V01,
	    QMI_WLANFW_CAP_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_cap_req_msg_v01_ei,
	    &what, qmi_wlanfw_cap_resp_msg_v01_ei, &target, false)) goto out;
	uint32_t board_id = target.board_info_valid ? target.board_info.board_id : 0xff;
	say("wifi: %u: QCA6390 reports chip %#x family %#x, board %#x, soc %#x, firmware %#x (%s)\n",
	    step, target.chip_info.chip_id, target.chip_info.chip_family,
	    board_id, target.soc_info.soc_id,
	    target.fw_version_info.fw_version,
	    target.fw_version_info.fw_build_timestamp);
	if (step >= 8) {
		struct qmi_wlanfw_m3_info_req_msg_v01 m3;
		struct qmi_wlanfw_m3_info_resp_msg_v01 m3_reply;
		uint64_t m3_size = 0;
		const uint8_t *m3_file = hk_wifi_driver_file("m3.bin", &m3_size);
		if (!target.chip_info_valid || target.chip_info.chip_family != 0xb ||
		    board_id != 0xff ||
		    !target.fw_version_info_valid || target.fw_version_info.fw_version != 0x1014046e ||
		    m3_file == NULL || m3_size != 0x411bc) {
			say("wifi: %u: UMI firmware identity or M3 size did not match stock; stopping\n", step);
			goto out;
		}
		if (!send_file("UMI regulatory database", "qca6390/regdb.bin",
		    ATH11K_QMI_BDF_TYPE_REGDB, board_id) ||
		    !send_file("UMI board data", "qca6390/bd_j1gl.elf",
		    ATH11K_QMI_BDF_TYPE_ELF, board_id)) goto out;
		void *m3_memory = lx_os_radio_memory_at(m3_size, UMI_QCA_M3_IOVA);
		if (m3_memory == NULL) goto out;
		memcpy(m3_memory, m3_file, m3_size);
		__asm__ volatile("dsb osh" ::: "memory");
		bzero(&m3, sizeof(m3));
		bzero(&m3_reply, sizeof(m3_reply));
		m3.addr = UMI_QCA_M3_IOVA;
		m3.size = (uint32_t)m3_size;
		if (!ask("UMI M3 information", QMI_WLANFW_M3_INFO_REQ_V01,
		    QMI_WLANFW_M3_INFO_REQ_MSG_V01_MAX_MSG_LEN,
		    qmi_wlanfw_m3_info_req_msg_v01_ei, &m3,
		    qmi_wlanfw_m3_info_resp_msg_v01_ei, &m3_reply, false)) goto out;
		if (!wait_for(&wlfw.init_done, 10000)) {
			say("wifi: %u: UMI firmware did not initialize after board data and M3\n", step);
			goto out;
		}
		if (step >= 9) {
			struct qmi_wlanfw_wlan_mode_req_msg_v01 mode;
			struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_reply;
			bzero(&mode, sizeof(mode));
			bzero(&mode_reply, sizeof(mode_reply));
			mode.mode = MODE_COLD_BOOT;
			mode.hw_debug_valid = 1;
			if (!ask("UMI cold-boot calibration", QMI_WLANFW_WLAN_MODE_REQ_V01,
			    QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
			    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode,
			    qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply, false)) goto out;
			for (unsigned ms = 0; ms < 60000 && !wlfw.cal_done && !wlfw.ready && !wlfw.gone; ms += 10) {
				if (hk_qmi_poll(&handle, 10)) goto out;
			}
			if (!wlfw.cal_done && !wlfw.ready) {
				say("wifi: %u: UMI calibration did not complete within 60 s\n", step);
				goto out;
			}
			say("wifi: %u: UMI cold-boot calibration completed; CAL memory retained until endpoint reset\n", step);
		}
	}
	ok = true;
out:
	qmi_handle_release(&handle);
	handle_open = false;
	return ok;
}

#endif
