#include "head_relay.h"

#include <string.h>

#include "hproto.h" /* HERR_* */

void HeadRelay_Init(HeadRelay_t *r)
{
    memset(r, 0, sizeof *r);
    HclRx_Init(&r->rx);
}

uint8_t HeadRelay_Start(HeadRelay_t *r, uint8_t hub, const uint8_t *msg, uint16_t len, uint32_t now_ms, uint32_t timeout_ms)
{
    if (r->state != HRELAY_IDLE)
        return HERR_BUSY;
    if (hub > 7u || len < 2u || len > HRELAY_MAX_MSG)
        return HERR_RANGE;
    if (!HclTx_Begin(&r->tx, msg[0], msg[1], msg + 2, (size_t)len - 2u))
        return HERR_RANGE;
    HclRx_Reset(&r->rx);
    r->hub = hub;
    r->req_seq = msg[1];
    r->deadline_ms = now_ms + timeout_ms;
    r->result = HERR_NONE;
    r->rsp_len = 0;
    r->state = HRELAY_SENDING;
    r->requests++;
    return HERR_NONE;
}

bool HeadRelay_OnFrame(HeadRelay_t *r, uint32_t id, const uint8_t *data, uint8_t len)
{
    if (HCL_ID_TYPE(id) != HCL_TYPE_ACK || (r->state != HRELAY_WAITING && r->state != HRELAY_SENDING) || HCL_ID_HUB(id) != r->hub)
        return false;
    if (HclRx_Feed(&r->rx, HCL_ID_SEG(id), data, len) == 1) {
        const uint8_t *m = HclRx_Msg(&r->rx);
        if (m[1] == r->req_seq) { /* an answer to something else (an old request) is ignored */
            memcpy(r->rsp, m, r->rx.msg_len);
            r->rsp_len = r->rx.msg_len;
            r->result = HERR_NONE;
            r->state = HRELAY_DONE;
            r->replies++;
        }
    }
    return true;
}

bool HeadRelay_Poll(HeadRelay_t *r, uint32_t now_ms, HrelayTxFn tx, void *ctx)
{
    if (r->state == HRELAY_SENDING) {
        uint8_t data[HCL_FRAME_BYTES], len, seg;
        while (HclTx_Next(&r->tx, data, &len, &seg)) {
            if (!tx(ctx, HCL_ID(HCL_TYPE_CONFIG, r->hub, seg), data, len)) {
                HclTx_Unget(&r->tx);
                r->tx_stalls++;
                break;
            }
        }
        if (HclTx_Done(&r->tx))
            r->state = HRELAY_WAITING;
    }
    if ((r->state == HRELAY_SENDING || r->state == HRELAY_WAITING) && (int32_t)(now_ms - r->deadline_ms) >= 0) {
        r->result = HERR_TIMEOUT;
        r->state = HRELAY_DONE;
        r->timeouts++;
    }
    return r->state == HRELAY_DONE;
}

void HeadRelay_Take(HeadRelay_t *r)
{
    r->state = HRELAY_IDLE;
}
