#include "app/calltx_store.h"
#include "driver/py25q16.h"

/* Preserve the established v7 Messenger layout without pulling its complete
 * inbox/outbox/draft persistence code into a CALLTX-only build.  Byte 317,
 * the former tone selector, stays reserved; only the volume at 318 is used. */
#define CALLTX_VOL_FLASH_ADDR (0x012000u + 318u)

uint8_t gCallTxVol;

void CALLTX_STORE_Init(void)
{
    uint8_t stored;
    PY25Q16_ReadBuffer(CALLTX_VOL_FLASH_ADDR, &stored, sizeof(stored));
    gCallTxVol = stored <= 1u ? stored : 1u;
}

void CALLTX_STORE_Save(void)
{
    PY25Q16_WriteBuffer(CALLTX_VOL_FLASH_ADDR, &gCallTxVol, sizeof(gCallTxVol), false);
}
