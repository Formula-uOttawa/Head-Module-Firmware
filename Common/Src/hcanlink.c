#include "hcanlink.h"

#include <string.h>

uint8_t Hcl_PadLen(uint8_t n)
{
    static const uint8_t l[] = { 8, 12, 16, 20, 24, 32, 48, 64 };
    for (unsigned i = 0; i < sizeof l; i++)
        if (n <= l[i])
            return l[i];
    return 64;
}

bool HclTx_Begin(HclTx_t *t, uint8_t type, uint8_t seq, const uint8_t *payload, size_t n)
{
    if (n > HCL_MAX_BODY - 4u)
        return false;
    const uint16_t body = (uint16_t)(2u + n + 2u);
    t->buf[0] = (uint8_t)body;
    t->buf[1] = (uint8_t)(body >> 8);
    t->buf[2] = type;
    t->buf[3] = seq;
    if (n)
        memcpy(t->buf + 4, payload, n);
    const uint16_t crc = Hframe_Crc16(t->buf + 2, 2u + n);
    t->buf[4 + n] = (uint8_t)crc;
    t->buf[5 + n] = (uint8_t)(crc >> 8);
    t->total = (uint16_t)(2u + body);
    t->sent = 0;
    t->idx = 0;
    return true;
}

bool HclTx_Done(const HclTx_t *t)
{
    return t->sent >= t->total;
}

bool HclTx_Next(HclTx_t *t, uint8_t data[HCL_FRAME_BYTES], uint8_t *len, uint8_t *seg)
{
    if (HclTx_Done(t))
        return false;
    uint16_t n = (uint16_t)(t->total - t->sent);
    if (n > HCL_FRAME_BYTES)
        n = HCL_FRAME_BYTES;
    const uint8_t padded = Hcl_PadLen((uint8_t)n);
    memset(data, 0, padded);
    memcpy(data, t->buf + t->sent, n);
    t->sent = (uint16_t)(t->sent + n);
    *len = padded;
    *seg = (uint8_t)((t->idx & HCL_SEG_IDX_MASK) | (HclTx_Done(t) ? HCL_SEG_LAST : 0u));
    t->idx++;
    return true;
}

void HclTx_Unget(HclTx_t *t)
{
    /* every fragment but the last carries exactly HCL_FRAME_BYTES, so the position follows from the index */
    if (t->idx == 0)
        return;
    t->idx--;
    t->sent = (uint16_t)(t->idx * HCL_FRAME_BYTES);
}

void HclRx_Init(HclRx_t *r)
{
    memset(r, 0, sizeof *r);
}

void HclRx_Reset(HclRx_t *r)
{
    r->active = false;
    r->have = 0;
    r->want = 0;
    r->next_idx = 0;
}

int HclRx_Feed(HclRx_t *r, uint8_t seg, const uint8_t *data, uint8_t len)
{
    const uint8_t idx = seg & HCL_SEG_IDX_MASK;
    const bool last = (seg & HCL_SEG_LAST) != 0;

    if (idx == 0) {
        HclRx_Reset(r); /* a new message always starts at fragment 0 */
        if (len < 2) {
            r->len_err++;
            return -1;
        }
        const uint16_t body = (uint16_t)(data[0] | (data[1] << 8));
        if (body < 4u || body > HCL_MAX_BODY) {
            r->len_err++;
            return -1;
        }
        r->want = (uint16_t)(2u + body);
        r->active = true;
    } else if (!r->active || idx != r->next_idx) {
        r->seq_err++;
        HclRx_Reset(r);
        return -1;
    }
    if (len > HCL_FRAME_BYTES) {
        r->len_err++;
        HclRx_Reset(r);
        return -1;
    }
    uint16_t take = len;
    if (take > (uint16_t)(r->want - r->have))
        take = (uint16_t)(r->want - r->have); /* padding of the last fragment */
    memcpy(r->buf + r->have, data, take);
    r->have = (uint16_t)(r->have + take);
    r->next_idx = (uint8_t)(idx + 1u);

    const bool complete = r->have >= r->want;
    if (complete != last) { /* the end must coincide with the last flag, or a fragment was lost */
        r->len_err++;
        HclRx_Reset(r);
        return -1;
    }
    if (!complete) {
        if (r->next_idx > HCL_MAX_FRAGMENTS) {
            r->len_err++;
            HclRx_Reset(r);
            return -1;
        }
        return 0;
    }
    const uint16_t body = (uint16_t)(r->want - 2u);
    const uint16_t crc = (uint16_t)(r->buf[2u + body - 2u] | (r->buf[2u + body - 1u] << 8));
    const bool good = Hframe_Crc16(r->buf + 2, (size_t)(body - 2u)) == crc;
    r->active = false;
    if (!good) {
        r->crc_fail++;
        return -1;
    }
    r->msg_len = (uint16_t)(body - 2u);
    r->ok++;
    return 1;
}
