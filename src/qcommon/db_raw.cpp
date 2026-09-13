#include <universal/q_shared.h>
#include <database64/database.h>
#include <qcommon/files.h>
#include <qcommon/threads.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/rb_shade.h>
#include <universal/com_files.h>
#include <universal/com_memory.h>

#ifndef KIWI_RAW_ONLY
#error This file replaces the fastfile registry only in raw-only builds.
#endif

// The raw loaders own these tables even when no fastfile registry is linked.
GfxWorld s_world;
MaterialGlobals materialGlobals;
ImgGlobals imageGlobals;
r_globals_t rg;
fileData_s *com_fileDataHashTable[1024];
volatile uint g_loadingAssets;
uint volatile g_mainThreadBlocked;

const char *g_assetNames[ASSET_TYPE_COUNT] =
{
    "xmodelpieces", "physpreset", "xanim", "xmodel", "material", "techset",
    "image", "sound", "sndcurve", "loaded_sound", "col_map_sp", "col_map_mp",
    "com_map", "game_map_sp", "game_map_mp", "map_ents", "gfx_map", "lightdef",
    "ui_map", "font", "menufile", "menu", "localize", "weapon", "snddriverglobals",
    "fx", "impactfx", "aitype", "mptype", "character", "xmodelalias", "rawfile", "stringtable"
};

void TRACK_db_registry() {}
void DB_SetInitializing(bool) {}
void DB_Update() {}
void DB_UpdateDebugZone() {}
void DB_SyncXAssets() {}
void DB_Cleanup() {}
void DB_ReleaseXAssets() {}
void DB_ShutdownXAssets()
{
#ifndef DEDICATED
    R_SyncRenderThread();
    RB_UnbindAllImages();
    R_ShutdownStreams();
    RB_ClearPixelShader();
    RB_ClearVertexShader();
    RB_ClearVertexDecl();
#endif
}
void DB_BeginRecoverLostDevice() {}
void DB_EndRecoverLostDevice() {}
void DB_LoadedExternalData(int) {}
void DB_ResetZoneSize(int) {}
double DB_GetLoadedFraction() { return 1.0; }
bool DB_IsMinimumFastFileLoaded() { return false; }
bool DB_ModFileExists() { return false; }
int DB_FileSize(const char *, int) { return 0; }
bool DB_IsXAssetDefault(XAssetType, const char *) { return false; }

void DB_InitThread()
{
    // Keep the existing synchronization contract without starting a loader thread.
    Sys_DatabaseCompleted();
    Sys_DatabaseCompleted2();
}

char *DB_ReferencedFFChecksums()
{
    static char empty[] = "";
    return empty;
}

char *DB_ReferencedFFNameList()
{
    static char empty[] = "";
    return empty;
}

XAssetHeader DB_FindXAssetHeader(XAssetType type, const char *name)
{
    Com_Error(ERR_DROP, "Fastfile asset lookup is unavailable in this raw-only build: type %i, %s",
        type, name ? name : "<null>");
    return XAssetHeader{};
}

void DB_LoadXAssets(XZoneInfo *, uint, int)
{
    Com_Error(ERR_DROP, "Fastfiles are unavailable in this raw-only build");
}

void DB_ReplaceModel(const char *, const char *)
{
    Com_Error(ERR_DROP, "Fastfile model replacement is unavailable in this raw-only build");
}

void DB_GetIndexBufferAndBase(uint8_t, void *, void **, int *)
{
    Com_Error(ERR_DROP, "Fastfile index buffers are unavailable in this raw-only build");
}

void DB_GetVertexBufferAndOffset(uint8_t, _BYTE *, void **, int *)
{
    Com_Error(ERR_DROP, "Fastfile vertex buffers are unavailable in this raw-only build");
}

void Load_GetCurrentZoneHandle(uint8_t *)
{
    Com_Error(ERR_DROP, "No fastfile zone exists in this raw-only build");
}

