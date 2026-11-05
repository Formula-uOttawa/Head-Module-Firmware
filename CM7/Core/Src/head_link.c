/*
 * head_link.c - UART4 (ST-LINK VCP) transport for the config/debug protocol (CM7).
 *
 * Receive is interrupt driven into a ring buffer, drained from the main loop into the framing state machine
 * (hframe.c), which hands binary messages to the transport independent dispatcher (hproto.c) and text lines to the
 * legacy debug commands. A UDP transport later only needs to call Hproto_Handle() with each datagram.
 *
 * UART4_IRQHandler is defined here because UART4's NVIC interrupt is not enabled in the .ioc. If it is ever enabled in
 * CubeMX (generating the handler in stm32h7xx_it.c), remove the handler below to avoid a duplicate symbol.
 */
#include "head_link.h"

#include <string.h>

#include "head_cfg.h"
#include "head_debug.h"
#include "head_shared.h"
#include "head_store.h"
#include "hframe.h"
#include "hproto.h"
#include "main.h"

extern UART_HandleTypeDef huart4;

/*
 * Board revision 1 has the ST-LINK VCP wired TX-to-TX / RX-to-RX (PC10 = UART4_TX goes to the ST-LINK's TX output).
 * Setting this to 1 swaps the UART's TX and RX in hardware (USART_CR2.SWAP): the UART transmits on PC11 and receives
 * on PC10, which matches the wiring. Set it to 0 on a board that has the VCP wired correctly.
 */
#ifndef HEAD_UART4_SWAP_TXRX
#define HEAD_UART4_SWAP_TXRX 1
#endif

#define HEAD_FW_VERSION 0x0100u /* major << 8 | minor */
#define RX_RING_SIZE 1024u
#define CM4_ALIVE_MS 500u
#define CM4_CMD_TIMEOUT_MS 1500u
#define SH HEAD_SHARED

static volatile uint8_t s_ring[RX_RING_SIZE];
static volatile uint16_t s_ring_head, s_ring_tail;
static uint8_t s_rx_byte;

static HeadConfig_t s_scratch; /* validation of uploaded blobs before they reach the CM4 or flash */
static Hproto_t s_proto;
static HframeRx_t s_frx;
static uint32_t s_hb_last, s_hb_seen_ms;

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
    if (next != s_ring_tail) { /* drop on overflow */
        s_ring[s_ring_head] = s_rx_byte;
        s_ring_head = next;
    }
    (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4)
        (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1); /* overrun/noise: keep listening */
}

/* Re-initialise UART4 with swapped TX/RX and set the pads to match (user code; the generated init is left alone). */
static void uart_apply_pin_swap(void)
{
#if HEAD_UART4_SWAP_TXRX
    huart4.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_SWAP_INIT;
    huart4.AdvancedInit.Swap = UART_ADVFEATURE_SWAP_ENABLE;
    if (HAL_UART_Init(&huart4) != HAL_OK) /* keeps the FIFO mode chosen by MX_UART4_Init() */
        Error_Handler();

    GPIO_InitTypeDef g = { 0 };
    g.Alternate = GPIO_AF8_UART4;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    /* PC10 now receives. Open drain + pull-up so its output stage can never fight the ST-LINK's TX line. */
    g.Pin = GPIO_PIN_10;
    g.Mode = GPIO_MODE_AF_OD;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &g);
    /* PC11 now transmits. */
    g.Pin = GPIO_PIN_11;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &g);
#endif
}

static void uart_send(const uint8_t *data, size_t n)
{
    (void)HAL_UART_Transmit(&huart4, (uint8_t *)data, (uint16_t)n, 250);
}

void HeadLink_SendText(const char *text)
{
    uart_send((const uint8_t *)text, strlen(text));
}

static void send_msg(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n)
{
    uint8_t out[HFRAME_MAX_ENCODED];
    size_t w = Hframe_Encode(type, seq, payload, n, out, sizeof out);
    if (w)
        uart_send(out, w);
}

