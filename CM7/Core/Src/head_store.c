/*
 * head_store.c - persistent head configuration (CM7).
 *
 * The configs live in flash bank 1 sector 7 (0x080E0000, 128 KB), above the CM7 image (see the CM7 linker script, FLASH
 * LENGTH 896K), as an append-only log of slots (Common/Src/head_slots.c). A store appends one slot and never touches the
 * slots before it, so the previous config stays valid until the new slot's header word (programmed LAST) is complete: a power
 * loss at any point leaves either the old or the new config, never neither. The only exception is the store that finds all 7
 * slots used: the sector must be erased first, and during that window (an erase of a 128 KB H7 sector takes ~1 s, datasheet max
 * several seconds, plus programming the new slot) NO stored config exists. A power loss there falls back to the compiled-in
 * default at the next boot. The same applies to CFG_CLEAR (which erases the sector on purpose).
 *
 * The CM7 runs from the same flash bank, so it stalls completely during erase/program (no UART RX interrupts, no SysTick):
 * a COMMIT with STORE needs about 30 ms without an erase and 1 s+ with one; the host must allow for it. The CM4 runs from
 * bank 2 and keeps receiving CAN. Do not move the sector to bank 2: that would stall the CAN core.
 *
 * HAL waits are bounded by the HAL (FLASH_TIMEOUT_VALUE); a timeout/error is returned as HERR_FLASH and the HAL status
 * and error flags are kept for the "flash" console command. The data cache is off, so the read-back verification reads real flash;
 * if it is ever enabled, invalidate the range (SCB_InvalidateDCache_by_Addr) before verifying.
 */
#include "head_store.h"

#include <string.h>

#include "head_cfg.h"
#include "head_shared.h"
#include "head_slots.h"
#include "hproto.h"
#include "main.h"

#define STORE_BASE ((const uint8_t *)HEAD_STORE_ADDR)

static struct {
    bool valid;
    const uint8_t *data;
    uint32_t len;
    uint64_t hash;
} s_cache;
static uint32_t s_last_hal, s_last_err;

/* one 256-bit flash word; static (not on the stack) so no stack realignment is needed */
static uint32_t s_word[HEAD_SLOT_PROGRAM_WORD / 4u] __attribute__((aligned(32)));

void HeadStore_Refresh(void)
{
    HeadSlotInfo_t info;
    s_cache.valid = HeadSlots_FindNewest(STORE_BASE, &info);
    s_cache.data = s_cache.valid ? info.blob : NULL;
    s_cache.len = s_cache.valid ? info.len : 0;
    s_cache.hash = s_cache.valid ? HeadCfg_BlobHash(info.blob, info.len) : 0;
}

bool HeadStore_Newest(const uint8_t **data, uint32_t *len, uint64_t *hash)
{
    if (!s_cache.valid)
        return false;
    if (data)
        *data = s_cache.data;
    if (len)
        *len = s_cache.len;
    if (hash)
        *hash = s_cache.hash;
    return true;
}

bool HeadStore_Valid(uint32_t *len, uint64_t *hash)
{
    return HeadStore_Newest(NULL, len, hash);
}

uint32_t HeadStore_LastHalStatus(void) { return s_last_hal; }
uint32_t HeadStore_LastFlashError(void) { return s_last_err; }

static uint8_t note(HAL_StatusTypeDef st)
{
    s_last_hal = (uint32_t)st;
    s_last_err = HAL_FLASH_GetError();
    return st == HAL_OK ? HERR_NONE : HERR_FLASH;
}

/* flash must be unlocked */
static uint8_t erase_sector(void)
{
    FLASH_EraseInitTypeDef e;
    uint32_t sector_error = 0;
    memset(&e, 0, sizeof e);
    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Banks = FLASH_BANK_1;
    e.Sector = FLASH_SECTOR_7;
    e.NbSectors = 1;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    return note(HAL_FLASHEx_Erase(&e, &sector_error));
}

uint8_t HeadStore_Erase(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK)
        return note(HAL_ERROR);
    uint8_t rc = erase_sector();
    (void)HAL_FLASH_Lock();
    HeadStore_Refresh();
    return rc;
}

/* program `words` 256-bit words from src (padded with 0xFF) to dst; flash must be unlocked */
static uint8_t program_words(uint32_t dst, const uint8_t *src, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += HEAD_SLOT_PROGRAM_WORD) {
        uint32_t n = len - off < HEAD_SLOT_PROGRAM_WORD ? len - off : HEAD_SLOT_PROGRAM_WORD;
        memset(s_word, 0xFF, sizeof s_word);
        memcpy(s_word, src + off, n);
        uint8_t rc = note(HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, dst + off, (uint32_t)s_word));
        if (rc != HERR_NONE)
            return rc;
    }
    return HERR_NONE;
}

uint8_t HeadStore_Write(const uint8_t *blob, uint32_t len)
{
    uint8_t hdr[HEAD_SLOT_HEADER_LEN] __attribute__((aligned(4)));

    if (len < HEAD_BLOB_HEADER_LEN || len > HEAD_MAX_BLOB || HeadSlots_PaddedLen(len) + HEAD_SLOT_HEADER_LEN > HEAD_SLOT_STRIDE)
        return HERR_TOO_BIG;
    if (HeadSlots_SameBlob(STORE_BASE, blob, len)) { /* identical to the newest stored blob: no erase, no wear */
        HeadStore_Refresh();
        return HERR_NONE;
    }
    int slot = HeadSlots_NextFree(STORE_BASE);               /* read both BEFORE a possible erase: the erase destroys them */
    const uint32_t gen = HeadSlots_NextGeneration(STORE_BASE);

    if (HAL_FLASH_Unlock() != HAL_OK)
        return note(HAL_ERROR);
    uint8_t rc = HERR_NONE;
    if (slot < 0) { /* sector full: erase it (all stored configs are lost until the new slot is complete), then use slot 0 */
        rc = erase_sector();
        slot = 0;
    }
    const uint32_t base = HEAD_STORE_ADDR + HeadSlots_SlotOffset(slot);
    if (rc == HERR_NONE)
        rc = program_words(base + HEAD_SLOT_HEADER_LEN, blob, len); /* body words in ascending order */
    if (rc == HERR_NONE) {
        (void)HeadSlots_BuildHeader(hdr, blob, len, gen);
        rc = program_words(base, hdr, HEAD_SLOT_HEADER_LEN); /* header word LAST: this is what makes the slot valid */
    }
    (void)HAL_FLASH_Lock();

    HeadStore_Refresh();
    if (rc != HERR_NONE)
        return rc;
    /* verify through the same path the CM4 uses at boot */
    HeadSlotInfo_t info;
    if (!HeadSlots_FindNewest(STORE_BASE, &info) || info.slot != slot || info.len != len || memcmp(info.blob, blob, len) != 0)
        return HERR_FLASH;
    return HERR_NONE;
}