void Hunk_OverrideDataForFile(int type, const char *name, void *data)
{
    for (fileData_s *file = com_fileDataHashTable[FS_HashFileName(name, ARRAY_COUNT(com_fileDataHashTable))];
        file; file = file->next)
    {
        if (file->type == type && !I_stricmp(file->name, name))
        {
            file->data = data;
            return;
        }
    }
    Com_Error(ERR_DROP, "Hunk_OverrideDataForFile: could not find %s", name);
}

void R_EnumMaterials(void (*func)(Material *, void *), void *data)
{
    for (unsigned int i = 0; i < ARRAY_COUNT(rg.materialHashTable); ++i)
    {
        if (rg.materialHashTable[i])
        {
            func(rg.materialHashTable[i], data);
        }
    }
}

void R_EnumTechniqueSets(void (*func)(MaterialTechniqueSet *, void *), void *data)
{
    for (unsigned int i = 0; i < ARRAY_COUNT(materialGlobals.techniqueSetHashTable); ++i)
    {
        if (materialGlobals.techniqueSetHashTable[i])
        {
            func(materialGlobals.techniqueSetHashTable[i], data);
        }
    }
}

void R_EnumImages(void (*func)(GfxImage *, void *), void *data)
{
    for (unsigned int i = 0; i < ARRAY_COUNT(imageGlobals.imageHashTable); ++i)
    {
        GfxImage *image = imageGlobals.imageHashTable[i];
        if (image && !Image_IsProg(image))
        {
            func(image, data);
        }
    }
}

void DB_EnumXAssetsFor(fileData_s *file, int type, void (*func)(void *, void *), void *data)
{
    for (; file; file = file->next)
    {
        if (file->type == type && type == 5)
        {
            func(file->data, data);
        }
    }
}

void DB_EnumXAssets(XAssetType type, void (*func)(XAssetHeader, void *), void *data, bool)
{
    XAssetHeader header = {};
    switch (type)
    {
    case ASSET_TYPE_XMODEL:
        for (unsigned int i = 0; i < ARRAY_COUNT(com_fileDataHashTable); ++i)
        {
            for (fileData_s *file = com_fileDataHashTable[i]; file; file = file->next)
            {
                if (file->type == 5)
                {
                    header.model = (XModel *)file->data;
                    func(header, data);
                }
            }
        }
        break;
    case ASSET_TYPE_MATERIAL:
        for (unsigned int i = 0; i < ARRAY_COUNT(rg.materialHashTable); ++i)
        {
            header.material = rg.materialHashTable[i];
            if (header.material)
            {
                func(header, data);
            }
        }
        break;
    case ASSET_TYPE_TECHNIQUE_SET:
        for (unsigned int i = 0; i < ARRAY_COUNT(materialGlobals.techniqueSetHashTable); ++i)
        {
            header.techniqueSet = materialGlobals.techniqueSetHashTable[i];
            if (header.techniqueSet)
            {
                func(header, data);
            }
        }
        break;
    case ASSET_TYPE_IMAGE:
        for (unsigned int i = 0; i < ARRAY_COUNT(imageGlobals.imageHashTable); ++i)
        {
            header.image = imageGlobals.imageHashTable[i];
            if (header.image && !Image_IsProg(header.image))
            {
                func(header, data);
            }
        }
        break;
    default:
        break;
    }
}

int DB_GetAllXAssetOfType(XAssetType type, XAssetHeader *assets, int maxCount)
{
    AssetList list = {0, maxCount, assets};
    DB_EnumXAssets(type, Hunk_AddAsset, &list, false);
    return list.assetCount;
}

void DB_EnumXAssets_FastFile(XAssetType, void (*)(XAssetHeader, void *), void *, bool)
{
    Com_Error(ERR_DROP, "Fastfile enumeration is unavailable in this raw-only build");
}

int DB_GetAllXAssetOfType_FastFile(XAssetType, XAssetHeader *, int)
{
    Com_Error(ERR_DROP, "Fastfile enumeration is unavailable in this raw-only build");
    return 0;
}
