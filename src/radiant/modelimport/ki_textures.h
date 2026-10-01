#pragma once

// KIWI model importer - InstaMAT texture-set discovery.
//
// InstaMAT writes one PNG per map per texture set per variant into one shared
// export folder, named
//     <prefix>_<Set>_<Set><Map>[-WxH].png          variant "1" (older exports)
//     <prefix>_<Set>_<V>_<Set><Map>[-WxH].png      variant V ("2", "3", "opaque_3", ...)
// where <Set> is the material/texture-set name ("Default", "Default_1", ...) and
// <Map> is BaseColor / Normal / Metalness / Roughness / Opacity (and aliases).
// Re-exports overwrite same-numbered files and leave higher-numbered stale ones,
// so every file's write time is kept for the caller to judge.

#include "ki_convert.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kimport {

enum class MapKind { BaseColor, Normal, Metalness, Roughness, Opacity, Count };

struct TextureVariant {
    std::string set;        // "Default"
    std::string id;         // "1", "2", "opaque_3"
    TextureFiles files;
    long long newestWrite = 0;   // file_time ticks of the newest map in the set
    int width = 0;               // from the -WxH suffix when present
    int height = 0;
};

struct TextureScan {
    // set name -> variants sorted by id (numeric ids first, ascending)
    std::map<std::string, std::vector<TextureVariant>> sets;
};

// Scans `folder` for files whose name starts with "<prefix>_" (case-insensitive).
bool ScanTextureSets(const std::filesystem::path& folder, const std::string& prefix,
                     TextureScan& scan, std::string& error);

// Lists the distinct "<prefix>" values in a folder (everything before the first
// "_<Set>_..._<Set><Map>" pattern), newest first, for "which models have textures".
std::vector<std::string> ListTexturePrefixes(const std::filesystem::path& folder,
                                             std::string& error);

// The asset name for a variant: variant "1" keeps the base name, others append
// "_<id>" (sanitized).
std::string VariantAssetName(const std::string& baseAsset, const std::string& variantId);

// A small RGBA8 copy of an image (longest side <= maxSize, box-filtered) for preview
// swatches. Pure CPU (stb_image), so it may run on a worker thread.
bool LoadThumbnail(const std::filesystem::path& path, int maxSize,
                   std::vector<unsigned char>& rgba, int& width, int& height,
                   std::string& error);

}  // namespace kimport
