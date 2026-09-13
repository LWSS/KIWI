#pragma once
#include <stddef.h>
#include <stdint.h>

struct XAsset;
struct ScriptStringList;

// Binds a compiler-owned asset pointer (NULL matches by name) to an existing asset in a previously loaded zone.
struct LinkerExternalAsset
{
    int type;
    const void *asset;
    const char *name;
};

// Serialize native engine assets in the stream order consumed by database64.
// Current asset support: RawFile, LocalizeEntry, StringTable, MapEnts, ComWorld,
// GameWorldMp, PhysPreset, SndCurve, GfxImage, GfxLightDef, MaterialTechniqueSet, Material,
// XModel (including physics geometry), XAnimParts, LoadedSound, sound alias lists,
// model-piece lists, effects, and clipmaps (including dynamic entities).
// Effect references are compiler-owned handles. Referenced effects are collected
// automatically; forward and cyclic references are supported.
// Image textures must point to
// compiler-owned load definitions, never live D3D resources.
// World records in one zone must name the same map. Unsupported types
// fail explicitly; they must never silently become raw-file payloads.
// On success the caller owns *data (free). On failure outputs remain empty.
bool Linker_BuildNativeZone(const XAsset *assets, int count, bool compress, uint8_t **data, size_t *size, char *error,
                            size_t errorSize);

// Asset script-string fields contain indices into this compiler-owned table,
// not runtime SL handles. Entry zero must be NULL (the absent-string handle).
bool Linker_BuildNativeZoneWithStrings(const XAsset *assets, int count, const ScriptStringList *strings, bool compress,
                                       uint8_t **data, size_t *size, char *error, size_t errorSize,
                                       const LinkerExternalAsset *external = NULL, size_t externalCount = 0,
                                       bool (*visit)(int, const char *, void *) = NULL, void *context = NULL);
