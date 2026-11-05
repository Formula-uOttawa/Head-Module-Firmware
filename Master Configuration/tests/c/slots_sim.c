/* Host test of Common/Src/head_slots.c on a RAM-backed fake flash sector, including power loss at EVERY program/erase step.
   The store routine below is the sequence the CM7 uses: erase only if HeadSlots_NextFree() == -1, program the body words in
   ascending order, program the header word last. A cut can happen before a step, in the middle of it (the word is left torn:
   half programmed, half garbage) or right after it. After every cut FindNewest must return exactly the previous blob or the
   new one (or, only while an erase was interrupted, nothing) - never anything else - and a following store must succeed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "head_cfg.h"
#include "head_slots.h"
#include "head_util.h"

#define SZ HEAD_STORE_SECTOR_SIZE
#define ERASE_CHUNKS 16u

static uint8_t flash[SZ];
static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint8_t b[HEAD_MAX_BLOB]; uint32_t len; } Blob_t;

/* a structurally valid blob (header + CRC) with no sections, `body` bytes of filler derived from tag */
static void make_blob(Blob_t *o, uint32_t len, uint32_t tag)
{
    memset(o->b, 0, sizeof o->b);
    HeadPutLe32(o->b, HEAD_BLOB_MAGIC);
    HeadPutLe16(o->b + 4, HEAD_BLOB_FORMAT);
    HeadPutLe16(o->b + 6, HEAD_BLOB_HEADER_LEN);
    HeadPutLe32(o->b + 8, len);
    HeadPutLe32(o->b + 12, 0); /* no sections */
    HeadPutLe64(o->b + 16, 0x1000000000ull + tag);
    uint32_t x = tag * 2654435761u + 1;
    for (uint32_t i = 32; i < len; i++) { x = x * 1664525u + 1013904223u; o->b[i] = (uint8_t)(x >> 24); }
    uint32_t crc = HeadCfg_Crc32(0, o->b, 28);
    static const uint8_t z[4] = { 0 };
    crc = HeadCfg_Crc32(crc, z, 4);
    crc = HeadCfg_Crc32(crc, o->b + 32, len - 32);
    HeadPutLe32(o->b + 28, crc);
    o->len = len;
}

/* ---- fake flash with steps and power cuts ---- */
static long step_no, cut_step;   /* cut at this step number (-1: never) */
static int cut_mode;             /* 0 before the step, 1 torn in the middle, 2 right after */
static bool cut_hit;
static bool erased_in_this_store;

static void rnd_fill(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rand(); }

/* returns false when the power is gone (the caller must stop) */
static bool do_erase_chunk(uint32_t chunk)
{
    uint32_t sz = SZ / ERASE_CHUNKS;
    long my = step_no++;
    if (my == cut_step) {
        cut_hit = true;
        if (cut_mode == 0) return false;
        if (cut_mode == 1) { rnd_fill(flash + chunk * sz, sz); return false; } /* half erased: random cells */
        memset(flash + chunk * sz, 0xFF, sz);
        return false;
    }
    memset(flash + chunk * sz, 0xFF, sz);
    return true;
}

static bool do_program(uint32_t off, const uint8_t *w32)
{
    long my = step_no++;
    if (my == cut_step) {
        cut_hit = true;
        if (cut_mode == 0) return false;
        if (cut_mode == 1) { /* torn word: first half programmed, second half garbage (programming can only clear bits) */
            for (uint32_t i = 0; i < 32; i++) flash[off + i] &= i < 16 ? w32[i] : (uint8_t)rand();
            return false;
        }
        for (uint32_t i = 0; i < 32; i++) flash[off + i] &= w32[i];
        return false;
    }
    for (uint32_t i = 0; i < 32; i++)
        flash[off + i] &= w32[i]; /* programming can only clear bits */
    return true;
}

/* the CM7 store sequence. Returns true if it ran to completion. */
static bool store(const Blob_t *bl)
{
    erased_in_this_store = false;
    int slot = HeadSlots_NextFree(flash);
    uint32_t gen = HeadSlots_NextGeneration(flash); /* read BEFORE an erase destroys it */
    if (slot < 0) {
        erased_in_this_store = true;
        for (uint32_t c = 0; c < ERASE_CHUNKS; c++)
            if (!do_erase_chunk(c)) return false;
        slot = 0;
    }
    uint32_t base = HeadSlots_SlotOffset(slot) + HEAD_SLOT_HEADER_LEN, padded = HeadSlots_PaddedLen(bl->len);
    uint8_t w[32];
    for (uint32_t o = 0; o < padded; o += 32) {
        memset(w, 0xFF, 32);
        memcpy(w, bl->b + o, bl->len - o < 32 ? bl->len - o : 32);
        if (!do_program(base + o, w)) return false;
    }
    uint8_t hdr[32];
    HeadSlots_BuildHeader(hdr, bl->b, bl->len, gen);
    return do_program(HeadSlots_SlotOffset(slot), hdr);
}

