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

#include <string.h>

#include "app/scr_hop.h"

#ifdef ENABLE_GOGUFW_CRYPTO

#include "app/messenger_store.h"
#include "driver/bk4819.h"
#include "driver/bk4819-regs.h"
#include "driver/system.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "scheduler.h"
#include "settings.h"

#define HOP_MAGIC      0x47u     /* word0 = 'G' << 8 | resync index */
/* Burst check salt, bumped with each incompatible change so radios with an
 * older HOP reject the burst as a wrong CryKey instead of hopping wrongly:
 * 0x48505351 16 carriers, 0x48505352 8 far carriers, 0x48505353 adds the
 * slot-1 repeat, 0x48505354 resyncs at 8 s then every 16 s (resync_ms()). */
#define HOP_CHECK_N    0x48505354u
#define HOP_SLOT_MS    500u
/* Burst idx ends its 8th byte at t0 + resync_ms(idx) + HOP_POS_MS: mid-slot,
 * away from hop boundaries. The TX never moves t0, so a receiver that misses
 * a resync stays aligned. */
#define HOP_RESYNC1_MS 8000u    /* first full resync: drift measured early */
#define HOP_RESYNC_MS  16000u   /* then one every HOP_RESYNC_MS */
#define HOP_POS_MS     290u
/* RX keeps hopping through fades: stop only after this long without RX. */
#define HOP_HANG_MS    2000u
/* RX guard mute around each slot boundary. Covers the chips' internal
 * latencies and one byte of REG_59 preamble uncertainty (6.7 ms). */
#define HOP_GUARD_MS   8u
/* REG_59 0x28F8: 16-byte preamble + 4-byte sync, then the 8 data bytes the
 * RX FIFO (threshold 4 words, REG_5E 0x3204) reports as "almost full".
 * 1200 bps -> 20/3 ms per byte. */
#define HOP_ANCHOR_MS  (((16u + 4u + 8u) * 20u + 1u) / 3u)
/* Whole burst on air: preamble + sync + 8 data + 2 CRC bytes. */
#define HOP_BURST_MS   (((16u + 4u + 8u + 2u) * 20u + 2u) / 3u)
/* RX audio output stays muted this long into a resync slot: the TX sends no
 * voice there, and its burst ends at HOP_POS_MS + 2 CRC bytes (~303 ms). */
#define HOP_RS_MUTE_MS (HOP_POS_MS + 30u)
/* RX drift tracking between precise resync stamps. Below the dead band (twice
 * the worst stamp-difference jitter: 2 ms TX start, ms truncation, IRQ
 * latency) no correction is applied, so drift-free radios behave exactly as
 * without it. Over 1/32 (~3 %) the measurement is taken as bogus. */
#define HOP_DRIFT_DEAD_MS  12u
#define HOP_DRIFT_SPAN     16u   /* resync intervals per baseline */

SCR_HOP_State_t gScrHop __attribute__((section(".noncacheable")));

void SCR_HOP_Reset(void)
{
    memset(&gScrHop, 0, sizeof(gScrHop));
}

/* Resync k starts at resync_ms(k) on the TX slot clock: the seed burst is
 * repeated at slot 1 (the UV-K1 FSK RX can miss a lone frame, as the
 * Messenger PING repeat notes), so a receiver that missed the first one
 * still joins before the voice. The first full resync comes at 8 s, so the
 * clock drift is measured early, then one every HOP_RESYNC_MS (each costs a
 * ~0.3 s mic cut): resync slots 1, 16, 48, 80... */
static uint32_t resync_ms(uint32_t k)
{
    return k < 2u ? k * HOP_SLOT_MS : HOP_RESYNC1_MS + (k - 2u) * HOP_RESYNC_MS;
}

static uint16_t carrier(uint32_t seed, uint32_t slot)
{
    /* 8 carriers over the whole 2600..3500 Hz the BK4819 note allows, 128.6 Hz
     * apart (REG_71: 1032 units = 100 Hz). HOP always runs Wide (TX LPF 3 kHz),
     * so even the top carrier keeps the voice above 500 Hz.
     *
     * A fixed-carrier scrambler understands the slots near its own carrier,
     * so hops must land far apart. Blocks start at the full resyncs (block
     * 0 is slots 0-15, then 32 slots from slots 16, 48, 80...), so a late
     * receiver joins at a block start. Each takes the carriers in the order
     * D + S * slot (mod 8), with D and the stride S (3 or 5) from
     * XTEA(CryKey, seed || block); blocks start on multiples of 8 slots. Each
     * carrier is used once every 8 slots, and the carrier 1, 2 and 3 slots
     * later is at least 3, 2 and 1 carriers away (386, 257 and 129 Hz). The
     * jump into a new block is free, but that slot carries little voice: the
     * resync burst mutes its first ~0.3 s. */
    const uint32_t r = CRYPTO_Word(seed, (slot + 16u) >> 5);
    return (uint16_t)(0x68DCu + ((r + (3u + ((r >> 2) & 2u)) * slot) & 7u) * 1327u);
}

