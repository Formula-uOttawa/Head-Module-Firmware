/*
 * head_cm4.c - runtime-configurable CAN receive for the head module (CM4).
 *
 * The configuration (buses, bit rates, filters, hub/custom frame decoding) is a binary blob parsed into RAM tables
 * (head_cfg.c). It comes from the flash config sector at boot (newest valid slot, head_slots.c), the compiled-in default
 * as a fallback, or from the CM7 at runtime through the shared-memory mailbox (head_shared.h). The FDCAN peripherals are
 * re-initialised in user code from the config, so no CubeMX settings other than pins/clocks/NVIC and the message RAM
 * element sizes matter for bus setup.
 *
 * Threading: the FDCAN interrupt only copies received frames into a small ring (head_rxq.h). Everything else - decoding,
 * statistics, sniff publishing, stale handling, the mailbox, config swaps and TX - runs in HeadCm4_Poll() on the main
 * loop, so the config tables and the live data have a single writer.
 */
#include "head_cm4.h"

#include <stddef.h>
#include <string.h>

#include "hcan_timing.h"
#include "head_cfg.h"
#include "head_decode.h"
#include "head_encode.h"
#include "head_rxq.h"
#include "head_shared.h"
#include "head_slots.h"
#include "head_util.h"
#include "hproto.h" /* HERR_* codes */

extern FDCAN_HandleTypeDef hfdcan1, hfdcan2;
extern const uint8_t HEAD_DEFAULT_BLOB[];
extern const uint32_t HEAD_DEFAULT_BLOB_LEN;

#define SH HEAD_SHARED
#define NO_BUS 0xFFu

#define NOMINAL_SAMPLE_PERMILLE 800u /* sample point of the arbitration phase */
#define DATA_SAMPLE_PERMILLE 750u    /* sample point of the data phase */
#define TDC_MIN_DATA_BITRATE 1000000u /* transmit delay compensation is needed above this data rate */
#define TDC_OFFSET_MAX 127u           /* TDCR.TDCO is 7 bits */
#define STALE_PERIOD_MS 10u
#define SLOW_PERIOD_MS 100u           /* bus status poll */
#define BOOT_RETRY_MS 100u            /* boot-time apply retry while the FDCAN kernel clock is not available */
#define SNIFF_RETRY_MS 100u           /* retry of a failed sniff toggle */
#define RX_ISR_MAX_FRAMES 32u         /* frames drained per interrupt (hardware FIFO holds 10) */
#define RX_POLL_MAX_FRAMES (2u * HEAD_RXQ_SIZE) /* frames decoded per HeadCm4_Poll() pass */

/* this HAL passes the DLC code (0..15) in FDCAN_{Tx,Rx}HeaderTypeDef.DataLength, not code << 16 */
_Static_assert(FDCAN_DLC_BYTES_8 == 8u && FDCAN_DLC_BYTES_64 == 15u, "HAL DataLength is expected to be the plain DLC code");

/* Config tables. Only the main loop reads or swaps g_cfg (the receive interrupt never touches it). */
static HeadConfig_t s_cfg[2];
static HeadConfig_t *g_cfg = &s_cfg[0];
static volatile uint8_t s_bus_of[HEAD_MAX_PERIPHERAL + 1u]; /* peripheral number (1..2) -> bus index; read by the RX interrupt */
static HeadRxQ_t s_rxq;
static HeadTxState_t s_tx;
static uint32_t s_sniff_applied, s_sniff_next_ms;
static uint32_t s_last_slow_ms, s_last_stale_ms;
static bool s_boot_pending;
static uint32_t s_boot_next_ms;

static FDCAN_HandleTypeDef *periph_handle(uint8_t p)
{
    return p == 1 ? &hfdcan1 : p == 2 ? &hfdcan2 : NULL;
}

/* payload bytes of a message RAM element size setting (FDCAN_DATA_BYTES_x) */
static uint32_t elmt_bytes(uint32_t code)
{
    switch (code) {
    case FDCAN_DATA_BYTES_8: return 8;
    case FDCAN_DATA_BYTES_12: return 12;
    case FDCAN_DATA_BYTES_16: return 16;
    case FDCAN_DATA_BYTES_20: return 20;
    case FDCAN_DATA_BYTES_24: return 24;
    case FDCAN_DATA_BYTES_32: return 32;
    case FDCAN_DATA_BYTES_48: return 48;
    case FDCAN_DATA_BYTES_64: return 64;
    default: return 0;
    }
}

