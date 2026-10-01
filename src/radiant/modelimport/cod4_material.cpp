#include "cod4_material.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cod4 {
namespace {

constexpr std::size_t kMaterialInfoSize = 0x28;
constexpr std::size_t kMaterialHeaderSize = 0x40;
constexpr std::size_t kTextureEntrySize = 0x0c;
constexpr std::size_t kConstantEntrySize = 0x14;
constexpr std::uint32_t kSurfaceTypeMask = 0x01f00000u;
constexpr std::size_t kMaxAssetNameBytes = 50;
constexpr std::size_t kTemplateCount =
    static_cast<std::size_t>(MaterialTemplate::Count);

constexpr std::array<MaterialTemplateInfo, kTemplateCount> kTemplateInfo{{
    {MaterialTemplate::WorldLitOpaque,
     "World - lit, opaque",
     "l_sm_r0c0",
     "Standard lit world surface. The usual choice for walls, floors and terrain.",
     MaterialSlotColor},
    {MaterialTemplate::WorldLitAlphaTest,
     "World - lit, alpha test",
     "l_sm_t0c0",
     "Lit, with hard cut-out transparency from the image's alpha. Fences, foliage, grates.",
     MaterialSlotColor},
    {MaterialTemplate::WorldLitAlphaBlend,
     "World - lit, alpha blend",
     "l_sm_b0c0",
     "Lit, with soft alpha blending and a decal sort key. Signs, posters, painted overlays.",
     MaterialSlotColor},
    {MaterialTemplate::WorldUnlitOpaque,
     "World - unlit, opaque",
     "unlit",
     "Fullbright, ignores lighting entirely. Pre-baked art and light-panel surfaces.",
     MaterialSlotColor},
    {MaterialTemplate::WorldLitNormalSpecular,
     "World - lit, normal + specular",
     "l_sm_r0c0n0s0",
     "Fully detailed lit surface. Shiny/detailed surfaces: metals, machined trim, wet stone.",
     MaterialSlotColor | MaterialSlotNormal | MaterialSlotSpecularGloss},
    {MaterialTemplate::WorldLitAlphaTestNormalSpecular,
     "World - lit, alpha test, normal + specular",
     "l_sm_t0c0n0s0",
     "Fully detailed with hard cut-out transparency. Metal grates, chain-link, railings.",
     MaterialSlotColor | MaterialSlotNormal | MaterialSlotSpecularGloss},
    {MaterialTemplate::WorldLitAlphaBlendNormalSpecular,
     "World - lit, alpha blend, normal + specular",
     "l_sm_b0c0n0s0",
     "Fully detailed soft-blended overlay with a decal sort key. Relief decals, grime, wet patches.",
     MaterialSlotColor | MaterialSlotNormal | MaterialSlotSpecularGloss},
    {MaterialTemplate::WorldLitNormal,
     "World - lit, normal",
     "l_sm_r0c0n0",
     "Lit and bumped but not shiny. Plaster, rough concrete, fabric, carpet.",
     MaterialSlotColor | MaterialSlotNormal},
    {MaterialTemplate::WorldLitAlphaTestNormal,
     "World - lit, alpha test, normal",
     "l_sm_t0c0n0",
     "Bumped, with hard cut-out transparency and no specular. Rugs, cut-out signage.",
     MaterialSlotColor | MaterialSlotNormal},
    {MaterialTemplate::WorldLitAlphaBlendNormal,
     "World - lit, alpha blend, normal",
     "l_sm_b0c0n0",
     "Bumped soft-blended overlay, decal sort key, no specular. Chipped paint, patches, tracks.",
     MaterialSlotColor | MaterialSlotNormal},
    {MaterialTemplate::WorldLitSpecular,
     "World - lit, specular",
     "l_sm_r0c0s0",
     "Shiny but flat: reflection without relief. Tile, painted metal, polished stone.",
     MaterialSlotColor | MaterialSlotSpecularGloss},
    {MaterialTemplate::WorldLitAlphaTestSpecular,
     "World - lit, alpha test, specular",
     "l_sm_t0c0s0",
     "Reflective and flat, with hard cut-out transparency.",
     MaterialSlotColor | MaterialSlotSpecularGloss},
    {MaterialTemplate::WorldLitAlphaBlendSpecular,
     "World - lit, alpha blend, specular",
     "l_sm_b0c0s0",
     "Reflective soft-blended overlay with a decal sort key.",
     MaterialSlotColor | MaterialSlotSpecularGloss},
    {MaterialTemplate::WorldGlass,
     "World - glass (alpha blend + specular)",
     "l_sm_b0c0s0",
     "What the shipped glass uses: blended, reflective, glass surface type and glass contents.",
     MaterialSlotColor | MaterialSlotSpecularGloss},
}};

struct TemplateRule {
    std::array<std::string_view, 5> candidates;
    std::uint8_t expectedSortKey = 0;
    std::uint16_t expectedTextureCount = 0;
};

// KIWI kiwi_matwriter.cpp:46-127, including candidate priority and the glass
// row's distinct sort key.
constexpr std::array<TemplateRule, kTemplateCount> kTemplateRules{{
    {{{"ch_concrete_01", "ch_brick_wall_03", "case256x32red", "", ""}}, 4, 2},
    {{{"icbm_wirescreen", "me_curtains_1", "ch_windowslatbroke01_a", "", ""}}, 4, 2},
    {{{"ch_decal_mural", "ad_sign256x256", "ad_sign128x256", "", ""}}, 12, 2},
    {{{"ac_building_wall01", "ac_building_door01", "ac_ground_base01", "", ""}}, 4, 1},
    {{{"ch_brick_wall_01", "ch_concrete_03", "ch_ceiling01", "ch_concrete_base_01",
       "ch_brick_wall_05"}}, 4, 3},
    {{{"ch_factory_floorgrate", "me_floor_metalgratingparallel_1side", "me_railing_plaster",
       "me_floor_metalgratingparallel", "me_fence_chainlink"}}, 4, 3},
    {{{"ch_dec_concretewall01", "me_decal_brick", "ch_concrete_floor01_dec",
       "me_metal_peelingpaint_01", "ch_patchydirt01"}}, 12, 3},
    {{{"case512", "case256", "ap_office_carpet", "case1024", "case"}}, 4, 2},
    {{{"com_garbage", "mtl_canopy06", "mtl_chechnya_log", "mtl_chechnya_trunk", ""}}, 4, 2},
    {{{"ch_wallpaintchipped_1_dec", "me_decal_concretepatch", "me_decal_wall_chipped_1",
       "ch_dec_carpettorn", "me_decal_bullet_holes02"}}, 12, 2},
    {{{"ch_reactor_metal_3", "ch_tile_floor01", "ch_tile_wall_01", "me_wallpaper2",
       "ch_wallwhiteblue01"}}, 4, 3},
    {{{"icbm_securityglass", "mtl_mig29_desert", "mtl_news_ticker_chain", "mtl_weapon_m84",
       "mtl_weapon_m84_burnt"}}, 4, 3},
    {{{"ch_cyrillic_dark01", "ch_trim_stainlessteel_1_decal", "icbm_decal_bloodpool",
       "kh_wall_damp01", "me_decal_oil_leak"}}, 12, 3},
    {{{"ap_glass_sanded", "com_glass_clear", "me_glass", "me_railingpanelsblueglass",
       "mtl_glass_clear_test"}}, 43, 3},
}};

struct TextureEntry {
    std::string name;
    std::string image;
    std::uint8_t samplerState = 0;
    std::uint8_t semantic = 0;
};

struct ConstantEntry {
    std::string name;
    std::array<std::uint32_t, 4> literalBits{};
};

struct ParsedMaterial {
    std::array<std::uint8_t, kMaterialInfoSize> infoRaw{};
    std::uint8_t gameFlags = 0;
    std::uint8_t sortKey = 0;
    std::uint8_t usage = 0;
    std::uint32_t locale = 0;
    std::uint16_t autoTexScaleWidth = 0;
    std::uint16_t autoTexScaleHeight = 0;
    std::int32_t surfaceFlags = 0;
    std::int32_t contents = 0;
    std::array<std::uint32_t, 2> refStateBits{};
    std::uint32_t textureTableOffset = 0;
    std::uint32_t constantTableOffset = 0;
    std::string name;
    std::string refImage;
    std::string techniqueSet;
    std::vector<TextureEntry> textures;
    std::vector<ConstantEntry> constants;
};

void SetError(std::string& error, std::string message) {
    error = std::move(message);
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void WriteU16(std::vector<std::uint8_t>& bytes,
              std::size_t offset,
              std::uint16_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void WriteU32(std::vector<std::uint8_t>& bytes,
              std::size_t offset,
              std::uint32_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

bool ReadString(const std::vector<std::uint8_t>& bytes,
                std::uint32_t offset,
                std::string& value) {
    if (offset == 0 || offset >= bytes.size())
        return false;
    for (std::size_t end = offset; end < bytes.size(); ++end) {
        if (bytes[end] == 0) {
            value.assign(reinterpret_cast<const char*>(bytes.data() + offset), end - offset);
            return true;
        }
    }
    return false;
}

bool ReadFile(const std::filesystem::path& path,
              std::vector<std::uint8_t>& bytes,
              std::string& error) {
    std::error_code filesystemError;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, filesystemError);
    if (filesystemError) {
        SetError(error, "could not stat '" + path.string() + "': " + filesystemError.message());
        return false;
    }
    if (fileSize > std::numeric_limits<std::size_t>::max()) {
        SetError(error, "material file is too large to address");
        return false;
    }
    if (fileSize > static_cast<std::uintmax_t>(
                       std::numeric_limits<std::streamsize>::max())) {
        SetError(error, "material file is too large for a stream read");
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        SetError(error, "could not open '" + path.string() + "' for reading");
        return false;
    }
    bytes.resize(static_cast<std::size_t>(fileSize));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        SetError(error, "short read from '" + path.string() + "'");
        return false;
    }
    return true;
}

bool WriteFile(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& bytes,
               std::string& error) {
    std::error_code filesystemError;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError) {
            SetError(error, "could not create '" + parent.string() + "': " +
                                filesystemError.message());
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        SetError(error, "could not open '" + path.string() + "' for writing");
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(path, filesystemError);
        SetError(error, "short write to '" + path.string() + "'");
        return false;
    }
    return true;
}

// KIWI kiwi_matwriter.cpp:170-260: parse exactly the offsets consumed by
// Material_LoadRaw, but do the reads explicitly so this module has no engine ABI dependency.
bool ParseMaterial(const std::vector<std::uint8_t>& bytes,
                   ParsedMaterial& material,
                   std::string& error) {
    material = {};
    if (bytes.size() < kMaterialHeaderSize) {
        SetError(error, "material is shorter than the 64-byte MaterialRaw header");
        return false;
    }

    std::copy_n(bytes.begin(), kMaterialInfoSize, material.infoRaw.begin());
    material.gameFlags = bytes[8];
    material.sortKey = bytes[9];
    material.usage = bytes[17];
    material.locale = ReadU32(bytes, 20);
    material.autoTexScaleWidth = ReadU16(bytes, 24);
    material.autoTexScaleHeight = ReadU16(bytes, 26);
    material.surfaceFlags = static_cast<std::int32_t>(ReadU32(bytes, 32));
    material.contents = static_cast<std::int32_t>(ReadU32(bytes, 36));
    material.refStateBits[0] = ReadU32(bytes, 40);
    material.refStateBits[1] = ReadU32(bytes, 44);

    const std::uint16_t textureCount = ReadU16(bytes, 48);
    const std::uint16_t constantCount = ReadU16(bytes, 50);
    const std::uint32_t techniqueSetOffset = ReadU32(bytes, 52);
    material.textureTableOffset = ReadU32(bytes, 56);
    material.constantTableOffset = ReadU32(bytes, 60);

    if (!ReadString(bytes, ReadU32(bytes, 0), material.name)) {
        SetError(error, "material name offset does not address a terminated string");
        return false;
    }
    if (!ReadString(bytes, techniqueSetOffset, material.techniqueSet)) {
        SetError(error, "technique-set offset does not address a terminated string");
        return false;
    }
    if (!ReadString(bytes, ReadU32(bytes, 4), material.refImage))
        material.refImage.clear();

    if (textureCount != 0 &&
        (material.textureTableOffset > bytes.size() ||
         textureCount > (bytes.size() - material.textureTableOffset) / kTextureEntrySize)) {
        SetError(error, "texture table extends beyond the material blob");
        return false;
    }
    material.textures.reserve(textureCount);
    for (std::uint32_t index = 0; index < textureCount; ++index) {
        const std::size_t offset =
            material.textureTableOffset + kTextureEntrySize * index;
        TextureEntry texture;
        if (!ReadString(bytes, ReadU32(bytes, offset), texture.name) ||
            !ReadString(bytes, ReadU32(bytes, offset + 8), texture.image)) {
            SetError(error, "texture entry contains an invalid string offset");
            return false;
        }
        texture.samplerState = bytes[offset + 4];
        texture.semantic = bytes[offset + 5];
        material.textures.push_back(std::move(texture));
    }

    if (constantCount != 0 &&
        (material.constantTableOffset > bytes.size() ||
         constantCount > (bytes.size() - material.constantTableOffset) / kConstantEntrySize)) {
        SetError(error, "constant table extends beyond the material blob");
        return false;
    }
    material.constants.reserve(constantCount);
    for (std::uint32_t index = 0; index < constantCount; ++index) {
        const std::size_t offset =
            material.constantTableOffset + kConstantEntrySize * index;
        ConstantEntry constant;
        if (!ReadString(bytes, ReadU32(bytes, offset), constant.name)) {
            SetError(error, "constant entry contains an invalid name offset");
            return false;
        }
        for (std::size_t component = 0; component < constant.literalBits.size(); ++component)
            constant.literalBits[component] = ReadU32(bytes, offset + 4 + 4 * component);
        material.constants.push_back(std::move(constant));
    }

    error.clear();
    return true;
}

std::uint32_t SlotForSemantic(std::uint8_t semantic) {
    if (semantic == 2)
        return MaterialSlotColor;
    if (semantic == 5)
        return MaterialSlotNormal;
    if (semantic == 8)
        return MaterialSlotSpecularGloss;
    return 0;
}

std::string_view BuiltinForSlot(std::uint32_t slot) {
    if (slot == MaterialSlotNormal)
        return "$identitynormalmap";
    if (slot == MaterialSlotSpecularGloss)
        return "$black";
    return {};
}

// KIWI kiwi_matwriter.cpp:281-323.
bool TemplateShapeMatches(std::size_t templateIndex, const ParsedMaterial& material) {
    const MaterialTemplateInfo& info = kTemplateInfo[templateIndex];
    const TemplateRule& rule = kTemplateRules[templateIndex];
    if (material.techniqueSet != info.techniqueSet)
        return false;
    if (info.techniqueSet.starts_with("l_sm_") && material.gameFlags != 0x12)
        return false;
    if (material.sortKey != rule.expectedSortKey ||
        material.textures.size() != rule.expectedTextureCount || material.constants.empty()) {
        return false;
    }

    std::uint32_t colorMaps = 0;
    for (const TextureEntry& texture : material.textures) {
        if (texture.semantic == 11 || texture.image.empty())
            return false;
        const std::uint32_t slot = SlotForSemantic(texture.semantic);
        const bool builtIn = texture.image.front() == '$';
        if (texture.semantic == 2)
            ++colorMaps;
        if (slot != 0 && (info.slots & slot) != 0) {
            if (builtIn)
                return false;
        } else if (!builtIn) {
            return false;
        }
    }
    return colorMaps == 1 && material.usage != 0 && material.locale != 0;
}

bool ValidTemplateIndex(MaterialTemplate value, std::size_t& index) {
    index = static_cast<std::size_t>(value);
    return index < kTemplateCount;
}

bool ResolveTemplate(const std::filesystem::path& rawRoot,
                     std::size_t templateIndex,
                     std::filesystem::path& resolvedPath,
                     ParsedMaterial* parsed,
                     std::string& error) {
    const std::filesystem::path materialsDirectory = rawRoot / "materials";
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(materialsDirectory, filesystemError)) {
        SetError(error, "template directory '" + materialsDirectory.string() +
                            "' is not readable");
        return false;
    }

