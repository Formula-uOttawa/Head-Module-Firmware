/*
 * head_cm4.c - runtime-configurable CAN receive for the head module (CM4).
 *
 * The configuration (buses, bit rates, filters, hub/custom frame decoding) is a binary blob parsed into RAM tables
 * (head_cfg.c). It comes from the flash config sector at boot, the compiled-in default as a fallback, or from the CM7 at
 * runtime through the shared-memory mailbox (head_shared.h). The FDCAN peripherals are re-initialised in user code from
 * the config, so no CubeMX settings other than pins/clocks/NVIC matter for bus setup.
 */
#include "head_cm4.h"

#include <string.h>

#include "hcan_timing.h"
#include "head_cfg.h"
#include "head_decode.h"
#include "head_encode.h"
#include "head_shared.h"
#include "hproto.h" /* HERR_* codes */

extern FDCAN_HandleTypeDef hfdcan1, hfdcan2;
extern const uint8_t HEAD_DEFAULT_BLOB[];
extern const uint32_t HEAD_DEFAULT_BLOB_LEN;

#define SH HEAD_SHARED
#define NO_BUS 0xFFu

static HeadConfig_t s_cfg[2];
static HeadConfig_t *volatile g_cfg = &s_cfg[0]; /* read by the RX interrupt, swapped with interrupts disabled */
static uint8_t s_bus_of[4] = { NO_BUS, NO_BUS, NO_BUS, NO_BUS }; /* peripheral number (1..3) -> bus index */
static HeadTxState_t s_tx;
static uint32_t s_sniff_applied;
static uint32_t s_last_slow_ms, s_last_stale_ms;

static FDCAN_HandleTypeDef *periph_handle(uint8_t p)
{
    return p == 1 ? &hfdcan1 : p == 2 ? &hfdcan2 : NULL;
}

static uint8_t dlc_to_len(uint32_t dl)
{
    static const uint8_t t[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    return t[(dl >> 16) & 0xF];
}

static uint32_t len_to_dlc(uint8_t len)
{
    static const uint8_t t[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    for (uint32_t i = 0; i < 16; i++)
        if (t[i] >= len)
            return i << 16; /* HAL FDCAN_DLC_BYTES_x constants are code << 16 */
    return 15u << 16;
}

/* HeadTxSendFn: queue one frame on a configured bus; false if the bus is stopped or its TX FIFO is full */
static bool tx_send(void *ctx, uint8_t bus, uint32_t can_id, bool extended, const uint8_t *data, uint8_t len)
{
    const HeadConfig_t *c = ctx;
    FDCAN_HandleTypeDef *h = periph_handle(c->buses[bus].peripheral);
    if (h == NULL || s_bus_of[c->buses[bus].peripheral] != bus || HAL_FDCAN_GetTxFifoFreeLevel(h) == 0)
        return false;
    FDCAN_TxHeaderTypeDef hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.Identifier = can_id;
    hdr.IdType = extended ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
    hdr.TxFrameType = FDCAN_DATA_FRAME;
    hdr.DataLength = len_to_dlc(len);
    hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr.BitRateSwitch = c->buses[bus].fd ? FDCAN_BRS_ON : FDCAN_BRS_OFF;
    hdr.FDFormat = c->buses[bus].fd ? FDCAN_FD_CAN : FDCAN_CLASSIC_CAN;
    hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    return HAL_FDCAN_AddMessageToTxFifoQ(h, &hdr, (uint8_t *)data) == HAL_OK;
}

static void set_global_filter(FDCAN_HandleTypeDef *h, bool accept_all)
{
    uint32_t nm = accept_all ? FDCAN_ACCEPT_IN_RX_FIFO0 : FDCAN_REJECT;
    (void)HAL_FDCAN_ConfigGlobalFilter(h, nm, nm, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);
}

static void stop_all_buses(void)
{
    for (uint8_t p = 1; p <= 2; p++) {
        FDCAN_HandleTypeDef *h = periph_handle(p);
        (void)HAL_FDCAN_DeactivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST);
        (void)HAL_FDCAN_Stop(h);
        s_bus_of[p] = NO_BUS;
    }
}

/* Re-initialise every configured FDCAN from the config and start it. Returns an HERR_* code. */
static uint8_t can_configure(const HeadConfig_t *c)
{
    const uint32_t kernel_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN);

    for (uint32_t b = 0; b < c->bus_count; b++) {
        const HeadBus_t *bus = &c->buses[b];
        FDCAN_HandleTypeDef *h = periph_handle(bus->peripheral);
        HcanTiming_t nt, dt = { 0, 0, 0, 0 };
        uint32_t nstd = 0, next = 0;

        if (h == NULL)
            return HERR_UNSUPPORTED;
        if (!Hcan_ComputeTiming(kernel_hz, bus->bitrate, 800, false, &nt))
            return HERR_INVALID_CONFIG;
        if (bus->fd && !Hcan_ComputeTiming(kernel_hz, bus->data_bitrate, 750, true, &dt))
            return HERR_INVALID_CONFIG;
        for (uint32_t i = 0; i < c->filter_count; i++)
            if (c->filters[i].bus == b) {
                if (c->filters[i].extended)
                    next++;
                else
                    nstd++;
            }
        if (nstd > 128u || next > 64u)
            return HERR_TOO_BIG;

        (void)HAL_FDCAN_DeInit(h);
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
        set_global_filter(h, (SH->sniff_mask >> b) & 1u);
        if (bus->fd && bus->data_bitrate > 1000000u) { /* transmit delay compensation, needed for TX above 1 Mbit/s */
            (void)HAL_FDCAN_ConfigTxDelayCompensation(h, h->Init.DataPrescaler * h->Init.DataTimeSeg1, 0);
            (void)HAL_FDCAN_EnableTxDelayCompensation(h);
        }
        if (HAL_FDCAN_ActivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST, 0) != HAL_OK ||
            HAL_FDCAN_Start(h) != HAL_OK)
            return HERR_INVALID_CONFIG;
        s_bus_of[bus->peripheral] = (uint8_t)b;
    }
    s_sniff_applied = SH->sniff_mask;
    return HERR_NONE;
}

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

