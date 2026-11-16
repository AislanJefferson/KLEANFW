/*
 * Minimal ST7565 UI for the stock stub.
 *
 * This is a deliberately small copy of the KLEANFW display path: SPI1, the
 * panel init sequence and a few drawing primitives. It does not use
 * App/driver/st7565.c, which pulls the settings globals, printf and the
 * screenshot/K5Viewer hooks.
 */

#include "stub_ui.h"

#include "stub.h"

#include "font.h"
#include "py32f071_ll_bus.h"
#include "py32f071_ll_gpio.h"
#include "py32f071_ll_spi.h"

#define LCD_CS_PIN LL_GPIO_PIN_2 /* PB2 */
#define LCD_A0_PIN LL_GPIO_PIN_6 /* PA6 */
#define LCD_BL_PIN LL_GPIO_PIN_8 /* PF8 */

uint8_t stub_fb[UI_PAGES][UI_WIDTH] __attribute__((aligned(4)));

/* -------------------------------------------------------------------------- */
/* Bus                                                                        */
/* -------------------------------------------------------------------------- */

static void lcd_byte(uint8_t value)
{
    while (!LL_SPI_IsActiveFlag_TXE(SPI1))
        ;
    LL_SPI_TransmitData8(SPI1, value);
    while (!LL_SPI_IsActiveFlag_RXNE(SPI1))
        ;
    (void)LL_SPI_ReceiveData8(SPI1);
}

static void lcd_cmd(uint8_t value)
{
    LL_GPIO_ResetOutputPin(GPIOA, LCD_A0_PIN);
    lcd_byte(value);
}

static void lcd_cs(bool select)
{
    if (select)
        LL_GPIO_ResetOutputPin(GPIOB, LCD_CS_PIN);
    else
        LL_GPIO_SetOutputPin(GPIOB, LCD_CS_PIN);
}

static void lcd_spi_init(void)
{
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SPI1);
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA | LL_IOP_GRP1_PERIPH_GPIOB |
                            LL_IOP_GRP1_PERIPH_GPIOF);

    LL_GPIO_InitTypeDef gpio;
    LL_GPIO_StructInit(&gpio);
    gpio.Mode       = LL_GPIO_MODE_ALTERNATE;
    gpio.Alternate  = LL_GPIO_AF0_SPI1;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;

    gpio.Pin  = LL_GPIO_PIN_5; /* SCK */
    gpio.Pull = LL_GPIO_PULL_UP;
    LL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin  = LL_GPIO_PIN_7; /* SDA */
    gpio.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOA, &gpio);

    /* CS, A0 and the backlight as plain outputs. */
    LL_GPIO_SetOutputPin(GPIOB, LCD_CS_PIN);
    LL_GPIO_SetOutputPin(GPIOA, LCD_A0_PIN);
    LL_GPIO_ResetOutputPin(GPIOF, LCD_BL_PIN);

    LL_GPIO_StructInit(&gpio);
    gpio.Mode       = LL_GPIO_MODE_OUTPUT;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Speed      = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Pin        = LCD_CS_PIN;
    LL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = LCD_A0_PIN;
    LL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = LCD_BL_PIN;
    LL_GPIO_Init(GPIOF, &gpio);

    LL_SPI_InitTypeDef spi;
    LL_SPI_StructInit(&spi);
    spi.TransferDirection = LL_SPI_FULL_DUPLEX;
    spi.Mode              = LL_SPI_MODE_MASTER;
    spi.DataWidth         = LL_SPI_DATAWIDTH_8BIT;
    spi.ClockPolarity     = LL_SPI_POLARITY_HIGH;
    spi.ClockPhase        = LL_SPI_PHASE_2EDGE;
    spi.NSS               = LL_SPI_NSS_SOFT;
    spi.BitOrder          = LL_SPI_MSB_FIRST;
    spi.CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE;
    spi.BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV64;
    LL_SPI_Init(SPI1, &spi);
    LL_SPI_Enable(SPI1);
}

/* Same sequence as ST7565_Init(), with the firmware's default contrast (10)
 * and no display inversion, since the stub has no settings to read. */
static void lcd_init(void)
{
    static const uint8_t cmds[] = {
        0xA2,     /* bias 1/9 */
        0xC0,     /* COM direction normal */
        0xA1,     /* SEG direction reverse */
        0xA6,     /* inverse display off */
        0xA4,     /* all pixels off */
        0x24,     /* regulation ratio 5.0 */
        0x81, 31, /* set contrast */
        0x2F,     /* power circuit on */
        0x40,     /* start line 0 */
        0xAF,     /* display on */
    };

    lcd_cs(true);
    lcd_cmd(0xE2); /* software reset */
    stub_delay_ms(120);

    for (unsigned int i = 0; i < 8; i++)
        lcd_cmd(cmds[i]);

    lcd_cmd(0x28 | 0x03);
    stub_delay_ms(1);
    lcd_cmd(0x28 | 0x06);
    stub_delay_ms(1);
    for (unsigned int i = 0; i < 4; i++)
        lcd_cmd(0x28 | 0x07);
    stub_delay_ms(40);

    lcd_cmd(0x40);
    lcd_cmd(0xAF);
    lcd_cs(false);
}

void ui_init(void)
{
    lcd_spi_init();
    lcd_init();

    ui_clear();
    ui_blit_all();
}

void ui_backlight(bool on)
{
    if (on)
        LL_GPIO_SetOutputPin(GPIOF, LCD_BL_PIN);
    else
        LL_GPIO_ResetOutputPin(GPIOF, LCD_BL_PIN);
}

