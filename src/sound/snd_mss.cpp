#include <universal/q_shared.h>
#include "snd_local.h"
#include "snd_public.h"
#include <universal/com_files.h>
#include <qcommon/qcommon.h>
#include <universal/com_memory.h>
#include <math.h>
#include <mss.h>

uint __stdcall MSS_FileOpenCallback(const MSS_FILE *pszFilename, UINTa *phFileHandle)
{
    return (FS_FOpenFileReadStream(pszFilename, (int *)phFileHandle) & 0x80000000) == 0;
}
void __stdcall MSS_FileCloseCallback(UINTa hFileHandle)
{
    FS_FCloseFile(hFileHandle);
}
int __stdcall MSS_FileSeekCallback(UINTa hFileHandle, int offset, uint type)
{
    if (type)
    {
        if (type == 1)
        {
            FS_Seek(hFileHandle, offset, 0);
        }
        else
        {
            if (type != 2)
                return 0;
            FS_Seek(hFileHandle, offset, 1);
        }
    }
    else
    {
        FS_Seek(hFileHandle, offset, 2);
    }
    return FS_FTell(hFileHandle);
}
uint __stdcall MSS_FileReadCallback(UINTa hFileHandle, void *pBuffer, uint bytes)
{
    return FS_Read((byte *)pBuffer, bytes, hFileHandle);
}

static void MSS_SetMixerPreferences(int hertz)
{
  // KIWI (miles9): DirectSound rounds a 1 ms fragment up to 256 sample frames
  // at every supported output rate (11/22/44 kHz). The SDK's default of 48
  // fragments therefore queues 279 ms at 44.1 kHz, not the intended 48 ms.
  // Target 24 ms: 1/2/4 fragments at 11025/22050/44100 Hz (~23 ms).
  int mixFragments = hertz * 24 / (256 * 1000);
  if (mixFragments < 1)
    mixFragments = 1;
  AIL_set_preference(DIG_MIXER_CHANNELS, SND_MAX_CHANNELS);
  AIL_set_preference(DIG_DS_FRAGMENT_SIZE, 1);
  AIL_set_preference(DIG_DS_MIX_FRAGMENT_CNT, mixFragments);
}

_DIG_DRIVER *__cdecl MSS_open_digital_driver(int hertz, int bits, int channels)
{
  bool v4; // [esp+0h] [ebp-10h]
  int outputChannels; // [esp+4h] [ebp-Ch] BYREF
  _DIG_DRIVER *driver; // [esp+8h] [ebp-8h]
  MSS_MC_SPEC mcSpec; // [esp+Ch] [ebp-4h] BYREF

  MSS_SetMixerPreferences(hertz);
  driver = (_DIG_DRIVER *)AIL_open_digital_driver(hertz, bits, channels, 0);
  if ( driver )
  {
    AIL_speaker_configuration(driver, 0, &outputChannels, 0, &mcSpec);
    v4 = outputChannels > 2 || !outputChannels;
    milesGlob.isMultiChannel = v4;
    if ( channels == 16 && mcSpec == MSS_MC_STEREO )
    {
      AIL_shutdown();
      if ( !MSS_Startup() )
      {
        MSS_InitFailed();
        return NULL;
      }
      MSS_SetMixerPreferences(hertz);
      return (_DIG_DRIVER *)AIL_open_digital_driver(hertz, bits, 32, 0);
    }
  }
  return driver;
}

void MSS_InitFailed()
{
  if ( Dvar_GetInt("r_vc_compile") != 2 )
    Com_Printf(9, "Miles sound system initialization failed\n");
}

MSS_MC_SPEC mss_spec[5] =
{
    MSS_MC_USE_SYSTEM_CONFIG,
    MSS_MC_MONO,
    MSS_MC_HEADPHONES,
    MSS_MC_40_DISCRETE,
    MSS_MC_51_DISCRETE
};

char __cdecl MSS_Init()
{
  const char *error; // eax
  int integer; // [esp+4h] [ebp-Ch]
  int hertz; // [esp+8h] [ebp-8h]

  integer = snd_khz->current.integer;
  if ( integer == 11 )
  {
    hertz = 11025;
  }
  else
  {
    if ( integer != 22 )
    {
      if ( integer == 44 )
      {
        hertz = 44100;
        goto LABEL_8;
      }
      Com_Printf(9, "invalid value %i for snd_khz, using 22 khz instead\n", snd_khz->current.integer);
    }
    hertz = 22050;
  }
LABEL_8:
  Com_Printf(
    9,
    "Attempting %i kHz %i bit [%s] sound\n",
    hertz / 1000,
    16,
    snd_outputConfigurationStrings[snd_outputConfiguration->current.integer]);
  milesGlob.driver = MSS_open_digital_driver(hertz, 16, mss_spec[snd_outputConfiguration->current.integer]);
  if ( milesGlob.driver )
  {
    AIL_set_3D_distance_factor(milesGlob.driver, 0.0254f);
    AIL_set_3D_rolloff_factor(milesGlob.driver, 0.0f);
    AIL_set_speaker_configuration(milesGlob.driver, 0, 0, 3.0f);
    g_snd.Initialized2d = 1;
    g_snd.Initialized3d = 1;
    g_snd.max_2D_channels = 8;
    g_snd.max_3D_channels = 32;
    g_snd.max_stream_channels = 13;
    g_snd.playback_rate = hertz + hertz / 2;
    if ( g_snd.playback_rate >= 44100 )
      g_snd.playback_rate = 0x7FFFFFFF;
    g_snd.playback_channels = (mss_spec[snd_outputConfiguration->current.integer] != MSS_MC_MONO) + 1;
    g_snd.timescale = 1.0;
    return 1;
  }
  else
  {
    error = (const char *)AIL_last_error();
    Com_PrintError(9, "ERROR: Couldn't initialize digital driver: %s\n", error);
    return 0;
  }
}

