/*
 * head_link.c - UART4 (ST-LINK VCP) transport for the config/debug protocol (CM7).
 *
 * Receive is interrupt driven into a ring buffer, drained from the main loop into the framing state machine
 * (hframe.c), which hands binary messages to the transport independent dispatcher (hproto.c) and text lines to the
 * ASCII debug commands. A UDP transport later only needs to call Hproto_Handle() with each datagram.
 *
 * UART4_IRQHandler is defined here because UART4's NVIC interrupt is not enabled in the .ioc. If it is ever enabled in
 * CubeMX (generating the handler in stm32h7xx_it.c), remove the handler below to avoid a duplicate symbol.
 *
 * The CM4 is only reached through the mailbox described in head_shared.h; this file implements the CM7 side of it
 * (cm4_command) and never writes a field the CM4 owns.
 */
#include "head_link.h"

#include <string.h>

#include "head_cfg.h"
#include "head_config.h"
#include "head_debug.h"
#include "head_shared.h"
#include "head_store.h"
#include "hframe.h"
#include "hproto.h"
#include "main.h"
#include "tcan.h"

extern UART_HandleTypeDef huart4;

/*
 * Board revision 1 has the ST-LINK VCP wired TX-to-TX / RX-to-RX (PC10 = UART4_TX goes to the ST-LINK's TX output).
 * Setting this to 1 swaps the UART's TX and RX in hardware (USART_CR2.SWAP): the UART transmits on PC11 and receives
 * on PC10, which matches the wiring. Set it to 0 on a board that has the VCP wired correctly.
 */
#ifndef HEAD_UART4_SWAP_TXRX
#define HEAD_UART4_SWAP_TXRX 1
#endif

#define RX_RING_SIZE 1024u
#define SH HEAD_SHARED

static volatile uint8_t s_ring[RX_RING_SIZE];
static volatile uint16_t s_ring_head, s_ring_tail;
static uint8_t s_rx_byte;
static volatile uint32_t s_rx_ring_overflow, s_uart_err; /* written by the UART ISR */
static uint32_t s_uart_tx_fail;                          /* main context only */

static HeadConfig_t s_scratch; /* validation of uploaded blobs before they reach the CM4 or flash */
static Hproto_t s_proto;
static HframeRx_t s_frx;
static uint32_t s_hb_last, s_hb_seen_ms;
static uint32_t s_hb_first;      /* CM4 heartbeat as first seen by this core (a stale region has a frozen heartbeat) */
static bool s_hb_changed;        /* the heartbeat moved since then: the CM4 booted after us */
static uint32_t s_cmd_seq;       /* last mailbox request id */
static uint32_t s_last_rx_ms;    /* time of the last byte taken from the RX ring */
static bool s_swapped;

/* ---- UART plumbing ------------------------------------------------------------------------------------------- */

void UART4_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart4);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != UART4)
        return;
    uint16_t next = (uint16_t)((s_ring_head + 1u) % RX_RING_SIZE);
    if (next != s_ring_tail) {
        s_ring[s_ring_head] = s_rx_byte;
        s_ring_head = next;
    } else {
        s_rx_ring_overflow++; /* drop the new byte (reported in INFO) */
    }
    (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != UART4)
        return;
    s_uart_err++; /* overrun / framing / noise / parity: counted, keep listening (HeadLink_Poll re-arms if this fails) */
    (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);
}

/*
 * Swap UART4's TX and RX and set the pads to match (user code; the generated init is left alone). Call right after
 * MX_UART4_Init(): until then PC10 drives the ST-LINK's TX output. Safe to call more than once.
 * HAL_UART_Init() is re-run with the SWAP advanced feature; because gState != RESET it skips HAL_UART_MspInit and keeps the
 * FIFO mode chosen by MX_UART4_Init().
 */
