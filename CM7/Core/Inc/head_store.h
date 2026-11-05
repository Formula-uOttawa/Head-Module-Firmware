/* head_store.h - persistent head configuration in the top flash sector of bank 1 (CM7 only; the CM4 reads it directly). */
#ifndef HEAD_STORE_H
#define HEAD_STORE_H

#include <stdbool.h>
#include <stdint.h>

/* Returns true and the blob length/hash if the sector holds a blob with a valid header and CRC. */
bool HeadStore_Valid(uint32_t *len, uint64_t *hash);
const uint8_t *HeadStore_Data(void);
/* Return HERR_* codes (0 = ok). */
uint8_t HeadStore_Erase(void);
uint8_t HeadStore_Write(const uint8_t *blob, uint32_t len);

#endif
