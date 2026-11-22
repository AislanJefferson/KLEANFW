/* Copyright 2026 AislanJefferson
 * Licensed under the Apache License, Version 2.0 (see LICENSE).
 *
 * The real aprsrx_app.c on a stub API, for its QR Map hand-off (check_qr.py).
 *
 *   host_qr <assets.bin> <cfg view> <installed 0|1> <step>...
 *
 * Steps, one per housekeeping slot group:
 *   f:<hex>   a received frame, FCS bytes included, through accept()
 *   k:<c>     a key press: U D X(EXIT) M(MENU) * or a digit
 *   ^:<c>     as first step: that key is still held when the app starts
 * EXIT is pressed after the last step.
 *
 * Prints "B <text>" for each bottom row drawn, "H <name> <hex>" for a hand-off,
 * "O" / "C" for a capture open / close, "E" if the ADC is read while the capture
 * is closed, and "R" when app_main returns. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define section(x) used
#include "../aprsrx_app.c"
#undef section

static uint8_t fb[8][128], status[128], assets[4096], cfg[2];
static bool installed, captureOpen;
static char **steps;
static int nSteps, step;
static unsigned phase, heldPolls;
static uint8_t heldKey = APP_KEY_INVALID;
static uint32_t cycles;

static uint8_t keyCode(char c){
    switch (c) {
    case 'U': return APP_KEY_UP;
    case 'D': return APP_KEY_DOWN;
    case 'X': return APP_KEY_EXIT;
    case 'M': return APP_KEY_MENU;
    case '*': return APP_KEY_STAR;
    default:  return (uint8_t)(c - '0');
    }
}

static void injectFrame(const char *hex){
    static sl_t m;
    m.n = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned b;
        sscanf(hex, "%2x", &b);
        m.buf[m.n++] = (uint8_t)b;
    }
    accept(&m);
}

/* Each key: two polls down, two released. */
static uint8_t s_get_key(void){
    static bool seeded;
    if (heldPolls) { heldPolls--; seeded = true; return heldKey; }
    if (!seeded) { seeded = true; return APP_KEY_INVALID; }   /* app_main's launch poll */
    if (step >= nSteps) {
        const unsigned p = phase++ & 3u;
        return p < 2u ? APP_KEY_EXIT : APP_KEY_INVALID;
    }
    const char *s = steps[step];
    if (s[0] == 'f') { injectFrame(s + 2); step++; return APP_KEY_INVALID; }
    const unsigned p = phase++;
    if (p >= 3u) { phase = 0; step++; }
    return p < 2u ? keyCode(s[2]) : APP_KEY_INVALID;
}

static void s_nop(void){}
static void s_clear(void){ memset(fb, 0, sizeof fb); }
static void s_status_clear(void){ memset(status, 0, sizeof status); }
static void s_tiny(const char *s, uint8_t x, uint8_t y, bool sb, bool fill){
    (void)sb; (void)fill;
    if (x == 0 && y == 49) printf("B %s\n", s);
}
static void s_normal(const char *s, uint8_t a, uint8_t b, uint8_t c){ (void)s; (void)a; (void)b; (void)c; }
static void s_inverse(const char *s, uint8_t x, uint8_t l, bool b, bool f, uint8_t e){
    (void)s; (void)x; (void)l; (void)b; (void)f; (void)e;
}
static void s_freq(uint32_t f, uint8_t x, uint8_t l){ (void)f; (void)x; (void)l; }
static void s_delay(uint32_t ms){ (void)ms; }
static void s_bool(bool b){ (void)b; }
static void s_af(uint8_t m){ (void)m; }
static int16_t s_rssi(void){ return -89; }
static uint16_t s_bk(uint8_t r){ (void)r; return 0; }
static uint32_t s_rx(void){ return 14480000u; }
static uint32_t s_ticks(void){ static uint32_t t; return t += 2000u; }
static uint32_t s_cycles(void){ return cycles += CYC_PER_SAMPLE; }
static char *s_i32(char *o, int32_t v){ return o + sprintf(o, "%d", v); }
static void s_cfg_load(uint8_t *b, uint8_t n){ memcpy(b, cfg, n); }
static void s_cfg_save(const uint8_t *b, uint8_t n){ (void)b; (void)n; }
static void s_open(void){ captureOpen = true; printf("O\n"); }
static void s_close(void){ if (captureOpen) printf("C\n"); captureOpen = false; }
static uint16_t s_sample(void){
    if (!captureOpen) { printf("E\n"); exit(3); }
    return 2048u;
}
static bool s_handoff(const char *name, const void *m, uint8_t n){
    s_close();                         /* as the resident app_handoff() does */
    if (!installed) { printf("H- %s\n", name); return false; }
    printf("H %s ", name);
    for (unsigned i = 0; i < n; i++) printf("%02x", ((const uint8_t *)m)[i]);
    printf("\n");
    return true;
}
static void s_tune(void){}
static gogu_api_t gogu;
static uint16_t s_asset(uint16_t off, void *b, uint16_t n){
    if (off == APP_GOGU_API_QUERY) { const gogu_api_t *p = &gogu; memcpy(b, &p, sizeof p); return sizeof p; }
    memcpy(b, assets + off, n);
    return n;
}

int main(int argc, char **argv){
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || !fread(assets, 1, sizeof assets, f)) return 2;
    fclose(f);
    cfg[0] = (uint8_t)atoi(argv[2]);
    cfg[1] = 0;
    installed = atoi(argv[3]) != 0;
    steps = argv + 4;
    nSteps = argc - 4;
    if (nSteps && steps[0][0] == '^') {
        heldKey = keyCode(steps[0][2]);
        heldPolls = 4;
        step = 1;
    }
    gogu.radio_tune_aprs = s_tune;

    static app_api_t api;
    api.fb = fb; api.status_line = status;
    api.display_clear = s_clear; api.status_clear = s_status_clear;
    api.print_tiny = s_tiny; api.print_normal = s_normal; api.print_bold = s_normal;
    api.print_inverse = s_inverse; api.draw_frequency = s_freq;
    api.blit_full = s_nop; api.blit_status = s_nop; api.draw_battery = s_nop;
    api.battery_sample = s_nop; api.backlight_on = s_nop; api.backlight_update = s_nop;
    api.get_key = s_get_key; api.delay_ms = s_delay;
    api.set_af = s_af; api.audio_path = s_bool; api.rssi_dbm = s_rssi; api.bk_read = s_bk;
    api.rx_freq = s_rx; api.ticks_ms = s_ticks; api.cycles_48mhz = s_cycles;
    api.append_i32 = s_i32; api.cfg_load = s_cfg_load; api.cfg_save = s_cfg_save;
    api.af_capture_open = s_open; api.af_capture_close = s_close;
    api.af_capture_sample = s_sample; api.handoff = s_handoff; api.asset_read = s_asset;
    app_main(&api);
    printf("R\n");
    return 0;
}
