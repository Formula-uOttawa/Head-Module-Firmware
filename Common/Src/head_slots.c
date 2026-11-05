#include "head_slots.h"

#include <string.h>

#include "head_cfg.h"
#include "head_util.h"

typedef struct { uint32_t len, crc, gen; } SlotHeader_t;

static const uint8_t *slot_ptr(const uint8_t *base, int slot) { return base + HeadSlots_SlotOffset(slot); }

static bool all_ff(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != 0xFF)
            return false;
    return true;
}

/* header word looks like one of ours (does not touch the blob) */
static bool header_ok(const uint8_t *s, SlotHeader_t *h)
{
    if (HeadLe32(s) != HEAD_SLOT_MAGIC)
        return false;
    h->len = HeadLe32(s + 4);
    h->crc = HeadLe32(s + 8);
    h->gen = HeadLe32(s + 12);
    if (HeadLe32(s + 16) != ((~HEAD_SLOT_MAGIC) ^ h->crc))
        return false;
    return h->len >= HEAD_BLOB_HEADER_LEN && h->len <= HEAD_MAX_BLOB;
}

/* header plus blob checks; only call after header_ok */
static bool blob_ok(const uint8_t *s, const SlotHeader_t *h)
{
    const uint8_t *blob = s + HEAD_SLOT_HEADER_LEN;
    return HeadCfg_Crc32(0, blob, h->len) == h->crc && HeadCfg_Verify(blob, h->len) == HEAD_CFG_OK;
}

static bool slot_free(const uint8_t *s)
{
    return all_ff(s, HEAD_SLOT_HEADER_LEN) && all_ff(s + HEAD_SLOT_HEADER_LEN, HEAD_SLOT_HEADER_LEN);
}

uint32_t HeadSlots_SlotOffset(int slot)
{
    return (uint32_t)slot * HEAD_SLOT_STRIDE;
}

uint32_t HeadSlots_PaddedLen(uint32_t len)
{
    return (len + (HEAD_SLOT_PROGRAM_WORD - 1u)) & ~(HEAD_SLOT_PROGRAM_WORD - 1u);
}

bool HeadSlots_FindNewest(const uint8_t *base, HeadSlotInfo_t *out)
{
    bool found = false;
    HeadSlotInfo_t best = { NULL, 0, 0, -1 };
    for (int i = 0; i < (int)HEAD_SLOT_COUNT; i++) {
        const uint8_t *s = slot_ptr(base, i);
        SlotHeader_t h;
        if (!header_ok(s, &h) || (found && h.gen <= best.generation))
            continue; /* cheap reject first; the CRC passes only run for a slot that could still win */
        if (!blob_ok(s, &h))
            continue;
        best.blob = s + HEAD_SLOT_HEADER_LEN;
        best.len = h.len;
        best.generation = h.gen;
        best.slot = i;
        found = true;
    }
    if (found && out)
        *out = best;
    return found;
}

int HeadSlots_NextFree(const uint8_t *base)
{
    HeadSlotInfo_t n;
    int first = HeadSlots_FindNewest(base, &n) ? n.slot + 1 : 0;
    for (int i = first; i < (int)HEAD_SLOT_COUNT; i++)
        if (slot_free(slot_ptr(base, i)))
            return i;
    return -1;
}

uint32_t HeadSlots_NextGeneration(const uint8_t *base)
{
    HeadSlotInfo_t n;
    return HeadSlots_FindNewest(base, &n) ? n.generation + 1u : 1u;
}

uint32_t HeadSlots_BuildHeader(uint8_t h[32], const uint8_t *blob, uint32_t len, uint32_t generation)
{
    const uint32_t crc = HeadCfg_Crc32(0, blob, len);
    memset(h, 0xFF, HEAD_SLOT_HEADER_LEN);
    HeadPutLe32(h, HEAD_SLOT_MAGIC);
    HeadPutLe32(h + 4, len);
    HeadPutLe32(h + 8, crc);
    HeadPutLe32(h + 12, generation);
    HeadPutLe32(h + 16, (~HEAD_SLOT_MAGIC) ^ crc);
    return HEAD_SLOT_HEADER_LEN;
}

bool HeadSlots_SameBlob(const uint8_t *base, const uint8_t *blob, uint32_t len)
{
    HeadSlotInfo_t n;
    return HeadSlots_FindNewest(base, &n) && n.len == len && memcmp(n.blob, blob, len) == 0;
}
