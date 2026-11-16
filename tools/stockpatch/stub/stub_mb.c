/*
 * Slot access for the stock stub.
 *
 * The multiboot format itself is not duplicated here: validation, the
 * active-state marker and the restore path are App/driver/mb_flash_logic.c,
 * built into the stub unchanged, so the on-flash format stays byte-identical
 * to what KLEANFW writes.
 */

#include "stub_mb.h"

#include <stdbool.h>
#include <stddef.h>

#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_spi.h"

/* -------------------------------------------------------------------------- */
/* Small libc bits                                                           */
/*                                                                            */
/* The stub is linked with -nostdlib; GCC still emits calls to these for      */
/* struct assignment and array zeroing in mb_flash_logic.c.                   */
/* -------------------------------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;

    while (n--) {
        if (*x != *y)
            return *x < *y ? -1 : 1;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* -------------------------------------------------------------------------- */
/* PY25Q16 shims                                                             */
/*                                                                            */
/* mb_flash_logic.c only needs these two to keep the stock's sector cache     */
/* coherent. The stub never uses that cache: it talks to SPI2 directly and    */
/* the stock re-initialises its own cache when it boots.                     */
/* -------------------------------------------------------------------------- */

void PY25Q16_Flush(void)
{
}

void PY25Q16_InvalidateCache(void)
{
}

/* -------------------------------------------------------------------------- */
/* SPI2                                                                       */
/* -------------------------------------------------------------------------- */

void stub_flash_init(void)
{
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_SPI2);
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

    LL_GPIO_InitTypeDef gpio;
    LL_GPIO_StructInit(&gpio);
    gpio.Mode       = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull       = LL_GPIO_PULL_UP;

    gpio.Pin       = LL_GPIO_PIN_0; /* SCK */
    gpio.Alternate = LL_GPIO_AF8_SPI2;
    LL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin       = LL_GPIO_PIN_1 | LL_GPIO_PIN_2; /* MOSI, MISO */
    gpio.Alternate = LL_GPIO_AF9_SPI2;
    LL_GPIO_Init(GPIOA, &gpio);

    /* Chip select: PA3, idle high, driven by mb_flash_logic.c. */
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_3);
    LL_GPIO_StructInit(&gpio);
    gpio.Mode       = LL_GPIO_MODE_OUTPUT;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Pin        = LL_GPIO_PIN_3;
    LL_GPIO_Init(GPIOA, &gpio);
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_3);

    LL_SPI_InitTypeDef spi;
    LL_SPI_StructInit(&spi);
    spi.Mode              = LL_SPI_MODE_MASTER;
    spi.TransferDirection = LL_SPI_FULL_DUPLEX;
    spi.ClockPhase        = LL_SPI_PHASE_2EDGE;
    spi.ClockPolarity     = LL_SPI_POLARITY_HIGH;
    spi.BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV2;
    spi.BitOrder          = LL_SPI_MSB_FIRST;
    spi.NSS               = LL_SPI_NSS_SOFT;
    spi.CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE;
    LL_SPI_Init(SPI2, &spi);
    LL_SPI_Enable(SPI2);
}

/* -------------------------------------------------------------------------- */
/* Slots                                                                      */
/* -------------------------------------------------------------------------- */

void stub_scan_slots(mb_slot_header_t *headers, uint8_t *status)
{
    for (uint8_t slot = 0; slot < MB_SLOT_COUNT; slot++)
        status[slot] = MB_ValidateSlot(slot, &headers[slot], NULL);
}

uint8_t stub_running_slot(void)
{
    mb_state_t state;

    if (MB_ReadActiveState(&state) == MB_MARK_VALID &&
        state.firmware_slot < MB_SLOT_COUNT)
        return state.firmware_slot;

    return MB_SLOT_BACKUP;
}

uint8_t stub_restore_slot(uint8_t slot, uint8_t *progress_line)
{
    const uint8_t err = MB_SetActiveSlot(slot);

    if (err != MB_OK)
        return err;

    return MB_RestoreSlot(slot, progress_line);
}

/* Word read by the SPI trampolines in stub_hooks.S. Above the stock's initial
 * stack pointer (0x20002FA0); the stock never reads or writes it. */
#define BANK_WORD (*(volatile uint32_t *)0x20003FFCu)

uint32_t stub_bank_apply(void)
{
    mb_state_t state;
    uint32_t base = 0;

    /* The marker records which image should be running and which config bank
     * it owns. It is only trusted when the internal image really is that
     * image: a stock flashed straight over USB (no multiboot) must keep using
     * bank 0 - the KLEANFW Main config - and must not touch the marker. */
    if (MB_ReadActiveState(&state) == MB_MARK_VALID && MB_InternalMatchesState(&state))
        base = MB_BankBase(state.config_bank);

    BANK_WORD = base;
    return base;
}

