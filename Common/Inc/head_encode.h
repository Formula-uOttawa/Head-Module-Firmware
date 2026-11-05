/* head_encode.h - periodic transmit frames built from live channel values (portable C, no HAL). */
#ifndef HEAD_ENCODE_H
#define HEAD_ENCODE_H

#include "head_decode.h"

/* Per-frame schedule state. Reset whenever a new config is installed. */
typedef struct {
    uint32_t next_ms[HEAD_MAX_TX_FRAMES];
    uint8_t started;
} HeadTxState_t;

/* Hands a built frame to the CAN driver. Returns false if the bus could not take it (the frame is retried on the next poll). */
typedef bool (*HeadTxSendFn)(void *ctx, uint8_t bus, uint32_t can_id, bool extended, const uint8_t *data, uint8_t len);

void HeadTx_Reset(HeadTxState_t *st);

/* Write the low n bits of `raw` at `start` (Intel: LSB start; Motorola/DBC: MSB start). Inverse of HeadDecode_ExtractBits. */
void HeadTx_InsertBits(uint8_t *d, uint8_t len, uint16_t start, uint8_t n, bool big_endian, uint32_t raw);

/* Build the payload of transmit frame `idx` from the live values. Returns the length (dlc). */
uint8_t HeadTx_Build(const HeadConfig_t *cfg, const HeadLive_t *live, uint16_t idx, uint8_t *out);

/* Send every frame that is due. Safe to call as often as you like (every main-loop pass). */
void HeadTx_Poll(const HeadConfig_t *cfg, const HeadLive_t *live, HeadTxState_t *st, HeadStats_t *stats, uint32_t now_ms,
                 HeadTxSendFn send, void *ctx);

#endif
