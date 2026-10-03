#pragma once

// KIWI model importer - the converter core (obj2xmodel's RunConvert as a library).
//
// Source mesh (OBJ, or FBX converted to OBJ by ki_mesh) + one PBR texture set ->
// raw xmodel / xmodelparts / xmodelsurfs + three IWIs (+ a glass IWI) + the
// materials, all under `outputRoot` (the game's raw\ folder). The code below the
// public API is obj2xmodel's (F:\gitshit\Plastic\src\obj2xmodel_main.cpp, 2026-09-29)
// so a build here writes the same bytes obj2xmodel did.

#include "cod4_material.h"
#include "ki_pieces.h"
#include "xmodel_write.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kimport {

using LogFn = std::function<void(const std::string&)>;

constexpr double kMetersPerInch = 0.0254;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    bool operator==(const Vec3&) const = default;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(const Vec3& value, double scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

inline Vec3 operator/(const Vec3& value, double scale) {
    return {value.x / scale, value.y / scale, value.z / scale};
}

inline Vec3& operator+=(Vec3& destination, const Vec3& value) {
    destination.x += value.x;
    destination.y += value.y;
    destination.z += value.z;
    return destination;
}

inline double Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline double Length(const Vec3& value) {
    return std::sqrt(Dot(value, value));
}

inline bool IsFinite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

inline bool Normalize(const Vec3& value, Vec3& normalized) {
    const double length = Length(value);
    if (!(length > 1.0e-12) || !std::isfinite(length)) return false;
    normalized = value / length;
    return IsFinite(normalized);
}

struct ObjCorner {
    std::size_t position = 0;
    std::size_t texcoord = 0;
    std::optional<std::size_t> normal;
};

struct IndexedTriangle {
    std::array<ObjCorner, 3> corners{};
    std::size_t sourceLine = 0;
};

struct ObjMesh {
    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;
    std::vector<IndexedTriangle> triangles;
    // `o` / `g` records, kept only to keep touching bodies apart when the
    // per-piece surface types split the mesh into connected components.
    std::vector<std::uint32_t> triangleGroup;   // aligned with triangles
    std::vector<std::string> groupNames;
    // `usemtl` index per triangle (aligned with triangles) and the names seen,
    // so a multi-material source can be reported instead of silently baked
    // with one texture set.
    std::vector<std::uint32_t> triangleMaterial;
    std::vector<std::string> materialNames;
};

bool LoadObj(const std::filesystem::path& path, bool flipWinding, ObjMesh& mesh,
             std::string& error);

pieces::MeshInput ToPieceMesh(const ObjMesh& mesh);

enum class NormalGreen {
    Auto,   // read the convention off the map (OpenGL exports get flipped)
    Flip,   // always invert green
    Keep,   // never invert
};

// One PBR texture set. baseColor is required; any other empty path falls back
// (flat normal, metallic 0, roughness 0.5, glass alpha from glassOpacity).
struct TextureFiles {
    std::filesystem::path baseColor;
    std::filesystem::path normal;
    std::filesystem::path metalness;
    std::filesystem::path roughness;
    std::filesystem::path opacity;
};

struct ConvertRequest {
    const ObjMesh* mesh = nullptr;
    std::filesystem::path sourcePath;         // log lines only
    std::string assetName;                    // already sanitized, <= 46 chars
    double unitsFactor = kMetersPerInch;      // source unit -> meters
    double scale = 1.0;
    xmodel::OriginMode originMode = xmodel::OriginMode::BaseCenter;
    bool flipUv = true;
    NormalGreen normalGreen = NormalGreen::Auto;
    int surfaceType = cod4::kSurfaceTypeConcrete;
    std::vector<int> pieceType;               // per piece (BuildPieces order); 0 = surfaceType
    TextureFiles textures;
    double glassOpacity = 0.35;
    std::filesystem::path outputRoot;         // the raw\ folder
    std::filesystem::path templateRoot;       // stock raw\ holding the template materials
};

struct ConvertResult {
    std::array<double, 3> originOffsetInches{};
    std::uint64_t vertexCount = 0;
    std::uint64_t triangleCount = 0;
    std::uint64_t surfaceCount = 0;
    std::vector<std::filesystem::path> writtenPaths;
};

bool Convert(const ConvertRequest& request, const LogFn& log, ConvertResult& result,
             std::string& error);

// The mesh exactly as the xmodel will hold it: inches, origin rebased with the
// writer's own rule (same triangles, same bounds), one entry per kept triangle.
// Feeds the collision generator and the editor preview.
struct ModelSpaceMesh {
    std::vector<std::array<float, 3>> positions;   // 3 per triangle
    std::vector<std::array<float, 2>> uvs;          // 3 per triangle
    std::vector<std::uint32_t> trianglePiece;       // per triangle
    std::array<double, 3> originOffsetInches{};
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
};

bool BuildModelSpaceMesh(const ConvertRequest& request, const pieces::PieceSet& pieceSet,
                         ModelSpaceMesh& out, std::string& error);

// KIWI GLASS
// PNGs -> <name>_col/_nml/_spc.iwi + a clone of templateMaterial.
// Normal is DirectX (green down) unless normalGreen is Flip.
struct ImageMaterialRequest {
    std::filesystem::path rawRoot;
    std::string name;
    std::filesystem::path templateMaterial;   // raw\materials\<stock material> to clone
    std::filesystem::path color;              // required
    std::filesystem::path normal;             // optional; the template needs a normal slot
    std::filesystem::path specularGloss;      // optional
    NormalGreen normalGreen = NormalGreen::Keep;
    int surfaceType = -1;                     // -1 keeps the template's
};

bool WriteImageMaterial(const ImageMaterialRequest& request, const LogFn& log,
                        std::vector<std::filesystem::path>& written, std::string& error);
// KIWI END

}  // namespace kimport
