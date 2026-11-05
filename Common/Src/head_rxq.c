#include "head_rxq.h"

#include <string.h>

#include "head_util.h"

void HeadRxQ_Init(HeadRxQ_t *q)
{
    memset((void *)q, 0, sizeof *q);
}

bool HeadRxQ_Push(HeadRxQ_t *q, uint8_t bus, uint32_t id, uint8_t flags, uint32_t ts_ms, const uint8_t *data, uint8_t len)
{
    const uint32_t head = q->head;
    const uint32_t fill = head - q->tail;
    if (fill >= HEAD_RXQ_SIZE) {
        q->overrun++;
        return false;
    }
    HeadRxEntry_t *e = &q->e[head % HEAD_RXQ_SIZE];
    if (len > sizeof e->data)
        len = (uint8_t)sizeof e->data;
    e->id = id;
    e->ts_ms = ts_ms;
    e->bus = bus;
    e->flags = flags;
    e->len = len;
    memcpy(e->data, data, len);
    HEAD_MEM_BARRIER(); /* entry contents before the new head becomes visible */
    q->head = head + 1u;
    if (fill + 1u > q->high)
        q->high = fill + 1u;
    return true;
}

const HeadRxEntry_t *HeadRxQ_Peek(const HeadRxQ_t *q)
{
    if (q->head == q->tail)
        return NULL;
    HEAD_MEM_BARRIER();
    return &q->e[q->tail % HEAD_RXQ_SIZE];
}

void HeadRxQ_Pop(HeadRxQ_t *q)
{
    HEAD_MEM_BARRIER(); /* finished reading the entry before the producer may reuse it */
    if (q->head != q->tail)
        q->tail = q->tail + 1u;
}

void HeadRxQ_Flush(HeadRxQ_t *q)
{
    q->tail = q->head;
}