/* Slot clock in TX ms. RX scales the time since the anchor by the measured
 * TX/RX clock ratio (adj = 0: plain difference, as on TX). The correction
 * stops growing after HOP_DRIFT_MAX_MS without a resync (a carrier can keep
 * the session alive): no int32 overflow, and the clock stays monotonic. */
#define HOP_DRIFT_MAX_MS  (1u << 19)   /* ~8.7 min */
static uint32_t elapsed(void)
{
    const int32_t d = (int32_t)(SCHEDULER_GetMs() - gScrHop.anchor);
    const int32_t c = d < (int32_t)HOP_DRIFT_MAX_MS ? d : (int32_t)HOP_DRIFT_MAX_MS;
    return gScrHop.base + (uint32_t)(d + ((c * gScrHop.adj) >> 16));
}

void SCR_HOP_Apply(bool tx)
{
    if (!gScrHop.active && !tx) {
        BK4819_DisableScramble();
        return;
    }
    BK4819_EnableScramble(0);
    BK4819_WriteRegister(BK4819_REG_71, gScrHop.active ? carrier(gScrHop.seed, gScrHop.slot)
                                                       : carrier(0, 0xFFFFFFFFu));
}

static void start(uint32_t seed, uint32_t anchor, uint32_t base, bool rx)
{
    gScrHop.seed    = seed;
    gScrHop.anchor  = anchor;
    gScrHop.base    = base;
    gScrHop.last_rx = SCHEDULER_GetMs();
    gScrHop.slot    = (uint16_t)(elapsed() / HOP_SLOT_MS);
    gScrHop.idx     = 0;
    gScrHop.rx      = rx;
    gScrHop.active  = 1;
    gUpdateDisplay  = true;   // the scrambler icon shows the session
    SCR_HOP_Apply(!rx);
}

static void wait_until(uint32_t ms)
{
    while ((int32_t)(elapsed() - ms) < 0) {}
}