    auto tryPath = [&](const std::filesystem::path& path, ParsedMaterial& material) {
        std::vector<std::uint8_t> bytes;
        std::string ignored;
        return ReadFile(path, bytes, ignored) && ParseMaterial(bytes, material, ignored) &&
               TemplateShapeMatches(templateIndex, material);
    };

    // KIWI kiwi_matwriter.cpp:351-362: named shipped candidates first.
    for (std::string_view candidate : kTemplateRules[templateIndex].candidates) {
        if (candidate.empty())
            continue;
        const std::filesystem::path path = materialsDirectory / std::string(candidate);
        ParsedMaterial material;
        if (!tryPath(path, material))
            continue;
        resolvedPath = path;
        if (parsed)
            *parsed = std::move(material);
        error.clear();
        return true;
    }

    // KIWI kiwi_matwriter.cpp:364-382: compatible-shape directory fallback.
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(materialsDirectory, filesystemError), end;
         !filesystemError && iterator != end; iterator.increment(filesystemError)) {
        if (iterator->is_regular_file(filesystemError) &&
            !iterator->path().filename().string().starts_with('$')) {
            candidates.push_back(iterator->path());
        }
    }
    if (filesystemError) {
        SetError(error, "could not scan '" + materialsDirectory.string() + "': " +
                            filesystemError.message());
        return false;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const std::filesystem::path& left, const std::filesystem::path& right) {
                  return left.filename().string() < right.filename().string();
              });
    for (const std::filesystem::path& path : candidates) {
        ParsedMaterial material;
        if (!tryPath(path, material))
            continue;
        resolvedPath = path;
        if (parsed)
            *parsed = std::move(material);
        error.clear();
        return true;
    }

    SetError(error, "no compatible shipped material template for '" +
                        std::string(kTemplateInfo[templateIndex].label) + "'");
    return false;
}

