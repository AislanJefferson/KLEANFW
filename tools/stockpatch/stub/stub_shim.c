#include "stub.h"

#include "py32f0xx.h"

#define PIN(n) (1u << (n))
#define MODER_BITS(n) (3u << ((n) * 2))

#define KBD_COLS (PIN(6) | PIN(5) | PIN(4) | PIN(3))
#define KBD_ROWS (PIN(15) | PIN(14) | PIN(13) | PIN(12))
#define KBD_COL(n) (1u << (6 - (n)))
#define PTT_PIN PIN(10)

/* -------------------------------------------------------------------------- */
/* SysTick as a polled counter.                                               */
/*                                                                            */
/* The stub runs before the stock's __main, so the SysTick vector in the       */
/* stock's table would point at code that is not initialised yet. The interrupt */
/* therefore stays off and only the free-running down-counter is used.        */
/* -------------------------------------------------------------------------- */

#define SYSTICK_COUNT_MASK 0x00FFFFFFu

/*
 * The stub runs before the stock configures its clocks: its SystemInit only
 * programs VTOR, so the core is still on the reset default, HSI (8 MHz). The
 * divider is read anyway instead of hard-coding 8, and anything other than HSI
 * falls back to the fastest supported clock, which keeps every delay at least
 * as long as requested.
 */
static uint32_t ticks_per_us(void)
{
    static const uint8_t hsi_mhz[8] = {4, 8, 16, 22, 24, 4, 4, 4};

    if ((RCC->CFGR & RCC_CFGR_SWS) != 0u)
        return 48u;

    return (uint32_t)(hsi_mhz[(RCC->ICSCR & RCC_ICSCR_HSI_FS) >> RCC_ICSCR_HSI_FS_Pos] >>
                      ((RCC->CR & RCC_CR_HSIDIV) >> RCC_CR_HSIDIV_Pos));
}

/* No initialiser: a .data section would need a loader that does not exist
 * before the stock's __main. Set by stub_delay_init(). */
static uint32_t g_ticks_per_us;

void stub_delay_init(void)
{
    g_ticks_per_us = ticks_per_us();

    SysTick->CTRL = 0;
    SysTick->LOAD = SYSTICK_COUNT_MASK;
    SysTick->VAL  = 0;
    /* CLKSOURCE = core clock, ENABLE; TICKINT left clear. */
    SysTick->CTRL = (1u << 2) | (1u << 0);
}

void stub_delay_us(uint32_t us)
{
    const uint32_t need = us * g_ticks_per_us;
    uint32_t last = SysTick->VAL & SYSTICK_COUNT_MASK;
    uint32_t acc  = 0;

    while (acc < need) {
        const uint32_t now = SysTick->VAL & SYSTICK_COUNT_MASK;
        acc += (last - now) & SYSTICK_COUNT_MASK; /* the counter counts down */
        last = now;
    }
}

void stub_delay_ms(uint32_t ms)
{
    while (ms--)
        stub_delay_us(1000);
}

/* -------------------------------------------------------------------------- */
/* GPIOB: keypad rows/columns and PTT only, same pinout as BOARD_GPIO_Init.   */
/* -------------------------------------------------------------------------- */

static void pin_input_pullup(unsigned int pin)
{
    GPIOB->MODER &= ~MODER_BITS(pin);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~MODER_BITS(pin)) | (1u << (pin * 2));
}

static void pin_output(unsigned int pin)
{
    GPIOB->MODER = (GPIOB->MODER & ~MODER_BITS(pin)) | (1u << (pin * 2));
}

void stub_gpio_init(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOBEN;
    (void)RCC->IOPENR;

    /* Columns idle high before they become outputs. */
    GPIOB->BSRR = KBD_COLS;

    for (unsigned int pin = 3; pin <= 6; pin++)
        pin_output(pin);

    pin_input_pullup(10);
    for (unsigned int pin = 12; pin <= 15; pin++)
        pin_input_pullup(pin);
}

bool stub_ptt_pressed(void)
{
    return !(GPIOB->IDR & PTT_PIN);
}

/* -------------------------------------------------------------------------- */
/* Keypad scan: same matrix and debounce as KEYBOARD_Poll(), without the       */
/* K5Viewer serial injection and without touching the firmware globals.       */
/* -------------------------------------------------------------------------- */

static const uint8_t keymap[5][4] = {
    {STUB_KEY_SIDE1, STUB_KEY_SIDE2, STUB_KEY_INVALID, STUB_KEY_INVALID},
    {STUB_KEY_MENU, STUB_KEY_1, STUB_KEY_4, STUB_KEY_7},
    {STUB_KEY_UP, STUB_KEY_2, STUB_KEY_5, STUB_KEY_8},
    {STUB_KEY_DOWN, STUB_KEY_3, STUB_KEY_6, STUB_KEY_9},
    {STUB_KEY_EXIT, STUB_KEY_STAR, STUB_KEY_0, STUB_KEY_F},
};

int stub_key_poll(void)
{
    int key = STUB_KEY_INVALID;

    /* Scan every column, never break early: a column left low would keep the
     * stock's own scan from seeing a clean keypad. */
    for (unsigned int col = 0; col < 5; col++) {
        uint32_t reg = 0;
        uint32_t stable = 0;

        GPIOB->BSRR = KBD_COLS;
        if (col > 0)
            GPIOB->BRR = KBD_COL(col - 1);

        for (unsigned int k = 0; k < 8; k++) {
            stub_delay_us(10);
            const uint32_t now = KBD_ROWS & GPIOB->IDR;
            if (now != reg) {
                stable = 0;
                reg = now;
            } else {
                stable++;
            }
            if (stable >= 2)
                break;
        }

        if (stable < 2)
            continue;

        for (unsigned int row = 0; row < 4; row++) {
            if (!(reg & (1u << (15 - row)))) {
                key = keymap[col][row];
                break;
            }
        }

        if (key != STUB_KEY_INVALID)
            break;
    }

    GPIOB->BSRR = KBD_COLS;
    return key;
}
