/*
 * head_relay.h - the head forwards one hub config/debug message at a time from the host (UART) to a hub over the hub bus
 * and returns the hub's reply (portable C; the CM4 provides the CAN transmit function and feeds received ACK frames).
 * Message framing on the bus: hcanlink.h. Host side: HMSG_HUB_RELAY in hproto.h.
 */
#ifndef HEAD_RELAY_H
#define HEAD_RELAY_H

#include <stdbool.h>
#include <stdint.h>

#include "hcanlink.h"

#define HRELAY_TIMEOUT_MS 1000u /* the hub must answer within this (a flash page write takes ~30 ms) */
#define HRELAY_MAX_MSG (HCL_MAX_BODY - 2u) /* type + seq + payload as the host sends it, up to the CRC */

typedef enum { HRELAY_IDLE = 0, HRELAY_SENDING, HRELAY_WAITING, HRELAY_DONE } HeadRelayState_t;

/* Queue one frame on the hub bus (extended id, FD, BRS). false = TX FIFO full / bus stopped: the relay retries on the next poll. */
typedef bool (*HrelayTxFn)(void *ctx, uint32_t id, const uint8_t *data, uint8_t len);

typedef struct {
    HclTx_t tx;
    HclRx_t rx;
    uint8_t state; /* HeadRelayState_t */
    uint8_t hub, req_seq;
    uint32_t deadline_ms;
    uint8_t result;            /* HERR_* once DONE */
    uint8_t rsp[HRELAY_MAX_MSG];
    uint16_t rsp_len;          /* type, seq, payload of the hub's reply */
    uint32_t requests, replies, timeouts, tx_stalls; /* counters since Init */
} HeadRelay_t;

void HeadRelay_Init(HeadRelay_t *r);
/* Start relaying msg (type, seq, payload; len bytes). Returns HERR_NONE, HERR_BUSY (one in flight), HERR_RANGE (bad hub / size). */
uint8_t HeadRelay_Start(HeadRelay_t *r, uint8_t hub, const uint8_t *msg, uint16_t len, uint32_t now_ms, uint32_t timeout_ms);
/* A frame received on the hub bus (any id): ACK frames of the hub being waited for are consumed. Returns true if consumed. */
bool HeadRelay_OnFrame(HeadRelay_t *r, uint32_t id, const uint8_t *data, uint8_t len);
/* Advance sending and the timeout. Returns true once DONE (result / rsp valid); then call HeadRelay_Take. */
bool HeadRelay_Poll(HeadRelay_t *r, uint32_t now_ms, HrelayTxFn tx, void *ctx);
void HeadRelay_Take(HeadRelay_t *r); /* back to IDLE */

#endif /* HEAD_RELAY_H */