static bool matches(const HeadSlotInfo_t *i, const Blob_t *b) { return i->len == b->len && memcmp(i->blob, b->b, b->len) == 0; }

static long count_steps(const Blob_t *bl)
{
    uint8_t save[SZ];
    memcpy(save, flash, SZ);
    step_no = 0; cut_step = -1;
    store(bl);
    long n = step_no;
    memcpy(flash, save, SZ);
    return n;
}

/* fill the sector with `k` complete stores of small blobs */
static void populate(unsigned k, Blob_t *last)
{
    memset(flash, 0xFF, SZ);
    cut_step = -1;
    for (unsigned i = 0; i < k; i++) {
        make_blob(last, 100 + 37 * i, 100 + i);
        step_no = 0;
        CHECK(store(last), "populate store %u", i);
    }
}

static void power_loss_scenario(unsigned prior, uint32_t newlen, long stride)
{
    Blob_t prev, nb, again;
    populate(prior, &prev);
    uint8_t base[SZ];
    memcpy(base, flash, SZ);
    make_blob(&nb, newlen, 9000 + prior);
    make_blob(&again, 200, 7777 + prior);
    long total = count_steps(&nb);
    for (long s = 0; s < total; s += (s > 20 && s < total - 20) ? stride : 1)
        for (int mode = 0; mode < 3; mode++) {
            memcpy(flash, base, SZ);
            step_no = 0; cut_step = s; cut_mode = mode; cut_hit = false;
            bool done = store(&nb);
            CHECK(!done && cut_hit, "cut at %ld not hit", s);
            HeadSlotInfo_t info;
            bool found = HeadSlots_FindNewest(flash, &info);
            if (found) {
                bool is_prev = prior && matches(&info, &prev), is_new = matches(&info, &nb);
                CHECK(is_prev || is_new, "prior=%u cut=%ld mode=%d: returned a blob that is neither previous nor new (slot %d len %u)", prior, s, mode, info.slot, info.len);
                if (is_new) CHECK(s >= total - 1, "new blob visible before its header word was programmed (cut %ld/%ld)", s, total);
            } else {
                CHECK(prior == 0 ? (s < total - 1 || mode < 2) : erased_in_this_store, "prior=%u cut=%ld mode=%d: no config left without an erase", prior, s, mode);
            }
            /* the device must accept the next store and then return it */
            step_no = 0; cut_step = -1;
            CHECK(store(&again), "recovery store after cut %ld mode %d failed", s, mode);
            CHECK(HeadSlots_FindNewest(flash, &info) && matches(&info, &again), "recovery store not returned (prior=%u cut=%ld mode=%d)", prior, s, mode);
        }
}

