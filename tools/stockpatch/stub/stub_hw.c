/*
 * Hand the hardware back to the stock exactly as the stub found it.
 *
 * The unpatched stock is only known to work when it starts from whatever state
 * the bootloader leaves. The stub reconfigures GPIOA/B/F, SPI1, SPI2, SysTick
 * and their clocks, so everything it can touch is captured on entry and put
 * back right before control returns to the stock's Reset_Handler.
 *
 * No initialiser anywhere: there is no .data loader before the stock's __main.
 */

#include "stub.h"

#include "py32f0xx.h"

struct gpio_regs {
    uint32_t moder;
    uint32_t otyper;
    uint32_t ospeedr;
    uint32_t pupdr;
    uint32_t odr;
    uint32_t afr0;
    uint32_t afr1;
};

static struct {
    uint32_t iopenr;
    uint32_t apbenr1;
    uint32_t apbenr2;
    struct gpio_regs gpio[3];
    uint32_t spi1_cr1;
    uint32_t spi1_cr2;
    uint32_t spi2_cr1;
    uint32_t spi2_cr2;
    uint32_t systick_ctrl;
    uint32_t systick_load;
} saved;

#define IOPENR_PORTS (RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN | RCC_IOPENR_GPIOFEN)

static GPIO_TypeDef *port(unsigned int i)
{
    return i == 0 ? GPIOA : (i == 1 ? GPIOB : GPIOF);
}

void stub_hw_save(void)
{
    saved.iopenr  = RCC->IOPENR;
    saved.apbenr1 = RCC->APBENR1;
    saved.apbenr2 = RCC->APBENR2;

    /* Registers of a gated peripheral keep their value; the clock is only
     * needed to read them back. */
    RCC->IOPENR = saved.iopenr | IOPENR_PORTS;
    (void)RCC->IOPENR;
    for (unsigned int i = 0; i < 3; i++) {
        GPIO_TypeDef *p = port(i);
        saved.gpio[i].moder   = p->MODER;
        saved.gpio[i].otyper  = p->OTYPER;
        saved.gpio[i].ospeedr = p->OSPEEDR;
        saved.gpio[i].pupdr   = p->PUPDR;
        saved.gpio[i].odr     = p->ODR;
        saved.gpio[i].afr0    = p->AFR[0];
        saved.gpio[i].afr1    = p->AFR[1];
    }

    RCC->APBENR1 = saved.apbenr1 | RCC_APBENR1_SPI2EN;
    RCC->APBENR2 = saved.apbenr2 | RCC_APBENR2_SPI1EN;
    (void)RCC->APBENR2;
    saved.spi1_cr1 = SPI1->CR1;
    saved.spi1_cr2 = SPI1->CR2;
    saved.spi2_cr1 = SPI2->CR1;
    saved.spi2_cr2 = SPI2->CR2;

    saved.systick_ctrl = SysTick->CTRL;
    saved.systick_load = SysTick->LOAD;
}

static void spi_restore(SPI_TypeDef *spi, volatile uint32_t *rstr, uint32_t rst_bit,
                        uint32_t cr1, uint32_t cr2)
{
    /* A peripheral reset clears SR and both FIFOs, which writing CR1/CR2 back
     * cannot do. */
    *rstr |= rst_bit;
    *rstr &= ~rst_bit;

    if (cr1 != 0u || cr2 != 0u) {
        spi->CR2 = cr2;
        spi->CR1 = cr1 & ~SPI_CR1_SPE;
        spi->CR1 = cr1;
    }
}

void stub_hw_restore(void)
{
    /* Peripheral clocks still on from stub_hw_save(): SPI first, while the
     * pins are still driving their idle levels. */
    spi_restore(SPI1, &RCC->APBRSTR2, RCC_APBRSTR2_SPI1RST, saved.spi1_cr1, saved.spi1_cr2);
    spi_restore(SPI2, &RCC->APBRSTR1, RCC_APBRSTR1_SPI2RST, saved.spi2_cr1, saved.spi2_cr2);

    RCC->IOPENR |= IOPENR_PORTS;
    (void)RCC->IOPENR;
    for (unsigned int i = 0; i < 3; i++) {
        GPIO_TypeDef *p = port(i);
        /* Output latch before the mode, so no pin glitches on the way back. */
        p->ODR     = saved.gpio[i].odr;
        p->OTYPER  = saved.gpio[i].otyper;
        p->OSPEEDR = saved.gpio[i].ospeedr;
        p->PUPDR   = saved.gpio[i].pupdr;
        p->AFR[0]  = saved.gpio[i].afr0;
        p->AFR[1]  = saved.gpio[i].afr1;
        p->MODER   = saved.gpio[i].moder;
    }

    SysTick->CTRL = 0;
    SysTick->LOAD = saved.systick_load;
    SysTick->VAL  = 0;
    SysTick->CTRL = saved.systick_ctrl & (SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk |
                                          SysTick_CTRL_ENABLE_Msk);

    RCC->APBENR1 = saved.apbenr1;
    RCC->APBENR2 = saved.apbenr2;
    RCC->IOPENR  = saved.iopenr;
    (void)RCC->IOPENR;
}