bool IsValidAssetName(std::string_view value, bool allowEmpty, std::string& reason) {
    if (value.empty()) {
        if (allowEmpty)
            return true;
        reason = "asset name is empty";
        return false;
    }
    if (value.size() > kMaxAssetNameBytes) {
        reason = "asset name exceeds the engine's 50-byte limit";
        return false;
    }
    if (value == "." || value == "..") {
        reason = "asset name cannot be '.' or '..'";
        return false;
    }
    for (unsigned char character : value) {
        if (character < 0x20 || character == 0x7f || character == '/' || character == '\\' ||
            character == ':') {
            reason = "asset name contains a path separator or control character";
            return false;
        }
    }
    return true;
}

bool ValidateWrittenMaterial(const std::vector<std::uint8_t>& bytes,
                             std::string_view expectedName,
                             std::string_view expectedTechniqueSet,
                             std::uint8_t expectedSortKey,
                             const std::vector<TextureEntry>& expectedTextures,
                             const std::vector<ConstantEntry>& expectedConstants,
                             std::string& error) {
    ParsedMaterial parsed;
    if (!ParseMaterial(bytes, parsed, error))
        return false;
    if (parsed.name != expectedName || parsed.techniqueSet != expectedTechniqueSet ||
        parsed.sortKey != expectedSortKey) {
        SetError(error,
                 "material read-back name, technique set, or sort key differs from the write plan");
        return false;
    }
    if (parsed.textures.size() != expectedTextures.size() ||
        parsed.constants.size() != expectedConstants.size()) {
        SetError(error, "material read-back table counts differ from the write plan");
        return false;
    }
    for (std::size_t index = 0; index < parsed.textures.size(); ++index) {
        const TextureEntry& actual = parsed.textures[index];
        const TextureEntry& expected = expectedTextures[index];
        if (actual.name != expected.name || actual.image != expected.image ||
            actual.samplerState != expected.samplerState || actual.semantic != expected.semantic) {
            SetError(error, "material texture table differs from the write plan");
            return false;
        }
    }
    for (std::size_t index = 0; index < parsed.constants.size(); ++index) {
        const ConstantEntry& actual = parsed.constants[index];
        const ConstantEntry& expected = expectedConstants[index];
        if (actual.name != expected.name || actual.literalBits != expected.literalBits) {
            SetError(error, "material constant table differs from the template");
            return false;
        }
    }
    if (parsed.usage == 0 || parsed.locale == 0 || parsed.autoTexScaleWidth == 0 ||
        parsed.autoTexScaleHeight == 0) {
        SetError(error, "material fails the Radiant usage/locale/autoTexScale browser gate");
        return false;
    }
    error.clear();
    return true;
}