void MSS_InitChannels()
{
  for ( int i = 0; i < g_snd.max_3D_channels + g_snd.max_2D_channels; ++i )
  {
    milesGlob.handle_sample[i] = (_SAMPLE *)AIL_allocate_sample_handle(milesGlob.driver);
    if ( !milesGlob.handle_sample[i] )
      Com_Error(ERR_DROP, "MILES sound sample allocation failed on channel %i", i + 1);
    //AIL_init_sample(milesGlob.handle_sample[i], 1, 0);
    AIL_init_sample(milesGlob.handle_sample[i], 1);
  }
  g_snd.ambient_track = 1;
}

// KISAK: only the EQ parameter table is initialized here. The "3 Band Parm Eq" Miles
// pipeline filter (milesEq.flt) that used to be located and opened below never worked
// reliably (AV inside the .flt on coup) and has been removed from the tree entirely.
void MSS_InitEq()
{
  milesGlob.eqLerp = 1.0f; // Added by someone?

  for ( int eqIndex = 0; eqIndex < 2; ++eqIndex )
  {
    for ( int band = 0; band < 3; ++band )
    {
      for ( int channelIndex = 0; channelIndex < 64; ++channelIndex )
      {
          SndEqParams *params = &milesGlob.eq[eqIndex].params[band][channelIndex];
        params->enabled = 0;
        params->freq = 20000.0f;
        params->gain = 1.0f;
        params->q = 1.0f;
        params->type = SND_EQTYPE_FIRST;
      }
    }
  }
}

bool __cdecl MSS_Startup()
{
  return AIL_startup() != 0;
}

void MSS_ShutdownCleanup()
{
  //Com_ClearMemTrack();
  memset((uint8_t *)&milesGlob, 0, sizeof(milesGlob));
}

float MSS_GetDryLevel()
{
    return 1.0f;
    //return g_snd.effect->drylevel;
}

float MSS_GetWetLevel(const snd_alias_t *pAlias)
{
    iassert(g_snd.effect->wetlevel >= 0 && g_snd.effect->wetlevel <= 1);

    if ( !pAlias )
        return g_snd.effect->wetlevel;

    if ( !snd_enableReverb->current.enabled || (pAlias->flags & 0x10) != 0 )
        return 0.0f;
    else
        return g_snd.effect->wetlevel;
}

// KISAK: intentional no-op. This used to push milesGlob.eq[][] into the removed
// "3 Band Parm Eq" pipeline filter via AIL_set_sample_processor/AIL_sample_stage_property.
// The parameter table, script commands (seteq/seteqlerp) and savegame fields are kept so
// GSC and save files keep working; the DSP just isn't applied.
void __cdecl MSS_ApplyEqFilter(_SAMPLE *s, int entchannel)
{
  (void)s;
  (void)entchannel;
}

void __cdecl MSS_ResumeSample(int i, int frametime)
{
    if ( g_snd.chaninfo[i].startDelay )
    {
        if (g_snd.chaninfo[i].startDelay - frametime > 0)
        {
            g_snd.chaninfo[i].startDelay = g_snd.chaninfo[i].startDelay - frametime;
        }
        else
        {
            g_snd.chaninfo[i].startDelay = 0;
        }
        if (!g_snd.chaninfo[i].startDelay)
        {
            AIL_resume_sample(milesGlob.handle_sample[i]);
        }
    }
}

_DIG_DRIVER *__cdecl MSS_GetDriver()
{
  return milesGlob.driver;
}

int __cdecl MSS_DigitalFormatType(int waveFormat, int bits, int channels)
{
  int digitalFormat; // [esp+0h] [ebp-4h]

  if ( waveFormat != 1 && waveFormat != 17 )
    Com_Error(ERR_FATAL, "unknown wave format %i", waveFormat);
  if ( channels != 1 && channels != 2 )
    Com_Error(ERR_FATAL, "Sound has %i channels; only 1 or 2 channels are supported.\n", channels);
  if ( bits != 8 && bits != 16 )
    Com_Error(ERR_FATAL, "Sound uses %i bits per channel; only 8 or 16 bit channels are supported.\n", bits);
  digitalFormat = 0;
  if ( waveFormat == 17 )
    digitalFormat = 4;
  if ( bits == 16 )
    digitalFormat |= 1u;
  if ( channels == 2 )
    return digitalFormat | 2;
  return digitalFormat;
}

uint8_t *__cdecl MSS_Alloc(uint bytes, uint rate)
{
  if ( IsFastFileLoad() )
    return (uint8_t *)((int (__cdecl *)(uint, uint))MSS_Alloc_FastFile)(bytes, rate);
  else
    return MSS_Alloc_LoadObj(bytes, rate);
}

uint8_t *__cdecl MSS_Alloc_LoadObj(uint bytes, uint rate)
{
  int min_Spec_bytes; // [esp+0h] [ebp-4h]

  min_Spec_bytes = bytes;
  while ( rate > 0x4099 )
  {
    rate >>= 1;
    min_Spec_bytes /= 2;
  }
  //track_hunk_alloc((min_Spec_bytes + 31) & 0xFFFFFFE0, 0x7FFFFFFF, "MSS_Alloc", 16);
  return Hunk_Alloc(bytes, "MSS_Alloc", 15);
}

uint *__cdecl MSS_Alloc_FastFile(int bytes)
{
  return (uint *)Z_Malloc(bytes, "MSS_Alloc", 15);
}
