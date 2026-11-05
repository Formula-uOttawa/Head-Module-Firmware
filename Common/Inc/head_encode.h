/* head_encode.h - periodic transmit frames built from live channel values (portable C, no HAL). */
#ifndef HEAD_ENCODE_H
#define HEAD_ENCODE_H

#include "head_decode.h"

/* Per-frame schedule state. Reset whenever a new config is installed. */
typedef struct {
    uint32_t next_ms[HEAD_MAX_TX_FRAMES];
    uint8_t started;
} HeadTxState_t;

/* A frame the bus cannot take is retried on every poll until its next period would start (phase is kept); only then it is
   skipped and counted in tx_dropped. */

/* Hands a built frame to the CAN driver. Returns false if the bus could not take it right now. */
typedef bool (*HeadTxSendFn)(void *ctx, uint8_t bus, uint32_t can_id, bool extended, const uint8_t *data, uint8_t len);

void HeadTx_Reset(HeadTxState_t *st);

/* Write the low n bits of `raw` at `start` (Intel: LSB start; Motorola/DBC: MSB start). Inverse of HeadDecode_ExtractBits. */
void HeadTx_InsertBits(uint8_t *d, uint8_t len, uint16_t start, uint8_t n, bool big_endian, uint32_t raw);

/* Build the payload of transmit frame `idx` from the live values. Returns the length (dlc); out must hold 64 bytes
   (only the first dlc are written; unused signal bits are zero). Single precision only: raw = round_half_away((v - offset) / mult)
   saturated to the field; u32/i32 channels with multiplier 1 and offset 0 take an exact integer path. */
uint8_t HeadTx_Build(const HeadConfig_t *cfg, const HeadLive_t *live, uint16_t idx, uint8_t *out);

/* Send every frame that is due. Safe to call as often as you like (every main-loop pass). The first call after a Reset
   staggers the frames (frame i starts i ms, at most period-1, after the first call) so they do not all hit the FIFO at once. */
void HeadTx_Poll(const HeadConfig_t *cfg, const HeadLive_t *live, HeadTxState_t *st, HeadStats_t *stats, uint32_t now_ms,
                 HeadTxSendFn send, void *ctx);

#endif