/* -------------------------------------------------------------------------- */
/* Taking ownership of a config bank                                          */
/*                                                                            */
/* The stock cannot boot from a config it did not write: on a bank holding    */
/* another firmware's settings it freezes on the VFO screen (confirmed on the */
/* radio: same image, bank erased by hand -> boots). It has no automatic      */
/* initialisation either: its factory reset is only reachable from its menu.  */
/* The one state it is known to boot from is the Main config at bank 0 (stock */
/* flashed by USB, no remap).                                                 */
/*                                                                            */
/* So a bank the stock does not own yet receives a copy of bank 0, and then   */
/* the ownership mark at SEED_MARK. 0xD000 is free in both layouts: the stock */
/* never reads or writes it (its factory reset skips 0xA000-0xDFFF too), and  */
/* KLEANFW banks use 0x0000-0xA170 and 0xB000-0xCFFF only. The mark is the    */
/* last write, so a power loss mid-copy restarts it on the next boot; sectors */
/* already equal to the source are skipped, never erased again.               */
/* -------------------------------------------------------------------------- */

#define SEED_MAIN_PROBE 0xA000u /* KLEANFW always writes its settings here */
#define SEED_MARK       0xD000u
/*
 * Sectors left erased instead of copied: same address, different meaning in
 * the two layouts, and an erased sector is exactly what the stock itself
 * produces (its loader's fallback for 0xFF equals its RESET default).
 *
 *   0x4000  stock settings (SQL, TOT, VOX, MIC, backlight...); KLEANFW keeps
 *           zero-padded channel names here, read as backlight time 0.
 *   0x8000  stock DTMF codes (ANI-ID, UP/DW codes), validated as DTMF text;
 *           KLEANFW keeps channel attributes here.
 *   0x9000  stock scan list, priority channels unclamped; KLEANFW VFOs.
 *   0xA000  stock custom AES key (any word != 0xFFFFFFFF sets the flag);
 *           KLEANFW settings.
 */
static bool seed_skip(uint32_t off)
{
    return off == 0x4000u || off == 0x8000u || off == 0x9000u || off == 0xA000u;
}
#define SEED_SECTOR     0x1000u
#define SEED_PAGE       0x100u
#define SPI_TIMEOUT     20000u
#define WIP_TIMEOUT     5000000u
#define FLASH_CS        LL_GPIO_PIN_3

static uint8_t seed_page[SEED_PAGE];
static uint8_t seed_dst[SEED_PAGE];
static bool seed_err;

static uint8_t seed_spi(uint8_t v)
{
    uint32_t to = SPI_TIMEOUT;

    while (!LL_SPI_IsActiveFlag_TXE(SPI2))
        if (!--to) {
            seed_err = true;
            return 0xFF;
        }
    LL_SPI_TransmitData8(SPI2, v);
    to = SPI_TIMEOUT;
    while (!LL_SPI_IsActiveFlag_RXNE(SPI2))
        if (!--to) {
            seed_err = true;
            return 0xFF;
        }
    return LL_SPI_ReceiveData8(SPI2);
}

static void seed_cmd_addr(uint8_t cmd, uint32_t addr)
{
    LL_GPIO_ResetOutputPin(GPIOA, FLASH_CS);
    seed_spi(cmd);
    seed_spi((uint8_t)(addr >> 16));
    seed_spi((uint8_t)(addr >> 8));
    seed_spi((uint8_t)addr);
}

static void seed_cs_high(void)
{
    LL_GPIO_SetOutputPin(GPIOA, FLASH_CS);
}

static void seed_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    seed_cmd_addr(0x03, addr);
    while (len-- && !seed_err)
        *buf++ = seed_spi(0xFF);
    seed_cs_high();
}

static bool seed_blank(const uint8_t *buf, uint32_t len)
{
    while (len--)
        if (*buf++ != 0xFF)
            return false;
    return true;
}

static bool seed_wait_wip(void)
{
    for (uint32_t i = 0; i < WIP_TIMEOUT && !seed_err; i++) {
        LL_GPIO_ResetOutputPin(GPIOA, FLASH_CS);
        seed_spi(0x05);
        const uint8_t status = seed_spi(0xFF);
        seed_cs_high();
        if (!(status & 1u))
            return !seed_err;
    }
    seed_err = true;
    return false;
}

static void seed_wren(void)
{
    LL_GPIO_ResetOutputPin(GPIOA, FLASH_CS);
    seed_spi(0x06);
    seed_cs_high();
}

static bool seed_erase(uint32_t addr)
{
    seed_wren();
    seed_cmd_addr(0x20, addr);
    seed_cs_high();
    return seed_wait_wip();
}