/* HeadTxSendFn: queue one frame on a configured bus; false if the bus is stopped or its TX FIFO is full */
static bool tx_send(void *ctx, uint8_t bus, uint32_t can_id, bool extended, const uint8_t *data, uint8_t len)
{
    const HeadConfig_t *c = ctx;
    const HeadBus_t *hb = &c->buses[bus];
    FDCAN_HandleTypeDef *h = periph_handle(hb->peripheral);
    if (h == NULL || s_bus_of[hb->peripheral] != bus)
        return false;
    const uint32_t code = HeadDlc_LenToCode(len);
    if (HeadDlc_CodeToLen(code) > elmt_bytes(h->Init.TxElmtSize) || HAL_FDCAN_GetTxFifoFreeLevel(h) == 0)
        return false; /* never let the HAL copy more than one message RAM element holds */
    uint8_t buf[HEAD_MAX_FRAME_BYTES]; /* the HAL reads whole words up to the rounded length: send zeros, not stack garbage */
    memset(buf, 0, sizeof buf);
    memcpy(buf, data, len < sizeof buf ? len : sizeof buf);
    FDCAN_TxHeaderTypeDef hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.Identifier = can_id;
    hdr.IdType = extended ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
    hdr.TxFrameType = FDCAN_DATA_FRAME;
    hdr.DataLength = code;
    hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr.BitRateSwitch = hb->fd ? FDCAN_BRS_ON : FDCAN_BRS_OFF;
    hdr.FDFormat = hb->fd ? FDCAN_FD_CAN : FDCAN_CLASSIC_CAN;
    hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    return HAL_FDCAN_AddMessageToTxFifoQ(h, &hdr, buf) == HAL_OK;
}

/* Global filter for frames that match no configured filter: sniff mode accepts them, otherwise they are rejected.
   Only legal while the peripheral is stopped (HAL requires State == READY). */
static HAL_StatusTypeDef set_global_filter(FDCAN_HandleTypeDef *h, bool accept_all)
{
    uint32_t nm = accept_all ? FDCAN_ACCEPT_IN_RX_FIFO0 : FDCAN_REJECT;
    return HAL_FDCAN_ConfigGlobalFilter(h, nm, nm, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);
}

static HAL_StatusTypeDef arm_notifications(FDCAN_HandleTypeDef *h)
{
    return HAL_FDCAN_ActivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST, 0);
}

/* Stop both peripherals. A stop of a peripheral that is not running reports an error, which is not a problem here. */
static void stop_all_buses(void)
{
    for (uint8_t p = 1; p <= HEAD_MAX_PERIPHERAL; p++) {
        FDCAN_HandleTypeDef *h = periph_handle(p);
        (void)HAL_FDCAN_DeactivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST);
        (void)HAL_FDCAN_Stop(h);
        s_bus_of[p] = NO_BUS;
    }
}

static uint32_t refresh_kernel_hz(void)
{
    const uint32_t hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN);
    SH->fdcan_kernel_hz = hz;
    return hz;
}

static uint8_t bus_timing(const HeadBus_t *bus, uint32_t kernel_hz, HcanTiming_t *nt, HcanTiming_t *dt)
{
    memset(dt, 0, sizeof *dt);
    if (!Hcan_ComputeTiming(kernel_hz, bus->bitrate, NOMINAL_SAMPLE_PERMILLE, false, nt))
        return HERR_INVALID_CONFIG;
    if (bus->fd && !Hcan_ComputeTiming(kernel_hz, bus->data_bitrate, DATA_SAMPLE_PERMILLE, true, dt))
        return HERR_INVALID_CONFIG;
    return HERR_NONE;
}

/*
 * Everything that can be decided without touching the hardware: the clock is up, every bus has a peripheral, an exact bit
 * timing, message RAM elements big enough for what the bus carries, and a valid TDC offset. Run BEFORE stop_all_buses so a
 * config that cannot be applied never takes the running buses down. HERR_BUSY = FDCAN kernel clock not available (yet).
 */
