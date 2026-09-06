#include <universal/q_shared.h>
#include "snd_local.h"
#include "snd_public.h"
#include <universal/com_files.h>
#include <universal/com_memory.h>

#ifdef KISAK_DEDICATED
LoadedSound* __cdecl SND_LoadFromBuffer(void* buffer, const char* soundName)
{
    return NULL;
}
#else

LoadedSound *__cdecl SND_LoadFromBuffer(void *buffer, const char *soundName)
{
    _AILSOUNDINFO info; // [esp+8h] [ebp-28h] BYREF
    LoadedSound *loadSnd; // [esp+2Ch] [ebp-4h]

    iassert(buffer);

    if (AIL_WAV_info(buffer, &info))
    {
        if (info.data_len)
        {
            loadSnd = (LoadedSound*)Hunk_Alloc(0x2Cu, "SND_LoadFromBuffer", 15);
            loadSnd->name = soundName;
            // KISAK (miles9): the original did qmemcpy(&loadSnd->sound, &info, 0x24) against a
            // Miles build whose AILSOUNDINFO had no channel_mask. Every Miles we ship (7.2e and
            // 9.3b) inserts channel_mask after `channels`, so a raw 0x24 copy shifted
            // samples/block_size/initial_ptr by one dword. Copy the CoD4 mirror field by field.
            loadSnd->sound.info.format = info.format;
            loadSnd->sound.info.data_ptr = info.data_ptr;
            loadSnd->sound.info.data_len = info.data_len;
            loadSnd->sound.info.rate = info.rate;
            loadSnd->sound.info.bits = info.bits;
            loadSnd->sound.info.channels = info.channels;
            loadSnd->sound.info.samples = info.samples;
            loadSnd->sound.info.block_size = info.block_size;
            loadSnd->sound.info.initial_ptr = info.initial_ptr;
            loadSnd->sound.data = NULL;
            SND_SetData(&loadSnd->sound, (void*)info.data_ptr);
            return loadSnd;
        }
        else
        {
            Com_PrintError(CON_CHANNEL_ERROR, "ERROR: Sound file '%s' is zero length, invalid\n", soundName);
            return 0;
        }
    }
    else
    {
        Com_PrintError(CON_CHANNEL_ERROR, "ERROR: Sound file '%s' is in an invalid or corrupted format\n", soundName);
        return 0;
    }
}

#endif // KISAK_DEDICATED

LoadedSound *__cdecl SND_LoadSoundFile(const char *name)
{
    void *buffer; // [esp+4h] [ebp-10Ch] BYREF
    char realname[256]; // [esp+8h] [ebp-108h] BYREF
    LoadedSound *loadSnd; // [esp+10Ch] [ebp-4h]

    if (IsFastFileLoad())
        MyAssertHandler(".\\win32\\snd_driver_load_obj.cpp", 175, 0, "%s", "IsObjFileLoad()");

    iassert(name);

    Com_sprintf(realname, 0x100u, "sound/%s", name);
    int fileLen = FS_ReadFile(realname, &buffer);
    if (fileLen >= 0)
    {
        loadSnd = SND_LoadFromBuffer(buffer, name);
        FS_FreeFile((char*)buffer);
        return loadSnd;
    }
    else
    {
        Com_PrintError(CON_CHANNEL_ERROR, "ERROR: Sound file '%s' not found\n", realname);
        return 0;
    }
}
