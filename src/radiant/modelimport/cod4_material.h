#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cod4 {

// Standalone writer for CoD4 load-object material binaries. The raw format
// contains compiler-generated refStateBits, so these files are cloned from a
// compatible shipped material rather than synthesized. The template list is
// the finished KIWI list, in KIWI order:
//
//   WorldLitOpaque                 -> l_sm_r0c0
//   WorldLitAlphaTest              -> l_sm_t0c0
//   WorldLitAlphaBlend             -> l_sm_b0c0
//   WorldUnlitOpaque               -> unlit
//   WorldLitNormalSpecular         -> l_sm_r0c0n0s0
//   WorldLitAlphaTestNormalSpecular-> l_sm_t0c0n0s0
//   WorldLitAlphaBlendNormalSpecular->l_sm_b0c0n0s0
//   WorldLitNormal                 -> l_sm_r0c0n0
//   WorldLitAlphaTestNormal        -> l_sm_t0c0n0
//   WorldLitAlphaBlendNormal       -> l_sm_b0c0n0
//   WorldLitSpecular               -> l_sm_r0c0s0
//   WorldLitAlphaTestSpecular      -> l_sm_t0c0s0
//   WorldLitAlphaBlendSpecular     -> l_sm_b0c0s0
//   WorldGlass                     -> l_sm_b0c0s0 (glass sort/state template)
//
// The l_sm_<r|t|b> names mean opaque/alpha-test/alpha-blend; c/n/s select
// colorMap, normalMap, and specularMap techniques. The material has no separate
// cosinePower entry: MaterialImageSlots::specularGloss names one semantic-8 IWI
// whose RGB is specular reflection color and whose alpha is gloss/cosine power.
// Template constants such as envMapParms and colorTint are preserved bit-for-bit.

enum class MaterialTemplate : std::uint8_t {
    WorldLitOpaque,
    WorldLitAlphaTest,
    WorldLitAlphaBlend,
    WorldUnlitOpaque,
    WorldLitNormalSpecular,
    WorldLitAlphaTestNormalSpecular,
    WorldLitAlphaBlendNormalSpecular,
    WorldLitNormal,
    WorldLitAlphaTestNormal,
    WorldLitAlphaBlendNormal,
    WorldLitSpecular,
    WorldLitAlphaTestSpecular,
    WorldLitAlphaBlendSpecular,
    WorldGlass,
    Count,
};

enum MaterialSlot : std::uint32_t {
    MaterialSlotColor = 1u,
    MaterialSlotNormal = 2u,
    MaterialSlotSpecularGloss = 4u,
};

// CoD4 surface types (surfaceFlags bits 20..24, SURF_TYPE_* in the engine's
// surfaceflags.h). Index 0 "default" is what a template usually carries, and
// it is NOT a usable collision type: Bullet_Trace gives it penetration depth
// 0 and no impact FX, so bullets stop silently and the model reads as having
// no collision at all. Every material and collision surface this tool writes
// therefore gets a real type; concrete is the neutral choice for props.
constexpr int kSurfaceTypeCount = 29;
constexpr int kSurfaceTypeConcrete = 5;
constexpr int kSurfaceTypeGlass = 9;
constexpr int kSurfaceTypeShift = 20;

inline const char* SurfaceTypeName(int index) {
    static constexpr const char* kNames[kSurfaceTypeCount] = {
        "default",    "bark",      "brick",  "carpet",    "cloth",
        "concrete",   "dirt",      "flesh",  "foliage",   "glass",
        "grass",      "gravel",    "ice",    "metal",     "mud",
        "paper",      "plaster",   "rock",   "sand",      "snow",
        "water",      "wood",      "asphalt", "ceramic",  "plastic",
        "rubber",     "cushion",   "fruit",  "paintedmetal",
    };
    return index >= 0 && index < kSurfaceTypeCount ? kNames[index] : "";
}

// Accepts a SurfaceTypeName spelling (ASCII case-insensitive) or a decimal
// index in [1, kSurfaceTypeCount). "default"/0 is rejected on purpose.
inline bool ParseSurfaceType(std::string_view text, int& index) {
    if (text.empty()) return false;
    bool numeric = true;
    int value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            numeric = false;
            break;
        }
        value = value * 10 + (character - '0');
        if (value >= kSurfaceTypeCount) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        if (value <= 0) return false;
        index = value;
        return true;
    }
    for (int candidate = 1; candidate < kSurfaceTypeCount; ++candidate) {
        const std::string_view name = SurfaceTypeName(candidate);
        if (name.size() != text.size()) continue;
        bool equal = true;
        for (std::size_t at = 0; at < name.size(); ++at) {
            char lhs = name[at];
            char rhs = text[at];
            if (rhs >= 'A' && rhs <= 'Z') rhs = static_cast<char>(rhs - 'A' + 'a');
            if (lhs != rhs) {
                equal = false;
                break;
            }
        }
        if (equal) {
            index = candidate;
            return true;
        }
    }
    return false;
}

struct MaterialTemplateInfo {
    MaterialTemplate value{};
    std::string_view label;
    std::string_view techniqueSet;
    std::string_view description;
    std::uint32_t slots = 0;
};

// Stable, process-lifetime metadata in the same order as MaterialTemplate.
std::span<const MaterialTemplateInfo> MaterialTemplates();
const MaterialTemplateInfo* GetMaterialTemplateInfo(MaterialTemplate value);

struct MaterialImageSlots {
    // Empty color defaults to the material name. Empty optional maps bind the
    // engine fallbacks $identitynormalmap and $black when the template consumes
    // those slots.
    std::string color;
    std::string normal;
    std::string specularGloss;
};

