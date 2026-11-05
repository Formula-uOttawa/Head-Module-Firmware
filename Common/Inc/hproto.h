/*
 * hproto.h - transport independent config/debug message handler (portable C).
 *
 * A transport (UART via hframe, or UDP later) hands a received message (type, seq, payload) to Hproto_Handle()
 * and sends back whatever it returns. All platform specifics (flash, the CAN core, ...) sit behind HprotoOps_t,
 * so adding a transport never touches the message handling. See docs/protocol.md section 3.
 */
#ifndef HPROTO_H
#define HPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HPROTO_VERSION 1u

typedef enum {
    HMSG_INFO = 0x01, HMSG_CFG_BEGIN = 0x02, HMSG_CFG_CHUNK = 0x03, HMSG_CFG_COMMIT = 0x04, HMSG_CFG_READ = 0x05,
    HMSG_CFG_CLEAR = 0x06, HMSG_LIVE_GET = 0x10, HMSG_STATS_GET = 0x11, HMSG_SNIFF_SET = 0x30, HMSG_SNIFF_FRAME = 0xB1,
    HMSG_REBOOT = 0x70, HMSG_ERROR = 0xFF
} HprotoType_t;

typedef enum {
    HERR_NONE = 0, HERR_BAD_REQUEST = 1, HERR_BUSY = 2, HERR_TOO_BIG = 3, HERR_CRC = 4, HERR_INVALID_CONFIG = 5,
    HERR_FLASH = 6, HERR_TIMEOUT = 7, HERR_UNSUPPORTED = 8, HERR_RANGE = 9
} HprotoError_t;

#define HPROTO_COMMIT_APPLY 0x01u
#define HPROTO_COMMIT_STORE 0x02u
#define HPROTO_CLEAR_REVERT 0x01u
#define HPROTO_CHUNK_MAX 240u

typedef struct {
    uint16_t fw_version; /* major << 8 | minor */
    uint32_t uptime_ms;
    uint64_t active_hash, stored_hash;
    uint8_t active_source; /* 0 compiled-in default, 1 flash, 2 uploaded (RAM only) */
    uint8_t cm4_alive;
    uint16_t channel_count;
    uint8_t uid[12];
    uint32_t fdcan_kernel_hz; /* 0 if unknown */
} HprotoInfo_t;

/* All ops return HERR_NONE (0) on success. ctx is passed through unchanged. */
typedef struct {
    void (*get_info)(void *ctx, HprotoInfo_t *info);
    /* blob is complete and its CRC already verified; the platform validates it further and applies/stores per flags */
    uint8_t (*cfg_commit)(void *ctx, const uint8_t *blob, uint32_t len, uint8_t flags, uint64_t *hash, uint8_t *applied, uint8_t *stored);
    /* source 0 = active, 1 = stored. Sets *total and copies up to max bytes from offset into dst, setting *n */
    uint8_t (*cfg_read)(void *ctx, uint8_t source, uint32_t offset, uint8_t *dst, uint16_t max, uint32_t *total, uint16_t *n);
    uint8_t (*cfg_clear)(void *ctx, uint8_t flags);
    /* write `count` entries of 5 bytes (u32 value LE, u8 flags) to dst; returns number written */
    uint16_t (*live_get)(void *ctx, uint16_t first, uint16_t count, uint8_t *dst);
    /* writes the STATS_GET response payload; returns its length (0 = unsupported) */
    uint16_t (*stats_get)(void *ctx, uint8_t *dst, uint16_t cap);
    uint8_t (*sniff_set)(void *ctx, uint8_t bus, uint8_t enable);
    void (*reboot)(void *ctx);
} HprotoOps_t;

typedef struct {
    const HprotoOps_t *ops;
    void *ctx;
    uint8_t *stage;      /* staging buffer for uploads (capacity stage_cap bytes) */
    uint32_t stage_cap;
    uint32_t stage_total, stage_received, stage_crc;
    bool stage_active;
    bool reboot_pending;
} Hproto_t;

void Hproto_Init(Hproto_t *p, const HprotoOps_t *ops, void *ctx, uint8_t *stage, uint32_t stage_cap);

/*
 * Handle one message (msg = type, seq, payload). Writes the response message (same layout) into resp and returns
 * its length, or 0 if there is nothing to send. After a response to REBOOT is sent, Hproto_TakeReboot() is true.
 */
size_t Hproto_Handle(Hproto_t *p, const uint8_t *msg, size_t len, uint8_t *resp, size_t resp_cap);
bool Hproto_TakeReboot(Hproto_t *p);

#endif
