/*
 * head_shared.h - memory shared by the CM4 (CAN) and CM7 (link/storage) cores.
 *
 * Lives in SRAM4 (D3 domain, 0x38000000, 64 KB), reachable by both cores at the same address, so no linker script
 * changes are needed. SRAM4 survives a system reset, so a stale `magic` can be seen after a reboot: the CM4 clears
 * `magic` as its very first action, initialises everything else and sets `magic` LAST. The CM7 must additionally see
 * `cm4_heartbeat` change after its own start before it trusts the region (see "readiness" below).
 *
 * Ownership (a field has exactly one writer):
 *   CM4: magic, cm4_heartbeat, active_*, publish_seq, fdcan_kernel_hz, bus_count, channel_count, boot_apply_*, cm4_status,
 *        cmd_ack, cmd_result, cmd_result_seq, sniff_head, sniff_dropped, stats, live, sniff[], active_blob, relay_len, can_diag.
 *   CM7: stage, cmd, cmd_arg, cmd_len, cmd_crc, cmd_seq, cmd_req, sniff_mask, sniff_tail.
 *   relay_buf: the CM7 writes the request before issuing HEAD_CMD_HUB_RELAY; the CM4 copies it at once and, only just before it
 *        acknowledges, writes the hub's reply into the same buffer (with relay_len); the CM7 reads it after the acknowledge.
 * (`active_source` is CM4-owned: the CM7 changes it only through HEAD_CMD_APPLY_STAGED's cmd_arg or HEAD_CMD_SET_SOURCE.)
 * Caches: the CM7 data cache is currently disabled. If it is ever enabled, mark this region non-cacheable in the MPU.
 *
 * Readiness (CM7): ready = (magic == HEAD_SHARED_MAGIC) && cm4_heartbeat differs from the value first read after the
 * CM7 started (a stale region from the previous boot has a frozen heartbeat). Do not touch stage/cmd* before that.
 *
 * Mailbox (CM7 -> CM4), one request at a time:
 *   1. The CM7 may issue a request only if cmd_req == cmd_ack. Otherwise it refuses the host's request with HERR_BUSY
 *      (this includes a request that timed out: it stays outstanding until the CM4 acknowledges it, and during that time
 *      the CM7 must neither start another request nor modify `stage`).
 *   2. It writes cmd, cmd_arg, cmd_len, cmd_crc, cmd_seq (a fresh non-zero id), then HEAD_MEM_BARRIER(), then cmd_req = cmd_req + 1.
 *   3. The CM4 snapshots req = cmd_req and the cmd* fields once, executes, writes cmd_result and cmd_result_seq = the snapshot's
 *      cmd_seq, HEAD_MEM_BARRIER(), then cmd_ack = req (the snapshot, never a re-read of cmd_req).
 *   4. The CM7 sees cmd_ack == cmd_req, HEAD_MEM_BARRIER(), and uses cmd_result (cmd_result_seq must equal its cmd_seq).
 * HEAD_CMD_APPLY_STAGED additionally carries cmd_crc = the blob's own header CRC field (HeadLe32(stage + 28)); the CM4 refuses to
 * apply (HERR_BUSY) if the stage header no longer has that value, so a late command can never apply a newer (half uploaded)
 * stage. (The full CRC pass happens once, inside HeadCfg_Parse.)
 *
 * Config publish consistency: the CM4 wraps every config swap in publish_seq (odd while the swap is in progress). A CM7
 * reader of active_blob / active_len / active_hash_* / active_source / bus_count / channel_count / stats / live samples
 * publish_seq before and after (HeadShared_SeqBegin / HeadShared_SeqValid) and retries or answers HERR_BUSY if it changed
 * or was odd.
 */
#ifndef HEAD_SHARED_H
#define HEAD_SHARED_H

#include "head_decode.h"
#include "head_util.h"

#define HEAD_SHARED_BASE 0x38000000UL
#define HEAD_SHARED_MAGIC 0x48534852u