void SCR_HOP_TxBurst(uint16_t idx)
{
    /* idx 0 starts a new over with a fresh seed; later ones resend it. */
    const uint32_t seed = idx ? gScrHop.seed
                              : CRYPTO_Word(((uint32_t)BK4819_ReadRegister(BK4819_REG_67) << 16) ^
                                            SysTick->VAL, SCHEDULER_GetMs());
    const uint16_t word0 = (HOP_MAGIC << 8) | (idx & 0xFFu);
    const uint16_t words[4] = {
        word0, (uint16_t)seed, (uint16_t)(seed >> 16),
        (uint16_t)CRYPTO_Word(seed, HOP_CHECK_N ^ word0),
    };
    const uint16_t irq_mask = BK4819_ReadRegister(BK4819_REG_3F);
    const uint16_t reg30    = BK4819_ReadRegister(BK4819_REG_30);   // voice TX link, mic on

    if (!idx) {
        gScrHop.active = 0;
        gScrHop.adj    = 0;
    }
    BK4819_EnterTxMute();   // silent setup, as the tone routines do
    BK4819_DisableScramble();
    /* HOP voice always runs Wide (RADIO_GetFmBandwidth). Same narrow-lock as
     * Messenger FSK TX: the burst goes out with Narrow (N+: Narrower)
     * deviation, the proven decode path for Wide, Narrow and Narrower
     * receivers alike. */
    BK4819_FilterBandwidth_t bw = BK4819_FILTER_BW_NARROW;
#ifdef ENABLE_FEAT_F4HWN_NARROWER
    if (gSetting_set_nfm == 1)
        bw = BK4819_FILTER_BW_NARROWER;
#endif
    BK4819_SetFilterBandwidth(bw, false);
    BK4819_SetupAircopy();
    /* TX DSP on, mic ADC off: the burst carries the FSK tone only. This, not
     * the AF TX mute, is how every tone in the firmware leaves the mic out. */
    BK4819_EnableTXLink();
    SYSTEM_DelayMs(30);                            // same settle as Messenger TX
    BK4819_WriteRegister(BK4819_REG_5D, (sizeof(words) - 1u) << 8);
    BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_TX_FINISHED);
    BK4819_WriteRegister(BK4819_REG_02, 0);
    BK4819_WriteRegister(BK4819_REG_59, 0x80F8);   // clear TX FIFO, 16-byte preamble
    BK4819_WriteRegister(BK4819_REG_59, 0x00F8);
    for (uint8_t i = 0; i < 4u; i++)
        BK4819_WriteRegister(BK4819_REG_5F, words[i]);
    SYSTEM_DelayMs(20);

    /* Resync: start on the exact ms so the 8th byte ends at its anchor. A late
     * one is skipped: it would re-anchor every receiver to the wrong time. */
    bool send = true;
    if (idx) {
        const uint32_t target = resync_ms(idx) + HOP_POS_MS - HOP_ANCHOR_MS;
        wait_until(target);
        send = elapsed() - target <= 2u;
    }
    /* Every FSK and tone TX in the firmware (Messenger, AirCopy, MDC, DTMF,
     * roger, BK4819_PlayToneRaw) goes on air with the AF TX mute off, so the
     * mute is taken to silence the tone generator too: the burst is never
     * sent muted. The mic stays out through EnableTXLink(). */
    BK4819_ExitTxMute();
    const uint32_t t_start = SCHEDULER_GetMs();
    if (send) {
        BK4819_WriteRegister(BK4819_REG_59, 0x28F8);   // start FSK TX
        /* Never stop before the whole burst is on air, whatever REG_0C says. */
        while (SCHEDULER_GetMs() - t_start < HOP_BURST_MS) {}
        for (uint8_t n = 0; n < 20u && !(BK4819_ReadRegister(BK4819_REG_0C) & 1u); n++)
            SYSTEM_DelayMs(5);
    }
    BK4819_EnterTxMute();

    /* Back to voice without BK4819_ResetFSK(): it would idle REG_30 (TX off). */
    BK4819_WriteRegister(BK4819_REG_02, 0);
    BK4819_WriteRegister(BK4819_REG_59, 0x0068);
    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_WriteRegister(BK4819_REG_58, 0);
    BK4819_WriteRegister(BK4819_REG_3F, irq_mask);
    BK4819_WriteRegister(BK4819_REG_30, reg30);    // mic back for the voice

    BK4819_SetFilterBandwidth(RADIO_GetFmBandwidth(gCurrentVfo, true), false);

    if (idx) {
        /* A late tick runs the resync before the slot switch: catch up. */
        gScrHop.slot = (uint16_t)(elapsed() / HOP_SLOT_MS);
        SCR_HOP_Apply(true);
    } else
        start(seed, t_start + HOP_ANCHOR_MS, HOP_POS_MS, false);
    BK4819_ExitTxMute();
}

bool SCR_HOP_RxStart(const uint16_t *words, uint32_t stamp_ms, bool precise)
{
    const uint32_t seed = (uint32_t)words[1] | ((uint32_t)words[2] << 16);
    const uint8_t  idx  = (uint8_t)words[0];

    if ((words[0] >> 8) != HOP_MAGIC ||
        words[3] != (uint16_t)CRYPTO_Word(seed, HOP_CHECK_N ^ words[0]))
        return false;

    /* A new over (or another transmitter) starts without a clock correction. */
    if (!gScrHop.active || !gScrHop.rx || gScrHop.seed != seed) {
        gScrHop.adj    = 0;
        gScrHop.ref_ok = 0;
    }
    /* Bursts idx and ref_idx were sent exactly n ms apart on the TX
     * clock: compare with the local interval from the first precise stamp.
     * A constant RX latency cancels out; late (imprecise) stamps are skipped. */
    if (precise) {
        const uint8_t span = (uint8_t)(idx - gScrHop.ref_idx);
        if (gScrHop.ref_ok && span - 1u < HOP_DRIFT_SPAN) {
            const uint32_t n   = resync_ms(idx) - resync_ms(gScrHop.ref_idx);
            const uint32_t m   = stamp_ms - gScrHop.ref_stamp;
            const int32_t  err = (int32_t)(n - m);
            const uint32_t a   = err < 0 ? (uint32_t)-err : (uint32_t)err;
            if (a <= HOP_DRIFT_DEAD_MS)
                gScrHop.adj = 0;
            else if (n > HOP_SLOT_MS && a <= n / 32u)   // not the 0.5 s repeat pair
                gScrHop.adj = (int16_t)((err * 65536) / (int32_t)m);
        } else {
            gScrHop.ref_stamp = stamp_ms;
            gScrHop.ref_idx   = idx;
            gScrHop.ref_ok    = 1;
        }
    }
    start(seed, stamp_ms, resync_ms(idx) + HOP_POS_MS, true);
    return true;
}

