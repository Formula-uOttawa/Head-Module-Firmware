/* Host harness for the firmware decode path: decode_sim <blob>; then stdin lines "bus id ext hexdata now" (feed a frame and update
   staleness at `now`) or "s now" (only update staleness). Payloads longer than 64 bytes are cut like the firmware does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "head_cfg.h"
#include "head_decode.h"

static HeadConfig_t cfg;
static HeadLive_t live;
static HeadStats_t stats;

int main(int argc, char **argv)
{
    static uint8_t blob[HEAD_MAX_BLOB];
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f) return 2;
    size_t n = fread(blob, 1, sizeof blob, f);
    fclose(f);
    HeadCfgStatus_t st = HeadCfg_Parse(blob, (uint32_t)n, &cfg);
    printf("cfg %d %s ch=%u hf=%u hr=%u rf=%u rs=%u b=%u f=%u hash=%016llx hubbus=%d\n", st, HeadCfg_StatusText(st), cfg.channel_count,
           cfg.hub_frame_count, cfg.hub_route_count, cfg.rx_frame_count, cfg.rx_signal_count, cfg.bus_count, cfg.filter_count,
           (unsigned long long)cfg.config_hash, cfg.hub_bus);
    fflush(stdout);
    if (st != HEAD_CFG_OK) return 1;
    HeadDecode_Reset(&live, &stats);
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        unsigned bus, ext;
        unsigned long id, now;
        char hexd[600];
        int h = 0;
        if (line[0] == 's') {
            if (sscanf(line + 1, "%lu", &now) != 1) break;
        } else {
            if (sscanf(line, "%u %lx %u %599s %lu", &bus, &id, &ext, hexd, &now) != 5) break;
            uint8_t d[64];
            memset(d, 0xEE, sizeof d); /* like the ISR buffer: stale bytes beyond the received length */
            size_t len = strlen(hexd) / 2;
            if (len > sizeof d) len = sizeof d;
            for (size_t i = 0; i < len; i++) { unsigned b; sscanf(hexd + 2 * i, "%2x", &b); d[i] = (uint8_t)b; }
            h = HeadDecode_Process(&cfg, &live, &stats, (uint8_t)bus, (uint32_t)id, ext != 0, d, (uint8_t)len, (uint32_t)now);
        }
        HeadDecode_UpdateStale(&cfg, &live, (uint32_t)now);
        printf("%d", h);
        for (unsigned s = 0; s < cfg.channel_count; s++) printf(" %08x:%d", live.values[s].u, live.stale[s]);
        printf(" lost=%u\n", stats.hub_lost[0]);
    }
    return 0;
}
