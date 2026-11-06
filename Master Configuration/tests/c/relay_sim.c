/* Host simulation of the whole hub configuration chain, in one process:
 *   host UART (stdin/stdout, COBS frames)  ->  head: hproto.c HUB_RELAY + head_relay.c + hcanlink.c
 *   fake CAN-FD hub bus (frames queued in memory, optional loss)
 *   ->  hub: hub_canlink.c + hub_proto.c + hub_cfg.c + hub_slots.c + hub_store.c (real) on a RAM model of the flash log
 * Usage: relay_sim <flash file> ; the process re-executes itself on a hub reset ("power cycle", the flash file persists).
 * ASCII lines on the UART control the model: sim:absent:<0|1> sim:drop:<n> sim:dup:<0|1> sim:budget:<dwords> sim:filter sim:txfull:<0|1> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hcanlink.h"
#include "head_relay.h"
#include "hframe.h"
#include "hproto.h"
#include "hub_canlink.h"
#include "hub_cfg.h"
#include "hub_proto.h"
#include "hub_store.h"
#include "stm32g4xx_hal.h"

void HubFake_Init(const char *file);
void HubFake_SetBudget(int dwords);

GPIO_TypeDef stub_gpioa, stub_gpioc, stub_gpiod;
static char **g_argv;
static uint32_t g_tick = 1000;
static FDCAN_HandleTypeDef hfdcan1;

typedef struct { uint32_t id; uint8_t len; uint8_t data[64]; } Frame_t;
typedef struct { Frame_t f[64]; int n; } Queue_t;
static Queue_t to_hub, to_head;
static int g_absent, g_drop_n, g_dup, g_txfull;
static uint32_t g_filter_id, g_filter_mask;
static volatile int g_reboot_pending;

static const uint8_t DLC_LEN[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

static void out(const char *s) { ssize_t r = write(1, s, strlen(s)); (void)r; }
static void qpush(Queue_t *q, uint32_t id, const uint8_t *d, uint8_t len)
{
    if (q->n >= 64) return;
    q->f[q->n].id = id; q->f[q->n].len = len; memcpy(q->f[q->n].data, d, len); q->n++;
}
static Frame_t qpop(Queue_t *q)
{
    Frame_t f = q->f[0];
    memmove(&q->f[0], &q->f[1], (size_t)(q->n - 1) * sizeof(Frame_t));
    q->n--;
    return f;
}

/* ---- HAL stubs (hub side) ---------------------------------------------------------------------------------------------- */
uint32_t HAL_GetTick(void) { if (g_txfull) g_tick++; return g_tick; } /* time passes while the hub waits for a TX FIFO element */
void HAL_Delay(uint32_t ms) { g_tick += ms; }
static volatile int g_exec;
void HAL_NVIC_SystemReset(void) { g_exec = 1; } /* the "power cycle" happens in main() once the head has answered the host */
HAL_StatusTypeDef HAL_FDCAN_Init(FDCAN_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_FDCAN_ConfigFilter(FDCAN_HandleTypeDef *h, FDCAN_FilterTypeDef *f) { (void)h; g_filter_id = f->FilterID1; g_filter_mask = f->FilterID2; return HAL_OK; }
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef *h, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)h; (void)a; (void)b; (void)c; (void)d; return HAL_OK; }
HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef *h, uint32_t its, uint32_t buffers) { (void)h; (void)its; (void)buffers; return HAL_OK; }
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *h, uint32_t fifo) { (void)h; (void)fifo; return (uint32_t)(to_hub.n > 3 ? 3 : to_hub.n); }
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *h, uint32_t fifo, FDCAN_RxHeaderTypeDef *hdr, uint8_t *data)
{
    (void)h; (void)fifo;
    Frame_t f = qpop(&to_hub);
    memset(hdr, 0, sizeof *hdr);
    hdr->Identifier = f.id;
    for (unsigned c = 0; c < 16; c++) if (DLC_LEN[c] == f.len) hdr->DataLength = c; /* stub DLC define == code */
    memcpy(data, f.data, f.len);
    return HAL_OK;
}
uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h) { (void)h; return g_txfull ? 0u : 3u; }
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h, FDCAN_TxHeaderTypeDef *hdr, uint8_t *data)
{
    (void)h;
    qpush(&to_head, hdr->Identifier, data, DLC_LEN[hdr->DataLength & 15u]);
    return HAL_OK;
}