MaterialBlendMode InferBlendMode(std::string_view techniqueSet) {
    if (techniqueSet.starts_with("l_sm_r"))
        return MaterialBlendMode::Opaque;
    if (techniqueSet.starts_with("l_sm_t"))
        return MaterialBlendMode::AlphaTest;
    if (techniqueSet.starts_with("l_sm_b"))
        return MaterialBlendMode::AlphaBlend;
    if (techniqueSet == "unlit")
        return MaterialBlendMode::Opaque;
    return MaterialBlendMode::Unknown;
}

bool ValidateRoundTripParsed(const std::filesystem::path& path,
                             const ParsedMaterial& material,
                             std::string& error) {
    if (material.name.empty() || material.techniqueSet.empty()) {
        SetError(error, "material has an empty embedded name or technique set");
        return false;
    }
    if (material.name != path.filename().string()) {
        SetError(error, "embedded material name does not match the file name");
        return false;
    }
    if (material.sortKey >= 0x40) {
        SetError(error, "material sort key is outside the engine's [0, 0x40) range");
        return false;
    }
    if (material.usage == 0 || material.locale == 0 ||
        material.autoTexScaleWidth == 0 || material.autoTexScaleHeight == 0) {
        SetError(error, "material fails the Radiant usage/locale/autoTexScale browser gate");
        return false;
    }
    for (const TextureEntry& texture : material.textures) {
        if (texture.name.empty() || texture.image.empty() ||
            (texture.samplerState & 7u) == 0 || texture.semantic == 11) {
            SetError(error, "material contains an invalid non-image texture entry");
            return false;
        }
    }
    for (const ConstantEntry& constant : material.constants) {
        if (constant.name.empty()) {
            SetError(error, "material contains an unnamed constant");
            return false;
        }
    }
    error.clear();
    return true;
}