static void guard_mute(bool mute)
{
    if (mute) {
        gScrHop.reg48 = BK4819_ReadRegister(BK4819_REG_48);
        BK4819_WriteRegister(BK4819_REG_48, gScrHop.reg48 & ~(0x3Fu << 4));
    } else {
        BK4819_WriteRegister(BK4819_REG_48, gScrHop.reg48);
    }
}

void SCR_HOP_Stop(void)
{
    const bool rx_session = gScrHop.active && gScrHop.rx;

    gScrHop.active  = 0;
    gScrHop.rx_wait = 0;
    gUpdateDisplay  = true;   // back to the plain scrambler icon
    if (gScrHop.rs_mute) {    // stopped inside a resync window
        gScrHop.rs_mute = 0;
        BK4819_SetAF(BK4819_AF_FM);
    }
    /* The RX VFO may already be a fixed-scrambler channel (channel change or
     * DW within the hang): restore its state instead of forcing REG_31<1> off. */
    if (rx_session)
        RADIO_ApplyScramble(gRxVfo, false);
}

bool SCR_HOP_VfoUsesHop(const VFO_Info_t *vfo)
{
    return vfo->SCRAMBLING_TYPE == SCRAMBLE_TYPE_HOP &&
           vfo->Modulation == MODULATION_FM && gSetting_ScrambleEnable;
}

bool SCR_HOP_Watching(void)
{
    /* Without MsgRx (or with VOX) the seed burst is never decoded: sleeping
     * costs nothing then. */
    if (!gMessengerConfig.msg_rx)
        return false;
#ifdef ENABLE_VOX
    if (gEeprom.VOX_SWITCH)
        return false;
#endif
    return SCR_HOP_VfoUsesHop(gRxVfo) ||
           (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF &&
            (SCR_HOP_VfoUsesHop(&gEeprom.VfoInfo[0]) ||
             SCR_HOP_VfoUsesHop(&gEeprom.VfoInfo[1])));
}

void SCR_HOP_Tick(void)
{
    if (!gScrHop.active)
        return;

    if (gScrHop.rx) {
        /* Keep the slot clock through squelch fades; DW moving off ends it. */
        if (FUNCTION_IsRx())
            gScrHop.last_rx = SCHEDULER_GetMs();
        if (SCHEDULER_GetMs() - gScrHop.last_rx > HOP_HANG_MS ||
            !SCR_HOP_VfoUsesHop(gRxVfo)) {
            SCR_HOP_Stop();
            return;
        }
    } else if (gCurrentFunction != FUNCTION_TRANSMIT) {
        SCR_HOP_Stop();
        return;
    } else if (elapsed() >= resync_ms(gScrHop.idx + 1u)) {
        SCR_HOP_TxBurst(++gScrHop.idx);   // resync for late/faded receivers
        return;
    }

    const uint32_t boundary = (gScrHop.slot + 1u) * HOP_SLOT_MS;
    const uint32_t now      = elapsed();
    /* RX resync window over: the TX mic is back after its burst. An accepted
     * resync has re-anchored the clock, so this lands on the TX ms. */
    if (gScrHop.rs_mute && now + HOP_SLOT_MS - boundary >= HOP_RS_MUTE_MS) {
        gScrHop.rs_mute = 0;
        BK4819_SetAF(BK4819_AF_FM);   // as RADIO_SetModulation() on FM
    }
    if ((int32_t)(boundary - now) > (int32_t)(10u + HOP_GUARD_MS))
        return;

    /* The boundary falls inside the next 10 ms tick: switch on the exact ms. */
    if (gScrHop.rx) {
        wait_until(boundary - HOP_GUARD_MS);
        guard_mute(true);
    }
    wait_until(boundary);
    gScrHop.slot = (uint16_t)(elapsed() / HOP_SLOT_MS);
    /* Scrambler off means DTMF/beep owns REG_71 (tone generator): leave it. */
    if (BK4819_ReadRegister(BK4819_REG_31) & (1u << 1))
        BK4819_WriteRegister(BK4819_REG_71, carrier(gScrHop.seed, gScrHop.slot));
    if (gScrHop.rx) {
        /* Resync slot (1, 16, 48, 80..., see resync_ms()): the TX cuts
         * its mic and sends the burst, which the descrambler would play as a
         * whistle. Mute the audio output (REG_47, not the REG_48 gain that the
         * guard and the Messenger save and restore) until HOP_RS_MUTE_MS. */
        if (gScrHop.slot == 1u || (gScrHop.slot & 31u) == 16u) {
            gScrHop.rs_mute = 1;
            BK4819_SetAF(BK4819_AF_MUTE);
        }
        wait_until(boundary + HOP_GUARD_MS);
        guard_mute(false);
    }
}

#endif
