/* Host harness for the transmit path. encode_sim <blob>; stdin: either "v <hex u32 per channel slot...>" (set live values and
 * print every transmit frame's payload) or "t <now_ms>" (run the scheduler, printing frames sent, "ok"/"busy" per frame via arg). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "head_cfg.h"
#include "head_encode.h"

static HeadConfig_t cfg;
static HeadLive_t live;
static HeadStats_t stats;
static HeadTxState_t tx;
static int busy;

static bool send(void *ctx, uint8_t bus, uint32_t id, bool ext, const uint8_t *d, uint8_t len)
{
    (void)ctx;
    if (busy) return false;
    printf(" [%u %x %u ", bus, (unsigned)id, ext);
    for (uint8_t i = 0; i < len; i++) printf("%02x", d[i]);
    printf("]");
    return true;
}

int main(int argc, char **argv)
{
    static uint8_t blob[HEAD_MAX_BLOB];
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f) return 2;
    size_t n = fread(blob, 1, sizeof blob, f);
    fclose(f);
    HeadCfgStatus_t st = HeadCfg_Parse(blob, (uint32_t)n, &cfg);
    printf("cfg %d %s tf=%u ts=%u\n", st, HeadCfg_StatusText(st), cfg.tx_frame_count, cfg.tx_signal_count);
    if (st != HEAD_CFG_OK) return 1;
    HeadDecode_Reset(&live, &stats);
    HeadTx_Reset(&tx);
    char line[2048];
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'v') {
            char *p = line + 1;
            for (unsigned s = 0; s < cfg.channel_count; s++) live.values[s].u = (uint32_t)strtoul(p, &p, 16);
            for (uint16_t i = 0; i < cfg.tx_frame_count; i++) {
                uint8_t d[64];
                uint8_t len = HeadTx_Build(&cfg, &live, i, d);
                for (uint8_t k = 0; k < len; k++) printf("%02x", d[k]);
                printf("\n");
            }
        } else if (line[0] == 'b') {
            busy = atoi(line + 1);
        } else if (line[0] == 't') {
            HeadTx_Poll(&cfg, &live, &tx, &stats, (uint32_t)strtoul(line + 1, NULL, 10), send, NULL);
            printf("\n");
        }
    }
    printf("stats tx=%u dropped=%u\n", stats.bus[1].tx_frames, stats.bus[1].tx_dropped);
    return 0;
}
