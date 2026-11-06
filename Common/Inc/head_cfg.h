/*
 * head_cfg.h - runtime head configuration (portable C, no HAL). Parsed from the binary config blob
 * described in "Master Configuration/docs/protocol.md" section 2.
 */
#ifndef HEAD_CFG_H
#define HEAD_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEAD_BLOB_MAGIC 0x46434843u
#define HEAD_BLOB_FORMAT 1u
#define HEAD_BLOB_HEADER_LEN 32u
#define HEAD_MAX_BLOB 16384u

#define HEAD_MAX_CHANNELS 256u
#define HEAD_MAX_HUB_FRAMES 64u
#define HEAD_MAX_HUB_ROUTES 512u
#define HEAD_MAX_RX_FRAMES 64u
#define HEAD_MAX_RX_SIGNALS 256u
#define HEAD_MAX_BUSES 4u
#define HEAD_MAX_FILTERS 128u
#define HEAD_MAX_TX_FRAMES 32u
#define HEAD_MAX_TX_SIGNALS 128u

/* hardware / protocol limits enforced by HeadCfg_Parse so that a bad blob is rejected before any bus is touched */
#define HEAD_MAX_HUBS 8u                 /* hub ids 0..7 (hub_lost[] / hub_last_counter[] size) */
#define HEAD_MAX_PERIPHERAL 2u           /* FDCAN1, FDCAN2 (peripheral numbers 1..2) */
#define HEAD_MAX_FRAME_BYTES 64u         /* CAN-FD payload */
#define HEAD_CLASSIC_FRAME_BYTES 8u
#define HEAD_MAX_STD_FILTERS_PER_BUS 128u /* FDCAN message RAM filter limits */
#define HEAD_MAX_EXT_FILTERS_PER_BUS 64u
#define HEAD_MAX_NOMINAL_BITRATE 1000000u
#define HEAD_MAX_DATA_BITRATE 8000000u
#define HEAD_STD_ID_MAX 0x7FFu
#define HEAD_EXT_ID_MAX 0x1FFFFFFFu

typedef enum { HEAD_VALUE_U32 = 0, HEAD_VALUE_I32 = 1, HEAD_VALUE_F32 = 2 } HeadValueType_t;
typedef enum { HEAD_SRC_HUB = 0, HEAD_SRC_CAN_RX = 1 } HeadSource_t;
typedef union { uint32_t u; int32_t i; float f; } HeadValue_t;

/* timeout_ms == 0: the channel never goes stale once it has been received */
typedef struct { uint16_t channel_id; uint8_t value_type; uint8_t source; uint16_t rate_hz; uint32_t timeout_ms; } HeadChannel_t;
/* matched with (id & id_mask) == id_base; payload must be at least min_len bytes */
typedef struct { uint32_t id_base; uint32_t id_mask; uint8_t hub; uint8_t min_len; uint16_t first_route; uint16_t route_count; } HeadHubFrame_t;
/* flags: HEAD_ROUTE_SCALED = the channel (value type F32) gets (float)raw * multiplier + value_offset, e.g. volts for an AFE code;
   HEAD_ROUTE_NO_DATA = the hub sends the 24-bit code -8388608 for "no sample" (length >= 3, signed): such a value is not stored,
   so the channel simply goes stale. Blobs with 8-byte route elements (older app) have flags 0 and no scaling. */
