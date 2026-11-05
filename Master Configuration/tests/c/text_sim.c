/* Host test of the CM7 console helpers (CM7/Core/Inc/head_text.h, tcan.h whitelist). Lines on stdin:
     p <text>   printable? -> "p 0|1"      (text may contain \xNN escapes)
     h <text>   parse exactly 4 hex digits -> "h 0" or "h 1 RR VV"
     w <hex>    TCAN console write whitelist for a register number -> "w 0|1" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "head_text.h"
#include "tcan.h"

static void unescape(const char *in, char *out)
{
    while (*in) {
        if (in[0] == '\\' && in[1] == 'x' && in[2] && in[3]) {
            char h[3] = { in[2], in[3], 0 };
            *out++ = (char)strtol(h, NULL, 16);
            in += 4;
        } else {
            *out++ = *in++;
        }
    }
    *out = 0;
}

int main(void)
{
    char line[256], txt[256];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0 || line[1] != ' ')
            continue;
        unescape(line + 2, txt);
        if (line[0] == 'p') {
            printf("p %d\n", HeadText_IsPrintable(txt));
        } else if (line[0] == 'h') {
            uint8_t a = 0, b = 0;
            if (HeadText_ParseHex4(txt, &a, &b)) printf("h 1 %02x %02x\n", a, b); else printf("h 0\n");
        } else if (line[0] == 'w') {
            printf("w %d\n", TCANConsoleWriteAllowed((uint8_t)strtol(txt, NULL, 16)));
        }
    }
    return 0;
}
