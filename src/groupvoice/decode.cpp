#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "directsound.h"
#include <speex/speex.h>
#include <qcommon/qcommon.h>

void *g_decoder;
int g_current_decode_bandwidth_setting;
SpeexBits decodeBits;
int g_decode_frame_size;


char __cdecl Decode_Init(int bandwidthEnum)
{
    const SpeexMode *mode; // [esp+8h] [ebp-4h]

    if (bandwidthEnum)
    {
        if (bandwidthEnum == 1)
        {
            mode = &speex_wb_mode;
        }
        else
        {
            if (bandwidthEnum != 2)
            {
                Com_Printf(CON_CHANNEL_SOUND, "Unknown bandwidth mode %i\n", bandwidthEnum);
                return 0;
            }
            mode = &speex_uwb_mode;
        }
    }
    else
    {
        mode = &speex_nb_mode;
    }
    g_decoder = speex_decoder_init(mode);
    int tmp = 1;
    speex_decoder_ctl(g_decoder, SPEEX_SET_ENH, &tmp);
    Decode_SetOptions();
    speex_decoder_ctl(g_decoder, SPEEX_GET_FRAME_SIZE, &g_decode_frame_size);
    g_current_decode_bandwidth_setting = bandwidthEnum;
    speex_bits_init(&decodeBits);
    return 1;
}

void __cdecl Decode_SetOptions()
{
    speex_decoder_ctl(g_decoder, SPEEX_SET_SAMPLING_RATE, &g_encoder_samplerate);
}

void __cdecl Decode_Shutdown()
{
    if (g_decoder)
    {
        speex_bits_destroy(&decodeBits);
        speex_decoder_destroy(g_decoder);
    }
    g_decoder = 0;
}

int __cdecl Decode_Sample(char *buffer, int maxLength, int16_t *out, int frame_size)
{
    if (!g_decoder || !buffer || !out || maxLength < 0 || maxLength > 4096
        || g_decode_frame_size <= 0 || g_decode_frame_size > frame_size)
    {
        return 0;
    }
    speex_bits_read_from(&decodeBits, buffer, maxLength);
    if (speex_decode_int(g_decoder, &decodeBits, out))
    {
        return 0;
    }
    return g_decode_frame_size * sizeof(int16_t);
}

