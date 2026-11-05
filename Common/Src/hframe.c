#include "hframe.h"

#include <stdbool.h>

uint16_t Hframe_Crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t Hcobs_Encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t write = 1, code_idx = 0;
    uint8_t code = 1;
    if (cap < 1)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (write >= cap)
            return 0;
        if (in[i] == 0) {
            out[code_idx] = code;
            code_idx = write++;
            code = 1;
        } else {
            out[write++] = in[i];
            if (++code == 0xFF) {
                if (write >= cap)
                    return 0;
                out[code_idx] = code;
                code_idx = write++;
                code = 1;
            }
        }
    }
    out[code_idx] = code;
    return write;
}

int Hcobs_Decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, w = 0;
    while (i < n) {
        uint8_t code = in[i];
        if (code == 0 || i + code > n)
            return -1;
        for (uint8_t k = 1; k < code; k++) {
            if (w >= cap)
                return -1;
            out[w++] = in[i + k];
        }
        i += code;
        if (code < 0xFF && i < n) {
            if (w >= cap)
                return -1;
            out[w++] = 0;
        }
    }
    return (int)w;
}

size_t Hframe_Encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n, uint8_t *out, size_t cap)
{
    uint8_t body[HFRAME_MAX_BODY];
    if (n > HFRAME_MAX_PAYLOAD)
        return 0;
    body[0] = type;
    body[1] = seq;
    for (size_t i = 0; i < n; i++)
        body[2 + i] = payload[i];
    uint16_t crc = Hframe_Crc16(body, 2 + n);
    body[2 + n] = (uint8_t)crc;
    body[3 + n] = (uint8_t)(crc >> 8);
    if (cap < 3)
        return 0;
    out[0] = 0;
    size_t e = Hcobs_Encode(body, 4 + n, out + 1, cap - 2);
    if (e == 0)
        return 0;
    out[1 + e] = 0;
    return e + 2;
}

void HframeRx_Init(HframeRx_t *rx, void (*on_msg)(void *, const uint8_t *, size_t), void (*on_line)(void *, const char *), void *ctx)
{
    rx->on_msg = on_msg;
    rx->on_line = on_line;
    rx->ctx = ctx;
    rx->mode = 0;
    rx->n = 0;
    rx->crc_fail = rx->overflow = rx->desync = 0;
}

void HframeRx_Reset(HframeRx_t *rx)
{
    if (rx->mode == 2 && rx->n)
        rx->desync++;
    rx->mode = 0;
    rx->n = 0;
}

/* a 0x00 ended a binary frame body of rx->n bytes */
static bool binary_frame_done(HframeRx_t *rx)
{
    uint8_t body[HFRAME_MAX_BODY];
    int d = Hcobs_Decode(rx->buf, rx->n, body, sizeof body);
    if (d < 4) {
        rx->desync++;
        return false;
    }
    if (Hframe_Crc16(body, (size_t)d - 2) != (uint16_t)(body[d - 2] | (body[d - 1] << 8))) {
        rx->crc_fail++;
        return false;
    }
    if (rx->on_msg)
        rx->on_msg(rx->ctx, body, (size_t)d - 2);
    return true;
}

void HframeRx_Feed(HframeRx_t *rx, uint8_t b)
{
    switch (rx->mode) {
    case 2: /* binary */
        if (b == 0) {
            /* An empty body is a repeated delimiter / idle filler: stay here. A good frame returns to idle (so ASCII
               lines work again). A bad frame stays in "binary, empty": its closing 0x00 may be the opening one of the
               next frame, which is then received correctly. */
            if (rx->n && binary_frame_done(rx))
                rx->mode = 0;
            rx->n = 0;
        } else if (rx->n < sizeof rx->buf) {
            rx->buf[rx->n++] = b;
        } else {
            rx->overflow++;
            rx->n = 0;
            rx->mode = 3;
        }
        break;
    case 3: /* oversized binary frame: skip the rest, the next 0x00 re-synchronises */
        if (b == 0) {
            rx->n = 0;
            rx->mode = 2;
        }
        break;
    case 1: /* ASCII line */
        if (b == '\n' || b == '\r') {
            rx->buf[rx->n] = 0;
            if (rx->n && rx->on_line)
                rx->on_line(rx->ctx, (const char *)rx->buf);
            rx->n = 0;
            rx->mode = 0;
        } else if (b == 0) {
            rx->n = 0;
            rx->mode = 2;
        } else if (rx->n < HFRAME_MAX_LINE - 1) {
            rx->buf[rx->n++] = b;
        } else {
            rx->n = 0;
            rx->mode = 0;
        }
        break;
    default: /* idle: the first byte decides */
        rx->n = 0;
        if (b == 0)
            rx->mode = 2;
        else if (b != '\n' && b != '\r') {
            rx->mode = 1;
            rx->buf[rx->n++] = b;
        }
    }
}
