/* head_util.h - small shared helpers for the portable head code: CAN DLC table, little-endian access, bit traversal. */
#ifndef HEAD_UTIL_H
#define HEAD_UTIL_H

#include <stdbool.h>
#include <stdint.h>

/* Memory barrier between the two cores / between an ISR and the main loop (portable: DMB on the Cortex-M, fence on the host). */
#define HEAD_MEM_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* CAN(-FD) DLC code (0..15) -> payload bytes. The single DLC table of the firmware. */
static inline uint8_t HeadDlc_CodeToLen(uint32_t code)
{
    static const uint8_t t[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    return t[code & 0xFu];
}

/* true if `len` is a payload length a CAN-FD frame can carry exactly: 0..8, 12, 16, 20, 24, 32, 48, 64 */
static inline bool HeadDlc_LenIsValid(uint32_t len)
{
    for (uint32_t c = 0; c < 16u; c++)
        if (HeadDlc_CodeToLen(c) == len)
            return true;
    return false;
}

/* payload bytes -> smallest DLC code that holds them (15 for anything above 64) */
static inline uint32_t HeadDlc_LenToCode(uint32_t len)
{
    for (uint32_t c = 0; c < 16u; c++)
        if (HeadDlc_CodeToLen(c) >= len)
            return c;
    return 15u;
}

static inline uint16_t HeadLe16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t HeadLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t HeadLe64(const uint8_t *p) { return (uint64_t)HeadLe32(p) | ((uint64_t)HeadLe32(p + 4) << 32); }
static inline void HeadPutLe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void HeadPutLe32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void HeadPutLe64(uint8_t *p, uint64_t v) { HeadPutLe32(p, (uint32_t)v); HeadPutLe32(p + 4, (uint32_t)(v >> 32)); }

/* Next bit position when walking a signal field: Intel (LSB start) goes up, Motorola/DBC (MSB start, sawtooth) goes down
   inside a byte and jumps to the MSB of the next byte. Shared by the receive and transmit bit loops. */
static inline uint16_t HeadBits_Next(uint16_t pos, bool big_endian)
{
    if (!big_endian)
        return (uint16_t)(pos + 1u);
    return (pos & 7u) ? (uint16_t)(pos - 1u) : (uint16_t)(pos + 15u);
}

#endif /* HEAD_UTIL_H */