/* Parse, install and start a config. On failure the previous config keeps (or regains) control. */
static uint8_t apply_blob(const uint8_t *blob, uint32_t len, uint8_t source)
{
    HeadConfig_t *prev = g_cfg;
    HeadConfig_t *next = (prev == &s_cfg[0]) ? &s_cfg[1] : &s_cfg[0];
    HeadCfgStatus_t st = HeadCfg_Parse(blob, len, next);
    if (st != HEAD_CFG_OK)
        return HERR_INVALID_CONFIG;

    stop_all_buses();
    __disable_irq();
    g_cfg = next;
    HeadDecode_Reset(&SH->live, &SH->stats);
    HeadTx_Reset(&s_tx);
    __enable_irq();

    uint8_t rc = can_configure(next);
    if (rc != HERR_NONE) { /* roll back to the previous config */
        stop_all_buses();
        __disable_irq();
        g_cfg = prev;
        HeadDecode_Reset(&SH->live, &SH->stats);
        HeadTx_Reset(&s_tx);
        __enable_irq();
        if (prev->bus_count)
            (void)can_configure(prev);
        return rc;
    }
    publish_active(next, blob, len, source);
    return HERR_NONE;
}

static uint8_t apply_flash(void)
{
    const uint8_t *p = (const uint8_t *)HEAD_STORE_ADDR;
    uint32_t len = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    if (len < HEAD_BLOB_HEADER_LEN || len > HEAD_MAX_BLOB || HeadCfg_Verify(p, len) != HEAD_CFG_OK)
        return HERR_INVALID_CONFIG;
    return apply_blob(p, len, 1);
}

static uint8_t apply_default(void)
{
    return apply_blob(HEAD_DEFAULT_BLOB, HEAD_DEFAULT_BLOB_LEN, 0);
}

void HeadCm4_Init(void)
{
    /* SRAM4 has no clock gate on the dual-core H745 (no SRAM4EN bit in the device header), so it is usable as is */
    memset((void *)SH, 0, sizeof *SH);
    HeadDecode_Reset(&SH->live, &SH->stats);
    SH->fdcan_kernel_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN);

    /* stored config first, then the compiled-in default; if both fail the buses stay stopped (reported via INFO) */
    if (apply_flash() != HERR_NONE)
        (void)apply_default();

    __DMB();
    SH->magic = HEAD_SHARED_MAGIC;
}