bool SerializeMaterial(const ParsedMaterial& source,
                       std::string_view newName,
                       std::string_view referenceImage,
                       std::uint8_t sortKey,
                       std::int32_t surfaceTypeBits,
                       const std::vector<TextureEntry>& textures,
                       const std::vector<ConstantEntry>& constants,
                       std::vector<std::uint8_t>& output,
                       std::string& error) {
    const std::uint64_t textureTableOffset = kMaterialHeaderSize;
    const std::uint64_t constantTableOffset =
        textureTableOffset + kTextureEntrySize * textures.size();
    const std::uint64_t stringBase =
        constantTableOffset + kConstantEntrySize * constants.size();
    if (textures.size() > std::numeric_limits<std::uint16_t>::max() ||
        constants.size() > std::numeric_limits<std::uint16_t>::max() ||
        stringBase > std::numeric_limits<std::uint32_t>::max()) {
        SetError(error, "material tables exceed the raw format limits");
        return false;
    }

    std::vector<std::uint8_t> strings;
    std::vector<std::string> interned;
    std::vector<std::uint32_t> internedOffsets;
    auto intern = [&](std::string_view value) {
        for (std::size_t index = 0; index < interned.size(); ++index) {
            if (interned[index] == value)
                return internedOffsets[index];
        }
        const std::uint64_t wideOffset = stringBase + strings.size();
        if (wideOffset > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("material string block exceeds 32-bit offsets");
        const std::uint32_t offset = static_cast<std::uint32_t>(wideOffset);
        strings.insert(strings.end(), value.begin(), value.end());
        strings.push_back(0);
        interned.emplace_back(value);
        internedOffsets.push_back(offset);
        return offset;
    };

    const std::uint32_t techniqueSetNameOffset = intern(source.techniqueSet);
    const std::uint32_t materialNameOffset = intern(newName);
    const std::uint32_t referenceImageOffset =
        referenceImage.empty() ? 0u : intern(referenceImage);
    std::vector<std::uint32_t> textureNameOffsets(textures.size());
    std::vector<std::uint32_t> textureImageOffsets(textures.size());
    for (std::size_t index = 0; index < textures.size(); ++index) {
        textureNameOffsets[index] = intern(textures[index].name);
        textureImageOffsets[index] = intern(textures[index].image);
    }
    std::vector<std::uint32_t> constantNameOffsets(constants.size());
    for (std::size_t index = 0; index < constants.size(); ++index)
        constantNameOffsets[index] = intern(constants[index].name);

    const std::uint64_t outputSize = stringBase + strings.size();
    if (outputSize > std::numeric_limits<std::uint32_t>::max()) {
        SetError(error, "material blob exceeds 32-bit raw offsets");
        return false;
    }
    output.assign(static_cast<std::size_t>(outputSize), 0u);
    std::copy(source.infoRaw.begin(), source.infoRaw.end(), output.begin());
    WriteU32(output, 0, materialNameOffset);
    WriteU32(output, 4, referenceImageOffset);
    output[9] = sortKey;
    if (surfaceTypeBits >= 0) {
        const std::uint32_t templateSurfaceFlags =
            static_cast<std::uint32_t>(source.surfaceFlags);
        const std::uint32_t surfaceFlags =
            (templateSurfaceFlags & ~kSurfaceTypeMask) |
            (static_cast<std::uint32_t>(surfaceTypeBits) & kSurfaceTypeMask);
        WriteU32(output, 32, surfaceFlags);
    }
    WriteU32(output, 40, source.refStateBits[0]);
    WriteU32(output, 44, source.refStateBits[1]);
    WriteU16(output, 48, static_cast<std::uint16_t>(textures.size()));
    WriteU16(output, 50, static_cast<std::uint16_t>(constants.size()));
    WriteU32(output, 52, techniqueSetNameOffset);
    WriteU32(output, 56, static_cast<std::uint32_t>(textureTableOffset));
    WriteU32(output, 60, static_cast<std::uint32_t>(constantTableOffset));

    for (std::size_t index = 0; index < textures.size(); ++index) {
        const std::size_t offset = textureTableOffset + kTextureEntrySize * index;
        WriteU32(output, offset, textureNameOffsets[index]);
        output[offset + 4] = textures[index].samplerState;
        output[offset + 5] = textures[index].semantic;
        WriteU32(output, offset + 8, textureImageOffsets[index]);
    }
    for (std::size_t index = 0; index < constants.size(); ++index) {
        const std::size_t offset = constantTableOffset + kConstantEntrySize * index;
        WriteU32(output, offset, constantNameOffsets[index]);
        for (std::size_t component = 0;
             component < constants[index].literalBits.size(); ++component) {
            WriteU32(output, offset + 4 + 4 * component,
                     constants[index].literalBits[component]);
        }
    }
    std::copy(strings.begin(), strings.end(),
              output.begin() + static_cast<std::ptrdiff_t>(stringBase));
    return ValidateWrittenMaterial(output, newName, source.techniqueSet,
                                   sortKey, textures, constants, error);
}

}  // namespace

std::span<const MaterialTemplateInfo> MaterialTemplates() {
    return kTemplateInfo;
}

const MaterialTemplateInfo* GetMaterialTemplateInfo(MaterialTemplate value) {
    std::size_t index = 0;
    return ValidTemplateIndex(value, index) ? &kTemplateInfo[index] : nullptr;
}

const char* MaterialBlendModeLabel(MaterialBlendMode mode) {
    switch (mode) {
    case MaterialBlendMode::Opaque: return "Opaque";
    case MaterialBlendMode::AlphaTest: return "Alpha test";
    case MaterialBlendMode::AlphaBlend: return "Alpha blend";
    default: return "Unknown";
    }
}