/* ---- CM4 mailbox --------------------------------------------------------------------------------------------- */

static bool shared_ready(void)
{
    return SH->magic == HEAD_SHARED_MAGIC;
}

static uint8_t cm4_command(uint32_t cmd, uint32_t arg, uint32_t len)
{
    if (!shared_ready())
        return HERR_TIMEOUT;
    SH->cmd = cmd;
    SH->cmd_arg = arg;
    SH->cmd_len = len;
    uint32_t req = SH->cmd_req + 1u;
    __DMB();
    SH->cmd_req = req;
    uint32_t t0 = HAL_GetTick();
    while (SH->cmd_ack != req) {
        if (HAL_GetTick() - t0 > CM4_CMD_TIMEOUT_MS)
            return HERR_TIMEOUT;
    }
    __DMB();
    return (uint8_t)SH->cmd_result;
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
    i->cm4_alive = shared_ready() && (now - s_hb_seen_ms) < CM4_ALIVE_MS;
    if (shared_ready()) {
        i->active_hash = (uint64_t)SH->active_hash_lo | ((uint64_t)SH->active_hash_hi << 32);
        i->active_source = SH->active_source;
        i->channel_count = SH->channel_count;
        i->fdcan_kernel_hz = SH->fdcan_kernel_hz;
    }
    uint64_t stored = 0;
    i->stored_hash = HeadStore_Valid(NULL, &stored) ? stored : 0;
    uint32_t uid[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
    memcpy(i->uid, uid, 12);
}

static uint8_t op_cfg_commit(void *ctx, const uint8_t *blob, uint32_t len, uint8_t flags, uint64_t *hash, uint8_t *applied, uint8_t *stored)
{
    (void)ctx;
    if (HeadCfg_Parse(blob, len, &s_scratch) != HEAD_CFG_OK)
        return HERR_INVALID_CONFIG;
    *hash = s_scratch.config_hash;
    uint8_t rc;
    if (flags & HPROTO_COMMIT_APPLY) { /* blob is already in shared->stage, where the CM4 reads it */
        rc = cm4_command(HEAD_CMD_APPLY_STAGED, 2, len);
        if (rc != HERR_NONE)
            return rc;
        *applied = 1;
    }
    if (flags & HPROTO_COMMIT_STORE) {
        rc = HeadStore_Write(blob, len);
        if (rc != HERR_NONE)
            return rc;
        *stored = 1;
        if (*applied)
            SH->active_source = 1;
    }
    return HERR_NONE;
}

static uint8_t op_cfg_read(void *ctx, uint8_t source, uint32_t offset, uint8_t *dst, uint16_t max, uint32_t *total, uint16_t *n)
{
    (void)ctx;
    const uint8_t *src;
    uint32_t len;
    if (source == 0) {
        if (!shared_ready() || SH->active_len == 0)
            return HERR_UNSUPPORTED;
        src = SH->active_blob;
        len = SH->active_len;
    } else if (source == 1) {
        if (!HeadStore_Valid(&len, NULL))
            return HERR_RANGE;
        src = HeadStore_Data();
    } else {
        return HERR_BAD_REQUEST;
    }
    if (offset > len)
        return HERR_RANGE;
    *total = len;
    *n = (uint16_t)((len - offset) < max ? (len - offset) : max);
    memcpy(dst, src + offset, *n);
    return HERR_NONE;
}

static uint8_t op_cfg_clear(void *ctx, uint8_t flags)
{
    (void)ctx;
    uint8_t rc = HeadStore_Erase();
    if (rc != HERR_NONE)
        return rc;
    return (flags & HPROTO_CLEAR_REVERT) ? cm4_command(HEAD_CMD_APPLY_DEFAULT, 0, 0) : HERR_NONE;
}

static uint16_t op_live_get(void *ctx, uint16_t first, uint16_t count, uint8_t *dst)
{
    (void)ctx;
    if (!shared_ready())
        return 0;
    uint16_t w = 0;
    for (; w < count && (uint32_t)first + w < SH->channel_count; w++) {
        uint32_t slot = (uint32_t)first + w, v = SH->live.values[slot].u;
        for (int b = 0; b < 4; b++)
            dst[5 * w + b] = (uint8_t)(v >> (8 * b));
        dst[5 * w + 4] = SH->live.stale[slot] ? 1u : 0u;
    }
    return w;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint16_t op_stats_get(void *ctx, uint8_t *dst, uint16_t cap)
{
    (void)ctx;
    if (!shared_ready() || cap < 1u + 18u * HEAD_MAX_BUSES + 1u + 32u + 1u + 8u * HEAD_MAX_BUSES)
        return 0;
    uint16_t o = 0;
    uint8_t nb = SH->bus_count;
    dst[o++] = nb;
    for (uint8_t b = 0; b < nb; b++) {
        const HeadBusStats_t *s = &SH->stats.bus[b];
        put32(dst + o, s->rx_frames); put32(dst + o + 4, s->rx_matched); put32(dst + o + 8, s->rx_dropped); put32(dst + o + 12, s->fifo_lost);
        dst[o + 16] = s->bus_off;
        dst[o + 17] = s->passive;
        o += 18;
    }
    dst[o++] = 8;
    for (int h = 0; h < 8; h++) {
        put32(dst + o, SH->stats.hub_lost[h]);
        o += 4;
    }
    dst[o++] = nb; /* appended later than the rest: per-bus transmit counters (old hosts ignore trailing bytes) */
    for (uint8_t b = 0; b < nb; b++) {
        put32(dst + o, SH->stats.bus[b].tx_frames);
        put32(dst + o + 4, SH->stats.bus[b].tx_dropped);
        o += 8;
    }
    return o;
}

static uint8_t op_sniff_set(void *ctx, uint8_t bus, uint8_t enable)
{
    (void)ctx;
    if (!shared_ready() || bus >= SH->bus_count)
        return HERR_RANGE;
    if (enable)
        SH->sniff_mask |= (1u << bus);
    else
        SH->sniff_mask &= ~(1u << bus);
    return HERR_NONE;
}

static void op_reboot(void *ctx)
{
    (void)ctx; /* the reset itself happens after the reply has been sent, in on_msg() */
}

static const HprotoOps_t s_ops = { op_info, op_cfg_commit, op_cfg_read, op_cfg_clear, op_live_get, op_stats_get, op_sniff_set, op_reboot };

/* ---- framing callbacks --------------------------------------------------------------------------------------- */

static void on_msg(void *ctx, const uint8_t *msg, size_t len)
{
    (void)ctx;
    uint8_t resp[HFRAME_MAX_BODY];
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
    uart_apply_pin_swap();
    HAL_NVIC_SetPriority(UART4_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(UART4_IRQn);
    (void)HAL_UART_Receive_IT(&huart4, &s_rx_byte, 1);
}

void HeadLink_Poll(void)
{
    while (s_ring_tail != s_ring_head) {
        uint8_t b = s_ring[s_ring_tail];
        s_ring_tail = (uint16_t)((s_ring_tail + 1u) % RX_RING_SIZE);
        HframeRx_Feed(&s_frx, b);
    }

    /* stream sniffed CAN frames (SNIFF_FRAME, unsolicited), a couple per pass so the loop stays responsive */
    for (int k = 0; k < 2 && shared_ready() && SH->sniff_tail != SH->sniff_head; k++) {
        __DMB();
        const HeadSniffEntry_t *e = &SH->sniff[SH->sniff_tail % HEAD_SNIFF_RING];
        uint8_t p[11 + 64];
        p[0] = e->bus;
        p[1] = e->flags;
        put32(p + 2, e->id);
        put32(p + 6, e->ts_ms);
        p[10] = e->len;
        memcpy(p + 11, e->data, e->len);
        send_msg(HMSG_SNIFF_FRAME, 0, p, 11u + e->len);
        SH->sniff_tail = SH->sniff_tail + 1u;
    }
}
