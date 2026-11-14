#include "app/chFrScanner.h"
#ifdef ENABLE_FEAT_F4HWN_FULL_WATCH
#include "app/app.h"
#endif
#include "audio.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"

void COMMON_KeypadLockToggle() 
{

    if (gScreenToDisplay != DISPLAY_MENU &&
        gCurrentFunction != FUNCTION_TRANSMIT)
    {   // toggle the keyboad lock

        #ifdef ENABLE_VOICE
            gAnotherVoiceID = gEeprom.KEY_LOCK ? VOICE_ID_UNLOCK : VOICE_ID_LOCK;
        #endif

        gEeprom.KEY_LOCK = !gEeprom.KEY_LOCK;

        gRequestSaveSettings = true;
    }
}

void COMMON_SwitchVFOs()
{
#ifdef ENABLE_SCAN_RANGES    
    gScanRangeStart = 0;
#endif
#ifdef ENABLE_FEAT_F4HWN_FULL_WATCH
    if (APP_FullWatchSelectNextTx()) {
        /* F+2 preselects VFO_CONFIGURE for the legacy A/B path.  No channel
         * data changed here, and allowing that deferred reload would discard
         * the priority-channel foreground selection we just made. */
        gVfoConfigureMode = VFO_CONFIGURE_NONE;
        if (gInputBoxIndex > 0) {
            gInputBoxIndex = 0;
            gHasVfoBackup = false;
        }
        gRequestSaveSettings = true;
        gRequestDisplayScreen = DISPLAY_MAIN;
        return;
    }
#endif
    gEeprom.TX_VFO ^= 1;

    if (gInputBoxIndex > 0) {
        gInputBoxIndex = 0;
        gHasVfoBackup = false;
    }

    if (gEeprom.CROSS_BAND_RX_TX != CROSS_BAND_OFF)
        gEeprom.CROSS_BAND_RX_TX = gEeprom.TX_VFO + 1;
    if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF
#ifdef ENABLE_FEAT_F4HWN_FULL_WATCH
        && gEeprom.DUAL_WATCH != DUAL_WATCH_FULL
#endif
    )
        gEeprom.DUAL_WATCH = gEeprom.TX_VFO + 1;

    gRequestSaveSettings  = 1;
    gFlagReconfigureVfos  = true;
    gScheduleDualWatch = true;

    gRequestDisplayScreen = DISPLAY_MAIN;
}

void COMMON_SwitchVFOMode()
{
#ifdef ENABLE_FEAT_F4HWN_FULL_WATCH
    if (APP_FullWatchSwitchSelectedVfoMode())
        return;
#endif
#ifdef ENABLE_NOAA
    if (gEeprom.VFO_OPEN && !IS_NOAA_CHANNEL(gTxVfo->CHANNEL_SAVE))
#else
    if (gEeprom.VFO_OPEN)
#endif
    {
        if (gInputBoxIndex > 0) {
            gInputBoxIndex = 0;
            gHasVfoBackup = false;
        }

        if (IS_MR_CHANNEL(gTxVfo->CHANNEL_SAVE))
        {   // swap to frequency mode carrying over the active MR channel frequency
            const uint8_t        vfo     = gEeprom.TX_VFO;
            const uint32_t       freq    = gTxVfo->freq_config_RX.Frequency;
            const STEP_Setting_t chStep  = gTxVfo->STEP_SETTING;
            const uint16_t       vfoChan = FREQ_CHANNEL_FIRST + FREQUENCY_GetBand(freq);

            // the band's VFO record supplies everything but the frequency
            gEeprom.FreqChannel[vfo]   = vfoChan;
            gEeprom.ScreenChannel[vfo] = vfoChan;
            RADIO_ConfigureChannel(vfo, VFO_CONFIGURE_RELOAD);

            VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];

            /* A VFO reload rounds to its step: keep the exact frequency by
             * taking the channel step (or 10 Hz) only when it does not fit. */
            if (FREQUENCY_RoundToStep(freq, pVfo->StepFrequency) != freq) {
                STEP_Setting_t step = chStep;
                if (FREQUENCY_RoundToStep(freq, gStepFrequencyTable[step]) != freq)
                    step = STEP_0_01kHz;
                pVfo->STEP_SETTING  = step;
                pVfo->StepFrequency = gStepFrequencyTable[step];
            }

            // plain simplex, exactly as a frequency typed on the keypad
            RADIO_SetSimplexFrequency(pVfo, freq);
            pVfo->pRX = &pVfo->freq_config_RX;
            pVfo->pTX = &pVfo->freq_config_TX;
            RADIO_ApplyOffset(pVfo);
            RADIO_ConfigureSquelchAndOutputPower(pVfo);

            #ifdef ENABLE_VOICE
                gAnotherVoiceID        = VOICE_ID_FREQUENCY_MODE;
            #endif
            gRequestSaveVFO            = true;
            gRequestSaveChannel        = 1;      // persist the VFO record first,
            gVfoConfigureMode          = VFO_CONFIGURE_NONE; // then the save path reconfigures
            gFlagReconfigureVfos       = true;
            gRequestDisplayScreen      = DISPLAY_MAIN;
            return;
        }

        uint16_t Channel = RADIO_FindNextChannel(gEeprom.MrChannel[gEeprom.TX_VFO], 1, false, 0);
        if (Channel != 0xFFFF)
        {   // swap to channel mode
            gEeprom.ScreenChannel[gEeprom.TX_VFO] = Channel;
            #ifdef ENABLE_VOICE
                AUDIO_SetVoiceID(0, VOICE_ID_CHANNEL_MODE);
                AUDIO_SetDigitVoice(1, Channel + 1);
                gAnotherVoiceID = (VOICE_ID_t)0xFE;
            #endif
            gRequestSaveVFO     = true;
            gVfoConfigureMode   = VFO_CONFIGURE_RELOAD;
            return;
        }
    }
}
