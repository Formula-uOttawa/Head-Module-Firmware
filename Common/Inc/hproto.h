/*
 * hproto.h - transport independent config/debug message handler (portable C).
 *
 * A transport (UART via hframe, or UDP later) hands a received message (type, seq, payload) to Hproto_Handle()
 * and sends back whatever it returns. All platform specifics (flash, the CAN core, ...) sit behind HprotoOps_t,
 * so adding a transport never touches the message handling. See docs/protocol.md section 3.
 */
#ifndef HPROTO_H
#define HPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "head_decode.h"

#define HPROTO_VERSION 1u
#define HPROTO_DEVICE_HEAD 1u /* IDENT device types: 1 = head module, 2 = hub module */

typedef enum {
    HMSG_INFO = 0x01, HMSG_CFG_BEGIN = 0x02, HMSG_CFG_CHUNK = 0x03, HMSG_CFG_COMMIT = 0x04, HMSG_CFG_READ = 0x05,
    HMSG_CFG_CLEAR = 0x06, HMSG_IDENT = 0x07, HMSG_LIVE_GET = 0x10, HMSG_STATS_GET = 0x11, HMSG_SNIFF_SET = 0x30, HMSG_SNIFF_FRAME = 0xB1,
    HMSG_HUB_RELAY = 0x40, HMSG_REBOOT = 0x70, HMSG_ERROR = 0xFF
} HprotoType_t;

typedef enum {
    HERR_NONE = 0, HERR_BAD_REQUEST = 1, HERR_BUSY = 2, HERR_TOO_BIG = 3, HERR_CRC = 4, HERR_INVALID_CONFIG = 5,
    HERR_FLASH = 6, HERR_TIMEOUT = 7, HERR_UNSUPPORTED = 8, HERR_RANGE = 9
} HprotoError_t;

#define HPROTO_COMMIT_APPLY 0x01u
#define HPROTO_COMMIT_STORE 0x02u
#define HPROTO_COMMIT_FLAGS_MASK (HPROTO_COMMIT_APPLY | HPROTO_COMMIT_STORE) /* flags == 0 or any other bit -> HERR_BAD_REQUEST */
#define HPROTO_CLEAR_REVERT 0x01u
#define HPROTO_CHUNK_MAX 240u
#define HPROTO_READ_MAX 232u   /* CFG_READ data bytes per reply (reply payload = 8 + data) */
#define HPROTO_LIVE_MAX 48u    /* LIVE_GET entries per reply (reply payload = 4 + 5 * entries) */
#define HPROTO_INFO_BASE_LEN 48u /* INFO payload up to and including fdcan_kernel_hz (offsets never change) */
#define HPROTO_INFO_LEN 80u      /* INFO payload: base 48 + 32 appended bytes (see Hproto_Handle) */
#define HPROTO_MIN_RESP 64u   /* smallest response buffer Hproto_Handle accepts (INFO then returns only its first 48 bytes; the full INFO needs 2 + 80) */
/* STATS payload size for nbus buses (nbus <= HEAD_MAX_BUSES): see Hproto_EncodeStats */
#define HPROTO_STATS_LEN(nbus) (1u + 18u * (nbus) + 1u + 32u + 1u + 8u * (nbus) + 12u)
#define HPROTO_REBOOT_MAGIC 0xB007F00Du /* REBOOT payload: this value, u32 little endian; anything else -> HERR_BAD_REQUEST */
#define HPROTO_RELAY_MAX_REQ 249u /* HMSG_HUB_RELAY request payload = u8 hub, u16 timeout_ms, then the hub message (<= this many bytes); the reply payload is u8 hub + the hub's reply */
#define HPROTO_COMMIT_REPLY_LEN 11u /* applied, stored, u64 hash, u8 store_err (older hosts ignore the trailing byte) */

typedef struct {
    uint16_t fw_version; /* major << 8 | minor */
    uint32_t uptime_ms;
    uint64_t active_hash, stored_hash;
    uint8_t active_source; /* 0 compiled-in default, 1 flash, 2 uploaded (RAM only) */
    uint8_t cm4_alive;
    uint16_t channel_count;
    uint8_t uid[12];
    uint32_t fdcan_kernel_hz; /* 0 if unknown */
    /* appended to the INFO reply after the first 48 bytes (older hosts ignore them) */
    uint8_t cm4_status;       /* HEAD_CM4_ST_* bits */
    uint8_t boot_apply_rc;    /* HERR_* of the CM4's last boot-time apply */
    uint16_t boot_apply_tries;
    uint32_t hframe_crc_fail, hframe_overflow, hframe_desync; /* UART framing error counters */
    uint32_t rx_ring_overflow; /* bytes dropped because the UART receive ring was full */
    uint32_t uart_err;         /* UART hardware errors (overrun, framing, noise, parity) */
    uint32_t uart_tx_fail;     /* UART transmissions that failed or timed out */
    uint8_t tcan_init_status;  /* TCAN_INIT_* bits of the SBC bring-up (0x04 = it failed) */
} HprotoInfo_t;