static uint8_t can_validate(const HeadConfig_t *c, uint32_t kernel_hz)
{
    if (kernel_hz == 0)
        return HERR_BUSY;
    for (uint32_t b = 0; b < c->bus_count; b++) {
        const HeadBus_t *bus = &c->buses[b];
        FDCAN_HandleTypeDef *h = periph_handle(bus->peripheral);
        HcanTiming_t nt, dt;
        if (h == NULL)
            return HERR_UNSUPPORTED;
        uint8_t rc = bus_timing(bus, kernel_hz, &nt, &dt);
        if (rc != HERR_NONE)
            return rc;
        /* The element sizes are fixed by CubeMX (FDCAN1: 64 B, FDCAN2: 8 B). A bus that needs more would make the HAL copy
           beyond the element into its neighbours, so refuse it. RX: an FD bus can receive up to 64 B, a classic one 8 B. */
        uint32_t rx_need = bus->fd ? HEAD_MAX_FRAME_BYTES : HEAD_CLASSIC_FRAME_BYTES, tx_need = 0;
        for (uint32_t i = 0; i < c->tx_frame_count; i++)
            if (c->tx_frames[i].bus == b && c->tx_frames[i].dlc > tx_need)
                tx_need = c->tx_frames[i].dlc;
        if (rx_need > elmt_bytes(h->Init.RxFifo0ElmtSize) || tx_need > elmt_bytes(h->Init.TxElmtSize))
            return HERR_UNSUPPORTED;
        if (bus->fd && bus->data_bitrate > TDC_MIN_DATA_BITRATE && dt.prescaler * dt.seg1 > TDC_OFFSET_MAX)
            return HERR_INVALID_CONFIG;
    }
    return HERR_NONE;
}

/* Re-initialise every configured FDCAN from the config and start it (can_validate must have passed). Returns an HERR_* code. */
static uint8_t can_start(const HeadConfig_t *c, uint32_t kernel_hz)
{
    const uint32_t sniff = SH->sniff_mask; /* sampled once: s_sniff_applied must describe what was programmed */

    for (uint32_t b = 0; b < c->bus_count; b++) {
        const HeadBus_t *bus = &c->buses[b];
        FDCAN_HandleTypeDef *h = periph_handle(bus->peripheral);
        HcanTiming_t nt, dt;
        uint32_t nstd = 0, next = 0;

        if (h == NULL)
            return HERR_UNSUPPORTED;
        uint8_t rc = bus_timing(bus, kernel_hz, &nt, &dt);
        if (rc != HERR_NONE)
            return rc;
        for (uint32_t i = 0; i < c->filter_count; i++)
            if (c->filters[i].bus == b) {
                if (c->filters[i].extended)
                    next++;
                else
                    nstd++;
            }
        if (nstd > HEAD_MAX_STD_FILTERS_PER_BUS || next > HEAD_MAX_EXT_FILTERS_PER_BUS)
            return HERR_TOO_BIG;

        if (HAL_FDCAN_DeInit(h) != HAL_OK)
            return HERR_INVALID_CONFIG;
        h->Init.FrameFormat = bus->fd ? FDCAN_FRAME_FD_BRS : FDCAN_FRAME_CLASSIC;
        h->Init.NominalPrescaler = nt.prescaler;
        h->Init.NominalSyncJumpWidth = nt.sjw;
        h->Init.NominalTimeSeg1 = nt.seg1;
        h->Init.NominalTimeSeg2 = nt.seg2;
        if (bus->fd) {
            h->Init.DataPrescaler = dt.prescaler;
            h->Init.DataSyncJumpWidth = dt.sjw;
            h->Init.DataTimeSeg1 = dt.seg1;
            h->Init.DataTimeSeg2 = dt.seg2;
        }
        h->Init.StdFiltersNbr = nstd;
        h->Init.ExtFiltersNbr = next;
        if (HAL_FDCAN_Init(h) != HAL_OK)
            return HERR_INVALID_CONFIG;

        uint32_t si = 0, ei = 0;
        for (uint32_t i = 0; i < c->filter_count; i++) {
            const HeadFilter_t *f = &c->filters[i];
            if (f->bus != b)
                continue;
            FDCAN_FilterTypeDef flt;
            memset(&flt, 0, sizeof flt);
            flt.IdType = f->extended ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
            flt.FilterIndex = f->extended ? ei++ : si++;
            flt.FilterType = FDCAN_FILTER_MASK;
            flt.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
            flt.FilterID1 = f->id;
            flt.FilterID2 = f->mask;
            if (HAL_FDCAN_ConfigFilter(h, &flt) != HAL_OK)
                return HERR_INVALID_CONFIG;
        }
        if (set_global_filter(h, (sniff >> b) & 1u) != HAL_OK)
            return HERR_INVALID_CONFIG;
        if (bus->fd && bus->data_bitrate > TDC_MIN_DATA_BITRATE) { /* transmit delay compensation, needed for TX above 1 Mbit/s */
            if (HAL_FDCAN_ConfigTxDelayCompensation(h, h->Init.DataPrescaler * h->Init.DataTimeSeg1, 0) != HAL_OK ||
                HAL_FDCAN_EnableTxDelayCompensation(h) != HAL_OK)
                return HERR_INVALID_CONFIG;
        }
        if (arm_notifications(h) != HAL_OK || HAL_FDCAN_Start(h) != HAL_OK)
            return HERR_INVALID_CONFIG;
        s_bus_of[bus->peripheral] = (uint8_t)b;
    }
    s_sniff_applied = sniff;
    return HERR_NONE;
}

