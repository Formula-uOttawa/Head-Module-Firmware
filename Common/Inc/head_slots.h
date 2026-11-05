/*
 * head_slots.h - append-only log of config blobs in one flash sector (portable C, host-testable).
 *
 * The 128 KB sector (flash bank 1 sector 7, HEAD_STORE_ADDR) is cut into HEAD_SLOT_COUNT slots of HEAD_SLOT_STRIDE bytes.
 * A slot is a 32-byte HEADER WORD followed by the blob (starting at slot + 32, padded with 0xFF to a multiple of 32):
 *     u32 magic "HLSL" (0x4C534C48), u32 blob_len, u32 blob_crc32 (HeadCfg_Crc32 of the blob), u32 generation,
 *     u32 (~magic ^ blob_crc32) check word, 12 bytes 0xFF            (little endian)
 * The header word is programmed LAST, so an interrupted store never produces a valid slot. This module only decides
 * what is where; the caller programs flash. Store sequence (CM7):
 *     slot = HeadSlots_NextFree(base); gen = HeadSlots_NextGeneration(base);       // read both BEFORE erasing
 *     if (slot < 0) { erase the whole sector; slot = 0; }                            // all stored configs are lost in this window
 *     program the padded blob words at HeadSlots_SlotOffset(slot) + 32 in ascending order;
 *     HeadSlots_BuildHeader(hdr, blob, len, gen); program hdr at HeadSlots_SlotOffset(slot) last.
 *
 * A slot is VALID iff its header magic and check word match, len is within [HEAD_BLOB_HEADER_LEN, HEAD_MAX_BLOB], the header
 * CRC matches and HeadCfg_Verify(blob) passes; the valid slot with the highest generation wins. The blob of a slot is never
 * read unless its header word is valid (an interrupted flash word can raise an ECC fault when read on the STM32H7).
 * A slot is FREE iff its header word and its first body word are entirely 0xFF; a torn store leaves programmed body words,
 * which makes the slot dirty (skipped). All functions take a memory-mapped pointer to the sector start.
 */
#ifndef HEAD_SLOTS_H
#define HEAD_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

#define HEAD_SLOT_COUNT 7u
#define HEAD_SLOT_STRIDE 0x4800u
#define HEAD_SLOT_HEADER_LEN 32u
#define HEAD_STORE_SECTOR_SIZE 0x20000u
#define HEAD_SLOT_MAGIC 0x4C534C48u
#define HEAD_SLOT_PROGRAM_WORD 32u /* flash programming granule (256 bit) */

typedef struct {
    const uint8_t *blob; /* points into the sector, valid for len bytes */
    uint32_t len;
    uint32_t generation;
    int slot;
} HeadSlotInfo_t;

/* Newest valid slot. Returns false if there is none (the sector is blank or holds only torn/old data). */
bool HeadSlots_FindNewest(const uint8_t *sector_base, HeadSlotInfo_t *out);

/* Slot to write next (0..HEAD_SLOT_COUNT-1), or -1 if the whole sector must be erased first. */
int HeadSlots_NextFree(const uint8_t *sector_base);

/* Generation number for the next store (newest valid generation + 1, or 1 if none). Read it BEFORE erasing. */
uint32_t HeadSlots_NextGeneration(const uint8_t *sector_base);

/* Byte offset of a slot inside the sector; its blob starts HEAD_SLOT_HEADER_LEN further. */
uint32_t HeadSlots_SlotOffset(int slot);

/* Blob length rounded up to the programming granule (the caller programs this many bytes, padding with 0xFF). */
uint32_t HeadSlots_PaddedLen(uint32_t len);

/* Fill the 32-byte header word for a blob. Returns 32. */
uint32_t HeadSlots_BuildHeader(uint8_t header32[32], const uint8_t *blob, uint32_t len, uint32_t generation);

/* true if the newest valid slot already holds exactly this blob (skip the store: saves wear and an erase). */
bool HeadSlots_SameBlob(const uint8_t *sector_base, const uint8_t *blob, uint32_t len);

#endif /* HEAD_SLOTS_H */
