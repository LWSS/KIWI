#include <universal/q_shared.h>
#include "database.h"
#include "db_native_file.h"
#include "db_effect_references.h"
#include "fastfile_format.h"
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_buffers.h>
#include <limits.h>

struct DB64LoadData
{
    HANDLE file;
    HANDLE readEvent;
    const char *filename;
    XZoneMemory *zoneMem;
    uint8_t *buffer;
    void (*interrupt)();
    int allocType;
    uint64_t offset;
    uint64_t fileSize;
    z_stream stream;
    bool inflateStarted;
    bool streamEnded;
};

static DB64LoadData g_load;
static bool g_minimumFastFileLoaded;
static volatile LONG g_externalBytes;
XAssetList g_varXAssetList;

void DB_CloseNativeFile()
{
    DB64_ResetEffectReferences();
    if (g_load.inflateStarted)
    {
        inflateEnd(&g_load.stream);
        g_load.inflateStarted = false;
    }
    if (g_load.readEvent)
    {
        CloseHandle(g_load.readEvent);
        g_load.readEvent = NULL;
    }
    if (g_load.file && g_load.file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_load.file);
        g_load.file = NULL;
    }
}

static void DB_NativeReadError(const char *reason)
{
    DB_CloseNativeFile();
    Com_Error(ERR_DROP, "Native fastfile '%s': %s", g_load.filename, reason);
}

static DWORD DB_ReadNativeBytes(void *buffer, DWORD size)
{
    if (g_load.interrupt)
    {
        g_load.interrupt();
    }
    DWORD bytes = 0;
    if (!DB64_ReadNativeFile(g_load.file, g_load.readEvent, &g_load.offset, buffer, size, &bytes))
    {
        DB_NativeReadError("file read failed");
    }
    return bytes;
}

void DB_LoadXFileData(uint8_t *pos, uint size)
{
    if (!size)
    {
        return;
    }
    if (!g_load.inflateStarted || g_load.streamEnded)
    {
        DB_NativeReadError("unexpected end of compressed stream");
    }
    g_load.stream.next_out = pos;
    g_load.stream.avail_out = size;
    while (g_load.stream.avail_out)
    {
        if (!g_load.stream.avail_in)
        {
            const DWORD bytes = DB_ReadNativeBytes(g_load.buffer, 0x40000);
            if (!bytes)
            {
                DB_NativeReadError("truncated compressed stream");
            }
            g_load.stream.next_in = g_load.buffer;
            g_load.stream.avail_in = bytes;
        }
        const uInt beforeIn = g_load.stream.avail_in;
        const uInt beforeOut = g_load.stream.avail_out;
        const int result = inflate(&g_load.stream, Z_NO_FLUSH);
        if (result == Z_STREAM_END)
        {
            g_load.streamEnded = true;
            if (g_load.stream.avail_out)
            {
                DB_NativeReadError("asset data exceeds compressed stream");
            }
            break;
        }
        if (result != Z_OK || (beforeIn == g_load.stream.avail_in && beforeOut == g_load.stream.avail_out))
        {
            DB_NativeReadError("invalid compressed data");
        }
    }
}

static void DB_FinishNativeStream()
{
    uint8_t extra;
    while (!g_load.streamEnded)
    {
        if (!g_load.stream.avail_in)
        {
            g_load.stream.avail_in = DB_ReadNativeBytes(g_load.buffer, 0x40000);
            g_load.stream.next_in = g_load.buffer;
            if (!g_load.stream.avail_in)
            {
                DB_NativeReadError("missing compressed stream footer");
            }
        }
        g_load.stream.next_out = &extra;
        g_load.stream.avail_out = 1;
        const int result = inflate(&g_load.stream, Z_NO_FLUSH);
        if (!g_load.stream.avail_out || (result != Z_OK && result != Z_STREAM_END))
        {
            DB_NativeReadError("unexpected trailing asset data or bad checksum");
        }
        g_load.streamEnded = result == Z_STREAM_END;
    }
    if (g_load.stream.avail_in || g_load.offset != g_load.fileSize)
    {
        DB_NativeReadError("unexpected trailing compressed bytes");
    }
}

static void DB_LoadNativeImage(XAssetHeader image, void *)
{
    R_DelayLoadImage(image);
}