int main(void)
{
    srand(1);
    Blob_t a, b, c;

    /* blank sector */
    memset(flash, 0xFF, SZ);
    HeadSlotInfo_t info;
    CHECK(!HeadSlots_FindNewest(flash, &info), "blank sector has no config");
    CHECK(HeadSlots_NextFree(flash) == 0 && HeadSlots_NextGeneration(flash) == 1, "blank sector: slot 0, generation 1");
    CHECK(HeadSlots_SlotOffset(0) == 0 && HeadSlots_SlotOffset(6) == 6 * 0x4800 && HEAD_SLOT_COUNT * HEAD_SLOT_STRIDE <= SZ, "geometry");
    CHECK(HEAD_SLOT_STRIDE >= HEAD_SLOT_HEADER_LEN + HeadSlots_PaddedLen(HEAD_MAX_BLOB), "a maximum blob fits a slot");
    CHECK(HeadSlots_PaddedLen(1) == 32 && HeadSlots_PaddedLen(32) == 32 && HeadSlots_PaddedLen(33) == 64, "padding");

    /* store / read back / identical detection */
    make_blob(&a, 500, 1);
    make_blob(&b, 700, 2);
    step_no = 0; cut_step = -1;
    CHECK(store(&a), "store a");
    CHECK(HeadSlots_FindNewest(flash, &info) && info.slot == 0 && info.generation == 1 && matches(&info, &a), "a stored in slot 0, gen 1");
    CHECK(HeadSlots_SameBlob(flash, a.b, a.len), "identical blob detected");
    CHECK(!HeadSlots_SameBlob(flash, b.b, b.len), "different blob not identical");
    CHECK(!HeadSlots_SameBlob(flash, a.b, a.len - 1) && !HeadSlots_SameBlob(flash, a.b, 0), "different length not identical");
    memcpy(c.b, a.b, a.len); c.len = a.len; c.b[100] ^= 1;
    CHECK(!HeadSlots_SameBlob(flash, c.b, c.len), "one flipped byte is not identical");
    step_no = 0; CHECK(store(&b), "store b");
    CHECK(HeadSlots_FindNewest(flash, &info) && info.slot == 1 && info.generation == 2 && matches(&info, &b), "b in slot 1, gen 2");
    CHECK(HeadSlots_SameBlob(flash, b.b, b.len) && !HeadSlots_SameBlob(flash, a.b, a.len), "only the newest counts for SameBlob");

    /* fill the log, wrap through the erase path */
    uint32_t gen_before = 0;
    for (unsigned i = 2; i < 20; i++) {
        make_blob(&c, 300 + i, 50 + i);
        int free_slot = HeadSlots_NextFree(flash);
        CHECK((free_slot == -1) == (i % 7 == 0), "NextFree = -1 exactly when all 7 slots are used (i=%u slot=%d)", i, free_slot);
        step_no = 0; CHECK(store(&c), "store %u", i);
        CHECK(HeadSlots_FindNewest(flash, &info) && matches(&info, &c) && info.slot == (int)(i % 7), "slot %u after store %u", info.slot, i);
        CHECK(info.generation == i + 1 && info.generation > gen_before, "generation keeps counting across the wrap (%u)", info.generation);
        gen_before = info.generation;
    }

    /* header-only damage is rejected without ever depending on the body */
    make_blob(&a, 400, 3);
    memset(flash, 0xFF, SZ);
    step_no = 0; CHECK(store(&a), "store");
    uint8_t keep[SZ];
    memcpy(keep, flash, SZ);
    flash[16] ^= 1; /* check word */
    CHECK(!HeadSlots_FindNewest(flash, &info), "bad check word rejected");
    memcpy(flash, keep, SZ);
    flash[4 + 3] = 0x7F; /* absurd length */
    CHECK(!HeadSlots_FindNewest(flash, &info), "absurd length rejected");
    memcpy(flash, keep, SZ);
    flash[HEAD_SLOT_HEADER_LEN + 200] ^= 0x10; /* body bit flip behind a valid header */
    CHECK(!HeadSlots_FindNewest(flash, &info), "corrupt body rejected by the CRC");
    CHECK(HeadSlots_NextFree(flash) == 1, "a damaged slot is never reused, next free is after it");
    memcpy(flash, keep, SZ);

    /* dirty slot (torn body, no header) is skipped, the previous config stays */
    memset(flash, 0xFF, SZ);
    make_blob(&a, 300, 4);
    step_no = 0; CHECK(store(&a), "store");
    memset(flash + HeadSlots_SlotOffset(1) + HEAD_SLOT_HEADER_LEN, 0x00, 32);
    CHECK(HeadSlots_FindNewest(flash, &info) && matches(&info, &a), "torn next slot ignored");
    CHECK(HeadSlots_NextFree(flash) == 2, "torn slot skipped when choosing the next one");
    memset(flash + HeadSlots_SlotOffset(2), 0x00, 32); /* torn header word */
    CHECK(HeadSlots_FindNewest(flash, &info) && matches(&info, &a) && HeadSlots_NextFree(flash) == 3, "torn header skipped");

    /* newest valid wins even when a later slot is invalid; an older generation never wins over a newer one */
    memset(flash, 0xFF, SZ);
    make_blob(&a, 300, 5); make_blob(&b, 300, 6);
    step_no = 0; CHECK(store(&a), "a"); CHECK(store(&b), "b");
    flash[HeadSlots_SlotOffset(1) + HEAD_SLOT_HEADER_LEN + 50] ^= 1;
    CHECK(HeadSlots_FindNewest(flash, &info) && matches(&info, &a) && info.slot == 0, "falls back to the older valid slot");

    /* power loss at every step, for different fill levels (7 prior stores = the sector is full and gets erased) */
    for (unsigned prior = 0; prior <= 7; prior++)
        power_loss_scenario(prior, 300 + prior, 1);
    power_loss_scenario(2, HEAD_MAX_BLOB, 53);   /* maximum blob, every 53rd body step plus the first/last 20 */
    power_loss_scenario(7, HEAD_MAX_BLOB, 97);   /* maximum blob through the erase path */

    printf("slots: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
