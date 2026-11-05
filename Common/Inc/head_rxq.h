/*
 * head_rxq.h - single-producer / single-consumer ring between the CAN receive interrupt and the main loop (portable C).
 *
 * The interrupt only copies the frame (and its arrival time) into the ring; decoding, statistics and sniff publishing run
 * in the main loop. The producer owns `head`, the consumer owns `tail`; a full ring drops the NEW frame and counts it.
 */
#ifndef HEAD_RXQ_H
#define HEAD_RXQ_H

#include <stdbool.h>
#include <stdint.h>

#define HEAD_RXQ_SIZE 32u /* must be a power of two: head/tail are free-running 32-bit counters */

#define HEAD_RXQ_FLAG_EXT 0x01u
#define HEAD_RXQ_FLAG_FD 0x02u

typedef struct {
    uint32_t id;
    uint32_t ts_ms; /* arrival time (ms tick) taken in the interrupt */
    uint8_t bus;
    uint8_t flags; /* HEAD_RXQ_FLAG_* */
    uint8_t len;   /* payload bytes, already clamped to 64 (8 for classic frames) */
    uint8_t data[64];
} HeadRxEntry_t;

typedef struct {
    HeadRxEntry_t e[HEAD_RXQ_SIZE];
    volatile uint32_t head, tail; /* free-running counters; head - tail = fill level */
    volatile uint32_t overrun;    /* frames dropped because the ring was full */
    volatile uint32_t high;       /* high-water mark of the fill level */
} HeadRxQ_t;

void HeadRxQ_Init(HeadRxQ_t *q);
/* producer side (interrupt). Returns false (and counts an overrun) if the ring is full. `len` is clamped to 64. */
bool HeadRxQ_Push(HeadRxQ_t *q, uint8_t bus, uint32_t id, uint8_t flags, uint32_t ts_ms, const uint8_t *data, uint8_t len);
/* consumer side (main loop): oldest entry or NULL. Call HeadRxQ_Pop() after using it. */
const HeadRxEntry_t *HeadRxQ_Peek(const HeadRxQ_t *q);
void HeadRxQ_Pop(HeadRxQ_t *q);
/* consumer side: drop everything queued (e.g. when a new config is installed) */
void HeadRxQ_Flush(HeadRxQ_t *q);

#endif /* HEAD_RXQ_H */
