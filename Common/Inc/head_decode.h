/* head_decode.h - routes received CAN frames into live channel values (portable C, no HAL). */
#ifndef HEAD_DECODE_H
#define HEAD_DECODE_H

#include "head_cfg.h"

#define HEAD_NEVER_RX 0xFFFFFFFFu

/* Live data. Lives in memory shared between the cores; written by the CAN core, read by the others. */
typedef struct {
    volatile HeadValue_t values[HEAD_MAX_CHANNELS];
    volatile uint32_t last_rx_ms[HEAD_MAX_CHANNELS]; /* HEAD_NEVER_RX until first reception */
    volatile uint8_t stale[HEAD_MAX_CHANNELS];       /* 1 = never received or timed out (see HeadDecode_UpdateStale) */
} HeadLive_t;

typedef struct {
    volatile uint32_t rx_frames;  /* every frame passed in */
    volatile uint32_t rx_matched; /* matched a configured hub or custom frame */
    volatile uint32_t rx_dropped; /* matched nothing (or too short) */
    volatile uint32_t fifo_lost;  /* reported by the driver */
    volatile uint32_t tx_frames;  /* transmit frames queued */
    volatile uint32_t tx_dropped; /* transmit frame skipped because the bus had no room */
    volatile uint8_t bus_off;
    volatile uint8_t passive;
} HeadBusStats_t;

typedef struct {
    HeadBusStats_t bus[HEAD_MAX_BUSES];
    volatile uint32_t hub_lost[8];       /* estimated frames lost per hub (frame counter gaps) */
    volatile int16_t hub_last_counter[8]; /* -1 = none seen yet */
} HeadStats_t;

void HeadDecode_Reset(HeadLive_t *live, HeadStats_t *stats);

/* Feed one received frame. Returns true if it matched a configured frame. */
bool HeadDecode_Process(const HeadConfig_t *cfg, HeadLive_t *live, HeadStats_t *stats, uint8_t bus, uint32_t can_id,
                        bool extended, const uint8_t *data, uint8_t len, uint32_t now_ms);

/* Refresh live->stale[] from last_rx_ms and the per-channel timeouts. Call periodically (e.g. every 10 ms). */
void HeadDecode_UpdateStale(const HeadConfig_t *cfg, HeadLive_t *live, uint32_t now_ms);

/* Exposed for tests: Intel (LSB start) or Motorola/DBC (MSB start, sawtooth) bit-field extraction. */
uint32_t HeadDecode_ExtractBits(const uint8_t *d, uint8_t len, uint16_t start, uint8_t n, bool big_endian);

#endif
