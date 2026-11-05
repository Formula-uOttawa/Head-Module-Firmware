/* head_store.h - persistent head configuration: an append-only slot log (head_slots.h) in the top flash sector of bank 1.
   CM7 only writes; the CM4 reads it through HeadSlots_FindNewest. */
#ifndef HEAD_STORE_H
#define HEAD_STORE_H

#include <stdbool.h>
#include <stdint.h>

/* Re-scan the sector (CRC over the newest candidates) and refresh the cache. Called at boot and after every write/erase. */
void HeadStore_Refresh(void);
/* Cached newest valid stored blob: returns false if there is none. len/hash/data may be NULL. `data` stays valid until the next
   write/erase. Never scans flash itself (INFO polls are cheap). */
bool HeadStore_Valid(uint32_t *len, uint64_t *hash);
bool HeadStore_Newest(const uint8_t **data, uint32_t *len, uint64_t *hash);

/* Return HERR_* codes (0 = ok). */
uint8_t HeadStore_Erase(void); /* erases the whole sector: every stored config is gone */
uint8_t HeadStore_Write(const uint8_t *blob, uint32_t len);

/* Status of the last flash operation (for the "flash" console command): HAL status and HAL_FLASH_GetError() bits. */
uint32_t HeadStore_LastHalStatus(void);
uint32_t HeadStore_LastFlashError(void);

#endif /* HEAD_STORE_H */
