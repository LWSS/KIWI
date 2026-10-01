#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xmodel {

struct BakeVertex {
    std::array<double, 3> positionMeters{};
    std::array<double, 3> normal{};
    bool hasUv = false;
    std::array<double, 2> uv{};
};

struct BakeTriangle {
    std::array<BakeVertex, 3> corners{};
};

// One raw xmodelsurfs surface per material. Keeping triangles grouped here
// lets a later CAD-face material assignment feed the writer without changing
// the raw-format layer.
struct MaterialTriangles {
    std::string materialName;
    std::vector<BakeTriangle> triangles;
    // Collision surfFlags for this material's collision surface. Negative
    // selects the writer default (SURF_TYPE_CONCRETE, 0x00500000). Type 0
    // ("default") must never be written: the engine stops bullets on it with
    // zero penetration depth and no impact FX, so the prop looks uncollidable.
    int32_t collisionSurfaceFlags = -1;
};

// One occupied runtime LOD. WriteRawXModelV25 packs request.lods contiguously
// into the loader's four config slots; distance 0 is the stock "last LOD"
// sentinel (the v25 loader expands it to 1,000,000 units).
struct BakeLod {
    float distance = 0.0f;
    std::vector<MaterialTriangles> materials;
};

// Plasticity scenes usually sit far from the world origin; a placeable prop
// wants its pivot AT the origin (rotation untouched — translation only).
enum class OriginMode {
    None,          // keep Plasticity world coordinates
    BaseCenter,    // bounds center in X/Y, bounds bottom in Z -> 0/0/0 (props)
    BoundsCenter,  // bounds center -> 0/0/0
};

enum class UvMode {
    TilingBox,
    UniqueAtlas,
};

struct BakeRequest {
    std::filesystem::path outputRoot;
    std::string modelName;
    double uvTileSizeInches = 64.0;
    OriginMode originMode = OriginMode::BaseCenter;
    std::vector<BakeLod> lods;
    uint32_t collisionLod = 0;
};

struct BakeResult {
    bool success = false;
    std::string message;
    std::vector<std::filesystem::path> writtenPaths;
    uint64_t lodCount = 0;
    uint64_t surfaceCount = 0;
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;
    // The translation subtracted from every vertex (inches), for the UI line.
    std::array<double, 3> appliedOffsetInches{};
};

struct BakeSettings {
    std::string modelName = "plastic_model";
    std::string outputRoot = ".";
    OriginMode originMode = OriginMode::BaseCenter;
    double uvTileSizeInches = 64.0;
    UvMode uvMode = UvMode::TilingBox;
    uint32_t textureSize = 1024;
};

// Lowercases ASCII, replaces every character outside [a-z0-9_] with '_', and
// truncates to the engine's shortest 64-byte prefixed-path limit.
std::string SanitizeModelName(std::string_view name);

bool LoadBakeSettings(const std::filesystem::path& path, BakeSettings& settings,
                      std::string& error);
bool SaveBakeSettings(const std::filesystem::path& path, const BakeSettings& settings,
                      std::string& error);

BakeResult WriteRawXModelV25(const BakeRequest& request);

}  // namespace xmodel
