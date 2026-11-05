/* Host simulation of the hub CAN scheduler: real can_scheduler.c + live_slots.c against a stub HAL.
 * Default build links the real generated sensor_config_generated.c; -DSIM_CUSTOM uses a deliberately broken table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "can_scheduler.h"
#include "live_slots.h"
#include "main.h"
#include "sensor_state.h"

GPIO_TypeDef stub_gpioa, stub_gpioc, stub_gpiod;
static uint32_t g_tick = 1000;
static uint16_t g_din_level; /* bit n = DINn high */
static uint32_t g_fifo_free = 3;
static uint32_t g_fifo_capacity = 3;
static int g_fail = 0;

typedef struct { uint32_t id, dlc, len; uint8_t data[64]; uint32_t tick; } Frame_t;
static Frame_t g_frames[4096];
static int g_nframes;

uint32_t HAL_GetTick(void) { return g_tick; }
void HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t pin, GPIO_PinState s) { (void)p; (void)pin; (void)s; }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t pin)
{
    if (p == GPIOD && pin == DIN0_Pin) return (g_din_level & 1) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    if (p == GPIOC && pin == DIN1_Pin) return (g_din_level & 2) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    if (p == GPIOC && pin == DIN2_Pin) return (g_din_level & 4) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    if (p == GPIOC && pin == DIN3_Pin) return (g_din_level & 8) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    return GPIO_PIN_RESET;
}
uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h) { (void)h; return g_fifo_free; }
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h, FDCAN_TxHeaderTypeDef *hdr, uint8_t *data)
{
    static const uint8_t len_of[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    (void)h;
    if (g_fifo_free == 0) return HAL_ERROR;
    g_fifo_free--;
    Frame_t *f = &g_frames[g_nframes++];
    f->id = hdr->Identifier; f->dlc = hdr->DataLength; f->len = len_of[hdr->DataLength]; f->tick = g_tick;
    memcpy(f->data, data, f->len);
    return HAL_OK;
}

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static FDCAN_HandleTypeDef can;

/* one 5 ms tick; the "bus" drains the TX FIFO completely between ticks */
static int tick_once(void)
{
    int before = g_nframes;
    g_fifo_free = g_fifo_capacity;
    CanScheduler_Tick200Hz();
    g_tick += 5;
    return g_nframes - before;
}

#ifdef SIM_CUSTOM
const uint8_t HUB_ID = 7;
volatile uint32_t LIVE_SENSOR_DATA[MAX_SENSORS];
#define SC(id, rate, off, len, slot) { .frame_id = (id), .afe = 1, .afe_channel = 0, .sensor_type = SENSOR_TYPE_ACTIVE, .sample_rate = (rate), .byte_offset = (off), .byte_length = (len), .live_sensor_mapping = (slot) }
const SensorConfig_t A_ACTIVE_SENSORS_CONFIG[] = {
    SC(0x00100000, RATE_200HZ, 4, 2, 0),    /* ok */
    SC(0x00100000, RATE_200HZ, 5, 2, 1),    /* overlaps byte 5 of the previous signal: rejected */
    SC(0x00100000, RATE_10HZ, 8, 2, 2),     /* same CAN id as an existing group with another rate: conflict */
    SC(0x00201200, RATE_200HZ, 4, 2, 3),    /* identifier uses bits 15:8, owned by the frame counter */
    SC(0x00300000, RATE_200HZ, 4, 2, 99),   /* slot out of range */
    SC(0x00310000, RATE_200HZ, 2, 2, 4),    /* overlaps the timestamp */
    SC(0x00320000, RATE_200HZ, 60, 8, 5),   /* runs past 64 bytes */
    SC(0x00110000, RATE_200HZ, 4, 2, 6),    /* more 200 Hz groups: >3 frames due on every tick (TX FIFO has 3 elements) */
    SC(0x00120000, RATE_200HZ, 4, 2, 7),
    SC(0x00130000, RATE_200HZ, 4, 2, 8),
};
const uint8_t A_ACTIVE_SENSORS_COUNT = 10;
const SensorAfeConfig_t A_ACTIVE_SENSORS_AFE_CONFIG[1];
const SensorConfig_t B_ACTIVE_SENSORS_CONFIG[1];
const SensorAfeConfig_t B_ACTIVE_SENSORS_AFE_CONFIG[1];
const uint8_t B_ACTIVE_SENSORS_COUNT = 0;
const SensorConfig_t DIG_ACTIVE_SENSORS_CONFIG[] = { SC(0x00400000, RATE_100HZ, 4, 2, 20) };
const uint8_t DIG_ACTIVE_SENSORS_COUNT = 1;
#endif

int main(void)
{
    LiveSlots_Init();
    CanScheduler_Init(&can);
    const CanSchedulerStats_t *st = CanScheduler_GetStats();

#ifdef SIM_CUSTOM
    /* rejected: overlap, same-id/other-rate group, counter bits in the id, bad slot, timestamp overlap, past 64 bytes;
     * the DIG entry has afe_channel 0 and is fine. Six findings, none of the valid groups is lost. */
    CHECK(st->invalid_config_count == 6, "invalid_config_count %u", st->invalid_config_count);
    CHECK(st->overload_ticks == 20, "overload_ticks %u (4 groups at 200 Hz do not fit 3 FIFO elements)", st->overload_ticks);
    LiveSlots_Register(0, 100000U); LiveSlots_Register(1, 100000U);   /* what the AFE driver does for its channels */
    LiveSlots_Update(0, 0xAABB); LiveSlots_Update(1, 0x1111);
    int n = tick_once();
    CHECK(n >= 3, "frames in first tick %d", n);
    int seen_overlap_fix = 0;
    for (int i = 0; i < g_nframes; i++)
        if ((g_frames[i].id & ~0xFF00u) == 0x00100000u) {
            CHECK(g_frames[i].data[4] == 0xBB && g_frames[i].data[5] == 0xAA, "kept signal bytes");
            CHECK(g_frames[i].data[6] == 0 && g_frames[i].data[7] == 0, "rejected overlapping signal wrote nothing");
            seen_overlap_fix = 1;
        }
    CHECK(seen_overlap_fix, "frame 0x100 missing");
    /* FIFO of 3: the 4th and 5th frame of the tick are dropped and counted, not silently lost */
    CHECK(st->drop_fifo_full_count > 0, "drop_fifo_full_count %u", st->drop_fifo_full_count);
    CHECK(((g_frames[g_nframes - 1].id >> 8) & 0xFF) == 2, "counter gap visible after a drop (last counter %u)", (g_frames[g_nframes - 1].id >> 8) & 0xFF);
#else
    CHECK(st->invalid_config_count == 0, "generated table must be clean, invalid=%u", st->invalid_config_count);
    CHECK(st->overload_ticks == 0, "overload %u", st->overload_ticks);

    LiveSlots_Register(0, 100000U); LiveSlots_Register(1, 100000U);
    LiveSlots_Update(0, (uint32_t)(int32_t)-2);      /* AFE value, negative: low 3 bytes go out */
    LiveSlots_Update(1, 0x00012345u);
    g_din_level = 0x1;                                /* DIN0 high, DIN1 low */
    int max_per_tick = 0, total = 0;
    int per_id[3] = { 0, 0, 0 };                       /* 0x200000 @200 Hz, 0x400000 @100 Hz, 0x800000 @10 Hz */
    for (int t = 0; t < 200; t++) {
        int n = tick_once();
        total += n;
        if (n > max_per_tick) max_per_tick = n;
    }
    CHECK(total == g_nframes, "total");
    CHECK(max_per_tick <= 2, "at most 2 frames per tick expected after staggering, got %d", max_per_tick);
    uint8_t last_counter = 0;
    for (int i = 0; i < g_nframes; i++) {
        const Frame_t *f = &g_frames[i];
        uint32_t base = f->id & ~0xFF00u;
        uint8_t counter = (uint8_t)(f->id >> 8);
        CHECK((f->id & ~0x1FFFFFFFu) == 0, "29 bit id");
        CHECK(counter == (uint8_t)(i & 0xFF), "frame counter %u at frame %d", counter, i);
        last_counter = counter;
        if (base == 0x00200000u) {
            per_id[0]++;
            /* the generated table has analog channels as 3 signed bytes: low 3 bytes of the sign extended value */
            CHECK(f->data[4] == 0xFE && f->data[5] == 0xFF && f->data[6] == 0xFF, "sign extended low bytes of -2: %02X %02X %02X", f->data[4], f->data[5], f->data[6]);
            CHECK(f->data[7] == 0x45 && f->data[8] == 0x23 && f->data[9] == 0x01, "slot 1 low bytes: %02X %02X %02X", f->data[7], f->data[8], f->data[9]);
            CHECK(f->len == 20 && f->dlc == FDCAN_DLC_BYTES_20, "length %u", f->len);
        } else if (base == 0x00400000u) {
            per_id[1]++;
            CHECK(f->data[4] == 1 && f->data[5] == 0 && f->data[6] == 0 && f->data[7] == 0, "digital levels DIN0=1 DIN1=0: %02X %02X", f->data[4], f->data[6]);
            CHECK(f->len == 8, "dig length %u", f->len);
        } else if (base == 0x00800000u) {
            per_id[2]++;
        } else {
            CHECK(0, "unexpected id %08X", f->id);
        }
        uint32_t ts = f->data[0] | (f->data[1] << 8) | (f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
        CHECK(ts == f->tick, "timestamp");
    }
    (void)last_counter;

    /* an AFE channel without a fresh sample is sent as the "no data" code: slot 1 goes stale, slot 0 stays fresh */
    LiveSlots_Register(1, 1U);
    g_tick += 50;                                    /* slot 1 expires, slot 0 keeps its 100 s window */
    LiveSlots_Update(0, (uint32_t)(int32_t)-2);
    int first = g_nframes;
    for (int t = 0; t < 20; t++) (void)tick_once();
    int checked = 0;
    for (int i = first; i < g_nframes; i++) {
        const Frame_t *f = &g_frames[i];
        if ((f->id & ~0xFF00u) != 0x00200000u) continue;
        CHECK(f->data[4] == 0xFE && f->data[5] == 0xFF && f->data[6] == 0xFF, "fresh channel still sent: %02X %02X %02X", f->data[4], f->data[5], f->data[6]);
        CHECK(f->data[7] == 0x00 && f->data[8] == 0x00 && f->data[9] == 0x80, "stale channel sent as -8388608: %02X %02X %02X", f->data[7], f->data[8], f->data[9]);
        checked++;
    }
    CHECK(checked > 0, "no 200 Hz frame after the staleness check");
    CHECK(per_id[0] == 200 && per_id[1] == 100 && per_id[2] == 10, "rates %d %d %d", per_id[0], per_id[1], per_id[2]);
    CHECK(st->tx_ok_count == (uint32_t)g_nframes && st->tx_fail_count == 0 && st->drop_fifo_full_count == 0, "stats");

    /* digital slots are sampled on the tick and flagged fresh (analog slots are registered by the AFE driver, see afe_sim.c) */
    CHECK(LIVE_SENSOR_DATA[20] == 1 && LIVE_SENSOR_DATA[21] == 0, "digital slots %u %u", LIVE_SENSOR_DATA[20], LIVE_SENSOR_DATA[21]);
    CHECK(LiveSlots_Flags(20) == 0, "digital slot fresh");
    CHECK(LiveSlots_Flags(12) == (LIVE_FLAG_STALE | LIVE_FLAG_UNCONFIGURED), "unmapped slot");
    g_tick += 100;
    CHECK(LiveSlots_Flags(20) == LIVE_FLAG_STALE, "digital slot stale after the tick stops");

    /* TX FIFO full: frames are dropped and counted per reason, the frame counter still advances (gap visible) */
    g_din_level = 0;
    int before = g_nframes;
    g_fifo_free = 0;
    uint32_t dropped_before = st->drop_fifo_full_count;
    CanScheduler_Tick200Hz();
    CHECK(g_nframes == before && st->drop_fifo_full_count > dropped_before, "fifo full handling");
    int next_before = g_nframes;
    tick_once();
    CHECK(g_nframes > next_before, "sending resumes");
    CHECK(((g_frames[g_nframes - 1].id >> 8) & 0xFF) != (uint8_t)((g_nframes - 1) & 0xFF), "counter must show the dropped frames as a gap");
#endif
    printf(g_fail ? "SIM FAILED (%d)\n" : "SIM OK\n", g_fail);
    return g_fail ? 1 : 0;
}