/* ---- hub side ops ------------------------------------------------------------------------------------------------------ */
bool HubLink_RebootPending(void) { return g_reboot_pending != 0; }
static void h_info(void *c, HubInfo_t *i)
{
    (void)c;
    const HubConfig_t *cfg = HubCfg_Active();
    const HubActiveInfo_t *ai = HubStore_Info();
    const HubCanLinkStats_t *cl = HubCanLink_GetStats();
    uint64_t stored = 0;
    i->fw_version = 0x0200;
    i->hub_id = cfg->hub_id;
    for (int k = 0; k < 12; k++) i->uid[k] = (uint8_t)(0xB0 + k);
    i->uptime_ms = g_tick;
    i->a_count = cfg->a_count; i->b_count = cfg->b_count; i->dig_count = cfg->dig_count;
    i->active_hash = ai->hash;
    i->stored_hash = HubStore_Stored(NULL, NULL, &stored) ? stored : 0;
    i->active_source = ai->source;
    i->cfg_status = (uint8_t)((ai->reject_status & 0xF) | (ai->default_status ? 0x80 : 0));
    i->canrx_frames = cl->frames; i->canrx_msgs = cl->msgs; i->canrx_bad = cl->bad; i->canrx_overrun = cl->overrun; i->cantx_fail = cl->tx_fail;
}
static uint16_t h_live(void *c, uint16_t first, uint16_t count, uint8_t *dst)
{
    (void)c;
    uint16_t w = 0;
    for (; w < count && first + w < HUB_LIVE_SLOTS; w++) {
        uint32_t v = 1000u + first + w;
        for (int k = 0; k < 4; k++) dst[5 * w + k] = (uint8_t)(v >> (8 * k));
        dst[5 * w + 4] = 0;
    }
    return w;
}
static void h_reboot(void *c) { (void)c; g_reboot_pending = 1; }
static uint8_t h_commit(void *c, const uint8_t *b, uint32_t n, uint64_t *hash) { (void)c; return HubStore_Commit(b, n, hash); }
static uint8_t h_read(void *c, uint8_t s, uint32_t o, uint8_t *d, uint16_t m, uint32_t *t, uint16_t *n) { (void)c; return HubStore_Read(s, o, d, m, t, n); }
static uint8_t h_clear(void *c) { (void)c; return HubStore_Clear(); }
static size_t h_ascii(void *c, const char *line, char *out, size_t cap)
{
    (void)c;
    if (strcmp(line, "reset") == 0) { g_reboot_pending = 1; } /* like the console: reset after the reply */
    return (size_t)snprintf(out, cap, "console:%s\r\n", line);
}
const HubProtoOps_t g_hub_proto_ops = { h_info, h_live, h_reboot, h_commit, h_read, h_clear, h_ascii };

/* ---- head side ----------------------------------------------------------------------------------------------------------- */
static HeadRelay_t g_relay;
static int g_sent_to_hub;

static bool head_tx(void *ctx, uint32_t id, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    static int budget_tick = -1, used;
    if ((int)g_tick != budget_tick) { budget_tick = (int)g_tick; used = 0; }
    if (used >= 3) return false; /* 3 frames per ms: the TX FIFO of the head, the relay must retry the rest */
    used++;
    g_sent_to_hub++;
    if (g_drop_n > 0 && --g_drop_n == 0) return true; /* lost on the bus */
    if (g_absent) return true;
    qpush(&to_hub, id, data, len);
    if (g_dup) qpush(&to_hub, id, data, len);
    return true;
}