/* -------------------------------------------------------------------------- */
/* Blit                                                                       */
/* -------------------------------------------------------------------------- */

void ui_blit_page(uint8_t page)
{
    if (page >= UI_PAGES)
        return;

    lcd_cs(true);
    lcd_cmd(0x40);
    lcd_cmd((uint8_t)(0xB0u | page));
    lcd_cmd(0x10);
    lcd_cmd(0x04); /* the visible RAM starts at column 4 */
    LL_GPIO_SetOutputPin(GPIOA, LCD_A0_PIN);
    for (unsigned int x = 0; x < UI_WIDTH; x++)
        lcd_byte(stub_fb[page][x]);
    lcd_cs(false);
}

void ui_blit_all(void)
{
    for (uint8_t page = 0; page < UI_PAGES; page++)
        ui_blit_page(page);
}

void ui_clear(void)
{
    for (uint8_t page = 0; page < UI_PAGES; page++)
        for (unsigned int x = 0; x < UI_WIDTH; x++)
            stub_fb[page][x] = 0;
}

/* -------------------------------------------------------------------------- */
/* Text                                                                       */
/* -------------------------------------------------------------------------- */

/* gFontSmallPacked: 6 columns of 7 bits per glyph, packed back to back. */
void ui_row_small(const char *text, uint8_t x, uint8_t row)
{
    uint8_t *line = stub_fb[row + 1u];

    for (unsigned int i = 0; text[i]; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c <= ' ' || c >= 127)
            continue;

        const unsigned int index = c - ' ' - 1u;
        uint32_t bit = index * FONT_SMALL_WIDTH * 7u;

        for (uint8_t col = 0; col < FONT_SMALL_WIDTH; col++) {
            const uint32_t byte = bit >> 3;
            const uint16_t packed =
                (uint16_t)gFontSmallPacked[byte] | ((uint16_t)gFontSmallPacked[byte + 1u] << 8);
            const unsigned int xx = x + i * (FONT_SMALL_WIDTH + 1u) + col;
            if (xx < UI_WIDTH)
                line[xx] = (uint8_t)((packed >> (bit & 7u)) & 0x7Fu);
            bit += 7u;
        }
    }
}

static void pixel(uint8_t x, uint8_t y, bool on)
{
    const uint8_t mask = (uint8_t)(1u << (y & 7u));

    if (x >= UI_WIDTH || y >= UI_PAGES * 8u)
        return;

    if (on)
        stub_fb[y >> 3][x] |= mask;
    else
        stub_fb[y >> 3][x] &= (uint8_t)~mask;
}

static void tiny_glyph(const char *text, unsigned int i, uint8_t x, uint8_t y)
{
    const unsigned char c = (unsigned char)text[i];
    const uint8_t *glyph = gFont3x5[c >= 0x20 ? c - 0x20 : 0];

    for (uint8_t col = 0; col < 3; col++) {
        uint8_t bits = glyph[col];
        for (uint8_t j = 0; j < 6; j++) {
            if (bits & 1u)
                pixel((uint8_t)(x + i * 4u + col), (uint8_t)(y + j), true);
            bits >>= 1;
        }
    }
}

void ui_row_tiny(const char *text, uint8_t x, uint8_t row)
{
    /* Same mapping as the firmware: gFrameBuffer[row] is shown on LCD page
     * row+1, so row N occupies pixels (N+1)*8 .. (N+1)*8+7. */
    const uint8_t y = (uint8_t)((row + 1u) * 8u + 1u);

    for (unsigned int i = 0; text[i]; i++)
        tiny_glyph(text, i, x, y);
}

void ui_row_inverse(uint8_t row, uint8_t x_text, uint8_t end_x)
{
    uint8_t *line = stub_fb[row + 1u];

    if (x_text < 2u || end_x >= UI_WIDTH)
        return;

    line[x_text - 2u] ^= 0x3E;
    for (unsigned int x = x_text - 1u; x < end_x; x++)
        line[x] ^= 0x7Fu;
    line[end_x] ^= 0x3E;
}

void ui_row_box(uint8_t row, uint8_t start, uint8_t end)
{
    uint8_t *line = stub_fb[row + 1u];
    uint8_t *above = stub_fb[row];

    if (end >= UI_WIDTH)
        return;

    line[start] ^= 0x7Fu;
    for (unsigned int x = start + 1u; x < end; x++) {
        line[x] ^= 0xFFu;
        above[x] ^= 0x80u;
    }
    line[end] ^= 0x7Fu;
}

void ui_banner(const char *text)
{
    unsigned int len = 0;
    while (text[len])
        len++;

    const unsigned int width = len * 4u;
    const uint8_t x = (uint8_t)((UI_WIDTH - width) / 2u);
    const uint8_t end = (uint8_t)(x + width - 1u);

    for (unsigned int i = 0; i < UI_WIDTH; i++)
        stub_fb[0][i] = 0;

    for (unsigned int i = 0; i < len; i++)
        tiny_glyph(text, i, x, 1);

    stub_fb[0][x - 2u] ^= 0x3E;
    for (unsigned int i = x - 1u; i <= end; i++)
        stub_fb[0][i] ^= 0x7Fu;
    stub_fb[0][end + 1u] ^= 0x3E;
}

void ui_dotted_line(void)
{
    for (unsigned int x = 2; x < UI_WIDTH - 2; x++)
        stub_fb[1][x] |= 0x08u;
}