bool LoadMaterialInfo(const std::filesystem::path& path,
                      MaterialInfo& info,
                      std::string& error) {
    info = {};
    error.clear();
    try {
        std::vector<std::uint8_t> bytes;
        if (!ReadFile(path, bytes, error))
            return false;
        ParsedMaterial material;
        if (!ParseMaterial(bytes, material, error)) {
            error = "'" + path.string() + "': " + error;
            return false;
        }

        info.path = path;
        info.name = material.name;
        info.referenceImage = material.refImage;
        info.techniqueSet = material.techniqueSet;
        info.gameFlags = material.gameFlags;
        info.sortKey = material.sortKey;
        info.usage = material.usage;
        info.locale = material.locale;
        info.autoTexScaleWidth = material.autoTexScaleWidth;
        info.autoTexScaleHeight = material.autoTexScaleHeight;
        info.surfaceFlags = material.surfaceFlags;
        info.contents = material.contents;
        info.blendMode = InferBlendMode(material.techniqueSet);

        for (std::size_t index = 0; index < kTemplateInfo.size(); ++index) {
            if (material.techniqueSet != kTemplateInfo[index].techniqueSet)
                continue;
            if (material.techniqueSet ==
                    kTemplateInfo[static_cast<std::size_t>(
                                      MaterialTemplate::WorldGlass)]
                        .techniqueSet &&
                material.sortKey ==
                    kTemplateRules[static_cast<std::size_t>(
                                       MaterialTemplate::WorldGlass)]
                        .expectedSortKey) {
                info.materialTemplate = MaterialTemplate::WorldGlass;
            } else {
                info.materialTemplate = kTemplateInfo[index].value;
            }
            info.materialTemplateKnown = true;
            break;
        }

        info.images.reserve(material.textures.size());
        for (const TextureEntry& texture : material.textures) {
            info.images.push_back({texture.name, texture.image,
                                   texture.samplerState, texture.semantic});
            if (texture.semantic == 2 && !info.colorMapKnown) {
                info.colorMapKnown = true;
                info.colorMap = texture.image;
            } else if (texture.semantic == 5 && !info.normalMapKnown) {
                info.normalMapKnown = true;
                info.normalMap = texture.image;
            } else if (texture.semantic == 8 &&
                       !info.specularGlossMapKnown) {
                info.specularGlossMapKnown = true;
                info.specularGlossMap = texture.image;
            }
        }

        info.constants.reserve(material.constants.size());
        for (const ConstantEntry& constant : material.constants) {
            MaterialConstantInfo publicConstant;
            publicConstant.name = constant.name;
            publicConstant.literalBits = constant.literalBits;
            publicConstant.finite = true;
            for (std::size_t component = 0;
                 component < constant.literalBits.size(); ++component) {
                publicConstant.values[component] =
                    std::bit_cast<float>(constant.literalBits[component]);
                publicConstant.finite =
                    publicConstant.finite &&
                    std::isfinite(publicConstant.values[component]);
            }
            if (publicConstant.name == "colorTint" &&
                publicConstant.finite) {
                info.colorTintKnown = true;
                info.colorTint = publicConstant.values;
            }
            info.constants.push_back(std::move(publicConstant));
        }

        std::string limitation;
        info.cloneWritable = ValidateRoundTripParsed(path, material, limitation);
        if (!info.cloneWritable)
            info.cloneLimitation = std::move(limitation);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        SetError(error, std::string("material info read failed: ") +
                            exception.what());
        return false;
    }
}

bool ResolveMaterialTemplate(const std::filesystem::path& rawRoot,
                             MaterialTemplate value,
                             std::filesystem::path& resolvedPath,
                             std::string& error) {
    resolvedPath.clear();
    error.clear();
    try {
        std::size_t index = 0;
        if (!ValidTemplateIndex(value, index)) {
            SetError(error, "invalid material template value");
            return false;
        }
        return ResolveTemplate(rawRoot, index, resolvedPath, nullptr, error);
    } catch (const std::exception& exception) {
        SetError(error, std::string("material template resolution failed: ") + exception.what());
        return false;
    }
}

