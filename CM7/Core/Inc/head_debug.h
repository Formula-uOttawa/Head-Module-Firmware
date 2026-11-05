/* head_debug.h - legacy ASCII debug commands (ping / send / sniff / reset), carried alongside the binary protocol. */
#ifndef HEAD_DEBUG_H
#define HEAD_DEBUG_H

#include <stdbool.h>

extern volatile bool sniff_tcan, sniff_afeA, sniff_afeB;

/* One complete text line (no terminator). Replies are sent over the same link. */
void HeadDebug_HandleLine(const char *line);

#endif