static bool seed_program(uint32_t addr, const uint8_t *buf)
{
    seed_wren();
    seed_cmd_addr(0x02, addr);
    for (uint32_t i = 0; i < SEED_PAGE && !seed_err; i++)
        seed_spi(buf[i]);
    seed_cs_high();
    return seed_wait_wip();
}

static bool seed_equal(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    while (len--)
        if (*a++ != *b++)
            return false;
    return true;
}

/* Make bank sector `off` byte-identical to the same sector of bank 0.
 *
 * A sector is erased only when it holds data that differs from the source -
 * leftovers of a stock boot on the erased bank. A sector an interrupted seed
 * already copied compares equal and costs nothing; an erased one is only
 * programmed. */
static bool seed_sector(uint32_t base, uint32_t off)
{
    const uint32_t dst = base + off;
    bool erase = false;

    for (uint32_t page = 0; page < SEED_SECTOR && !erase; page += SEED_PAGE) {
        seed_read(off + page, seed_page, SEED_PAGE);
        seed_read(dst + page, seed_dst, SEED_PAGE);
        if (seed_err)
            return false;
        if (!seed_equal(seed_page, seed_dst, SEED_PAGE) && !seed_blank(seed_dst, SEED_PAGE))
            erase = true;
    }

    if (erase && !seed_erase(dst))
        return false;

    for (uint32_t page = 0; page < SEED_SECTOR; page += SEED_PAGE) {
        seed_read(off + page, seed_page, SEED_PAGE);
        if (seed_err)
            return false;
        if (seed_blank(seed_page, SEED_PAGE))
            continue;
        if (!erase) {
            seed_read(dst + page, seed_dst, SEED_PAGE);
            if (seed_err)
                return false;
            if (seed_equal(seed_page, seed_dst, SEED_PAGE))
                continue;
        }
        if (!seed_program(dst + page, seed_page))
            return false;
    }
    return !seed_err;
}

static const uint8_t seed_magic[16] = {
    'K', 'L', 'E', 'A', 'N', 'F', 'W', ' ', 'S', 'T', 'O', 'C', 'K', ' ', 'B', '3',
};

/* Leave bank sector `dst` erased, erasing it only if it is not. */
static bool seed_clear(uint32_t dst)
{
    bool blank = true;

    for (uint32_t page = 0; page < SEED_SECTOR && blank; page += SEED_PAGE) {
        seed_read(dst + page, seed_dst, SEED_PAGE);
        if (seed_err)
            return false;
        blank = seed_blank(seed_dst, SEED_PAGE);
    }
    return blank || seed_erase(dst);
}

/* Erase the mark sector if needed and write the ownership record. */
static bool seed_mark(uint32_t base)
{
    const uint32_t dst = base + SEED_MARK;

    if (!seed_clear(dst))
        return false;

    memset(seed_page, 0xFF, SEED_PAGE);
    memcpy(seed_page, seed_magic, sizeof(seed_magic));
    return seed_program(dst, seed_page);
}

void stub_bank_seed(uint32_t base)
{
    if (base == 0)
        return;

    seed_err = false;

    /* Already owned by the stock: whatever it saved there is its own. */
    seed_read(base + SEED_MARK, seed_page, sizeof(seed_magic));
    if (seed_err || seed_equal(seed_page, seed_magic, sizeof(seed_magic)))
        return;

    /* Nothing proven-good to copy: keep the stock's own behaviour. */
    seed_read(SEED_MAIN_PROBE, seed_page, 16);
    if (seed_err || seed_blank(seed_page, 16))
        return;

    for (uint32_t off = 0; off < MB_BANK_SIZE; off += SEED_SECTOR) {
        if (off == SEED_MARK)
            continue;
        const bool ok = seed_skip(off) ? seed_clear(base + off) : seed_sector(base, off);
        if (!ok)
            return; /* no mark: retried on the next boot */
    }

    (void)seed_mark(base);
}

const char *stub_slot_error(uint8_t err)
{
    switch (err) {
    case MB_OK:
        return "OK";
    case MB_ERR_MAGIC:
        return "empty";
    case MB_ERR_VERSION:
        return "new header";
    case MB_ERR_NOT_COMMITTED:
        return "incomplete";
    case MB_ERR_SIZE:
        return "bad size";
    case MB_ERR_CRC:
        return "CRC ERROR";
    case MB_ERR_SPI:
        return "SPI ERROR";
    case MB_ERR_SLOT:
        return "bad index";
    case MB_ERR_AUTH:
        return "auth";
    case MB_ERR_RAM_LOAD:
        return "RAM LOAD ERROR";
    default:
        return "error";
    }
}