bool WriteMaterial(const std::filesystem::path& rawRoot,
                   std::string_view name,
                   const MaterialOptions& options,
                   MaterialWriteResult* result,
                   std::string& error) {
    if (result)
        *result = {};
    error.clear();

    try {
        std::size_t templateIndex = 0;
        if (!ValidTemplateIndex(options.materialTemplate, templateIndex)) {
            SetError(error, "invalid material template value");
            return false;
        }
        if (rawRoot.empty()) {
            SetError(error, "no CoD4 raw root");
            return false;
        }

        std::string reason;
        if (!IsValidAssetName(name, false, reason) ||
            !IsValidAssetName(options.images.color, true, reason) ||
            !IsValidAssetName(options.images.normal, true, reason) ||
            !IsValidAssetName(options.images.specularGloss, true, reason)) {
            SetError(error, reason);
            return false;
        }
        if (options.usage == 0 || options.locale == 0 || options.autoTexScaleWidth == 0 ||
            options.autoTexScaleHeight == 0) {
            SetError(error,
                     "usage, locale, and both autoTexScale values must be non-zero");
            return false;
        }

        std::filesystem::path templatePath;
        ParsedMaterial source;
        if (!ResolveTemplate(rawRoot, templateIndex, templatePath, &source, error))
            return false;

        const std::string newName(name);
        const std::string colorImage =
            options.images.color.empty() ? newName : options.images.color;
        const MaterialTemplateInfo& info = kTemplateInfo[templateIndex];

        const std::uint64_t textureTableOffset = kMaterialHeaderSize;
        const std::uint64_t constantTableOffset =
            textureTableOffset + kTextureEntrySize * source.textures.size();
        const std::uint64_t stringBase =
            constantTableOffset + kConstantEntrySize * source.constants.size();
        if (stringBase > std::numeric_limits<std::uint32_t>::max()) {
            SetError(error, "material tables exceed the 32-bit raw-offset range");
            return false;
        }

        // KIWI kiwi_matwriter.cpp:555-621: same string interning order.
        std::vector<std::uint8_t> strings;
        std::vector<std::string> interned;
        std::vector<std::uint32_t> internedOffsets;
        auto intern = [&](std::string_view value) {
            for (std::size_t index = 0; index < interned.size(); ++index) {
                if (interned[index] == value)
                    return internedOffsets[index];
            }
            const std::uint64_t wideOffset = stringBase + strings.size();
            if (wideOffset > std::numeric_limits<std::uint32_t>::max())
                throw std::length_error("material string block exceeds 32-bit offsets");
            const std::uint32_t offset = static_cast<std::uint32_t>(wideOffset);
            strings.insert(strings.end(), value.begin(), value.end());
            strings.push_back(0);
            interned.emplace_back(value);
            internedOffsets.push_back(offset);
            return offset;
        };

        const std::uint32_t techniqueSetNameOffset = intern(source.techniqueSet);
        const std::uint32_t materialNameOffset = intern(newName);
        const std::uint32_t colorImageOffset = intern(colorImage);

        std::vector<TextureEntry> textures = source.textures;
        std::vector<std::uint32_t> textureNameOffsets(textures.size());
        std::vector<std::uint32_t> textureImageOffsets(textures.size());
        for (std::size_t index = 0; index < textures.size(); ++index) {
            textureNameOffsets[index] = intern(textures[index].name);
            const std::uint32_t slot = SlotForSemantic(textures[index].semantic);
            if (slot != 0 && (info.slots & slot) != 0) {
                if (slot == MaterialSlotColor) {
                    textures[index].image = colorImage;
                } else if (slot == MaterialSlotNormal) {
                    textures[index].image = options.images.normal.empty()
                                                ? std::string(BuiltinForSlot(slot))
                                                : options.images.normal;
                } else {
                    textures[index].image = options.images.specularGloss.empty()
                                                ? std::string(BuiltinForSlot(slot))
                                                : options.images.specularGloss;
                }
            }
            textureImageOffsets[index] = intern(textures[index].image);
        }
        std::vector<std::uint32_t> constantNameOffsets(source.constants.size());
        for (std::size_t index = 0; index < source.constants.size(); ++index)
            constantNameOffsets[index] = intern(source.constants[index].name);

        const std::uint64_t outputSize = stringBase + strings.size();
        if (outputSize > std::numeric_limits<std::uint32_t>::max()) {
            SetError(error, "material blob exceeds 32-bit raw offsets");
            return false;
        }
        std::vector<std::uint8_t> output(static_cast<std::size_t>(outputSize), 0);

        // KIWI kiwi_matwriter.cpp:624-645: clone all unowned header bytes and
        // replace only names, browser fields, surface type, and table metadata.
        std::copy(source.infoRaw.begin(), source.infoRaw.end(), output.begin());
        WriteU32(output, 0, materialNameOffset);
        WriteU32(output, 4, colorImageOffset);
        output[17] = options.usage;
        WriteU32(output, 20, options.locale);
        WriteU16(output, 24, options.autoTexScaleWidth);
        WriteU16(output, 26, options.autoTexScaleHeight);
        if (options.surfaceTypeBits >= 0) {
            const std::uint32_t templateSurfaceFlags =
                static_cast<std::uint32_t>(source.surfaceFlags);
            const std::uint32_t surfaceFlags =
                (templateSurfaceFlags & ~kSurfaceTypeMask) |
                (static_cast<std::uint32_t>(options.surfaceTypeBits) & kSurfaceTypeMask);
            WriteU32(output, 32, surfaceFlags);
        }
        WriteU32(output, 40, source.refStateBits[0]);
        WriteU32(output, 44, source.refStateBits[1]);
        WriteU16(output, 48, static_cast<std::uint16_t>(textures.size()));
        WriteU16(output, 50, static_cast<std::uint16_t>(source.constants.size()));
        WriteU32(output, 52, techniqueSetNameOffset);
        WriteU32(output, 56, static_cast<std::uint32_t>(textureTableOffset));
        WriteU32(output, 60, static_cast<std::uint32_t>(constantTableOffset));

        // KIWI kiwi_matwriter.cpp:647-663: entry metadata and literal float bits
        // are copied verbatim; only string offsets change.
        for (std::size_t index = 0; index < textures.size(); ++index) {
            const std::size_t offset = textureTableOffset + kTextureEntrySize * index;
            WriteU32(output, offset, textureNameOffsets[index]);
            output[offset + 4] = textures[index].samplerState;
            output[offset + 5] = textures[index].semantic;
            WriteU32(output, offset + 8, textureImageOffsets[index]);
        }
        for (std::size_t index = 0; index < source.constants.size(); ++index) {
            const std::size_t offset = constantTableOffset + kConstantEntrySize * index;
            WriteU32(output, offset, constantNameOffsets[index]);
            for (std::size_t component = 0;
                 component < source.constants[index].literalBits.size(); ++component) {
                WriteU32(output, offset + 4 + 4 * component,
                         source.constants[index].literalBits[component]);
            }
        }
        std::copy(strings.begin(), strings.end(),
                  output.begin() + static_cast<std::ptrdiff_t>(stringBase));

        if (!ValidateWrittenMaterial(output, newName, source.techniqueSet,
                                     source.sortKey, textures,
                                     source.constants, error)) {
            return false;
        }

        const std::filesystem::path outputPath = rawRoot / "materials" / newName;
        if (!WriteFile(outputPath, output, error))
            return false;

        if (result) {
            result->path = outputPath;
            result->templatePath = templatePath;
            result->techniqueSet = source.techniqueSet;
            result->textureCount = static_cast<std::uint32_t>(textures.size());
            result->constantCount = static_cast<std::uint32_t>(source.constants.size());
            result->fileSize = output.size();
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        SetError(error, std::string("material write failed: ") + exception.what());
        return false;
    }
}

bool WriteEditedMaterial(const std::filesystem::path& sourcePath,
                         const std::filesystem::path& outputRawRoot,
                         std::string_view name,
                         const MaterialEditOptions& edits,
                         MaterialWriteResult* result,
                         std::string& error) {
    if (result)
        *result = {};
    error.clear();
    try {
        if (outputRawRoot.empty()) {
            SetError(error, "no CoD4 output raw root");
            return false;
        }
        std::string reason;
        if (!IsValidAssetName(name, false, reason)) {
            SetError(error, reason);
            return false;
        }
        if ((edits.replaceColorMap &&
             !IsValidAssetName(edits.colorMap, false, reason)) ||
            (edits.replaceNormalMap &&
             !IsValidAssetName(edits.normalMap, false, reason)) ||
            (edits.replaceSpecularGlossMap &&
             !IsValidAssetName(edits.specularGlossMap, false, reason))) {
            SetError(error, reason);
            return false;
        }
        if (edits.replaceSortKey && edits.sortKey >= 0x40) {
            SetError(error,
                     "sort key must be in the engine's [0, 0x40) range");
            return false;
        }

        std::vector<std::uint8_t> sourceBytes;
        if (!ReadFile(sourcePath, sourceBytes, error))
            return false;
        ParsedMaterial source;
        if (!ParseMaterial(sourceBytes, source, error)) {
            error = "'" + sourcePath.string() + "': " + error;
            return false;
        }
        std::string limitation;
        if (!ValidateRoundTripParsed(sourcePath, source, limitation)) {
            SetError(error,
                     "source material is not safe to clone: " + limitation);
            return false;
        }

        std::vector<TextureEntry> textures = source.textures;
        bool replacedColor = false;
        bool replacedNormal = false;
        bool replacedSpecularGloss = false;
        for (TextureEntry& texture : textures) {
            if (texture.semantic == 2 && edits.replaceColorMap) {
                texture.image = edits.colorMap;
                replacedColor = true;
            } else if (texture.semantic == 5 && edits.replaceNormalMap) {
                texture.image = edits.normalMap;
                replacedNormal = true;
            } else if (texture.semantic == 8 &&
                       edits.replaceSpecularGlossMap) {
                texture.image = edits.specularGlossMap;
                replacedSpecularGloss = true;
            }
        }
        if ((edits.replaceColorMap && !replacedColor) ||
            (edits.replaceNormalMap && !replacedNormal) ||
            (edits.replaceSpecularGlossMap && !replacedSpecularGloss)) {
            SetError(error,
                     "an edited image semantic is not present in the source material");
            return false;
        }

        std::vector<ConstantEntry> constants = source.constants;
        for (std::size_t editIndex = 0; editIndex < edits.constants.size();
             ++editIndex) {
            const MaterialConstantOverride& edit = edits.constants[editIndex];
            if (edit.name.empty()) {
                SetError(error, "constant override has an empty name");
                return false;
            }
            for (std::size_t earlier = 0; earlier < editIndex; ++earlier) {
                if (edits.constants[earlier].name == edit.name) {
                    SetError(error, "constant override '" + edit.name +
                                        "' is duplicated");
                    return false;
                }
            }
            auto found = std::find_if(
                constants.begin(), constants.end(),
                [&](const ConstantEntry& constant) {
                    return constant.name == edit.name;
                });
            if (found == constants.end()) {
                SetError(error, "constant override '" + edit.name +
                                    "' is not present in the source material");
                return false;
            }
            for (std::size_t component = 0; component < edit.values.size();
                 ++component) {
                if (!std::isfinite(edit.values[component])) {
                    SetError(error, "constant override '" + edit.name +
                                        "' contains a non-finite value");
                    return false;
                }
                found->literalBits[component] =
                    std::bit_cast<std::uint32_t>(edit.values[component]);
            }
        }

        std::string referenceImage = source.refImage;
        auto color = std::find_if(
            textures.begin(), textures.end(),
            [](const TextureEntry& texture) { return texture.semantic == 2; });
        if (color != textures.end())
            referenceImage = color->image;

        const std::uint8_t sortKey =
            edits.replaceSortKey ? edits.sortKey : source.sortKey;
        std::vector<std::uint8_t> output;
        if (!SerializeMaterial(source, name, referenceImage, sortKey,
                               edits.surfaceTypeBits, textures, constants,
                               output, error)) {
            return false;
        }

        const std::filesystem::path outputPath =
            outputRawRoot / "materials" / std::string(name);
        std::error_code filesystemError;
        const bool outputExisted =
            std::filesystem::exists(outputPath, filesystemError);
        if (filesystemError) {
            SetError(error, "could not inspect output material '" +
                                outputPath.string() + "': " +
                                filesystemError.message());
            return false;
        }
        std::vector<std::uint8_t> previousOutput;
        if (outputExisted && !ReadFile(outputPath, previousOutput, error))
            return false;
        if (!WriteFile(outputPath, output, error)) {
            const std::string writeError = error;
            if (outputExisted) {
                std::string rollbackError;
                if (!WriteFile(outputPath, previousOutput, rollbackError)) {
                    error = writeError + "; rollback also failed: " +
                            rollbackError;
                    return false;
                }
            }
            error = writeError;
            return false;
        }

        std::string roundTripError;
        if (!RoundTripMaterial(outputPath, roundTripError)) {
            std::string rollbackError;
            if (outputExisted) {
                if (!WriteFile(outputPath, previousOutput, rollbackError))
                    roundTripError +=
                        "; rollback also failed: " + rollbackError;
            } else {
                std::filesystem::remove(outputPath, filesystemError);
                if (filesystemError)
                    roundTripError +=
                        "; could not remove failed output: " +
                        filesystemError.message();
            }
            SetError(error,
                     "written material failed round-trip validation: " +
                         roundTripError);
            return false;
        }

        if (result) {
            result->path = outputPath;
            result->templatePath = sourcePath;
            result->techniqueSet = source.techniqueSet;
            result->textureCount =
                static_cast<std::uint32_t>(textures.size());
            result->constantCount =
                static_cast<std::uint32_t>(constants.size());
            result->fileSize = output.size();
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        SetError(error, std::string("edited material write failed: ") +
                            exception.what());
        return false;
    }
}

bool RoundTripMaterial(const std::filesystem::path& path, std::string& error) {
    error.clear();
    try {
        std::vector<std::uint8_t> bytes;
        if (!ReadFile(path, bytes, error))
            return false;
        ParsedMaterial material;
        if (!ParseMaterial(bytes, material, error)) {
            error = "'" + path.string() + "': " + error;
            return false;
        }
        return ValidateRoundTripParsed(path, material, error);
    } catch (const std::exception& exception) {
        SetError(error, std::string("material round-trip failed: ") + exception.what());
        return false;
    }
}

}  // namespace cod4
