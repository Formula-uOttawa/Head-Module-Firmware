/* Host model of the hub's flash log (hub_flash.h): RAM pages that persist in a file between runs ("power cycles") and a write
 * budget that cuts a program/erase short, like a power loss. Used by relay_sim.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hub_flash.h"
#include "hub_slots.h"

#define FLASH_BYTES (HUB_SLOT_COUNT * HUB_SLOT_STRIDE)
static uint8_t g_flash[FLASH_BYTES];
static const char *g_file;
static int g_budget = -1; /* double words that may still be programmed; -1 = unlimited; 0 = power is gone */

static void save(void)
{
    FILE *f = g_file ? fopen(g_file, "wb") : NULL;
    if (f) { fwrite(g_flash, 1, sizeof g_flash, f); fclose(f); }
}

void HubFake_Init(const char *file)
{
    g_file = file;
    memset(g_flash, 0xFF, sizeof g_flash);
    FILE *f = file ? fopen(file, "rb") : NULL;
    if (f) {
        size_t n = fread(g_flash, 1, sizeof g_flash, f);
        (void)n;
        fclose(f);
    }
}

void HubFake_SetBudget(int dwords) { g_budget = dwords; }

const uint8_t *HubFlash_Base(void) { return g_flash; }
bool HubFlash_Usable(void) { return true; }

bool HubFlash_ErasePage(uint32_t offset)
{
    if (g_budget == 0 || offset % HUB_SLOT_STRIDE != 0 || offset >= FLASH_BYTES)
        return false;
    memset(g_flash + offset, 0xFF, HUB_SLOT_STRIDE);
    save();
    return true;
}

bool HubFlash_Program(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (offset % 8 != 0 || len % 8 != 0 || offset + len > FLASH_BYTES)
        return false;
    for (uint32_t o = 0; o < len; o += 8) {
        if (g_budget == 0) { save(); return false; }
        for (int k = 0; k < 8; k++) {
            if (g_flash[offset + o + k] != 0xFF) { fprintf(stderr, "double program at %u\n", offset + o + k); abort(); } /* target must be erased */
            g_flash[offset + o + k] = data[o + k];
        }
        if (g_budget > 0) g_budget--;
    }
    save();
    return true;
}
