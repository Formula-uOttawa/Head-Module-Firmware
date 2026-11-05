/*
 * head_shared.h - memory shared by the CM4 (CAN) and CM7 (link/storage) cores.
 *
 * Lives in SRAM4 (D3 domain, 0x38000000, 64 KB), reachable by both cores at the same address, so no linker script
 * changes are needed. The CM4 initialises it (memset + magic last); the CM7 must wait for the magic before use.
 * Ownership: the CM4 writes everything except `stage`, `cmd*`, `sniff_mask`, `sniff_tail` which belong to the CM7.
 * Caches: the CM7 data cache is currently disabled. If it is ever enabled, mark this region non-cacheable in the MPU.
 */
#ifndef HEAD_SHARED_H
#define HEAD_SHARED_H

#include "head_decode.h"

#define HEAD_SHARED_BASE 0x38000000UL
#define HEAD_SHARED_MAGIC 0x48534852u

/* Persistent config sector: top 128 KB sector of flash bank 1 (outside the CM7 image, see CM7 linker script) */
#define HEAD_STORE_ADDR 0x080E0000UL
#define HEAD_STORE_SIZE 0x00020000UL

/* CM7 -> CM4 commands (mailbox) */
#define HEAD_CMD_NONE 0u
#define HEAD_CMD_APPLY_STAGED 1u  /* blob in `stage`, length in cmd_len, cmd_arg = active_source to record */
#define HEAD_CMD_APPLY_FLASH 2u   /* blob in the config sector */
#define HEAD_CMD_APPLY_DEFAULT 3u /* compiled-in default blob */

#define HEAD_SNIFF_RING 32u
typedef struct {
    uint32_t id;
    uint32_t ts_ms;
    uint8_t bus;
    uint8_t flags; /* bit0 extended, bit1 FD */
    uint8_t len;
    uint8_t data[64];
} HeadSniffEntry_t;

typedef struct {
    uint32_t magic;
    volatile uint32_t cm4_heartbeat; /* incremented every ms by the CM4 main loop */
    volatile uint32_t active_hash_lo, active_hash_hi;
    volatile uint32_t active_len;
    volatile uint32_t fdcan_kernel_hz; /* measured by the CM4 at boot, reported in INFO */
    volatile uint8_t active_source; /* 0 default, 1 flash, 2 uploaded */
    volatile uint8_t bus_count;
    volatile uint16_t channel_count;
    volatile uint32_t sniff_mask; /* CM7: bit per bus index */

    volatile uint32_t cmd, cmd_arg, cmd_len, cmd_req, cmd_ack, cmd_result;

    volatile uint32_t sniff_head, sniff_tail, sniff_dropped;
    HeadStats_t stats;
    HeadLive_t live;
    HeadSniffEntry_t sniff[HEAD_SNIFF_RING];
    uint8_t active_blob[HEAD_MAX_BLOB] __attribute__((aligned(4)));
    uint8_t stage[HEAD_MAX_BLOB] __attribute__((aligned(4)));
} HeadShared_t;

_Static_assert(sizeof(HeadShared_t) <= 0x10000, "HeadShared_t must fit in SRAM4");

#define HEAD_SHARED ((HeadShared_t *)HEAD_SHARED_BASE)

#endif
