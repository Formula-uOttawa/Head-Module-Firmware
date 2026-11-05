/* hframe.h - COBS + CRC-16 framing for the UART transport (portable C). See docs/protocol.md section 1. */
#ifndef HFRAME_H
#define HFRAME_H

#include <stddef.h>
#include <stdint.h>

#define HFRAME_MAX_BODY 256u   /* type + seq + payload + crc16 */
#define HFRAME_MAX_PAYLOAD (HFRAME_MAX_BODY - 4u)
#define HFRAME_MAX_ENCODED (HFRAME_MAX_BODY + HFRAME_MAX_BODY / 254u + 3u) /* delimiters + COBS overhead */
#define HFRAME_MAX_LINE 128u

uint16_t Hframe_Crc16(const uint8_t *data, size_t len);
size_t Hcobs_Encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/* returns decoded length, or -1 on malformed input */
int Hcobs_Decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* Build a complete wire frame: 0x00 + COBS(type seq payload crc16) + 0x00. Returns bytes written, 0 if it does not fit. */
size_t Hframe_Encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n, uint8_t *out, size_t cap);

typedef struct HframeRx {
    void (*on_msg)(void *ctx, const uint8_t *msg, size_t len);   /* msg = type seq payload (CRC already checked) */
    void (*on_line)(void *ctx, const char *line);                /* legacy ASCII debug line, no terminator */
    void *ctx;
    uint8_t mode; /* 0 idle, 1 text, 2 binary, 3 binary overflow (discard until the next 0x00) */
    uint16_t n;
    uint8_t buf[HFRAME_MAX_ENCODED];
    /* error counters since Init (wrap at 2^32) */
    uint32_t crc_fail; /* delimited binary frames whose CRC-16 did not match */
    uint32_t overflow; /* binary frames longer than the buffer */
    uint32_t desync;   /* malformed COBS / too-short frames, and frames cut short by HframeRx_Reset */
} HframeRx_t;

void HframeRx_Init(HframeRx_t *rx, void (*on_msg)(void *, const uint8_t *, size_t), void (*on_line)(void *, const char *), void *ctx);
void HframeRx_Feed(HframeRx_t *rx, uint8_t byte);
/*
 * Back to idle (a half-received frame is discarded). Link code calls this after ~50 ms without received bytes while
 * a frame is in progress. After a bad frame the parser stays in "binary, empty" so the next 0x00-delimited frame is
 * received correctly even when its leading delimiter was consumed as the previous frame's terminator.
 */
void HframeRx_Reset(HframeRx_t *rx);

#endif
