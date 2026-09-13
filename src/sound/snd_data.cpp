#include <universal/q_shared.h>
#include "snd_local.h"
#include <msslib/mss.h>
#include <qcommon/qcommon.h>

int __cdecl MSS_DigitalFormatType(int waveFormat, int bits, int channels)
{
    if (waveFormat != 1 && waveFormat != 17)
    {
        Com_Error(ERR_FATAL, "Unknown wave format %i", waveFormat);
    }
    if (channels != 1 && channels != 2)
    {
        Com_Error(ERR_FATAL, "Sound has %i channels; only 1 or 2 channels are supported", channels);
    }
    if (bits != 8 && bits != 16)
    {
        Com_Error(ERR_FATAL, "Sound uses %i bits per channel; only 8 or 16 are supported", bits);
    }
    int format = waveFormat == 17 ? DIG_F_ADPCM_MASK : 0;
    if (bits == 16)
    {
        format |= DIG_F_16BITS_MASK;
    }
    if (channels == 2)
    {
        format |= DIG_F_STEREO_MASK;
    }
    return format;
}

void __cdecl SND_SetDataForRate(MssSoundCOD4 *sound, void *srcData, uint playbackRate)
{
    if (!playbackRate)
    {
        Com_Error(ERR_DROP, "Cannot prepare sound before the playback rate is initialized");
        return;
    }
    if (sound->info.rate > playbackRate && sound->info.format != 17)
    {
        AILMIXINFO mix = {};
        mix.Info.format = sound->info.format;
        mix.Info.data_ptr = mix.Info.initial_ptr = srcData;
        mix.Info.data_len = sound->info.data_len;
        mix.Info.rate = sound->info.rate;
        mix.Info.bits = sound->info.bits;
        mix.Info.channels = sound->info.channels;
        mix.Info.channel_mask = ~0U;
        mix.Info.samples = sound->info.samples;
        mix.Info.block_size = sound->info.block_size;
        while (sound->info.rate > playbackRate)
        {
            sound->info.rate /= 2;
        }
        const int format = MSS_DigitalFormatType(sound->info.format, sound->info.bits, sound->info.channels);
        const int capacity = AIL_size_processed_digital_audio(sound->info.rate, format, 1, &mix);
        if (capacity <= 0)
        {
            Com_Error(ERR_DROP, "Miles could not size resampled sound data");
            return;
        }
        sound->data = MSS_Alloc(capacity, sound->info.rate);
        const int written = AIL_process_digital_audio(sound->data, capacity, sound->info.rate, format, 1, &mix);
        if (written <= 0 || written > capacity)
        {
            Com_Error(ERR_DROP, "Miles could not resample sound data");
            return;
        }
        // The sizing call returns capacity, not the number of valid audio bytes.
        sound->info.data_len = written;
        sound->info.samples = written / (sound->info.channels * (sound->info.bits / 8));
    }
    else
    {
        sound->data = MSS_Alloc(sound->info.data_len, sound->info.rate);
        memcpy(sound->data, srcData, sound->info.data_len);
    }
    sound->info.data_ptr = sound->data;
    sound->info.initial_ptr = sound->data;
}
