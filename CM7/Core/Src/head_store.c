/*
 * head_store.c - persistent head configuration (CM7).
 *
 * The blob lives in flash bank 1 sector 7 (0x080E0000, 128 KB), above the CM7 image (see the CM7 linker script, FLASH
 * LENGTH 896K). The CM7 erases/programs it while running from the same bank, so it stalls for the duration of the erase
 * (about a second); the CM4 runs from bank 2 and keeps receiving CAN. Do not move this to bank 2: erasing there would
 * stall the CAN core.
 */
#include "head_store.h"

#include <string.h>

#include "head_cfg.h"
#include "head_shared.h"
#include "hproto.h"
#include "main.h"

const uint8_t *HeadStore_Data(void)
{
    return (const uint8_t *)HEAD_STORE_ADDR;
}

bool HeadStore_Valid(uint32_t *len, uint64_t *hash)
{
    const uint8_t *p = HeadStore_Data();
    uint32_t l = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    if (l < HEAD_BLOB_HEADER_LEN || l > HEAD_MAX_BLOB || HeadCfg_Verify(p, l) != HEAD_CFG_OK)
        return false;
    if (len)
        *len = l;
    if (hash)
        *hash = HeadCfg_BlobHash(p, l);
    return true;
}

uint8_t HeadStore_Erase(void)
{
    FLASH_EraseInitTypeDef e;
    uint32_t sector_error = 0;
    memset(&e, 0, sizeof e);
    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Banks = FLASH_BANK_1;
    e.Sector = FLASH_SECTOR_7;
    e.NbSectors = 1;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
        return HERR_FLASH;
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &sector_error);
    (void)HAL_FLASH_Lock();
    return st == HAL_OK ? HERR_NONE : HERR_FLASH;
}

uint8_t HeadStore_Write(const uint8_t *blob, uint32_t len)
{
    uint32_t word[8] __attribute__((aligned(32))); /* one 256-bit flash word */

    if (len == 0 || len > HEAD_MAX_BLOB || len > HEAD_STORE_SIZE)
        return HERR_TOO_BIG;
    uint8_t rc = HeadStore_Erase();
    if (rc != HERR_NONE)
        return rc;
    if (HAL_FLASH_Unlock() != HAL_OK)
        return HERR_FLASH;
    for (uint32_t off = 0; off < len; off += 32) {
        uint32_t n = len - off < 32 ? len - off : 32;
        memset(word, 0xFF, sizeof word);
        memcpy(word, blob + off, n);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, HEAD_STORE_ADDR + off, (uint32_t)word) != HAL_OK) {
            (void)HAL_FLASH_Lock();
            return HERR_FLASH;
        }
    }
    (void)HAL_FLASH_Lock();
    return memcmp(HeadStore_Data(), blob, len) == 0 ? HERR_NONE : HERR_FLASH;
}