/* Make `cfg` the active tables with all buses stopped and no stale state left over. */
static void swap_config(HeadConfig_t *cfg)
{
    stop_all_buses();
    __disable_irq();
    HeadRxQ_Flush(&s_rxq); /* frames queued under the old config must not be decoded with the new one */
    s_rxq.overrun = 0;
    s_rxq.high = 0;
    __enable_irq();
    g_cfg = cfg;
    HeadDecode_Reset(&SH->live, &SH->stats);
    HeadTx_Reset(&s_tx);
}

/* Called with publish_seq odd: every field a CM7 reader samples changes inside one seqlock window. */
static void publish_active(const HeadConfig_t *c, const uint8_t *blob, uint32_t len, uint8_t source)
{
    memcpy(SH->active_blob, blob, len);
    SH->active_len = len;
    SH->active_hash_lo = (uint32_t)c->config_hash;
    SH->active_hash_hi = (uint32_t)(c->config_hash >> 32);
    SH->bus_count = (uint8_t)c->bus_count;
    SH->channel_count = c->channel_count;
    SH->active_source = source;
}

/*
 * Parse, validate, install and start a config. `verified`: the blob's CRC was just checked (HeadSlots_FindNewest), so the
 * CRC pass is skipped. Everything that can fail without hardware access (parse, bit timing, element sizes) is checked before
 * the buses are stopped. On a hardware failure the previous config is restored and restarted; if even that fails the buses
 * stay down and HEAD_CM4_ST_BUSES_DOWN is set.
 */
static uint8_t apply_blob(const uint8_t *blob, uint32_t len, uint8_t source, bool verified)
{
    HeadConfig_t *prev = g_cfg;
    HeadConfig_t *next = (prev == &s_cfg[0]) ? &s_cfg[1] : &s_cfg[0];
    const uint32_t kernel_hz = refresh_kernel_hz();
    HeadCfgStatus_t st = verified ? HeadCfg_ParseVerified(blob, len, next) : HeadCfg_Parse(blob, len, next);
    if (st != HEAD_CFG_OK)
        return HERR_INVALID_CONFIG;
    uint8_t rc = can_validate(next, kernel_hz);
    if (rc != HERR_NONE)
        return rc; /* nothing has been touched */

    HeadShared_PublishBegin(SH);
    swap_config(next);
    rc = can_start(next, kernel_hz);
    if (rc != HERR_NONE) { /* roll back to the previous config */
        swap_config(prev);
        if (prev->bus_count) {
            if (can_start(prev, kernel_hz) != HERR_NONE)
                SH->cm4_status |= HEAD_CM4_ST_BUSES_DOWN; /* SH->active_* still describes `prev`, but nothing is running */
            else
                SH->cm4_status &= ~HEAD_CM4_ST_BUSES_DOWN;
        }
        HeadShared_PublishEnd(SH);
        return rc;
    }
    publish_active(next, blob, len, source);
    SH->cm4_status &= ~(HEAD_CM4_ST_BUSES_DOWN | HEAD_CM4_ST_BOOT_PENDING);
    s_boot_pending = false;
    HeadShared_PublishEnd(SH);
    return HERR_NONE;
}