void DB_LoadXFileInternal()
{
    static_assert(sizeof(void *) == 8, "Native stream database requires x64");
    DB64FilePrefix prefix;
    if (DB_ReadNativeBytes(&prefix, sizeof(DB64FilePrefix)) != sizeof(DB64FilePrefix) ||
        memcmp(prefix.magic, DB64_FASTFILE_MAGIC, sizeof(prefix.magic)) || prefix.version != DB64_FASTFILE_VERSION)
    {
        DB_NativeReadError("not a compatible prelinked KIWI x64 fastfile");
    }
    if (inflateInit(&g_load.stream) != Z_OK)
    {
        DB_NativeReadError("cannot initialize decompressor");
    }
    g_load.inflateStarted = true;
    XFile file;
    DB_LoadXFileData((uint8_t *)&file, sizeof(XFile));
    DB_AllocXZoneMemory(file.blockSize, g_load.filename, g_load.zoneMem, g_load.allocType);
    DB_InitStreams(g_load.zoneMem);
    DB_LoadXFileData((uint8_t *)&g_varXAssetList, sizeof(XAssetList));
    varXAssetList = &g_varXAssetList;
    if (varXAssetList->assetCount < 0 || varXAssetList->assetCount >= 32768 || varXAssetList->stringList.count < 0 ||
        varXAssetList->stringList.count > 65536 || (!!varXAssetList->assets != (varXAssetList->assetCount != 0)) ||
        (!!varXAssetList->stringList.strings != (varXAssetList->stringList.count != 0)))
    {
        DB_NativeReadError("invalid asset or script-string count");
    }
    DB_PushStreamPos(4);
    varScriptStringList = &varXAssetList->stringList;
    Load_ScriptStringList(false);
    DB_PopStreamPos();
    DB_PushStreamPos(4);
    if (varXAssetList->assets)
    {
        varXAssetList->assets = AllocLoad_FxElemVisStateSample();
        varXAsset = varXAssetList->assets;
        Load_Stream(true, (uint8_t *)varXAsset, DB_StreamArraySize(sizeof(XAsset), varXAssetList->assetCount));
        for (int i = 0; i < varXAssetList->assetCount; ++i)
        {
            varXAsset = &varXAssetList->assets[i];
            if (varXAsset->type < 0 || varXAsset->type >= ASSET_TYPE_COUNT)
            {
                DB_NativeReadError("invalid asset type");
            }
            Load_XAsset(false);
        }
    }
    DB_PopStreamPos();
    char effectError[512];
    if (!DB64_ResolveEffectReferences(DB64_FindLoadedEffect, effectError, sizeof(effectError)))
    {
        DB_NativeReadError(effectError);
    }
    Load_DelayStream();
    DB_FinishNativeStream();
    if (g_load.zoneMem->lockedVertexData)
    {
        R_FinishStaticVertexBuffer((IDirect3DVertexBuffer9 *)g_load.zoneMem->vertexBuffer);
        g_load.zoneMem->lockedVertexData = NULL;
    }
    if (g_load.zoneMem->lockedIndexData)
    {
        R_FinishStaticIndexBuffer((IDirect3DIndexBuffer9 *)g_load.zoneMem->indexBuffer);
        g_load.zoneMem->lockedIndexData = NULL;
    }
    --g_loadingAssets;
    DB_EnumXAssets(ASSET_TYPE_IMAGE, DB_LoadNativeImage, NULL, false);
    for (uint i = 0; i < g_copyInfoCount; ++i)
    {
        if (g_copyInfo[i]->asset.type == ASSET_TYPE_IMAGE)
        {
            R_DelayLoadImage(g_copyInfo[i]->asset.header);
        }
    }
    if (!I_stricmp(g_load.filename, "localized_code_post_gfx_mp"))
    {
        g_minimumFastFileLoaded = true;
    }
    Com_Printf(CON_CHANNEL_FILES, "Loaded native zone '%s'\n", g_load.filename);
    DB_CloseNativeFile();
}

bool DB_IsMinimumFastFileLoaded()
{
    return g_minimumFastFileLoaded;
}

void DB_ResetZoneSize(int)
{
    g_externalBytes = 0;
}

void DB_LoadedExternalData(int size)
{
    InterlockedExchangeAdd(&g_externalBytes, size);
}

double DB_GetLoadedFraction()
{
    return g_load.fileSize ? (double)g_load.offset / (double)g_load.fileSize : 0.0;
}

void __stdcall DB_FileReadCompletion(uint, uint, _OVERLAPPED *)
{
    // The native reader waits on its own OVERLAPPED event, not APC completion.
}

void DB_LoadXFile(const char *, void *file, const char *filename, XZoneMemory *zoneMem, void (*interrupt)(),
                  uint8_t *buffer, int allocType)
{
    DB64_ResetEffectReferences();
    memset(&g_load, 0, sizeof(DB64LoadData));
    g_load.file = file;
    g_load.filename = filename;
    g_load.zoneMem = zoneMem;
    g_load.buffer = buffer;
    g_load.interrupt = interrupt;
    g_load.allocType = allocType;
    g_load.readEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    LARGE_INTEGER size;
    if (!g_load.readEvent || !GetFileSizeEx(file, &size) || size.QuadPart < 0)
    {
        DB_NativeReadError("cannot initialize input file");
    }
    g_load.fileSize = (uint64_t)size.QuadPart;
}