#define HEAD_ROUTE_SCALED 0x01u
#define HEAD_ROUTE_NO_DATA 0x02u
#define HEAD_NO_DATA_CODE (-8388608)
typedef struct { uint16_t slot; uint8_t offset; uint8_t length; uint8_t is_signed; uint8_t flags; float multiplier; float value_offset; } HeadHubRoute_t;
typedef struct {
    uint8_t bus; uint8_t extended; uint8_t dlc; uint8_t mux_enabled;
    uint32_t can_id; uint32_t id_mask; uint32_t timeout_ms;
    uint16_t first_signal; uint16_t signal_count; uint8_t mux_start_bit; uint8_t mux_bit_length;
} HeadRxFrame_t;
typedef struct {
    uint16_t slot; uint8_t start_bit; uint8_t bit_length; uint8_t big_endian; uint8_t is_signed; uint8_t is_float;
    int16_t mux_value; float multiplier; float offset;
} HeadRxSignal_t;
/* periodic transmit frame; value = raw * multiplier + offset on the receiver, so raw = (value - offset) / multiplier */
typedef struct { uint8_t bus; uint8_t extended; uint8_t dlc; uint32_t can_id; uint16_t period_ms; uint16_t first_signal; uint16_t signal_count; } HeadTxFrame_t;
typedef struct { uint16_t slot; uint8_t start_bit; uint8_t bit_length; uint8_t big_endian; uint8_t is_signed; uint8_t is_float; float multiplier; float offset; } HeadTxSignal_t;
typedef struct { uint8_t role; uint8_t fd; uint8_t peripheral; uint32_t bitrate; uint32_t data_bitrate; } HeadBus_t; /* role 1 = hub bus */
typedef struct { uint8_t bus; uint8_t extended; uint32_t id; uint32_t mask; } HeadFilter_t;

typedef struct {
    uint64_t config_hash;
    int8_t hub_bus; /* index into buses, -1 if none */
    uint16_t channel_count, hub_frame_count, hub_route_count, rx_frame_count, rx_signal_count, bus_count, filter_count, tx_frame_count, tx_signal_count;
    HeadChannel_t channels[HEAD_MAX_CHANNELS];
    HeadHubFrame_t hub_frames[HEAD_MAX_HUB_FRAMES];
    HeadHubRoute_t hub_routes[HEAD_MAX_HUB_ROUTES];
    HeadRxFrame_t rx_frames[HEAD_MAX_RX_FRAMES];
    HeadRxSignal_t rx_signals[HEAD_MAX_RX_SIGNALS];
    HeadBus_t buses[HEAD_MAX_BUSES];
    HeadFilter_t filters[HEAD_MAX_FILTERS];
    HeadTxFrame_t tx_frames[HEAD_MAX_TX_FRAMES];
    HeadTxSignal_t tx_signals[HEAD_MAX_TX_SIGNALS];
} HeadConfig_t;

typedef enum {
    HEAD_CFG_OK = 0, HEAD_CFG_ERR_SHORT, HEAD_CFG_ERR_MAGIC, HEAD_CFG_ERR_VERSION, HEAD_CFG_ERR_LENGTH,
    HEAD_CFG_ERR_CRC, HEAD_CFG_ERR_SECTION, HEAD_CFG_ERR_LIMIT, HEAD_CFG_ERR_RANGE
} HeadCfgStatus_t;

/* CRC-32 (IEEE, zlib compatible). Pass the previous result as `crc` to continue (start with 0). */
uint32_t HeadCfg_Crc32(uint32_t crc, const uint8_t *data, size_t len);

/* Validate and decode a blob into *out. On any error *out is left zeroed (safe to ignore). */
HeadCfgStatus_t HeadCfg_Parse(const uint8_t *blob, uint32_t len, HeadConfig_t *out);

/* Same as HeadCfg_Parse but without the CRC pass: only for blobs whose CRC was just verified (HeadCfg_Verify,
   HeadSlots_FindNewest), so the 16 KB are not checksummed twice. Header magic/version/length are still checked. */
HeadCfgStatus_t HeadCfg_ParseVerified(const uint8_t *blob, uint32_t len, HeadConfig_t *out);

/* Header + length + CRC check only (cheap, no 16 KB scratch needed). Used for flash contents before a full parse. */
HeadCfgStatus_t HeadCfg_Verify(const uint8_t *blob, uint32_t len);

/* Config hash straight from a blob header without parsing (0 if the header is not valid). */
uint64_t HeadCfg_BlobHash(const uint8_t *blob, uint32_t len);

const char *HeadCfg_StatusText(HeadCfgStatus_t s);

#endif /* HEAD_CFG_H */