/* Persistent config sector: top 128 KB sector of flash bank 1 (outside the CM7 image, see CM7 linker script).
   Its content is a slot log managed by head_slots.h; the CM4 only reads it through HeadSlots_FindNewest. */
#define HEAD_STORE_ADDR 0x080E0000UL
#define HEAD_STORE_SIZE 0x00020000UL

/* CM7 -> CM4 commands (mailbox) */
#define HEAD_CMD_NONE 0u
#define HEAD_CMD_APPLY_STAGED 1u  /* blob in `stage`, length in cmd_len, blob header CRC (offset 28) in cmd_crc, cmd_arg = active_source to record (0..2) */
#define HEAD_CMD_APPLY_FLASH 2u   /* newest valid blob of the config sector */
#define HEAD_CMD_APPLY_DEFAULT 3u /* compiled-in default blob */
#define HEAD_CMD_SET_SOURCE 4u    /* cmd_arg = 0 default / 1 flash / 2 uploaded: update active_source after a store or a clear
                                     (1 is refused with HERR_INVALID_CONFIG unless the newest stored blob has the active hash) */

/* values of cm4_status (bit mask) */
#define HEAD_CM4_ST_BUSES_DOWN 0x01u   /* an apply failed AND the rollback to the previous config failed: the buses are stopped */
#define HEAD_CM4_ST_BOOT_PENDING 0x02u /* the boot-time apply is still being retried (FDCAN kernel clock not available yet) */

#define HEAD_CMD_HUB_RELAY 5u     /* relay one hub protocol message over the hub bus: cmd_arg = hub id (0..7), request message (type, seq, payload)
                                     in relay_buf, length in cmd_len, cmd_crc = timeout in ms (0 = default). The CM4 acknowledges when the hub has answered or HRELAY_TIMEOUT_MS passed
                                     (cmd_result HERR_TIMEOUT); the reply message is then in relay_buf, its length in relay_len. */
#define HEAD_RELAY_BUF 256u

/* FDCAN controller snapshot per peripheral (index = peripheral - 1), refreshed by the CM4 every 100 ms; read by the "can:status" console
   command. Plain statistics: no seqlock, a torn read only shows a mixed snapshot. */
typedef struct {
    volatile uint32_t running;   /* 1 = started by the active config */
    volatile uint32_t nbtp, dbtp, tdcr, cccr; /* raw registers: nominal / data bit timing, delay compensation, control */
    volatile uint32_t tec, rec;  /* transmit / receive error counters */
    volatile uint32_t bus_off, passive, warning;
    volatile uint32_t activity;  /* 0 synchronising, 1 idle, 2 receiving, 3 transmitting */
    volatile uint32_t tdcv;      /* measured transmitter delay (TDC value) */
    volatile uint32_t lec_seen;  /* bit n set = last error code n seen since the config was applied (arbitration phase) */
    volatile uint32_t dlec_seen; /* the same for the data phase. Codes: 1 stuff, 2 form, 3 ack, 4 bit1, 5 bit0, 6 crc */
} HeadCanDiag_t;

#define HEAD_SNIFF_RING 32u
typedef struct {
    uint32_t id;
    uint32_t ts_ms;
    uint8_t bus;
    uint8_t flags; /* bit0 extended, bit1 FD */
    uint8_t len;   /* 0..64 (readers must still clamp: HeadShared_SniffLen) */
    uint8_t data[64];
} HeadSniffEntry_t;

