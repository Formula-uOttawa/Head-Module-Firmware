/*
 * hcanlink.h - the config/debug message layer carried over the CAN-FD hub bus (portable C, shared by the head and the hubs).
 *
 * A message is the same `type, seq, payload` as on the UART (docs/protocol.md section 3). On CAN it travels as
 *   stream = u16 body_len, body            body = type, seq, payload, crc16 (CRC-16/CCITT-FALSE over type..payload)
 * cut into fragments of up to 64 bytes, one CAN-FD frame each. Extended identifier (see the CAN ID standard):
 *   bits 28:27 message type   1 = CONFIG (request, host/head -> hub), 2 = ACK (reply, hub -> head)
 *   bits 26:24 hub id         the hub that is addressed (CONFIG) or that answers (ACK)
 *   bits 23:8  0
 *   bits  7:0  fragment       bit 7 = last fragment, bits 6:0 = fragment index (0, 1, ...)
 * The last fragment is padded with zeros up to the next CAN-FD length; the receiver cuts the message at body_len.
 * Fragments of one message must arrive in order (they do: one sender, one FIFO, increasing identifier priority is irrelevant
 * because they are queued in order); any gap, repeat, or CRC error discards the message.
 */
#ifndef HCANLINK_H
#define HCANLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hframe.h"

#define HCL_TYPE_DATA 0u
#define HCL_TYPE_CONFIG 1u
#define HCL_TYPE_ACK 2u
#define HCL_TYPE_ERROR 3u

#define HCL_TYPE_SHIFT 27u
#define HCL_HUB_SHIFT 24u
#define HCL_SEG_LAST 0x80u
#define HCL_SEG_IDX_MASK 0x7Fu
#define HCL_FRAME_BYTES 64u
#define HCL_MAX_BODY HFRAME_MAX_BODY /* type + seq + payload + crc16 */
#define HCL_MAX_FRAGMENTS ((2u + HCL_MAX_BODY + HCL_FRAME_BYTES - 1u) / HCL_FRAME_BYTES)
/* hardware filter that accepts every frame of one message type and hub: id = HCL_ID(type, hub, 0), mask = HCL_FILTER_MASK */
#define HCL_FILTER_MASK 0x1F000000u /* type + hub bits */
#define HCL_TYPE_FILTER_MASK 0x18000000u /* type bits only (the head accepts ACKs of every hub) */

#define HCL_ID(type, hub, seg) ((((uint32_t)(type) & 3u) << HCL_TYPE_SHIFT) | (((uint32_t)(hub) & 7u) << HCL_HUB_SHIFT) | ((uint32_t)(seg) & 0xFFu))
#define HCL_ID_TYPE(id) ((uint8_t)(((id) >> HCL_TYPE_SHIFT) & 3u))
#define HCL_ID_HUB(id) ((uint8_t)(((id) >> HCL_HUB_SHIFT) & 7u))
#define HCL_ID_SEG(id) ((uint8_t)((id) & 0xFFu))

/* ---- sender -------------------------------------------------------------------------------------------------------- */
typedef struct {
    uint8_t buf[2u + HCL_MAX_BODY];
    uint16_t total; /* bytes of the stream */
    uint16_t sent;
    uint8_t idx;
} HclTx_t;

/* Build the stream of one message. false if the payload does not fit. */
bool HclTx_Begin(HclTx_t *t, uint8_t type, uint8_t seq, const uint8_t *payload, size_t n);
/* Next fragment: fills data (zero padded to a CAN-FD length, *len bytes), *seg (index | last flag). false when finished. */
bool HclTx_Next(HclTx_t *t, uint8_t data[HCL_FRAME_BYTES], uint8_t *len, uint8_t *seg);
/* Take back the last fragment (the CAN TX FIFO refused it): the next HclTx_Next returns it again. */
void HclTx_Unget(HclTx_t *t);
bool HclTx_Done(const HclTx_t *t);

/* ---- receiver ------------------------------------------------------------------------------------------------------ */
typedef struct {
    uint8_t buf[2u + HCL_MAX_BODY];
    uint16_t have;    /* stream bytes collected */
    uint16_t want;    /* stream bytes expected (2 + body_len), known after fragment 0 */
    uint8_t next_idx;
    bool active;
    uint16_t msg_len; /* valid after HclRx_Feed returned 1: length of type, seq, payload */
    /* counters since Init */
    uint32_t ok;       /* messages delivered */
    uint32_t crc_fail; /* body CRC mismatch */
    uint32_t seq_err;  /* fragment out of order, repeated or without a start */
    uint32_t len_err;  /* bad length / overrun */
} HclRx_t;

void HclRx_Init(HclRx_t *r);
void HclRx_Reset(HclRx_t *r); /* drop a half-received message (caller timeout) */
/* Feed one received frame (seg = identifier bits 7:0). Returns 1 when a message is complete (HclRx_Msg / r->msg_len), -1 when the
   frame was bad and the message dropped, 0 if more fragments are needed. */
int HclRx_Feed(HclRx_t *r, uint8_t seg, const uint8_t *data, uint8_t len);
static inline const uint8_t *HclRx_Msg(const HclRx_t *r) { return r->buf + 2; } /* type, seq, payload */

/* CAN-FD payload length that holds `n` bytes (8, 12, ..., 64) - never below 8 so short fragments stay legal */
uint8_t Hcl_PadLen(uint8_t n);

#endif /* HCANLINK_H */