void HeadLink_PinSwap(void)
{
#if HEAD_UART4_SWAP_TXRX
    if (s_swapped)
        return;
    s_swapped = true;

    GPIO_InitTypeDef g = { 0 };
    g.Alternate = GPIO_AF8_UART4;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    /* PC10 will receive. Open drain + pull-up first, so its output stage stops fighting the ST-LINK's TX line at once. */
    g.Pin = GPIO_PIN_10;
    g.Mode = GPIO_MODE_AF_OD;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &g);

    huart4.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT;
    huart4.AdvancedInit.Swap = UART_ADVFEATURE_SWAP_ENABLE;
    if (HAL_UART_Init(&huart4) != HAL_OK)
        Error_Handler();

    /* PC11 now transmits. */
    g.Pin = GPIO_PIN_11;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &g);
#endif
}

/* Blocking transmit with a timeout that scales with the size (a frame is ~23 ms at 115200 baud). Failures are counted. */
static bool uart_send(const uint8_t *data, size_t n)
{
    if (n == 0)
        return true;
    if (HAL_UART_Transmit(&huart4, (uint8_t *)data, (uint16_t)n, HEAD_UART_TX_TIMEOUT_BASE_MS + (uint32_t)n / HEAD_UART_TX_BYTES_PER_MS) != HAL_OK) {
        s_uart_tx_fail++;
        return false;
    }
    return true;
}

void HeadLink_SendText(const char *text)
{
    (void)uart_send((const uint8_t *)text, strlen(text));
}

static void send_msg(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n)
{
    uint8_t out[HFRAME_MAX_ENCODED];
    size_t w = Hframe_Encode(type, seq, payload, n, out, sizeof out);
    if (w)
        (void)uart_send(out, w);
}

static void send_error(uint8_t req_type, uint8_t seq, uint8_t code)
{
    const uint8_t p[2] = { req_type, code };
    send_msg(HMSG_ERROR, seq, p, sizeof p);
}

/* ---- CM4 mailbox --------------------------------------------------------------------------------------------- */

/* Ready = the CM4 has published its magic AND its heartbeat moved since this core started: a stale region left by the
   previous boot (SRAM4 survives a reset) has a frozen heartbeat. Never clears the magic (the CM4 owns it). */
static bool shared_ready(void)
{
    if (SH->magic != HEAD_SHARED_MAGIC)
        return false;
    if (!s_hb_changed) {
        if (SH->cm4_heartbeat == s_hb_first)
            return false;
        s_hb_changed = true;
    }
    return true;
}

/* A request may be issued only when the previous one was acknowledged (also after a timeout: it stays outstanding). */
static bool mailbox_idle(void)
{
    return SH->cmd_req == SH->cmd_ack;
}

/* One request/response with the CM4. Returns HERR_BUSY if the CM4 is not ready or a request is still outstanding (the stage is
   then also off limits), HERR_TIMEOUT if the CM4 did not answer in time (the request stays outstanding). */
static uint8_t cm4_command(uint32_t cmd, uint32_t arg, uint32_t len, uint32_t crc)
{
    if (!shared_ready() || !mailbox_idle())
        return HERR_BUSY;
    if (++s_cmd_seq == 0)
        s_cmd_seq = 1;
    const uint32_t seq = s_cmd_seq;
    SH->cmd = cmd;
    SH->cmd_arg = arg;
    SH->cmd_len = len;
    SH->cmd_crc = crc;
    SH->cmd_seq = seq;
    const uint32_t req = SH->cmd_req + 1u;
    HEAD_MEM_BARRIER();
    SH->cmd_req = req;
    const uint32_t t0 = HAL_GetTick();
    while (SH->cmd_ack != req) {
        if (HAL_GetTick() - t0 > HEAD_CM4_CMD_TIMEOUT_MS)
            return HERR_TIMEOUT;
    }
    HEAD_MEM_BARRIER();
    if (SH->cmd_result_seq != seq)
        return HERR_BUSY; /* an answer to some other request: never trust it */
    return (uint8_t)SH->cmd_result;
}

/* ---- consistent reads of the CM4's published state (seqlock, head_shared.h) --------------------------------- */

typedef struct {
    uint64_t hash;
    uint8_t source;
    uint16_t channels;
} ActiveInfo_t;