static uint8_t apply_flash(void)
{
    HeadSlotInfo_t info;
    if (!HeadSlots_FindNewest((const uint8_t *)HEAD_STORE_ADDR, &info))
        return HERR_INVALID_CONFIG; /* nothing valid stored */
    return apply_blob(info.blob, info.len, 1, true);
}

static uint8_t apply_default(void)
{
    return apply_blob(HEAD_DEFAULT_BLOB, HEAD_DEFAULT_BLOB_LEN, 0, false);
}

/* Stored config first, then the compiled-in default; if both fail the buses stay stopped (reported via INFO). While the failure
   is "FDCAN kernel clock not available" (the CM7 has not configured PLL2 yet) HeadCm4_Poll retries every BOOT_RETRY_MS. */
static uint8_t boot_apply(void)
{
    SH->boot_apply_tries++;
    uint8_t rc = apply_flash();
    if (rc != HERR_NONE)
        rc = apply_default();
    SH->boot_apply_rc = rc;
    s_boot_pending = (rc == HERR_BUSY);
    if (s_boot_pending)
        SH->cm4_status |= HEAD_CM4_ST_BOOT_PENDING;
    else
        SH->cm4_status &= ~HEAD_CM4_ST_BOOT_PENDING;
    return rc;
}

/* HEAD_CMD_SET_SOURCE: the CM7 reports where the active config now lives (after a store / clear) */
static uint8_t set_source(uint32_t source)
{
    if (source > 2u)
        return HERR_BAD_REQUEST;
    if (source == 1u) { /* never claim "flash" unless the newest stored blob really is the active one */
        HeadSlotInfo_t info;
        const uint64_t active = ((uint64_t)SH->active_hash_hi << 32) | SH->active_hash_lo;
        if (!HeadSlots_FindNewest((const uint8_t *)HEAD_STORE_ADDR, &info) || HeadCfg_BlobHash(info.blob, info.len) != active)
            return HERR_INVALID_CONFIG;
    }
    SH->active_source = (uint8_t)source;
    return HERR_NONE;
}

static uint8_t run_command(uint32_t cmd, uint32_t arg, uint32_t len, uint32_t crc)
{
    switch (cmd) {
    case HEAD_CMD_APPLY_STAGED:
        if (arg > 2u)
            return HERR_BAD_REQUEST;
        if (len < HEAD_BLOB_HEADER_LEN || len > HEAD_MAX_BLOB)
            return HERR_RANGE;
        if (HeadLe32(SH->stage + 28) != crc)
            return HERR_BUSY; /* the stage was modified since the command was issued: never apply a different blob */
        return apply_blob(SH->stage, len, (uint8_t)arg, false);
    case HEAD_CMD_APPLY_FLASH: return apply_flash();
    case HEAD_CMD_APPLY_DEFAULT: return apply_default();
    case HEAD_CMD_SET_SOURCE: return set_source(arg);
    default: return HERR_BAD_REQUEST;
    }
}

void HeadCm4_Init(void)
{
    /* SRAM4 survives a reset, so the previous boot's magic may still be there: invalidate it first, set it last. The stage
       (CM7-owned upload buffer) is not touched. SRAM4 has no clock gate on the dual-core H745 (no SRAM4EN bit). */
    SH->magic = 0;
    HEAD_MEM_BARRIER();
    memset((void *)&SH->cm4_heartbeat, 0, offsetof(HeadShared_t, stage) - offsetof(HeadShared_t, cm4_heartbeat));
    HeadRxQ_Init(&s_rxq);
    for (uint32_t i = 0; i < 2; i++)
        s_cfg[i].hub_bus = -1;
    for (uint8_t p = 0; p <= HEAD_MAX_PERIPHERAL; p++)
        s_bus_of[p] = NO_BUS;
    HeadDecode_Reset(&SH->live, &SH->stats);
    (void)refresh_kernel_hz();

    (void)boot_apply();

    HEAD_MEM_BARRIER();
    SH->magic = HEAD_SHARED_MAGIC;
}

