#ifndef STOCKPATCH_STUB_UI_H
#define STOCKPATCH_STUB_UI_H

#include <stdbool.h>
#include <stdint.h>

#define UI_WIDTH 128
#define UI_PAGES 8

/*
 * Page layout, identical to the KLEANFW selector:
 *   page 0      status banner
 *   page 1      decorative dotted line
 *   pages 2..6  the five slots
 *   page 7      key hints / restore progress
 *
 * "row" below is the KLEANFW gFrameBuffer index (0..6): row N is shown on LCD
 * page N+1, and its text baseline sits at pixel y = N*8 + 1.
 */
extern uint8_t stub_fb[UI_PAGES][UI_WIDTH];

void ui_init(void);
void ui_backlight(bool on);

void ui_clear(void);
void ui_blit_page(uint8_t page);
void ui_blit_all(void);

void ui_banner(const char *text);
void ui_dotted_line(void);

/* 6x7 variable-width font (gFontSmallPacked), clipped to the row. */
void ui_row_small(const char *text, uint8_t x, uint8_t row);
/* 3x5 font (gFont3x5), at pixel y = row*8 + 1. */
void ui_row_tiny(const char *text, uint8_t x, uint8_t row);
/* Capsule around a short label: text at x_text, capsule ending at end_x. */
void ui_row_inverse(uint8_t row, uint8_t x_text, uint8_t end_x);
/* Wide selection box around a firmware name (start/end are box edges). */
void ui_row_box(uint8_t row, uint8_t start, uint8_t end);

#endif
