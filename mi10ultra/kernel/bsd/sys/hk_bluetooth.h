/* Bluetooth H4 framing shared by the UMI UART transport and host checks.
 * QCA IBS bytes are control messages only between complete H4 packets. */
#ifndef HK_BLUETOOTH_H
#define HK_BLUETOOTH_H
#include <stdint.h>
#include <stddef.h>
#define HK_BT_H4_MAX 4096u
struct hk_bt_h4 {
    uint8_t bytes[HK_BT_H4_MAX];
    uint32_t used, wanted, header;
    int complete;
};
/* 0: incomplete, 1: packet ready, 2: QCA IBS byte, -1: bad type,
 * -2: packet exceeds the fixed bound. Transport must reset on framing errors.
 * Complete bytes remain valid until the next call. */
static inline int hk_bt_h4_push(struct hk_bt_h4 *p, uint8_t byte)
{
    if (p->complete) { p->used = p->wanted = p->header = 0; p->complete = 0; }
    if (p->used == 0) {
        if (byte >= 0xfc && byte <= 0xfe) return 2;
        switch (byte) {
        case 1: case 3: p->header = 4; break;
        case 2: case 5: p->header = 5; break;
        case 4: p->header = 3; break;
        default: return -1;
        }
        p->wanted = p->header;
    }
    if (p->used >= HK_BT_H4_MAX) return -2;
    p->bytes[p->used++] = byte;
    if (p->used == p->header) {
        uint32_t length;
        switch (p->bytes[0]) {
        case 1: case 3: length = p->bytes[3]; break;
        case 4: length = p->bytes[2]; break;
        default:
            length = p->bytes[3] | (uint32_t)p->bytes[4] << 8;
            if (p->bytes[0] == 5) length &= 0x3fff;
            break;
        }
        if (length > HK_BT_H4_MAX - p->header) {
            p->used = p->wanted = p->header = 0;
            return -2;
        }
        p->wanted = p->header + length;
    }
    if (p->used == p->wanted) { p->complete = 1; return 1; }
    return 0;
}
void umi_bluetooth_init(void);
/* HCI Command Complete: verify framing/opcode before reading status. */
static inline int hk_bt_cc_status(const uint8_t *e, size_t n, uint16_t opcode)
{
    if (n < 7 || e[0] != 4 || e[1] != 0x0e || (size_t)e[2] + 3 != n ||
        e[4] != (uint8_t)opcode || e[5] != (uint8_t)(opcode >> 8)) return -1;
    return e[6];
}
static inline int hk_bt_qca_tlv_ack(const uint8_t *e, size_t n)
{
    /* Hastings uses unified Command Complete, including the EDL subopcode. */
    return (n == 8 && hk_bt_cc_status(e, n, 0xfc00) == 0 && e[7] == 0x1e) ||
        (n == 6 && e[0] == 4 && e[1] == 0xff && e[2] == 3 &&
         e[3] == 0 && e[4] == 4 && e[5] == 0);
}
/* Hastings/WCN3991+ tag 17 puts baud in byte 1; Rome used byte 2.
 * Validate all records before changing anything. Preserve the adjacent flags. */
static inline int hk_bt_qca6390_nvm_prepare(uint8_t *data, size_t n)
{
    if (n < 4 || data[0] != 2 ||
        ((uint32_t)data[1] | (uint32_t)data[2] << 8 | (uint32_t)data[3] << 16) != n - 4)
        return -1;
    size_t hci = 0;
    for (size_t off = 4; off < n;) {
        if (n - off < 12) return -1;
        unsigned id = data[off] | (unsigned)data[off + 1] << 8;
        unsigned len = data[off + 2] | (unsigned)data[off + 3] << 8;
        if (len > n - off - 12) return -1;
        if (id == 17) {
            if (len < 3 || hci) return -1;
            hci = off + 12;
        }
        off += 12 + len;
    }
    if (!hci) return -1;
    data[hci + 1] = 0; /* QCA_BAUDRATE_115200 */
    return 0;
}
#endif
