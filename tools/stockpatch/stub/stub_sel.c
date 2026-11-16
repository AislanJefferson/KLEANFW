/* Copyright 2026 AislanJefferson
 * https://github.com/AislanJefferson/KLEANFW
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

/*
 * The multiboot selector, shown when MENU is held at power-on.
 *
 * Layout and key model follow App/ui/multiboot.c, but the drawing is local
 * (stub_ui.c) and the slot logic is the firmware's own mb_flash_logic.c, so
 * the on-flash format and the restore path are identical.
 */

#include "stub.h"
#include "stub_mb.h"
#include "stub_ui.h"

#include <stddef.h>
#include <string.h>

#define NAME_X 15
#define NAME_BOX_START 12
#define NAME_BOX_END 96
#define HINT_ROW 6

static void copy_label(char *dst, unsigned int cap, const char *src, unsigned int src_cap)
{
    unsigned int n = 0;

    while (n + 1u < cap && n < src_cap && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static void copy_version(char *dst, unsigned int cap, const mb_slot_header_t *header)
{
    unsigned int n = 0;

    for (unsigned int i = 0; i + 1u < cap && i < MB_VERSION_LEN && header->fw_version[i]; i++)
        dst[n++] = header->fw_version[i];
    dst[n] = 0;
}

static void hints(const char *menu_action, const char *exit_action)
{
    const unsigned int exit_len = strlen(exit_action);
    const uint8_t exit_x = (uint8_t)(124u - exit_len * 4u - 6u - 16u);

    ui_row_tiny("MENU", 4, HINT_ROW);
    ui_row_inverse(HINT_ROW, 4, 4 + 16);
    ui_row_tiny(menu_action, 4 + 16 + 6, HINT_ROW);

    ui_row_tiny("EXIT", exit_x, HINT_ROW);
    ui_row_inverse(HINT_ROW, exit_x, (uint8_t)(exit_x + 16u));
    ui_row_tiny(exit_action, (uint8_t)(exit_x + 16u + 6u), HINT_ROW);
}

static void screen_header(void)
{
    ui_clear();
    ui_banner("STOCK MULTIBOOT");
    ui_dotted_line();
}

static void message(const char *line1, const char *line2)
{
    screen_header();
    if (line1)
        ui_row_small(line1, 2, 2);
    if (line2)
        ui_row_small(line2, 2, 3);
    ui_row_tiny("PRESS ANY KEY", 2, HINT_ROW);
    ui_blit_all();
}

static void render_slots(uint8_t selected, const mb_slot_header_t *headers,
                         const uint8_t *status)
{
    screen_header();

    for (uint8_t slot = 0; slot < MB_SLOT_COUNT; slot++) {
        const uint8_t row = (uint8_t)(slot + 1u);
        char index[2];

        /* Slot 0 is the auto-backed-up main firmware: label it 'M'. */
        index[0] = (slot == 0) ? 'M' : (char)('0' + slot);
        index[1] = 0;
        ui_row_small(index, 2, row);

        if (status[slot] == MB_OK) {
            char name[13];
            char version[9];

            if (headers[slot].name[0])
                copy_label(name, sizeof name, headers[slot].name, MB_NAME_LEN);
            else
                copy_label(name, sizeof name, headers[slot].fw_version, MB_VERSION_LEN);
            ui_row_small(name, NAME_X, row);

            copy_version(version, sizeof version, &headers[slot]);
            if (version[0]) {
                const unsigned int len = strlen(version);
                ui_row_tiny(version, (uint8_t)(126u - len * 4u), row);
            }
        } else {
            ui_row_small(stub_slot_error(status[slot]), NAME_X, row);
        }

        if (slot == selected)
            ui_row_box(row, NAME_BOX_START, NAME_BOX_END);
    }

    hints("SELECT", "QUIT");
    ui_blit_all();
}

static void render_confirm(uint8_t slot)
{
    char title[] = "Restore slot 0?";
    title[13] = (char)('0' + slot);

    screen_header();
    ui_row_small(slot == 0 ? "Restore Main?" : title, 2, 3);
    hints("CONFIRM", "BACK");
    ui_blit_all();
}

static void wait_release(void)
{
    uint8_t stable = 0;

    while (stable < 10u) {
        if (!stub_ptt_pressed() && stub_key_poll() == STUB_KEY_INVALID)
            stable++;
        else
            stable = 0;
        stub_delay_ms(10);
    }
}

static int get_key(void)
{
    for (;;) {
        const int key = stub_key_poll();

        if (key != STUB_KEY_INVALID) {
            stub_delay_ms(30);
            if (stub_key_poll() == key) {
                while (stub_key_poll() != STUB_KEY_INVALID)
                    stub_delay_ms(10);
                return key;
            }
        }
        stub_delay_ms(10);
    }
}

/* Empty gauge that the RAM copier fills as it reflashes. Same rounded outline
 * and hatch pattern as mb_draw_progress_outline() in App/ui/multiboot.c; the
 * copier only writes the filled columns into this same buffer, so it has to be
 * prepared here or the bar is drawn over whatever text row 6 holds. */
static void draw_progress_outline(void)
{
    uint8_t *line = stub_fb[7]; /* row 6 */

    line[3] = 0x0Cu;
    line[4] = 0x12u;
    line[123] = 0x12u;
    line[124] = 0x0Cu;
    for (unsigned int x = 5; x < 123u; x++)
        line[x] = 0x21u;
}

static void render_restore(uint8_t slot)
{
    char title[] = "Restore slot 0";
    title[13] = (char)('0' + slot);

    screen_header();
    ui_row_small(slot == 0 ? "Restore Main" : title, 2, 1);
    ui_row_small("DO NOT POWER OFF", 2, 3);
    ui_row_small("Writing / Verify", 2, 5);
    draw_progress_outline();

    for (uint8_t page = 0; page < UI_PAGES; page++)
        ui_blit_page(page);
}

void stub_selector(void)
{
    mb_slot_header_t headers[MB_SLOT_COUNT];
    uint8_t status[MB_SLOT_COUNT];
    uint8_t selected;

    /* Clear and blit before the backlight comes on, so the random power-on
     * contents of the display RAM are never visible. */
    ui_init();
    message("Release keys", NULL);
    ui_backlight(true);
    wait_release();

    message("Scanning slots...", "Please wait");
    stub_scan_slots(headers, status);

    /* Land the cursor on the slot the radio actually booted from. */
    selected = stub_running_slot();
    if (selected >= MB_SLOT_COUNT || status[selected] != MB_OK) {
        for (uint8_t slot = 0; slot < MB_SLOT_COUNT; slot++) {
            if (status[slot] == MB_OK) {
                selected = slot;
                break;
            }
        }
    }

    for (;;) {
        render_slots(selected, headers, status);

        const int key = get_key();

        if (key == STUB_KEY_EXIT) {
            /* The stock's own startup reads the keypad: make sure the MENU
             * press that opened the selector is not seen as a fresh press. */
            (void)stub_key_poll();
            return;
        }

        if (key == STUB_KEY_UP) {
            selected = (selected == 0u) ? (uint8_t)(MB_SLOT_COUNT - 1u)
                                        : (uint8_t)(selected - 1u);
            continue;
        }

        if (key == STUB_KEY_DOWN) {
            selected = (uint8_t)((selected + 1u == MB_SLOT_COUNT) ? 0u : selected + 1u);
            continue;
        }

        if (key != STUB_KEY_MENU)
            continue;

        if (status[selected] != MB_OK) {
            message("SLOT NOT VALID", stub_slot_error(status[selected]));
            (void)get_key();
            continue;
        }

        render_confirm(selected);
        if (get_key() != STUB_KEY_MENU)
            continue;

        render_restore(selected);

        /* Binds the slot to its own config bank before reflashing, then
         * reflashes and resets. Only returns when the slot is refused. */
        const uint8_t err = stub_restore_slot(selected, stub_fb[7]);
        status[selected] = err;
        message("RESTORE REFUSED", stub_slot_error(err));
        (void)get_key();
        stub_scan_slots(headers, status);
    }
}
