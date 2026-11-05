/* head_text.h - pure helpers for the ASCII debug console (host-testable: no HAL). */
#ifndef HEAD_TEXT_H
#define HEAD_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* true if every byte of the NUL-terminated line is printable ASCII (0x20..0x7E). Anything else (binary garbage that fell
   through the framing parser, control characters) must never be executed or echoed. */
static inline bool HeadText_IsPrintable(const char *line)
{
    for (; *line; line++)
        if ((unsigned char)*line < 0x20u || (unsigned char)*line > 0x7Eu)
            return false;
    return true;
}

/* value of one hex digit, or -1 */
static inline int HeadText_HexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parse exactly four hex digits ("RRVV") into two bytes. Anything else (length != 4, a non-hex character) is rejected. */
static inline bool HeadText_ParseHex4(const char *s, uint8_t *b0, uint8_t *b1)
{
    int v[4];
    if (s == NULL)
        return false;
    for (int i = 0; i < 4; i++) {
        if (s[i] == '\0' || (v[i] = HeadText_HexVal(s[i])) < 0)
            return false;
    }
    if (s[4] != '\0')
        return false;
    *b0 = (uint8_t)((v[0] << 4) | v[1]);
    *b1 = (uint8_t)((v[2] << 4) | v[3]);
    return true;
}

#endif /* HEAD_TEXT_H */