struct MaterialOptions {
    MaterialTemplate materialTemplate = MaterialTemplate::WorldLitOpaque;
    MaterialImageSlots images;

    // CoD4Radiant's browser gate rejects zero values for all four fields.
    std::uint8_t usage = 1;
    std::uint32_t locale = 1;
    std::uint16_t autoTexScaleWidth = 512;
    std::uint16_t autoTexScaleHeight = 512;

    // Negative preserves the template. Otherwise pass the already-shifted
    // CoD4 surface type field (index << 20); only mask 0x01f00000 is replaced.
    std::int32_t surfaceTypeBits = -1;
};

struct MaterialWriteResult {
    std::filesystem::path path;
    std::filesystem::path templatePath;
    std::string techniqueSet;
    std::uint32_t textureCount = 0;
    std::uint32_t constantCount = 0;
    std::uint64_t fileSize = 0;
};

// Read-only view of an existing load-object material. This deliberately
// reports only fields that are physically present in the material blob. In
// particular, specular RGB and gloss usually live in the semantic-8 IWI and
// therefore remain unknown here; they are not fabricated from template names.
enum class MaterialBlendMode : std::uint8_t {
    Unknown,
    Opaque,
    AlphaTest,
    AlphaBlend,
};

struct MaterialImageInfo {
    std::string slotName;
    std::string imageName;
    std::uint8_t samplerState = 0;
    std::uint8_t semantic = 0;
};

struct MaterialConstantInfo {
    std::string name;
    std::array<std::uint32_t, 4> literalBits{};
    std::array<float, 4> values{};
    bool finite = false;
};

struct MaterialInfo {
    std::filesystem::path path;
    std::string name;
    std::string referenceImage;
    std::string techniqueSet;
    std::uint8_t gameFlags = 0;
    std::uint8_t sortKey = 0;
    std::uint8_t usage = 0;
    std::uint32_t locale = 0;
    std::uint16_t autoTexScaleWidth = 0;
    std::uint16_t autoTexScaleHeight = 0;
    std::int32_t surfaceFlags = 0;
    std::int32_t contents = 0;
    MaterialBlendMode blendMode = MaterialBlendMode::Unknown;
    bool materialTemplateKnown = false;
    MaterialTemplate materialTemplate = MaterialTemplate::WorldLitOpaque;
    std::vector<MaterialImageInfo> images;
    std::vector<MaterialConstantInfo> constants;

    bool colorMapKnown = false;
    bool normalMapKnown = false;
    bool specularGlossMapKnown = false;
    std::string colorMap;
    std::string normalMap;
    std::string specularGlossMap;

    bool colorTintKnown = false;
    std::array<float, 4> colorTint{1.0f, 1.0f, 1.0f, 1.0f};

    // These stay false unless the material blob itself contains recoverable
    // values. Stock world materials normally put them in an IWI instead.
    bool specularColorKnown = false;
    std::array<float, 3> specularColor{};
    bool glossKnown = false;
    float gloss = 0.0f;

    // The browser may contain parseable effect/special materials that the
    // conservative clone writer intentionally refuses. Keep them browsable
    // and explain why editing is unavailable.
    bool cloneWritable = false;
    std::string cloneLimitation;
};

struct MaterialConstantOverride {
    std::string name;
    std::array<float, 4> values{};
};

struct MaterialEditOptions {
    bool replaceColorMap = false;
    bool replaceNormalMap = false;
    bool replaceSpecularGlossMap = false;
    std::string colorMap;
    std::string normalMap;
    std::string specularGlossMap;
    bool replaceSortKey = false;
    std::uint8_t sortKey = 0;
    std::vector<MaterialConstantOverride> constants;
    // Negative preserves the template's surface type. Otherwise the already
    // shifted CoD4 surface type field (index << kSurfaceTypeShift); only the
    // 0x01f00000 type bits are replaced, every other surfaceFlags bit is kept.
    std::int32_t surfaceTypeBits = -1;
};

const char* MaterialBlendModeLabel(MaterialBlendMode mode);

// Parses any structurally readable raw materials/<name> file without changing
// it. Unknown semantic values remain in images with their numeric semantic.
bool LoadMaterialInfo(const std::filesystem::path& path,
                      MaterialInfo& info,
                      std::string& error);

// Clones sourcePath, substitutes only the explicitly supported fields, and
// writes outputRawRoot/materials/<name>. Callers keep stock roots out of the
// output destination; saving an already-output asset may intentionally replace
// that output copy. The completed file is validated with RoundTripMaterial; a
// failed validation is rolled back when an older output file existed.
bool WriteEditedMaterial(const std::filesystem::path& sourcePath,
                         const std::filesystem::path& outputRawRoot,
                         std::string_view name,
                         const MaterialEditOptions& edits,
                         MaterialWriteResult* result,
                         std::string& error);

// Finds a compatible shipped template under rawRoot/materials. Named KIWI
// candidates are tried first, then the directory is scanned by shape.
bool ResolveMaterialTemplate(const std::filesystem::path& rawRoot,
                             MaterialTemplate value,
                             std::filesystem::path& resolvedPath,
                             std::string& error);

// Writes rawRoot/materials/<name>. `name` and image slot strings are CoD asset
// names, not filesystem paths; separators and control characters are rejected.
bool WriteMaterial(const std::filesystem::path& rawRoot,
                   std::string_view name,
                   const MaterialOptions& options,
                   MaterialWriteResult* result,
                   std::string& error);

// Standalone structural read-back for a written material. It validates every
// loader-dereferenced offset/string/table and the Radiant browser gate.
bool RoundTripMaterial(const std::filesystem::path& path, std::string& error);

}  // namespace cod4