/* All ops return HERR_NONE (0) on success. ctx is passed through unchanged. */
typedef struct {
    void (*get_info)(void *ctx, HprotoInfo_t *info);
    /*
     * blob is complete and its transfer CRC already verified; flags is a non-zero combination of HPROTO_COMMIT_*.
     * The platform validates and applies first; then stores if requested. If the store fails AFTER a successful apply it
     * returns HERR_NONE with *applied = 1, *stored = 0 and *store_err = the HERR_* code of the failure (reported to the host
     * as a trailing reply byte). If APPLY was not requested and the store fails it returns the error code instead.
     * *store_err is 0 (HERR_NONE) on entry.
     */
    uint8_t (*cfg_commit)(void *ctx, const uint8_t *blob, uint32_t len, uint8_t flags, uint64_t *hash, uint8_t *applied, uint8_t *stored,
                          uint8_t *store_err);
    /* source 0 = active, 1 = stored. Sets *total and copies up to max bytes from offset into dst, setting *n */
    uint8_t (*cfg_read)(void *ctx, uint8_t source, uint32_t offset, uint8_t *dst, uint16_t max, uint32_t *total, uint16_t *n);
    uint8_t (*cfg_clear)(void *ctx, uint8_t flags);
    /* write `count` entries of 5 bytes (u32 value LE, u8 flags) to dst; returns number written */
    uint16_t (*live_get)(void *ctx, uint16_t first, uint16_t count, uint8_t *dst);
    /* writes the STATS_GET response payload; returns its length (0 = unsupported) */
    uint16_t (*stats_get)(void *ctx, uint8_t *dst, uint16_t cap);
    uint8_t (*sniff_set)(void *ctx, uint8_t bus, uint8_t enable);
    /*
     * Forward one message (req = type, seq, payload of the hub protocol) to hub `hub` over the hub bus and wait for its reply
     * (hcanlink.h) for at most timeout_ms (0 = the default, HRELAY_TIMEOUT_MS; larger values are clamped to it). On success copies the hub's reply message (type, seq, payload) into rsp (at most cap bytes), sets *rsp_len and
     * returns HERR_NONE; HERR_TIMEOUT if the hub did not answer, HERR_UNSUPPORTED without a hub bus, HERR_BUSY while another
     * relay is in flight. NULL = unsupported.
     */
    uint8_t (*hub_relay)(void *ctx, uint8_t hub, uint16_t timeout_ms, const uint8_t *req, uint16_t req_len, uint8_t *rsp, uint16_t cap,
                         uint16_t *rsp_len);
    void (*reboot)(void *ctx);
} HprotoOps_t;

typedef struct {
    const HprotoOps_t *ops;
    void *ctx;
    uint8_t *stage;      /* staging buffer for uploads (capacity stage_cap bytes) */
    uint32_t stage_cap;
    uint32_t stage_total, stage_received, stage_crc;
    uint32_t stage_last_off, stage_last_len; /* the last accepted chunk (a retransmit of it is re-acknowledged) */
    bool stage_have_last;
    bool stage_active;
    bool reboot_pending;
} Hproto_t;

void Hproto_Init(Hproto_t *p, const HprotoOps_t *ops, void *ctx, uint8_t *stage, uint32_t stage_cap);

/*
 * STATS payload: u8 nbus, per bus {u32 rx_frames, rx_matched, rx_dropped, fifo_lost, u8 bus_off, u8 passive} (18 B); u8 8, 8 x u32 hub_lost;
 * u8 nbus, per bus {u32 tx_frames, tx_dropped} (8 B); then (appended last, optional for old hosts) u32 rx_nobus, rx_ring_overrun,
 * rx_ring_high. Returns the length (HPROTO_STATS_LEN(nbus)), or 0 if cap is too small. nbus is clamped to HEAD_MAX_BUSES.
 */
uint16_t Hproto_EncodeStats(const HeadStats_t *st, uint8_t nbus, uint8_t *dst, uint16_t cap);

/*
 * Handle one message (msg = type, seq, payload). Writes the response message (same layout) into resp and returns
 * its length, or 0 if there is nothing to send. After a response to REBOOT is sent, Hproto_TakeReboot() is true.
 * resp_cap must be at least HPROTO_MIN_RESP; replies are clamped to it (CFG_READ data to cap - 10, LIVE_GET entries to (cap - 6) / 5).
 */
size_t Hproto_Handle(Hproto_t *p, const uint8_t *msg, size_t len, uint8_t *resp, size_t resp_cap);
bool Hproto_TakeReboot(Hproto_t *p);

#endif
