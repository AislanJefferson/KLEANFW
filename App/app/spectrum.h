/* Copyright 2023 fagci
 * https://github.com/fagci
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
 *
 * Modified by AislanJefferson for KLEANFW, 2026 (see git history).
 */

#ifndef SPECTRUM_H
#define SPECTRUM_H

#pragma once

#include "keyboard_state.h"

#include "../bitmaps.h"
#include "../board.h"
#include "py32f0xx.h"
#include "../driver/bk4819-regs.h"
#include "../driver/bk4819.h"
#include "../driver/gpio.h"
#include "../driver/keyboard.h"
#include "../driver/st7565.h"
#include "../driver/system.h"
#include "../driver/systick.h"
#include "../external/printf/printf.h"
#include "../font.h"
#include "../helper/battery.h"
#include "../misc.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/helper.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const uint8_t DrawingEndY  = 40;
static const uint8_t DrawingTopY  =  8;  // Reserve top 8px for frequency display (gFrameBuffer[0])

static const uint8_t U8RssiMap[] = {
    121,
    115,
    109,
    103,
    97,
    91,
    85,
    79,
    73,
    63,
};

static const uint16_t scanStepValues[] = {
    1,
    10,
    50,
    100,
    250,
    500,
    625,
    833,
    1000,
    1250,
    1500,
    2000,
    2500,
    5000,
    10000,
};

/* REG_43 during the sweep, one byte per scan step: bits 15:8 are 0 on
 * purpose. <14:12> RF and <11:9> weak-RF filter = 000 (2.0 kHz, doubled to
 * 4.0 kHz in 25k mode), the narrowest, so weak signals stand out of the
 * noise. <8:6> AF Tx LPF2, <5:4> BW mode (00 12.5k, 01 6.25k, 10 25k), <3> 1.
 * These are the words the sweep has always written (the table used to hold
 * 16-bit values that GetBWRegValueForScan() cut to 8 bits). v0.2.0 wrote the
 * whole word: RF/weak-RF up to 5.5/5.5 kHz at 12.5 kHz steps and 7.0/6.0 kHz
 * at 25 kHz, a 2-4 dB higher noise floor that hid weak (FRS) signals. */
static const uint8_t scanStepBWRegValues[] = {
    0x58,   // 1     (S_STEP_0_01kHz,  index 0)  6.25k mode
    0x58,   // 10    (S_STEP_0_1kHz,   index 1)
    0x58,   // 50    (S_STEP_0_5kHz,   index 2)
    0x58,   // 100   (S_STEP_1_0kHz,   index 3)
    0x58,   // 250   (S_STEP_2_5kHz,   index 4)
    0x58,   // 500   (S_STEP_5_0kHz,   index 5)
    0x58,   // 625   (S_STEP_6_25kHz,  index 6)
    0x48,   // 833   (S_STEP_8_33kHz,  index 7)  12.5k mode
    0x48,   // 1000  (S_STEP_10_0kHz,  index 8)
    0x08,   // 1250  (S_STEP_12_5kHz,  index 9)  12.5k mode, LPF2 3.0k
    0x28,   // 1500  (S_STEP_15_0kHz,  index 10) 25k mode
    0x28,   // 2000  (S_STEP_20_0kHz,  index 11)
    0x28,   // 2500  (S_STEP_25_0kHz,  index 12)
    0x28,   // 5000  (S_STEP_50_0kHz,  index 13)
    0x28,   // 10000 (S_STEP_100_0kHz, index 14)
};

typedef enum State
{
    SPECTRUM,
    FREQ_INPUT,
    STILL,
} State;

typedef enum StepsCount
{
    STEPS_16,
    STEPS_32,
    STEPS_64,
    STEPS_128,
    STEPS_256,
    STEPS_512,
    STEPS_1024,
    STEPS_2048,
} StepsCount;

typedef enum ScanStep
{
    S_STEP_0_01kHz,
    S_STEP_0_1kHz,
    S_STEP_0_5kHz,
    S_STEP_1_0kHz,

    S_STEP_2_5kHz,
    S_STEP_5_0kHz,
    S_STEP_6_25kHz,
    S_STEP_8_33kHz,
    S_STEP_10_0kHz,
    S_STEP_12_5kHz,
    S_STEP_15_0kHz,
    S_STEP_20_0kHz,
    S_STEP_25_0kHz,
    S_STEP_50_0kHz,
    S_STEP_100_0kHz,
} ScanStep;

typedef struct SpectrumSettings
{
    uint32_t frequencyChangeStep;
    StepsCount stepsCount;
    ScanStep scanStepIndex;
    uint16_t scanDelay;
    uint16_t rssiTriggerLevel;
    BK4819_FilterBandwidth_t bw;
    BK4819_FilterBandwidth_t listenBw;
    int dbMin;
    int dbMax;
    ModulationMode_t modulationType;
} SpectrumSettings;

typedef struct ScanInfo
{
    uint16_t rssi, rssiMin, rssiMax;
    uint16_t i, iPeak;
    uint32_t f, fPeak;
    uint16_t scanStep;
    uint16_t measurementsCount;
} ScanInfo;

typedef struct PeakInfo
{
    uint16_t t;
    uint16_t rssi;
    uint32_t f;
    uint16_t i;
} PeakInfo;

void APP_RunSpectrum(void);

#endif /* ifndef SPECTRUM_H */

// vim: ft=c