static ActiveInfo_t s_active_cache; /* last consistent snapshot, used if the CM4 is busy swapping when INFO is asked */

static bool read_active_info(ActiveInfo_t *a)
{
    for (uint32_t t = 0; t < HEAD_SEQ_RETRIES; t++) {
        const uint32_t seq = HeadShared_SeqBegin(SH);
        if (!(seq & 1u)) {
            a->hash = (uint64_t)SH->active_hash_lo | ((uint64_t)SH->active_hash_hi << 32);
            a->source = SH->active_source;
            a->channels = HeadShared_ChannelCount(SH);
            if (HeadShared_SeqValid(SH, seq))
                return true;
        }
        HAL_Delay(1);
    }
    return false;
}

/* ---- protocol operations ------------------------------------------------------------------------------------- */

static void op_info(void *ctx, HprotoInfo_t *i)
{
    (void)ctx;
    uint32_t now = HAL_GetTick();
    uint32_t hb = SH->cm4_heartbeat;
    if (hb != s_hb_last) {
        s_hb_last = hb;
        s_hb_seen_ms = now;
    }
    i->fw_version = HEAD_FW_VERSION;
    i->uptime_ms = now;
    const bool ready = shared_ready();
    i->cm4_alive = ready && (now - s_hb_seen_ms) < HEAD_CM4_ALIVE_MS;
    if (ready) {
        ActiveInfo_t a;
        if (read_active_info(&a))
            s_active_cache = a; /* else: the previous consistent snapshot (a swap is in progress) */
        i->active_hash = s_active_cache.hash;
        i->active_source = s_active_cache.source;
        i->channel_count = s_active_cache.channels;
        i->fdcan_kernel_hz = SH->fdcan_kernel_hz;
        i->cm4_status = (uint8_t)SH->cm4_status;
        i->boot_apply_rc = (uint8_t)SH->boot_apply_rc;
        i->boot_apply_tries = SH->boot_apply_tries > 0xFFFFu ? 0xFFFFu : (uint16_t)SH->boot_apply_tries;
    }
    uint64_t stored = 0;
    i->stored_hash = HeadStore_Valid(NULL, &stored) ? stored : 0; /* cached: refreshed after every write/erase */
    uint32_t uid[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
    memcpy(i->uid, uid, 12);
    i->hframe_crc_fail = s_frx.crc_fail;
    i->hframe_overflow = s_frx.overflow;
    i->hframe_desync = s_frx.desync;
    i->rx_ring_overflow = s_rx_ring_overflow;
    i->uart_err = s_uart_err;
    i->uart_tx_fail = s_uart_tx_fail;
    i->tcan_init_status = tcan_init_status;
}

/*
 * COMMIT policy: validate, then apply (CM4), then store. If the store fails AFTER a successful apply the reply is a success
 * with applied = 1, stored = 0 and store_err = the failure. The CM7 never writes active_source: APPLY_STAGED carries the
 * source (2 = uploaded) and, after a successful store, HEAD_CMD_SET_SOURCE(1) tells the CM4 the active config is the stored one.
 * The store stalls this core for ~30 ms, or 1 s+ when the sector is full and must be erased first (see head_store.c).
 */
static uint8_t op_cfg_commit(void *ctx, const uint8_t *blob, uint32_t len, uint8_t flags, uint64_t *hash, uint8_t *applied, uint8_t *stored,
                             uint8_t *store_err)
{
    (void)ctx;
    if (HeadCfg_Parse(blob, len, &s_scratch) != HEAD_CFG_OK)
        return HERR_INVALID_CONFIG;
    *hash = s_scratch.config_hash;
    if (!shared_ready())
        return HERR_BUSY; /* the CM4 may still be reading the config sector during its boot apply: no flash writes either */
    uint8_t rc;
    if (flags & HPROTO_COMMIT_APPLY) { /* blob is already in shared->stage, where the CM4 reads it */
        rc = cm4_command(HEAD_CMD_APPLY_STAGED, 2, len, HeadLe32(blob + 28));
        if (rc != HERR_NONE)
            return rc;
        *applied = 1;
    }
    if (flags & HPROTO_COMMIT_STORE) {
        rc = HeadStore_Write(blob, len);
        if (rc != HERR_NONE) {
            if (!*applied)
                return rc;
            *store_err = rc; /* applied but not stored: success with a warning (see hproto.h) */
            return HERR_NONE;
        }
        *stored = 1;
        if (*applied)
            (void)cm4_command(HEAD_CMD_SET_SOURCE, 1, 0, 0); /* if this fails INFO keeps saying "uploaded", which is safe */
    }
    return HERR_NONE;
}

/* CFG_READ across several messages: remember the hash seen by the first chunk of each source and refuse later chunks of a
   different config (HERR_BUSY: the config changed while it was being read, start again at offset 0). */
static uint64_t s_read_hash[2];
static bool s_read_have[2];

static uint8_t read_consistent(uint8_t source, uint64_t hash, uint32_t offset)
{
    if (offset == 0 || !s_read_have[source]) {
        s_read_hash[source] = hash;
        s_read_have[source] = true;
        return HERR_NONE;
    }
    return s_read_hash[source] == hash ? HERR_NONE : HERR_BUSY;
}

static uint8_t op_cfg_read(void *ctx, uint8_t source, uint32_t offset, uint8_t *dst, uint16_t max, uint32_t *total, uint16_t *n)
{
    (void)ctx;
    if (source == 0) {
        if (!shared_ready())
            return HERR_UNSUPPORTED;
        for (uint32_t t = 0; t < HEAD_SEQ_RETRIES; t++) {
            const uint32_t seq = HeadShared_SeqBegin(SH);
            if (!(seq & 1u)) {
                const uint32_t len = HeadShared_ActiveLen(SH);
                const uint64_t hash = (uint64_t)SH->active_hash_lo | ((uint64_t)SH->active_hash_hi << 32);
                uint32_t cnt = 0;
                if (len != 0 && offset <= len) {
                    cnt = (len - offset) < max ? (len - offset) : max;
                    memcpy(dst, SH->active_blob + offset, cnt);
                }
                if (HeadShared_SeqValid(SH, seq)) { /* copy and header fields belong to one config */
                    if (len == 0)
                        return HERR_UNSUPPORTED;
                    if (offset > len)
                        return HERR_RANGE;
                    uint8_t rc = read_consistent(0, hash, offset);
                    if (rc != HERR_NONE)
                        return rc;
                    *total = len;
                    *n = (uint16_t)cnt;
                    return HERR_NONE;
                }
            }
            HAL_Delay(1);
        }
        return HERR_BUSY; /* the CM4 is swapping the config: try again */
    } else if (source == 1) {
        const uint8_t *src;
        uint32_t len;
        uint64_t hash;
        if (!HeadStore_Newest(&src, &len, &hash))
            return HERR_RANGE;
        if (offset > len)
            return HERR_RANGE;
        uint8_t rc = read_consistent(1, hash, offset);
        if (rc != HERR_NONE)
            return rc;
        *total = len;
        *n = (uint16_t)((len - offset) < max ? (len - offset) : max);
        memcpy(dst, src + offset, *n);
        return HERR_NONE;
    }
    return HERR_BAD_REQUEST;
}

/*
 * CLEAR erases the whole config sector. Without REVERT the active config keeps running; if it was marked as coming from flash
 * the CM4 is told it is now RAM only (SET_SOURCE 2) so INFO does not claim "flash" for a blank sector. With REVERT the CM4
 * applies the compiled-in default (which records source 0). The mailbox is checked BEFORE erasing.
 */
static uint8_t op_cfg_clear(void *ctx, uint8_t flags)
{
    (void)ctx;
    if (flags & (uint8_t)~HPROTO_CLEAR_REVERT)
        return HERR_BAD_REQUEST;
    if (!shared_ready() || !mailbox_idle())
        return HERR_BUSY;
    const bool revert = (flags & HPROTO_CLEAR_REVERT) != 0;
    const bool was_flash = SH->active_source == 1;
    uint8_t rc = HeadStore_Erase();
    if (rc != HERR_NONE)
        return rc;
    if (revert)
        return cm4_command(HEAD_CMD_APPLY_DEFAULT, 0, 0, 0);
    return was_flash ? cm4_command(HEAD_CMD_SET_SOURCE, 2, 0, 0) : HERR_NONE;
}

static uint16_t op_live_get(void *ctx, uint16_t first, uint16_t count, uint8_t *dst)
{
    (void)ctx;
    if (!shared_ready())
        return 0;
    const uint32_t channels = HeadShared_ChannelCount(SH);
    uint16_t w = 0;
    for (; w < count && (uint32_t)first + w < channels; w++) {
        uint32_t slot = (uint32_t)first + w;
        HeadPutLe32(dst + 5u * w, SH->live.values[slot].u);
        dst[5u * w + 4u] = SH->live.stale[slot] ? 1u : 0u;
    }
    return w;
}

static uint16_t op_stats_get(void *ctx, uint8_t *dst, uint16_t cap)
{
    (void)ctx;
    if (!shared_ready())
        return 0;
    uint16_t w = 0;
    for (uint32_t t = 0; t < HEAD_SEQ_RETRIES; t++) { /* a config swap resets the counters: encode inside one seqlock window */
        const uint32_t seq = HeadShared_SeqBegin(SH);
        w = Hproto_EncodeStats((const HeadStats_t *)&SH->stats, HeadShared_BusCount(SH), dst, cap); /* clamps the bus count */
        if (HeadShared_SeqValid(SH, seq))
            break;
    }
    return w; /* counters are statistics: if the swap never settled the last encoding is still safe (bounds were clamped) */
}

static uint8_t op_sniff_set(void *ctx, uint8_t bus, uint8_t enable)
{
    (void)ctx;
    if (!shared_ready() || bus >= HeadShared_BusCount(SH))
        return HERR_RANGE;
    if (enable)
        SH->sniff_mask |= (1u << bus);
    else
        SH->sniff_mask &= ~(1u << bus);
    return HERR_NONE;
}

static void op_reboot(void *ctx)
{
    (void)ctx; /* the reset itself happens after the reply has been sent, in on_msg(); the magic in the request is checked in hproto.c.
                  There is no mailbox "quiesce" command: a CM4 mid-apply or mid-transmit is cut by the system reset. */
}

/* Forward a hub protocol message over the hub bus (the CM4 owns the CAN peripheral). The CM4 answers within HRELAY_TIMEOUT_MS
   (1 s), which is shorter than HEAD_CM4_CMD_TIMEOUT_MS, so a missing hub is reported as HERR_TIMEOUT by the CM4 itself. */
static uint8_t op_hub_relay(void *ctx, uint8_t hub, uint16_t timeout_ms, const uint8_t *req, uint16_t req_len, uint8_t *rsp, uint16_t cap,
                            uint16_t *rsp_len)
{
    (void)ctx;
    if (!shared_ready() || !mailbox_idle())
        return HERR_BUSY;
    if (hub > 7u || req_len < 2u || req_len > HEAD_RELAY_BUF)
        return HERR_RANGE;
    memcpy(SH->relay_buf, req, req_len);
    const uint8_t rc = cm4_command(HEAD_CMD_HUB_RELAY, hub, req_len, timeout_ms); /* cmd_crc carries the timeout */
    if (rc != HERR_NONE)
        return rc;
    const uint32_t n = SH->relay_len;
    if (n < 2u || n > HEAD_RELAY_BUF || n > cap)
        return HERR_RANGE;
    memcpy(rsp, SH->relay_buf, n);
    *rsp_len = (uint16_t)n;
    return HERR_NONE;
}

static const HprotoOps_t s_ops = { op_info, op_cfg_commit, op_cfg_read, op_cfg_clear, op_live_get, op_stats_get, op_sniff_set, op_hub_relay, op_reboot };

/* ---- framing callbacks --------------------------------------------------------------------------------------- */

static void on_msg(void *ctx, const uint8_t *msg, size_t len)
{
    (void)ctx;
    uint8_t resp[HFRAME_MAX_BODY];
    /* The upload stage belongs to the CM7 only while the CM4 is ready and no request (which may read the stage) is outstanding. */
    if (len >= 2 && (msg[0] == HMSG_CFG_BEGIN || msg[0] == HMSG_CFG_CHUNK) && (!shared_ready() || !mailbox_idle())) {
        send_error(msg[0], msg[1], HERR_BUSY);
        return;
    }
    size_t n = Hproto_Handle(&s_proto, msg, len, resp, sizeof resp);
    if (n)
        send_msg(resp[0], resp[1], resp + 2, n - 2);
    if (Hproto_TakeReboot(&s_proto)) {
        HAL_Delay(5);
        HAL_NVIC_SystemReset();
    }
}

static void on_line(void *ctx, const char *line)
{
    (void)ctx;
    HeadDebug_HandleLine(line);
}

/* ---- public -------------------------------------------------------------------------------------------------- */

void HeadLink_Init(void)
{
    Hproto_Init(&s_proto, &s_ops, NULL, SH->stage, HEAD_MAX_BLOB);
    HframeRx_Init(&s_frx, on_msg, on_line, NULL);
    s_hb_first = SH->cm4_heartbeat;
    s_hb_last = s_hb_first;
    s_hb_seen_ms = HAL_GetTick();
    HeadStore_Refresh();
    HeadLink_PinSwap(); /* normally already done by main.c right after MX_UART4_Init() */
    s_last_rx_ms = HAL_GetTick();
    HAL_NVIC_SetPriority(UART4_IRQn, HEAD_UART4_NVIC_PRIO, 0);
    HAL_NVIC_EnableIRQ(UART4_IRQn);
    (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);
}

void HeadLink_Poll(void)
{
    bool got = false;
    while (s_ring_tail != s_ring_head) {
        uint8_t b = s_ring[s_ring_tail];
        s_ring_tail = (uint16_t)((s_ring_tail + 1u) % RX_RING_SIZE);
        HframeRx_Feed(&s_frx, b);
        got = true;
    }
    const uint32_t now = HAL_GetTick();
    if (got)
        s_last_rx_ms = now;
    else if ((s_frx.mode == 3u || (s_frx.mode == 2u && s_frx.n > 0u)) && (now - s_last_rx_ms) >= HEAD_RX_IDLE_RESET_MS)
        HframeRx_Reset(&s_frx); /* a binary frame went silent half way: do not wait for it forever */

    if (huart4.RxState == HAL_UART_STATE_READY) /* receiver stopped (an error path could not re-arm it): restart it */
        (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);

    /* stream one sniffed CAN frame (SNIFF_FRAME, unsolicited) per pass so a slow link cannot stall the loop */
    if (shared_ready() && SH->sniff_tail != SH->sniff_head) {
        uint32_t tail = SH->sniff_tail;
        const uint32_t head = SH->sniff_head;
        HEAD_MEM_BARRIER();
        if (head - tail > HEAD_SNIFF_RING) /* corrupt indices: resynchronise instead of reading outside the ring */
            tail = head;
        else {
            const HeadSniffEntry_t *e = &SH->sniff[tail % HEAD_SNIFF_RING];
            uint8_t p[11 + sizeof e->data];
            const uint8_t elen = HeadShared_SniffLen(e);
            p[0] = e->bus;
            p[1] = e->flags;
            HeadPutLe32(p + 2, e->id);
            HeadPutLe32(p + 6, e->ts_ms);
            p[10] = elen;
            memcpy(p + 11, e->data, elen);
            send_msg(HMSG_SNIFF_FRAME, 0, p, 11u + elen);
            tail++;
        }
        HEAD_MEM_BARRIER(); /* entry reads complete before the slot is handed back to the CM4 */
        SH->sniff_tail = tail;
    }
}