void HeadCm4_OnRxFifo0(FDCAN_HandleTypeDef *h, uint32_t its)
{
    uint8_t per = (h->Instance == FDCAN1) ? 1u : (h->Instance == FDCAN2) ? 2u : 0u;
    uint8_t bus = s_bus_of[per];
    const HeadConfig_t *cfg = g_cfg;
    FDCAN_RxHeaderTypeDef hdr;
    uint8_t data[64];

    if (bus != NO_BUS && (its & FDCAN_IT_RX_FIFO0_MESSAGE_LOST))
        SH->stats.bus[bus].fifo_lost++;

    while (HAL_FDCAN_GetRxFifoFillLevel(h, FDCAN_RX_FIFO0) > 0) {
        if (HAL_FDCAN_GetRxMessage(h, FDCAN_RX_FIFO0, &hdr, data) != HAL_OK)
            break;
        if (bus == NO_BUS)
            continue;
        uint8_t len = dlc_to_len(hdr.DataLength);
        bool ext = hdr.IdType == FDCAN_EXTENDED_ID;
        uint32_t now = HAL_GetTick();

        (void)HeadDecode_Process(cfg, &SH->live, &SH->stats, bus, hdr.Identifier, ext, data, len, now);

        if ((SH->sniff_mask >> bus) & 1u) {
            uint32_t head = SH->sniff_head;
            if (head - SH->sniff_tail >= HEAD_SNIFF_RING) {
                SH->sniff_dropped++;
            } else {
                HeadSniffEntry_t *e = &SH->sniff[head % HEAD_SNIFF_RING];
                e->id = hdr.Identifier;
                e->ts_ms = now;
                e->bus = bus;
                e->flags = (uint8_t)((ext ? 1u : 0u) | (hdr.FDFormat == FDCAN_FD_CAN ? 2u : 0u));
                e->len = len;
                memcpy(e->data, data, len);
                __DMB();
                SH->sniff_head = head + 1;
            }
        }
    }
}

void HeadCm4_Poll(void)
{
    uint32_t now = HAL_GetTick();

    if (SH->magic != HEAD_SHARED_MAGIC)
        return;
    if (SH->cm4_heartbeat != now)
        SH->cm4_heartbeat = now;

    if (SH->cmd_req != SH->cmd_ack) { /* command from the CM7 */
        __DMB();
        uint8_t rc = HERR_BAD_REQUEST;
        switch (SH->cmd) {
        case HEAD_CMD_APPLY_STAGED: rc = apply_blob(SH->stage, SH->cmd_len, (uint8_t)SH->cmd_arg); break;
        case HEAD_CMD_APPLY_FLASH: rc = apply_flash(); break;
        case HEAD_CMD_APPLY_DEFAULT: rc = apply_default(); break;
        default: break;
        }
        SH->cmd_result = rc;
        __DMB();
        SH->cmd_ack = SH->cmd_req;
    }

    if (SH->sniff_mask != s_sniff_applied) { /* sniff mode opens the global filter so every frame is seen */
        const HeadConfig_t *c = g_cfg;
        for (uint32_t b = 0; b < c->bus_count; b++) {
            FDCAN_HandleTypeDef *h = periph_handle(c->buses[b].peripheral);
            if (h && s_bus_of[c->buses[b].peripheral] != NO_BUS)
                set_global_filter(h, (SH->sniff_mask >> b) & 1u);
        }
        s_sniff_applied = SH->sniff_mask;
    }

    if (g_cfg->tx_frame_count)
        HeadTx_Poll(g_cfg, &SH->live, &s_tx, &SH->stats, now, tx_send, (void *)g_cfg);

    if ((uint32_t)(now - s_last_stale_ms) >= 10u) {
        s_last_stale_ms = now;
        HeadDecode_UpdateStale(g_cfg, &SH->live, now);
    }
    if ((uint32_t)(now - s_last_slow_ms) >= 100u) {
        s_last_slow_ms = now;
        const HeadConfig_t *c = g_cfg;
        for (uint32_t b = 0; b < c->bus_count; b++) {
            FDCAN_HandleTypeDef *h = periph_handle(c->buses[b].peripheral);
            FDCAN_ProtocolStatusTypeDef ps;
            if (h && s_bus_of[c->buses[b].peripheral] != NO_BUS && HAL_FDCAN_GetProtocolStatus(h, &ps) == HAL_OK) {
                SH->stats.bus[b].bus_off = (uint8_t)(ps.BusOff != 0);
                SH->stats.bus[b].passive = (uint8_t)(ps.ErrorPassive != 0);
            }
        }
    }
}