/* Receive interrupt: copy frames into the ring and nothing else (bounded, no decoding). */
void HeadCm4_OnRxFifo0(FDCAN_HandleTypeDef *h, uint32_t its)
{
    uint8_t per = (h->Instance == FDCAN1) ? 1u : (h->Instance == FDCAN2) ? 2u : 0u;
    uint8_t bus = s_bus_of[per];
    FDCAN_RxHeaderTypeDef hdr;
    uint8_t data[HEAD_MAX_FRAME_BYTES];

    if (bus != NO_BUS && (its & FDCAN_IT_RX_FIFO0_MESSAGE_LOST))
        SH->stats.bus[bus].fifo_lost++; /* one per interrupt: an event, the real number of lost frames is not known */

    for (uint32_t n = 0; n < RX_ISR_MAX_FRAMES && HAL_FDCAN_GetRxFifoFillLevel(h, FDCAN_RX_FIFO0) > 0; n++) {
        if (HAL_FDCAN_GetRxMessage(h, FDCAN_RX_FIFO0, &hdr, data) != HAL_OK)
            break;
        if (bus == NO_BUS) { /* peripheral not part of the active config: drain and count */
            SH->stats.rx_nobus++;
            continue;
        }
        const bool fd = hdr.FDFormat == FDCAN_FD_CAN;
        uint8_t len = HeadDlc_CodeToLen(hdr.DataLength);
        if (!fd && len > HEAD_CLASSIC_FRAME_BYTES)
            len = HEAD_CLASSIC_FRAME_BYTES; /* classic CAN: DLC 9..15 all mean 8 bytes */
        const uint8_t flags = (uint8_t)((hdr.IdType == FDCAN_EXTENDED_ID ? HEAD_RXQ_FLAG_EXT : 0u) | (fd ? HEAD_RXQ_FLAG_FD : 0u));
        if (!HeadRxQ_Push(&s_rxq, bus, hdr.Identifier, flags, HAL_GetTick(), data, len))
            SH->stats.bus[bus].fifo_lost++; /* ring full: counted as a lost-frame event (and in rx_ring_overrun) */
    }
}

/* Decode and publish the queued frames (main loop). */
static void drain_rx(void)
{
    const HeadConfig_t *cfg = g_cfg;
    const HeadRxEntry_t *e;
    for (uint32_t n = 0; n < RX_POLL_MAX_FRAMES && (e = HeadRxQ_Peek(&s_rxq)) != NULL; n++) {
        const bool ext = (e->flags & HEAD_RXQ_FLAG_EXT) != 0;
        (void)HeadDecode_Process(cfg, &SH->live, &SH->stats, e->bus, e->id, ext, e->data, e->len, e->ts_ms);

        if (e->bus < 32u && ((SH->sniff_mask >> e->bus) & 1u)) {
            uint32_t head = SH->sniff_head;
            if (head - SH->sniff_tail >= HEAD_SNIFF_RING) {
                SH->sniff_dropped++;
            } else {
                HeadSniffEntry_t *s = &SH->sniff[head % HEAD_SNIFF_RING];
                s->id = e->id;
                s->ts_ms = e->ts_ms;
                s->bus = e->bus;
                s->flags = (uint8_t)((ext ? 1u : 0u) | ((e->flags & HEAD_RXQ_FLAG_FD) ? 2u : 0u));
                s->len = e->len;
                memcpy(s->data, e->data, e->len);
                HEAD_MEM_BARRIER();
                SH->sniff_head = head + 1;
            }
        }
        HeadRxQ_Pop(&s_rxq);
    }
    SH->stats.rx_ring_overrun = s_rxq.overrun;
    SH->stats.rx_ring_high = s_rxq.high;
}

