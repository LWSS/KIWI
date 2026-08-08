#pragma once
#include <cstdint>

#include <Windows.h>
#include <dsound.h>

static const int MIN_COMFORTABLE_BUFFER_AMOUNT = 0x140;
static const int COMFORTABLE_BUFFER_AMOUNT = 0x1770;
static const int MAX_COMFORTABLE_BUFFER_AMOUNT = 0x2710;
static const uint g_sound_playBufferSize = 0xC800;
static const int g_sound_recordBufferSize = 0xC800;
static const int g_sound_recordFrequency = 0x2000;
static const int g_sound_recordVolume = 0xFF;
static const int g_sound_channels = 1;

inline int g_encoder_samplerate = 0x1F40;
inline int g_encoder_quality = 1;

static const float maxScalingDownMultiplier = 0.0099999998f;
static const float maxScalingUpMultiplier = 0.0099999998f;


struct dsound_sample_t // sizeof=0x48
{                                       // ...
    IDirectSoundCaptureBuffer *DSCB;
    IDirectSoundBuffer *DSB;
    uint dwBufferSize;
    uint dwCaptureOffset;
    uint currentOffset;
    uint lastOffset;
    uint currentBufferLength;
    int stopPosition;
    uint lastPlayPos;
    int bytesBuffered;
    int mode;
    int frequency;
    int volume;
    int pan;
    int channels;
    bool playing;
    // padding byte
    // padding byte
    // padding byte
    int channel;
    uint8_t playMode;
    // padding byte
    // padding byte
    // padding byte
};
static_assert(sizeof(dsound_sample_t) == 0x48);

struct audioSample_t // sizeof=0x20
{                                       // ...
    uint8_t *buffer;            // ...
    int lengthInBytes;                  // ...
    int lengthInSamples;                // ...
    int bytesPerSample;                 // ...
    int frequency;                      // ...
    bool stereo;                        // ...
    // padding byte
    // padding byte
    // padding byte
    int channels;                       // ...
    int sampleOffset;                   // ...
};
static_assert(sizeof(audioSample_t) == 0x20);


// play_dsound
uint __cdecl DSound_UpdateSample(dsound_sample_t *sample, char *data, int data_len);
int __cdecl DSound_GetBytesLeft(dsound_sample_t *sample);
void __cdecl DSound_AdjustSamplePlayback(dsound_sample_t *sample, int bytesLeft);
bool __cdecl DSound_BufferUnderrunOccurred(dsound_sample_t *sample);
void __cdecl DSound_HandleBufferUnderrun(dsound_sample_t *sample);
void __cdecl DSound_SampleFrame(dsound_sample_t *sample);
dsound_sample_t *__cdecl DSound_NewSample();
char __cdecl DSound_StopSample(dsound_sample_t *sample);
int __cdecl DSound_Init(bool callDsoundInit, HWND__ *handle);
void __cdecl DSound_Shutdown();



// record_dsound
void __cdecl DSOUNDRecord_UpdateSample(dsound_sample_t *pRecSample);
void __cdecl DSOUNDRecord_Frame();
dsound_sample_t *__cdecl DSOUNDRecord_NewSample();
int __cdecl DSOUNDRecord_DestroySample(dsound_sample_t *pRecSample);
HRESULT __cdecl DSOUNDRecord_Start(dsound_sample_t *pRecSample);
HRESULT __cdecl DSOUNDRecord_Stop(dsound_sample_t *pRecSample);
int __cdecl DSOUNDRecord_Init(bool bCallDsoundInit);
void __cdecl DSOUNDRecord_Shutdown();

void __cdecl Record_SetRecordingCallback(int(__cdecl *new_audioCallback)(audioSample_t *));



// encode
void __cdecl Encode_SetOptions(int frequency, int quality);
bool __cdecl Encode_Init(int bandwidthEnum);
char __cdecl Encode_Shutdown();
int __cdecl Encode_Sample(int16_t *buffer_in, char *buffer_out, int maxLength);
int __cdecl Encode_GetFrameSize();


// decode
char __cdecl Decode_Init(int bandwidthEnum);
void __cdecl Decode_SetOptions();
void __cdecl Decode_Shutdown();
int __cdecl Decode_Sample(char *buffer, int maxLength, int16_t *out, int frame_size);


extern int g_frame_size;
extern int g_current_bandwidth_setting;