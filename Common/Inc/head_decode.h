/* head_decode.h - routes received CAN frames into live channel values (portable C, no HAL). */
#ifndef HEAD_DECODE_H
#define HEAD_DECODE_H

#include "head_cfg.h"
#include "head_util.h"

/* A timestamp slightly in the future (a frame stamped after `now` was sampled) counts as fresh; a difference further
   negative than this means the 32-bit millisecond clock wrapped past a very old value. */
#define HEAD_STALE_FUTURE_MS 1000u

/* Live data. Lives in memory shared between the cores; written by the CAN core, read by the others. */
typedef struct {
    volatile HeadValue_t values[HEAD_MAX_CHANNELS];
    volatile uint32_t last_rx_ms[HEAD_MAX_CHANNELS]; /* valid only while rx_seen[] is set */
    volatile uint8_t rx_seen[HEAD_MAX_CHANNELS];     /* 1 once the channel has been received since the config was applied */
    volatile uint8_t stale[HEAD_MAX_CHANNELS];       /* 1 = never received or timed out (see HeadDecode_UpdateStale) */
} HeadLive_t;

typedef struct {
    volatile uint32_t rx_frames;  /* every frame passed in */
    volatile uint32_t rx_matched; /* matched a configured hub or custom frame */
    volatile uint32_t rx_dropped; /* matched nothing (or too short) */
    volatile uint32_t fifo_lost;  /* lost-frame EVENTS: one per hardware "message lost" interrupt (not a frame count) plus one per
                                     frame dropped because the CM4's software receive ring was full */
    volatile uint32_t tx_frames;  /* transmit frames queued */
    volatile uint32_t tx_dropped; /* transmit frame skipped because the bus had no room */
    volatile uint8_t bus_off;
    volatile uint8_t passive;
} HeadBusStats_t;

typedef struct {
    HeadBusStats_t bus[HEAD_MAX_BUSES];
    volatile uint32_t hub_lost[8];       /* estimated frames lost per hub (frame counter gaps) */
    volatile int16_t hub_last_counter[8]; /* -1 = none seen yet */
    volatile uint32_t rx_nobus;           /* frames received on a peripheral that is not mapped to a configured bus */
    volatile uint32_t rx_ring_overrun;    /* frames dropped because the ISR->main receive ring was full */
    volatile uint32_t rx_ring_high;       /* receive ring high-water mark (entries) */
} HeadStats_t;

void HeadDecode_Reset(HeadLive_t *live, HeadStats_t *stats);

/* Feed one received frame; `now_ms` is its arrival time. Returns true if it matched a configured frame.
   Hub routes / signals that reach beyond `len` are skipped (and do not refresh their channel). */
bool HeadDecode_Process(const HeadConfig_t *cfg, HeadLive_t *live, HeadStats_t *stats, uint8_t bus, uint32_t can_id,
                        bool extended, const uint8_t *data, uint8_t len, uint32_t now_ms);

/* Refresh live->stale[] from rx_seen/last_rx_ms and the per-channel timeouts (timeout_ms == 0: never stale once seen).
   Uses a signed time difference, so a frame stamped slightly after `now_ms` is fresh. Call periodically (e.g. every 10 ms). */
void HeadDecode_UpdateStale(const HeadConfig_t *cfg, HeadLive_t *live, uint32_t now_ms);

/* Exposed for tests: Intel (LSB start) or Motorola/DBC (MSB start, sawtooth) bit-field extraction. */
uint32_t HeadDecode_ExtractBits(const uint8_t *d, uint8_t len, uint16_t start, uint8_t n, bool big_endian);

/* true if every bit of the field lies inside the first `len` payload bytes */
bool HeadDecode_BitsInRange(uint8_t len, uint16_t start, uint8_t n, bool big_endian);

#endif
