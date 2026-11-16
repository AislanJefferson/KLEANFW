/* Modified by AislanJefferson for KLEANFW, 2026 (see git history). */
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

        /* Test and edit the same object: with Full Watch, gTxVfo can be the
         * other slot promoted to the foreground. */
        const uint8_t Vfo = gEeprom.TX_VFO;
        VFO_Info_t *const pVfo = &gEeprom.VfoInfo[Vfo];
        const uint32_t Frequency = pVfo->freq_config_RX.Frequency;

        if (IS_MR_CHANNEL(pVfo->CHANNEL_SAVE))
        {   // swap to frequency mode
            /* The VFO takes only the channel frequency: the band's VFO record
             * keeps its own step/modulation/BW/power, while tones, offset,
             * reverse and scrambler (HOP too) are cleared.  Saved here, not
             * via gRequestSaveChannel: that save is deferred while a key is
             * held, and the reload below would first re-read the old VFO
             * record from flash.  The driver skips unchanged bytes, so this
             * costs at most one erase of the VFO sector, only on a change. */
            const uint16_t Channel = FREQ_CHANNEL_FIRST + FREQUENCY_GetBand(Frequency);
            gEeprom.ScreenChannel[Vfo] = Channel;
            gEeprom.FreqChannel[Vfo]   = Channel;
            RADIO_ConfigureChannel(Vfo, VFO_CONFIGURE_RELOAD);
            RADIO_SetSimplexFrequency(pVfo, FREQUENCY_RoundToStep(Frequency, pVfo->StepFrequency));
            pVfo->SCRAMBLING_TYPE = 0;
            SETTINGS_SaveChannel(Channel, Vfo, pVfo, 1);
            #ifdef ENABLE_VOICE
                gAnotherVoiceID        = VOICE_ID_FREQUENCY_MODE;
            #endif
            gRequestSaveVFO            = true;
            gVfoConfigureMode          = VFO_CONFIGURE_RELOAD;
            return;
        }

        /* Prefer a stored channel with the VFO frequency, searching from the
         * last MR channel so MR->VFO->MR returns to it on duplicates.  With
         * no match, keep the last MR channel: RADIO_ConfigureChannel moves
         * an invalid one to the next valid channel, or back to the VFO when
         * none exists (the old RADIO_FindNextChannel outcome). */
        uint16_t Channel = gEeprom.MrChannel[Vfo];
        uint16_t ch = Channel;
        for (uint16_t i = 0; IS_MR_CHANNEL(i); i++, ch++) {
            if (!IS_MR_CHANNEL(ch))
                ch = MR_CHANNEL_FIRST;
            if (RADIO_CheckValidChannel(ch, false, 0) &&
                SETTINGS_FetchChannelFrequency(ch) == Frequency) {
                Channel = ch;
                break;
            }
        }
        // swap to channel mode
        gEeprom.ScreenChannel[Vfo] = Channel;
        #ifdef ENABLE_VOICE
            AUDIO_SetVoiceID(0, VOICE_ID_CHANNEL_MODE);
            AUDIO_SetDigitVoice(1, Channel + 1);
            gAnotherVoiceID = (VOICE_ID_t)0xFE;
        #endif
        gRequestSaveVFO     = true;
        gVfoConfigureMode   = VFO_CONFIGURE_RELOAD;
    }
}