typedef struct {
    volatile uint32_t magic;
    volatile uint32_t cm4_heartbeat; /* incremented every ms by the CM4 main loop */
    volatile uint32_t publish_seq;   /* seqlock around config swaps: odd while the CM4 is changing the active config */
    volatile uint32_t active_hash_lo, active_hash_hi;
    volatile uint32_t active_len;
    volatile uint32_t fdcan_kernel_hz; /* read by the CM4 at boot (retried until non-zero), reported in INFO; 0 = clock not available */
    volatile uint8_t active_source; /* 0 default, 1 flash, 2 uploaded */
    volatile uint8_t bus_count;
    volatile uint16_t channel_count;
    volatile uint32_t sniff_mask; /* CM7: bit per bus index */
    volatile uint32_t boot_apply_rc;      /* HERR_* of the last boot-time apply attempt (0 = ok) */
    volatile uint32_t boot_apply_tries;   /* boot-time apply attempts (1 = first try worked or failed for good) */
    volatile uint32_t cm4_status;         /* HEAD_CM4_ST_* */

    volatile uint32_t cmd, cmd_arg, cmd_len, cmd_crc, cmd_seq, cmd_req; /* CM7 -> CM4 */
    volatile uint32_t cmd_ack, cmd_result, cmd_result_seq;              /* CM4 -> CM7 */

    volatile uint32_t sniff_head, sniff_tail, sniff_dropped;
    volatile uint32_t relay_len; /* CM4 -> CM7: length of the reply in relay_buf */
    HeadCanDiag_t can_diag[2];   /* peripheral 1 (FDCAN1), 2 (FDCAN2) */
    uint8_t relay_buf[HEAD_RELAY_BUF] __attribute__((aligned(4)));
    HeadStats_t stats;
    HeadLive_t live;
    HeadSniffEntry_t sniff[HEAD_SNIFF_RING];
    uint8_t active_blob[HEAD_MAX_BLOB] __attribute__((aligned(4)));
    uint8_t stage[HEAD_MAX_BLOB] __attribute__((aligned(4)));
} HeadShared_t;

_Static_assert(sizeof(HeadShared_t) <= 0x10000, "HeadShared_t must fit in SRAM4");

#define HEAD_SHARED ((HeadShared_t *)HEAD_SHARED_BASE)

/* ---- CM4: bracket a config swap ---- */
static inline void HeadShared_PublishBegin(HeadShared_t *sh)
{
    sh->publish_seq = sh->publish_seq + 1u; /* odd */
    HEAD_MEM_BARRIER();
}
static inline void HeadShared_PublishEnd(HeadShared_t *sh)
{
    HEAD_MEM_BARRIER();
    sh->publish_seq = sh->publish_seq + 1u; /* even */
}

/* ---- CM7: consistent reads. seq = HeadShared_SeqBegin(sh); if (seq & 1) -> busy; read; if (!HeadShared_SeqValid(sh, seq)) retry ---- */
static inline uint32_t HeadShared_SeqBegin(const HeadShared_t *sh)
{
    uint32_t s = sh->publish_seq;
    HEAD_MEM_BARRIER();
    return s;
}
static inline bool HeadShared_SeqValid(const HeadShared_t *sh, uint32_t seq)
{
    HEAD_MEM_BARRIER();
    return (seq & 1u) == 0u && sh->publish_seq == seq;
}

/* ---- CM7: clamps for values that come from the other core ---- */
static inline uint8_t HeadShared_SniffLen(const HeadSniffEntry_t *e)
{
    return e->len > sizeof e->data ? (uint8_t)sizeof e->data : e->len;
}
static inline uint8_t HeadShared_BusCount(const HeadShared_t *sh)
{
    return sh->bus_count > HEAD_MAX_BUSES ? (uint8_t)HEAD_MAX_BUSES : sh->bus_count;
}
static inline uint16_t HeadShared_ChannelCount(const HeadShared_t *sh)
{
    return sh->channel_count > HEAD_MAX_CHANNELS ? (uint16_t)HEAD_MAX_CHANNELS : sh->channel_count;
}
static inline uint32_t HeadShared_ActiveLen(const HeadShared_t *sh)
{
    return sh->active_len > HEAD_MAX_BLOB ? (uint32_t)HEAD_MAX_BLOB : sh->active_len;
}

#endif