static void pump(void)
{
    (void)HeadRelay_Poll(&g_relay, g_tick, head_tx, NULL);
    while (to_hub.n) HAL_FDCAN_RxFifo0Callback(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE);
    HubCanLink_Poll();
    while (to_head.n) { Frame_t f = qpop(&to_head); (void)HeadRelay_OnFrame(&g_relay, f.id, f.data, f.len); }
    g_tick++;
}

static uint8_t relay_op(void *c, uint8_t hub, uint16_t timeout_ms, const uint8_t *req, uint16_t rl, uint8_t *rsp, uint16_t cap, uint16_t *rsp_len)
{
    (void)c;
    if (timeout_ms == 0 || timeout_ms > HRELAY_TIMEOUT_MS) timeout_ms = HRELAY_TIMEOUT_MS;
    uint8_t rc = HeadRelay_Start(&g_relay, hub, req, rl, g_tick, timeout_ms);
    if (rc) return rc;
    for (int it = 0; it < 20000 && g_relay.state != HRELAY_DONE; it++) pump();
    rc = g_relay.result;
    if (rc == HERR_NONE && g_relay.rsp_len <= cap) { memcpy(rsp, g_relay.rsp, g_relay.rsp_len); *rsp_len = g_relay.rsp_len; }
    else if (rc == HERR_NONE) rc = HERR_RANGE;
    HeadRelay_Take(&g_relay);
    return rc;
}

static void get_info(void *c, HprotoInfo_t *i) { (void)c; i->fw_version = 0x0105; for (int k = 0; k < 12; k++) i->uid[k] = (uint8_t)(0x10 + k); }
static const HprotoOps_t ops = { get_info, NULL, NULL, NULL, NULL, NULL, NULL, relay_op, NULL };

static Hproto_t proto;
static uint8_t stage[256];
static HframeRx_t rx;

static void on_msg(void *c, const uint8_t *msg, size_t len)
{
    (void)c;
    uint8_t resp[HFRAME_MAX_BODY], enc[HFRAME_MAX_ENCODED];
    size_t n = Hproto_Handle(&proto, msg, len, resp, sizeof resp);
    if (!n) return;
    size_t w = Hframe_Encode(resp[0], resp[1], resp + 2, n - 2, enc, sizeof enc);
    if (w) { ssize_t r = write(1, enc, w); (void)r; }
}

static void on_line(void *c, const char *line)
{
    (void)c;
    char buf[160];
    int v = 0;
    if (sscanf(line, "sim:absent:%d", &v) == 1) g_absent = v;
    else if (sscanf(line, "sim:drop:%d", &v) == 1) g_drop_n = v;
    else if (sscanf(line, "sim:dup:%d", &v) == 1) g_dup = v;
    else if (sscanf(line, "sim:txfull:%d", &v) == 1) g_txfull = v;
    else if (sscanf(line, "sim:budget:%d", &v) == 1) HubFake_SetBudget(v);
    else if (strcmp(line, "sim:filter") == 0) {
        snprintf(buf, sizeof buf, "ascii:filter:%08X:%08X:%u:sent=%d\n", g_filter_id, g_filter_mask, hfdcan1.Init.ExtFiltersNbr, g_sent_to_hub);
        out(buf);
        return;
    }
    out("ascii:ok\n");
}

int main(int argc, char **argv)
{
    g_argv = argv;
    HubFake_Init(argc > 1 ? argv[1] : NULL);
    HubStore_Load();
    if (!HubCanLink_Init(&hfdcan1, HubCfg_Active()->hub_id)) return 2;
    HeadRelay_Init(&g_relay);
    Hproto_Init(&proto, &ops, NULL, stage, sizeof stage);
    HframeRx_Init(&rx, on_msg, on_line, NULL);
    uint8_t buf[256];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0)
    {
        for (ssize_t i = 0; i < n; i++) HframeRx_Feed(&rx, buf[i]);
        if (g_exec) { execv(g_argv[0], g_argv); _exit(3); } /* all RAM state is gone, the flash file persists */
    }
    return 0;
}
