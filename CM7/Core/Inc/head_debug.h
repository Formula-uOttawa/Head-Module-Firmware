/* head_debug.h - ASCII debug commands (ping / send:tcan / tcan:wd / reset), carried alongside the binary protocol. */
#ifndef HEAD_DEBUG_H
#define HEAD_DEBUG_H

#include <stdbool.h>

/* One complete text line (no terminator). Lines with non-printable bytes are ignored. Replies are sent over the same link. */
void HeadDebug_HandleLine(const char *line);

/* Print the one-line boot report of the SBC bring-up ("boot:tcan:..."). */
void HeadDebug_BootReport(bool tcan_ok, unsigned tries);

#endif /* HEAD_DEBUG_H */
