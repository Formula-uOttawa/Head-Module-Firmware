/* Host harness for HframeRx. stdin commands, one per line:
     b <hex>                 feed raw bytes
     f <type> <seq> <hex>    feed a complete valid frame built by Hframe_Encode (type/seq in hex, payload may be "-" for none)
     r                       HframeRx_Reset (link idle timeout)
     x                       print the counters
   Output: "msg <hex>" per delivered message (type seq payload), "line <text>" per ASCII line, "cnt crc=.. ovf=.. desync=..". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hframe.h"

static HframeRx_t rx;

static void on_msg(void *c, const uint8_t *m, size_t n)
{
    (void)c;
    printf("msg");
    for (size_t i = 0; i < n; i++) printf(i ? "%02x" : " %02x", m[i]);
    printf("\n");
}
static void on_line(void *c, const char *l) { (void)c; printf("line %s\n", l); }

static size_t unhex(const char *s, uint8_t *o, size_t cap)
{
    size_t n = 0;
    if (s[0] == '-') return 0;
    while (s[0] && s[1] && s[0] != '\n' && n < cap) { unsigned v; if (sscanf(s, "%2x", &v) != 1) break; o[n++] = (uint8_t)v; s += 2; }
    return n;
}

int main(void)
{
    HframeRx_Init(&rx, on_msg, on_line, NULL);
    static char line[4096];
    static uint8_t buf[2048];
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'b') {
            size_t n = unhex(line + 2, buf, sizeof buf);
            for (size_t i = 0; i < n; i++) HframeRx_Feed(&rx, buf[i]);
        } else if (line[0] == 'f') {
            unsigned t, s;
            char hex[1024] = "-";
            if (sscanf(line + 2, "%x %x %1023s", &t, &s, hex) < 2) continue;
            uint8_t pl[300], out[600];
            size_t n = unhex(hex, pl, sizeof pl);
            size_t w = Hframe_Encode((uint8_t)t, (uint8_t)s, pl, n, out, sizeof out);
            for (size_t i = 0; i < w; i++) HframeRx_Feed(&rx, out[i]);
        } else if (line[0] == 'r') {
            HframeRx_Reset(&rx);
        } else if (line[0] == 'x') {
            printf("cnt crc=%u ovf=%u desync=%u\n", rx.crc_fail, rx.overflow, rx.desync);
        }
    }
    return 0;
}