/* Switch one running bus between "filters only" and "sniff everything": the global filter can only be written while stopped. */
static bool sniff_apply_bus(FDCAN_HandleTypeDef *h, bool on)
{
    bool ok = HAL_FDCAN_Stop(h) == HAL_OK;
    ok = ok && set_global_filter(h, on) == HAL_OK;
    ok = (HAL_FDCAN_Start(h) == HAL_OK) && ok; /* always try to bring the bus back */
    ok = (arm_notifications(h) == HAL_OK) && ok;
    return ok;
}

static void sniff_poll(uint32_t now)
{
    const HeadConfig_t *c = g_cfg;
    const uint32_t want = SH->sniff_mask;
    uint32_t relevant = c->bus_count >= 32u ? 0xFFFFFFFFu : ((1u << c->bus_count) - 1u);
    uint32_t diff = (want ^ s_sniff_applied) & relevant;
    if (diff == 0) {
        s_sniff_applied = (s_sniff_applied & relevant) | (want & ~relevant); /* bits of buses that do not exist need no hardware */
        return;
    }
    if ((int32_t)(now - s_sniff_next_ms) < 0)
        return;
    bool failed = false;
    for (uint32_t b = 0; b < c->bus_count; b++) {
        if (!((diff >> b) & 1u))
            continue;
        FDCAN_HandleTypeDef *h = periph_handle(c->buses[b].peripheral);
        bool ok = true;
        if (h && s_bus_of[c->buses[b].peripheral] != NO_BUS)
            ok = sniff_apply_bus(h, (want >> b) & 1u);
        if (ok)
            s_sniff_applied = (s_sniff_applied & ~(1u << b)) | (want & (1u << b)); /* only mark applied on success */
        else
            failed = true;
    }
    if (failed)
        s_sniff_next_ms = now + SNIFF_RETRY_MS;
}

void HeadCm4_Poll(void)
{
    if (SH->magic != HEAD_SHARED_MAGIC)
        return;
    uint32_t now = HAL_GetTick();
    if (SH->cm4_heartbeat != now)
        SH->cm4_heartbeat = now;

    drain_rx();

    const uint32_t req = SH->cmd_req; /* snapshot: a new request is only legal once this one is acknowledged */
    if (req != SH->cmd_ack) { /* command from the CM7 */
        HEAD_MEM_BARRIER();
        const uint32_t seq = SH->cmd_seq;
        uint8_t rc = run_command(SH->cmd, SH->cmd_arg, SH->cmd_len, SH->cmd_crc);
        SH->cmd_result = rc;
        SH->cmd_result_seq = seq;
        HEAD_MEM_BARRIER();
        SH->cmd_ack = req;
    }

    if (s_boot_pending && (int32_t)(now - s_boot_next_ms) >= 0) {
        s_boot_next_ms = now + BOOT_RETRY_MS;
        (void)boot_apply();
    }

    sniff_poll(now);

    if (g_cfg->tx_frame_count)
        HeadTx_Poll(g_cfg, &SH->live, &s_tx, &SH->stats, now, tx_send, (void *)g_cfg);

    now = HAL_GetTick(); /* fresh time for the stale check: frames decoded above are stamped before this */
    if ((uint32_t)(now - s_last_stale_ms) >= STALE_PERIOD_MS) {
        s_last_stale_ms = now;
        HeadDecode_UpdateStale(g_cfg, &SH->live, now);
    }
    if ((uint32_t)(now - s_last_slow_ms) >= SLOW_PERIOD_MS) {
        s_last_slow_ms = now;
        const HeadConfig_t *c = g_cfg;
        for (uint32_t b = 0; b < c->bus_count; b++) {
            FDCAN_HandleTypeDef *h = periph_handle(c->buses[b].peripheral);
            FDCAN_ProtocolStatusTypeDef ps;
            if (h && s_bus_of[c->buses[b].peripheral] != NO_BUS && HAL_FDCAN_GetProtocolStatus(h, &ps) == HAL_OK) {
                SH->stats.bus[b].bus_off = (uint8_t)(ps.BusOff != 0); /* reported only; there is no automatic bus-off recovery */
                SH->stats.bus[b].passive = (uint8_t)(ps.ErrorPassive != 0);
            }
        }
    }
}
