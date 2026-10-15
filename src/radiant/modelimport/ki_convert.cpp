// KIWI model importer - converter core. See ki_convert.h.
//
// Everything inside the anonymous namespace is obj2xmodel's converter
// (F:\gitshit\Plastic\src\obj2xmodel_main.cpp as of 2026-09-29), kept byte-for-byte
// where possible so a KIWI build writes what obj2xmodel wrote. KIWI changes:
//   - explicit texture paths (ki_textures resolves InstaMAT variants) instead of
//     scanning a folder by asset name;
//   - per-piece surface types come from the recipe, not the .surfaces.txt sidecar;
//   - the normal-green convention is a request field (auto / flip / keep);
//   - `usemtl` is recorded so multi-material sources are reported;
//   - the writer's origin offset is returned for the collision hulls.

#include "ki_convert.h"

#include "cod4_iwi.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kimport {
namespace {

namespace fs = std::filesystem;

void LogPrintf(const LogFn& log, const char* format, ...) {
    if (!log) return;

    va_list arguments;
    va_start(arguments, format);
    va_list countArguments;
    va_copy(countArguments, arguments);
    const int required = std::vsnprintf(nullptr, 0, format, countArguments);
    va_end(countArguments);
    if (required < 0) {
        va_end(arguments);
        log("Log formatting failed");
        return;
    }

    std::vector<char> buffer(static_cast<std::size_t>(required) + 1u);
    std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    log(std::string(buffer.data(), static_cast<std::size_t>(required)));
}

struct Vec3Hash {
    std::size_t operator()(const Vec3& value) const noexcept {
        const auto hashComponent = [](double component) {
            // Equality treats the two signed zero representations as equal.
            if (component == 0.0) component = 0.0;
            return std::hash<double>{}(component);
        };
        std::size_t hash = hashComponent(value.x);
        hash ^= hashComponent(value.y) + 0x9e3779b9u + (hash << 6u) +
                (hash >> 2u);
        hash ^= hashComponent(value.z) + 0x9e3779b9u + (hash << 6u) +
                (hash >> 2u);
        return hash;
    }
};

struct Bounds {
    Vec3 minimum{std::numeric_limits<double>::max(),
                 std::numeric_limits<double>::max(),
                 std::numeric_limits<double>::max()};
    Vec3 maximum{-std::numeric_limits<double>::max(),
                 -std::numeric_limits<double>::max(),
                 -std::numeric_limits<double>::max()};
    bool populated = false;

    void Add(const Vec3& value) {
        minimum.x = std::min(minimum.x, value.x);
        minimum.y = std::min(minimum.y, value.y);
        minimum.z = std::min(minimum.z, value.z);
        maximum.x = std::max(maximum.x, value.x);
        maximum.y = std::max(maximum.y, value.y);
        maximum.z = std::max(maximum.z, value.z);
        populated = true;
    }
};

// obj2xmodel's option block, filled from a ConvertRequest (OptionsFromRequest).
struct Options {
    fs::path meshPath;                        // log lines only
    fs::path outputRoot;
    fs::path templateRoot;
    std::string requestedName;
    std::string assetName;
    double unitsFactor = 1.0;
    std::string units = "m";
    double scale = 1.0;
    xmodel::OriginMode originMode = xmodel::OriginMode::BaseCenter;
    std::string origin = "base";
    NormalGreen normalGreen = NormalGreen::Auto;
    bool flipUv = true;
    // CoD4 surface type written into the material's surfaceFlags AND the
    // xmodel collision surfaces (bullet impact FX + penetration). Never 0.
    int surfaceType = cod4::kSurfaceTypeConcrete;
    // KIWI: the resolved texture set (ki_textures) instead of a folder scan.
    TextureFiles textures;
    // KIWI: per source material, when there are several (see ConvertRequest).
    std::vector<TextureFiles> materialTextures;
    // Glass pieces (surface type glass) get the shipped glass material family
    // (l_sm_b0c0s0, window sort, blended) over <asset>_glass_col whose alpha is
    // the InstaMAT opacity map when one is exported, else this constant.
    double glassOpacity = 0.35;
};

bool ParseDouble(std::string_view text, double& value) {
    if (text.empty()) return false;
    if (text.front() == '+') text.remove_prefix(1u);
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result =
        std::from_chars(begin, end, value, std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == end &&
           std::isfinite(value);
}

pieces::MeshInput ToPieceMeshImpl(const ObjMesh& mesh) {
    pieces::MeshInput input;
    input.positions.reserve(mesh.positions.size());
    for (const Vec3& p : mesh.positions)
        input.positions.push_back({static_cast<float>(p.x), static_cast<float>(p.y),
                                   static_cast<float>(p.z)});
    input.triangles.reserve(mesh.triangles.size());
    for (const IndexedTriangle& t : mesh.triangles)
        input.triangles.push_back({static_cast<std::uint32_t>(t.corners[0].position),
                                   static_cast<std::uint32_t>(t.corners[1].position),
                                   static_cast<std::uint32_t>(t.corners[2].position)});
    input.triangleGroup = mesh.triangleGroup;
    input.groupNames = mesh.groupNames;
    return input;
}

bool ParseInteger(std::string_view text, std::int64_t& value) {
    if (text.empty()) return false;
    if (text.front() == '+') text.remove_prefix(1u);
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool ResolveObjIndex(std::string_view text, std::size_t elementCount,
                     std::string_view kind, std::size_t& resolved,
                     std::string& error) {
    std::int64_t index = 0;
    if (!ParseInteger(text, index)) {
        error = "invalid " + std::string(kind) + " index '" +
                std::string(text) + "'";
        return false;
    }
    if (index == 0) {
        error = std::string(kind) + " index 0 is invalid in OBJ";
        return false;
    }
    if (index > 0) {
        const std::uint64_t oneBased = static_cast<std::uint64_t>(index);
        if (oneBased > elementCount) {
            error = std::string(kind) + " index " + std::to_string(index) +
                    " is out of range (have " +
                    std::to_string(elementCount) + ")";
            return false;
        }
        resolved = static_cast<std::size_t>(oneBased - 1u);
        return true;
    }

    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(-(index + 1)) + 1u;
    if (magnitude > elementCount) {
        error = std::string(kind) + " relative index " +
                std::to_string(index) + " is out of range (have " +
                std::to_string(elementCount) + ")";
        return false;
    }
    resolved = elementCount - static_cast<std::size_t>(magnitude);
    return true;
}

bool ParseFaceCorner(std::string_view token, const ObjMesh& mesh,
                     ObjCorner& corner, std::string& error) {
    const std::size_t firstSlash = token.find('/');
    if (firstSlash == std::string_view::npos) {
        error = "face corner '" + std::string(token) +
                "' has no vt index; UVs are required for PBR textures";
        return false;
    }
    const std::size_t secondSlash = token.find('/', firstSlash + 1u);
    if (secondSlash != std::string_view::npos &&
        token.find('/', secondSlash + 1u) != std::string_view::npos) {
        error = "malformed face corner '" + std::string(token) + "'";
        return false;
    }

    const std::string_view positionText = token.substr(0, firstSlash);
    const std::string_view texcoordText =
        secondSlash == std::string_view::npos
            ? token.substr(firstSlash + 1u)
            : token.substr(firstSlash + 1u, secondSlash - firstSlash - 1u);
    if (texcoordText.empty()) {
        error = "face corner '" + std::string(token) +
                "' has no vt index; UVs are required for PBR textures";
        return false;
    }
    if (!ResolveObjIndex(positionText, mesh.positions.size(), "position",
                         corner.position, error) ||
        !ResolveObjIndex(texcoordText, mesh.texcoords.size(), "texcoord",
                         corner.texcoord, error)) {
        return false;
    }

    if (secondSlash != std::string_view::npos && secondSlash + 1u < token.size()) {
        std::size_t normal = 0;
        if (!ResolveObjIndex(token.substr(secondSlash + 1u), mesh.normals.size(),
                             "normal", normal, error)) {
            return false;
        }
        corner.normal = normal;
    }
    return true;
}

bool ParseVec3Record(std::istringstream& parser, std::size_t lineNumber,
                     std::string_view recordName, Vec3& value,
                     std::string& error) {
    std::string x;
    std::string y;
    std::string z;
    if (!(parser >> x >> y >> z) || !ParseDouble(x, value.x) ||
        !ParseDouble(y, value.y) || !ParseDouble(z, value.z)) {
        error = "OBJ line " + std::to_string(lineNumber) + ": malformed " +
                std::string(recordName) + " record";
        return false;
    }
    if (!IsFinite(value)) {
        error = "OBJ line " + std::to_string(lineNumber) + ": non-finite " +
                std::string(recordName) + " record";
        return false;
    }
    return true;
}

bool LoadObjImpl(const fs::path& path, bool flipWinding, ObjMesh& mesh,
                 std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open OBJ '" + path.string() + "'";
        return false;
    }

    std::string line;
    std::size_t lineNumber = 0;
    std::uint32_t currentGroup = 0;
    std::uint32_t currentMaterial = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        std::istringstream parser(line);
        std::string record;
        if (!(parser >> record) || record.starts_with('#')) continue;

        if (record == "usemtl") {
            // KIWI: recorded only, so a multi-material source can be reported.
            std::string name;
            parser >> name;
            const auto found =
                std::find(mesh.materialNames.begin(), mesh.materialNames.end(), name);
            if (found == mesh.materialNames.end()) {
                currentMaterial = static_cast<std::uint32_t>(mesh.materialNames.size());
                mesh.materialNames.push_back(name);
            } else {
                currentMaterial =
                    static_cast<std::uint32_t>(found - mesh.materialNames.begin());
            }
        } else if (record == "o" || record == "g") {
            // Plasticity writes one `o <body name>` per body; a repeated name
            // continues the earlier group. usemtl is deliberately NOT a group
            // boundary: a body with two paint materials is still one piece.
            std::string name;
            std::getline(parser, name);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
                name.erase(name.begin());
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t' ||
                                     name.back() == '\r'))
                name.pop_back();
            const auto found =
                std::find(mesh.groupNames.begin(), mesh.groupNames.end(), name);
            if (found == mesh.groupNames.end()) {
                currentGroup = static_cast<std::uint32_t>(mesh.groupNames.size());
                mesh.groupNames.push_back(name);
            } else {
                currentGroup = static_cast<std::uint32_t>(found - mesh.groupNames.begin());
            }
        } else if (record == "v") {
            Vec3 position;
            if (!ParseVec3Record(parser, lineNumber, "v", position, error))
                return false;
            mesh.positions.push_back(position);
        } else if (record == "vt") {
            std::string u;
            std::string v;
            Vec2 texcoord;
            if (!(parser >> u >> v) || !ParseDouble(u, texcoord.x) ||
                !ParseDouble(v, texcoord.y)) {
                error = "OBJ line " + std::to_string(lineNumber) +
                        ": malformed vt record";
                return false;
            }
            mesh.texcoords.push_back(texcoord);
        } else if (record == "vn") {
            Vec3 normal;
            if (!ParseVec3Record(parser, lineNumber, "vn", normal, error))
                return false;
            mesh.normals.push_back(normal);
        } else if (record == "f") {
            std::vector<ObjCorner> polygon;
            std::string token;
            while (parser >> token) {
                if (token.starts_with('#')) break;
                ObjCorner corner;
                std::string cornerError;
                if (!ParseFaceCorner(token, mesh, corner, cornerError)) {
                    error = "OBJ line " + std::to_string(lineNumber) + ": " +
                            cornerError;
                    return false;
                }
                polygon.push_back(corner);
            }
            if (polygon.size() < 3u) {
                error = "OBJ line " + std::to_string(lineNumber) +
                        ": face has fewer than three corners";
                return false;
            }
            for (std::size_t corner = 1; corner + 1u < polygon.size(); ++corner) {
                IndexedTriangle triangle;
                triangle.corners = {polygon[0], polygon[corner],
                                    polygon[corner + 1u]};
                triangle.sourceLine = lineNumber;
                if (flipWinding)
                    std::swap(triangle.corners[1], triangle.corners[2]);
                if (mesh.groupNames.empty()) mesh.groupNames.push_back("");
                mesh.triangles.push_back(std::move(triangle));
                mesh.triangleGroup.push_back(currentGroup);
                mesh.triangleMaterial.push_back(currentMaterial);
            }
        }
    }
    if (!input.eof()) {
        error = "failed while reading OBJ '" + path.string() + "'";
        return false;
    }
    if (mesh.positions.empty()) {
        error = "OBJ contains no vertex positions";
        return false;
    }
    if (mesh.triangles.empty()) {
        error = "OBJ contains no faces";
        return false;
    }
    return true;
}

double CornerAngle(const Vec3& center, const Vec3& first,
                   const Vec3& second) {
    const Vec3 a = first - center;
    const Vec3 b = second - center;
    const double denominator = Length(a) * Length(b);
    if (!(denominator > 0.0) || !std::isfinite(denominator)) return 0.0;
    const double cosine = std::clamp(Dot(a, b) / denominator, -1.0, 1.0);
    return std::acos(cosine);
}

// `sourceIndices` (optional) receives, per output triangle, the index of the
// ObjMesh triangle it came from; degenerate drops make the two lists differ.
bool BuildBakeTriangles(const ObjMesh& mesh, const Options& options,
                         std::vector<xmodel::BakeTriangle>& output,
                         Bounds& bounds, const LogFn& log,
                         std::string& error,
                         std::vector<std::size_t>* sourceIndices = nullptr) {
    if (sourceIndices) sourceIndices->clear();
    bool needsSmoothNormals = false;
    for (const IndexedTriangle& triangle : mesh.triangles) {
        for (const ObjCorner& corner : triangle.corners)
            needsSmoothNormals |= !corner.normal.has_value();
    }

    std::vector<std::size_t> positionWeld;
    std::vector<Vec3> smoothNormalSums;
    if (needsSmoothNormals) {
        std::unordered_map<Vec3, std::size_t, Vec3Hash> weldMap;
        weldMap.reserve(mesh.positions.size());
        positionWeld.resize(mesh.positions.size());
        for (std::size_t index = 0; index < mesh.positions.size(); ++index) {
            const auto [found, inserted] =
                weldMap.try_emplace(mesh.positions[index], smoothNormalSums.size());
            if (inserted) smoothNormalSums.push_back({});
            positionWeld[index] = found->second;
        }

        for (const IndexedTriangle& triangle : mesh.triangles) {
            const Vec3& p0 = mesh.positions[triangle.corners[0].position];
            const Vec3& p1 = mesh.positions[triangle.corners[1].position];
            const Vec3& p2 = mesh.positions[triangle.corners[2].position];
            Vec3 faceNormal;
            if (!Normalize(Cross(p1 - p0, p2 - p0), faceNormal)) continue;
            const std::array<double, 3> angles{
                CornerAngle(p0, p1, p2), CornerAngle(p1, p2, p0),
                CornerAngle(p2, p0, p1)};
            for (std::size_t corner = 0; corner < 3u; ++corner) {
                smoothNormalSums[positionWeld[triangle.corners[corner].position]] +=
                    faceNormal * angles[corner];
            }
        }
    }

    std::vector<Vec3> normalizedObjNormals(mesh.normals.size());
    std::vector<std::uint8_t> normalState(mesh.normals.size(), 0u);
    const double positionScale = options.unitsFactor * options.scale;
    output.reserve(mesh.triangles.size());
    std::size_t degenerateUvRescues = 0;
    std::size_t degenerateDrops = 0;
    std::size_t perpendicularNormalRescues = 0;

    for (std::size_t meshTriangle = 0; meshTriangle < mesh.triangles.size();
         ++meshTriangle) {
        const IndexedTriangle& indexed = mesh.triangles[meshTriangle];
        xmodel::BakeTriangle triangle;
        for (std::size_t cornerIndex = 0; cornerIndex < 3u; ++cornerIndex) {
            const ObjCorner& sourceCorner = indexed.corners[cornerIndex];
            xmodel::BakeVertex& destination = triangle.corners[cornerIndex];
            const Vec3 position =
                mesh.positions[sourceCorner.position] * positionScale;
            if (!IsFinite(position)) {
                error = "OBJ line " + std::to_string(indexed.sourceLine) +
                        ": scaled position is outside the finite range";
                return false;
            }
            destination.positionMeters = {position.x, position.y, position.z};
            bounds.Add(position);

            Vec3 normal;
            if (sourceCorner.normal.has_value()) {
                const std::size_t normalIndex = *sourceCorner.normal;
                if (normalState[normalIndex] == 0u) {
                    if (!Normalize(mesh.normals[normalIndex],
                                   normalizedObjNormals[normalIndex])) {
                        normalState[normalIndex] = 2u;
                    } else {
                        normalState[normalIndex] = 1u;
                    }
                }
                if (normalState[normalIndex] != 1u) {
                    error = "OBJ line " + std::to_string(indexed.sourceLine) +
                            ": referenced vn is zero-length";
                    return false;
                }
                normal = normalizedObjNormals[normalIndex];
            } else {
                if (!Normalize(
                        smoothNormalSums[positionWeld[sourceCorner.position]],
                        normal)) {
                    error = "OBJ line " + std::to_string(indexed.sourceLine) +
                            ": cannot compute a smooth normal for a degenerate "
                            "vertex neighborhood";
                    return false;
                }
            }
            destination.normal = {normal.x, normal.y, normal.z};

            const Vec2& texcoord = mesh.texcoords[sourceCorner.texcoord];
            destination.hasUv = true;
            destination.uv = {texcoord.x,
                              options.flipUv ? 1.0 - texcoord.y : texcoord.y};
        }
        // Zero-area triangles (the writer's own sin-angle test, which is
        // scale-invariant, so meters here match its inches) contribute nothing
        // — drop them instead of failing the bake.
        {
            const auto& a = triangle.corners[0].positionMeters;
            const auto& b = triangle.corners[1].positionMeters;
            const auto& c = triangle.corners[2].positionMeters;
            const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const Vec3 e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const Vec3 crossE{e1.y * e2.z - e1.z * e2.y,
                              e1.z * e2.x - e1.x * e2.z,
                              e1.x * e2.y - e1.y * e2.x};
            const double crossLength =
                std::sqrt(crossE.x * crossE.x + crossE.y * crossE.y +
                          crossE.z * crossE.z);
            const double edgeScale =
                std::sqrt(e1.x * e1.x + e1.y * e1.y + e1.z * e1.z) *
                std::sqrt(e2.x * e2.x + e2.y * e2.y + e2.z * e2.z);
            if (!(edgeScale > 0.0) || crossLength <= edgeScale * 1.0e-12) {
                ++degenerateDrops;
                continue;
            }
            // The writer also rejects triangles whose corner normals are
            // perpendicular to the face plane (sliver artifacts). Replace such
            // normals with the geometric normal, which agrees with winding.
            Vec3 normalSum{};
            for (const auto& cornerVertex : triangle.corners) {
                normalSum.x += cornerVertex.normal[0];
                normalSum.y += cornerVertex.normal[1];
                normalSum.z += cornerVertex.normal[2];
            }
            const double orientation = crossE.x * normalSum.x +
                                       crossE.y * normalSum.y +
                                       crossE.z * normalSum.z;
            const double normalSumLength =
                std::sqrt(normalSum.x * normalSum.x +
                          normalSum.y * normalSum.y +
                          normalSum.z * normalSum.z);
            if (std::abs(orientation) <=
                crossLength * std::max(normalSumLength, 1.0e-12) * 1.0e-6) {
                const std::array<double, 3> geometricNormal{
                    crossE.x / crossLength, crossE.y / crossLength,
                    crossE.z / crossLength};
                for (auto& cornerVertex : triangle.corners)
                    cornerVertex.normal = geometricNormal;
                ++perpendicularNormalRescues;
            }
        }
        // The xmodel writer derives the tangent frame from the UV determinant
        // and hard-fails when it is exactly zero. A UV-degenerate triangle was
        // already sampling a single texel, so give it a micro epsilon frame
        // instead of failing the whole bake.
        {
            const auto& uv0 = triangle.corners[0].uv;
            auto& uv1 = triangle.corners[1].uv;
            auto& uv2 = triangle.corners[2].uv;
            const double det = (uv1[0] - uv0[0]) * (uv2[1] - uv0[1]) -
                               (uv2[0] - uv0[0]) * (uv1[1] - uv0[1]);
            if (!std::isfinite(det) || det == 0.0) {
                constexpr double kUvEpsilon = 1.0e-6;
                uv1 = {uv0[0] + kUvEpsilon, uv0[1]};
                uv2 = {uv0[0], uv0[1] + kUvEpsilon};
                ++degenerateUvRescues;
            }
        }
        output.push_back(std::move(triangle));
        if (sourceIndices) sourceIndices->push_back(meshTriangle);
    }
    if (degenerateDrops > 0u)
        LogPrintf(log, "Dropped %zu zero-area triangles", degenerateDrops);
    if (perpendicularNormalRescues > 0u)
        LogPrintf(log,
                  "Rescued %zu sliver triangles with face-perpendicular "
                  "normals (replaced with the geometric normal)",
                  perpendicularNormalRescues);
    if (degenerateUvRescues > 0u)
        LogPrintf(log,
                  "Rescued %zu UV-degenerate triangles with an epsilon UV "
                  "frame",
                  degenerateUvRescues);
    return true;
}

void PrintObjStats(const ObjMesh& mesh, const Bounds& bounds,
                   const LogFn& log) {
    const Vec3 extent = bounds.maximum - bounds.minimum;
    const Vec3 minimumInches = bounds.minimum / kMetersPerInch;
    const Vec3 maximumInches = bounds.maximum / kMetersPerInch;
    const Vec3 extentInches = extent / kMetersPerInch;
    LogPrintf(log, "Parsed OBJ: %zu verts, %zu tris (%zu vt, %zu vn)",
              mesh.positions.size(), mesh.triangles.size(),
              mesh.texcoords.size(), mesh.normals.size());
    LogPrintf(
        log,
        "Bounds meters: min (%.6f, %.6f, %.6f), max (%.6f, %.6f, "
        "%.6f), size (%.6f, %.6f, %.6f)",
        bounds.minimum.x, bounds.minimum.y, bounds.minimum.z,
        bounds.maximum.x, bounds.maximum.y, bounds.maximum.z, extent.x,
        extent.y, extent.z);
    LogPrintf(
        log,
        "Bounds inches: min (%.3f, %.3f, %.3f), max (%.3f, %.3f, "
        "%.3f), size (%.3f, %.3f, %.3f)",
        minimumInches.x, minimumInches.y, minimumInches.z, maximumInches.x,
        maximumInches.y, maximumInches.z, extentInches.x, extentInches.y,
        extentInches.z);
}

std::string LowerAscii(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

// Lowercase and drop everything except letters/digits, so an asset name like
// "batwing_antenna" matches an export named "BatwingAntenna2_Default_...".
std::string AlnumKey(std::string_view value) {
    std::string key;
    key.reserve(value.size());
    for (char character : value) {
        if (character >= 'A' && character <= 'Z') {
            key.push_back(static_cast<char>(character - 'A' + 'a'));
        } else if ((character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9')) {
            key.push_back(character);
        }
    }
    return key;
}

// Export folders are shared across assets (InstaMAT writes every project to
// one directory), so a bare substring match on "basecolor" would pick
// whichever asset sorts first. Pass 1 restricts candidates to files whose
// name also contains one of the asset's own name keys; pass 2 (clean folder
// or unexpected naming) falls back to any match so a lone set still works.
bool FindPng(const fs::path& directory,
             std::initializer_list<std::string_view> keys,
             std::initializer_list<std::string_view> excludes,
             const std::vector<std::string>& preferKeys,
             std::optional<fs::path>& result, bool& usedFallback,
             std::string& error) {
    result.reset();
    usedFallback = false;
    std::error_code filesystemError;
    if (!fs::is_directory(directory, filesystemError)) {
        error = filesystemError
                    ? "cannot inspect textures directory '" +
                          directory.string() + "': " + filesystemError.message()
                    : "textures path is not a directory: '" +
                          directory.string() + "'";
        return false;
    }

    std::vector<fs::path> files;
    for (fs::directory_iterator iterator(directory, filesystemError), end;
         !filesystemError && iterator != end;
         iterator.increment(filesystemError)) {
        std::error_code typeError;
        if (iterator->is_regular_file(typeError) && !typeError)
            files.push_back(iterator->path());
    }
    if (filesystemError) {
        error = "cannot scan textures directory '" + directory.string() +
                "': " + filesystemError.message();
        return false;
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return LowerAscii(a.filename().string()) <
               LowerAscii(b.filename().string());
    });
    auto matches = [&](const fs::path& path, bool requirePrefer) {
        const std::string filename = LowerAscii(path.filename().string());
        if (!std::string_view(filename).ends_with(".png")) return false;
        // Skip InstaMAT's mesh-map outputs (MeshNormal*, NormalDirect, etc.)
        // so a positive key match can never latch onto the wrong image.
        for (std::string_view skip : excludes) {
            if (filename.find(skip) != std::string::npos) return false;
        }
        bool keyHit = false;
        for (std::string_view key : keys) {
            if (filename.find(key) != std::string::npos) {
                keyHit = true;
                break;
            }
        }
        if (!keyHit) return false;
        if (!requirePrefer) return true;
        const std::string alnum = AlnumKey(filename);
        for (const std::string& prefer : preferKeys) {
            if (!prefer.empty() && alnum.find(prefer) != std::string::npos)
                return true;
        }
        return false;
    };

    if (!preferKeys.empty()) {
        for (const fs::path& path : files) {
            if (matches(path, true)) {
                result = path;
                return true;
            }
        }
    }
    for (const fs::path& path : files) {
        if (matches(path, false)) {
            result = path;
            usedFallback = !preferKeys.empty();
            return true;
        }
    }
    return true;
}

bool LoadRgba(const fs::path& path, int& width, int& height,
              std::vector<std::uint8_t>& pixels, std::string& error) {
    int sourceChannels = 0;
    stbi_uc* loaded =
        stbi_load(path.string().c_str(), &width, &height, &sourceChannels, 4);
    if (!loaded) {
        const char* reason = stbi_failure_reason();
        error = "cannot load PNG '" + path.string() + "'";
        if (reason && *reason) error += ": " + std::string(reason);
        return false;
    }
    const std::uint64_t byteCount =
        4ull * static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height);
    if (width <= 0 || height <= 0 ||
        byteCount > std::numeric_limits<std::size_t>::max()) {
        stbi_image_free(loaded);
        error = "PNG dimensions are invalid or too large: '" + path.string() +
                "'";
        return false;
    }
    pixels.assign(loaded, loaded + static_cast<std::size_t>(byteCount));
    stbi_image_free(loaded);
    return true;
}

bool LoadResized(const fs::path& path, int targetWidth, int targetHeight,
                 std::vector<std::uint8_t>& output, std::string& error) {
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::vector<std::uint8_t> source;
    if (!LoadRgba(path, sourceWidth, sourceHeight, source, error)) return false;
    output.resize(static_cast<std::size_t>(targetWidth) *
                  static_cast<std::size_t>(targetHeight) * 4u);
    for (int y = 0; y < targetHeight; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
            const int sourceX = x * sourceWidth / targetWidth;
            const int sourceY = y * sourceHeight / targetHeight;
            const std::size_t sourceOffset =
                (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) *
                4u;
            const std::size_t destinationOffset =
                (static_cast<std::size_t>(y) * targetWidth + x) * 4u;
            std::copy_n(source.data() + sourceOffset, 4u,
                        output.data() + destinationOffset);
        }
    }
    return true;
}

bool IsPowerOfTwo(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

// Detects a tangent-space normal map's green convention from its own data.
// A normal map derived from a height field is integrable, which fixes the sign
// of dR/dy * dG/dx (image y down): NEGATIVE for OpenGL (green = V up),
// POSITIVE for DirectX (green = V down). CoD4 is a D3D9 title and wants
// DirectX, so an OpenGL export must have its green channel inverted.
// Returns +1 DirectX, -1 OpenGL, 0 undetermined (flat map / too little detail).
int DetectNormalGreenConvention(const std::vector<std::uint8_t>& rgba, int width,
                                int height, std::size_t& detailPixels,
                                double& positiveFraction) {
    detailPixels = 0;
    positiveFraction = 0.0;
    if (width < 3 || height < 3) return 0;
    const auto channel = [&](int x, int y, int c) {
        return rgba[(static_cast<std::size_t>(y) * width + x) * 4u + c] /
                   255.0 * 2.0 - 1.0;
    };
    double sum = 0.0;
    std::size_t positive = 0;
    for (int y = 1; y + 1 < height; ++y) {
        for (int x = 1; x + 1 < width; ++x) {
            const double dRdy = (channel(x, y + 1, 0) - channel(x, y - 1, 0)) * 0.5;
            const double dGdx = (channel(x + 1, y, 1) - channel(x - 1, y, 1)) * 0.5;
            if (std::abs(dRdy) <= 1.0e-3 || std::abs(dGdx) <= 1.0e-3) continue;
            const double product = dRdy * dGdx;
            sum += product;
            if (product > 0.0) ++positive;
            ++detailPixels;
        }
    }
    if (detailPixels < 1000u) return 0;
    positiveFraction = static_cast<double>(positive) / detailPixels;
    // Require a clear majority so a noisy, near-flat map stays "undetermined"
    // instead of silently flipping on a coin toss.
    if (positiveFraction > 0.58) return 1;
    if (positiveFraction < 0.42) return -1;
    return 0;
}

struct TextureSet {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> color;
    std::vector<std::uint8_t> normal;
    std::vector<std::uint8_t> specularGloss;
    // Colour with the glass alpha in A (written only when a glass piece exists).
    std::vector<std::uint8_t> glassColor;
    std::string glassAlphaSource;   // "opacity map <file>", "basecolor alpha", "constant"
};

bool PrepareTextures(const Options& options, TextureSet& textures,
                     const LogFn& log, std::string& error) {
    // KIWI: the set is resolved by ki_textures (variant-aware) and handed in.
    auto given = [](const fs::path& path) -> std::optional<fs::path> {
        if (path.empty()) return std::nullopt;
        return path;
    };
    const std::optional<fs::path> basePath = given(options.textures.baseColor);
    const std::optional<fs::path> normalPath = given(options.textures.normal);
    const std::optional<fs::path> metalPath = given(options.textures.metalness);
    const std::optional<fs::path> roughnessPath = given(options.textures.roughness);
    if (!basePath.has_value()) {
        error = "no basecolor texture in the texture set";
        return false;
    }

    int width = 0;
    int height = 0;
    if (!LoadRgba(*basePath, width, height, textures.color, error)) return false;
    if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)) {
        error = "basecolor PNG is " + std::to_string(width) + "x" +
                std::to_string(height) +
                "; re-export textures with power-of-two width and height";
        return false;
    }
    textures.width = static_cast<std::uint32_t>(width);
    textures.height = static_cast<std::uint32_t>(height);

    std::vector<std::uint8_t> normal;
    std::vector<std::uint8_t> metal;
    std::vector<std::uint8_t> roughness;
    auto loadOptional = [&](const std::optional<fs::path>& path,
                            std::vector<std::uint8_t>& destination,
                            std::string_view label) {
        if (!path.has_value()) return false;
        std::string loadError;
        if (LoadResized(*path, width, height, destination, loadError)) return true;
        LogPrintf(log, "Warning: %.*s map fallback: %s",
                  static_cast<int>(label.size()), label.data(),
                  loadError.c_str());
        destination.clear();
        return false;
    };
    const bool hasNormal = loadOptional(normalPath, normal, "normal");
    const bool hasMetal = loadOptional(metalPath, metal, "metallic");
    const bool hasRoughness =
        loadOptional(roughnessPath, roughness, "roughness");

    // Glass: an InstaMAT opacity map (white = solid) is the alpha of the glass
    // colour image; without one, a basecolor that already carries alpha keeps
    // it; otherwise the constant.
    std::optional<fs::path> opacityPath = given(options.textures.opacity);
    std::vector<std::uint8_t> opacity;
    if (opacityPath.has_value() && !loadOptional(opacityPath, opacity, "opacity"))
        opacityPath.reset();
    bool baseHasAlpha = false;
    for (std::size_t offset = 3; offset < textures.color.size() && !baseHasAlpha;
         offset += 4u)
        baseHasAlpha = textures.color[offset] != 255u;
    textures.glassColor = textures.color;
    const std::uint8_t constantAlpha = static_cast<std::uint8_t>(
        std::clamp(options.glassOpacity * 255.0, 0.0, 255.0) + 0.5);
    for (std::size_t offset = 0; offset + 3u < textures.glassColor.size(); offset += 4u) {
        if (opacityPath.has_value())
            textures.glassColor[offset + 3u] = opacity[offset];
        else if (!baseHasAlpha)
            textures.glassColor[offset + 3u] = constantAlpha;
    }
    textures.glassAlphaSource =
        opacityPath.has_value() ? "opacity map " + opacityPath->string()
        : baseHasAlpha          ? std::string("basecolor alpha")
                                : "constant " + std::to_string(options.glassOpacity);

    // Green convention: Flip forces the inversion, Keep forbids it; Auto reads
    // it off the map itself so an OpenGL export (InstaMAT's default) is
    // corrected for CoD4 without the user having to know which preset they used.
    bool flipGreen = options.normalGreen == NormalGreen::Flip;
    if (hasNormal) {
        if (options.normalGreen == NormalGreen::Flip) {
            LogPrintf(log, "Normal green: forced flip");
        } else if (options.normalGreen == NormalGreen::Keep) {
            LogPrintf(log, "Normal green: kept as exported (forced)");
        } else {
            std::size_t detailPixels = 0;
            double positiveFraction = 0.0;
            const int convention = DetectNormalGreenConvention(
                normal, width, height, detailPixels, positiveFraction);
            if (convention < 0) {
                flipGreen = true;
                LogPrintf(log,
                          "Normal green: detected OpenGL (green up; %zu detail "
                          "px, %.0f%% positive) -> flipping to DirectX for CoD4",
                          detailPixels, positiveFraction * 100.0);
            } else if (convention > 0) {
                LogPrintf(log,
                          "Normal green: detected DirectX (green down; %zu "
                          "detail px, %.0f%% positive) -> left as-is",
                          detailPixels, positiveFraction * 100.0);
            } else {
                LogPrintf(log,
                          "Warning: normal green convention undetermined (%zu "
                          "detail px); left as-is. Set the recipe's normalGreen to flip "
                          "if surface detail looks inverted in-game",
                          detailPixels);
            }
        }
    }

    const std::size_t bytes = textures.color.size();
    textures.normal.resize(bytes);
    textures.specularGloss.resize(bytes);
    for (std::size_t offset = 0; offset < bytes; offset += 4u) {
        if (hasNormal) {
            textures.normal[offset + 0u] = normal[offset + 0u];
            textures.normal[offset + 1u] =
                flipGreen
                    ? static_cast<std::uint8_t>(255u - normal[offset + 1u])
                    : normal[offset + 1u];
            textures.normal[offset + 2u] = normal[offset + 2u];
        } else {
            textures.normal[offset + 0u] = 128u;
            textures.normal[offset + 1u] = 128u;
            textures.normal[offset + 2u] = 255u;
        }
        textures.normal[offset + 3u] = 255u;

        const float metallic =
            hasMetal ? metal[offset] / 255.0f : 0.0f;
        const float gloss =
            hasRoughness ? 1.0f - roughness[offset] / 255.0f : 0.5f;
        for (std::size_t channel = 0; channel < 3u; ++channel) {
            const float value =
                (0.04f * 255.0f) * (1.0f - metallic) +
                textures.color[offset + channel] * metallic;
            textures.specularGloss[offset + channel] =
                static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
            // KIWI (2026-10-14, user: "my vertical gutter pipes look dark grey now instead
            // of white. That shouldn't be happening"): the diffuse keeps the full base
            // colour.  Darkening metal's diffuse (it kept 35%) turned InstaMAT's ~0.9-metal
            // white paint dark grey under CoD4's sparse probes.
        }
        textures.specularGloss[offset + 3u] =
            static_cast<std::uint8_t>(gloss * 255.0f);
    }

    LogPrintf(log,
              "InstaMAT textures: %ux%u base='%s', normal=%s, metallic=%s, "
              "roughness=%s",
              textures.width, textures.height, basePath->string().c_str(),
              hasNormal ? normalPath->string().c_str() : "fallback",
              hasMetal ? metalPath->string().c_str() : "fallback",
              hasRoughness ? roughnessPath->string().c_str() : "fallback");
    return true;
}

bool WriteAndValidateIwi(const fs::path& path, const TextureSet& textures,
                          std::span<const std::uint8_t> pixels,
                          cod4::IwiKind kind, const LogFn& log,
                          std::string& error) {
    cod4::IwiOptions options;
    options.mipmaps = true;
    cod4::IwiWriteResult result;
    if (!cod4::WriteIwi(path, textures.width, textures.height, pixels, kind,
                        options, &result, error)) {
        return false;
    }
    if (!cod4::RoundTrip(path, error)) return false;
    LogPrintf(log, "Wrote + validated %s (%ux%u, %u mips, %llu bytes)",
              path.string().c_str(), result.width, result.height,
              result.mipCount,
              static_cast<unsigned long long>(result.fileSize));
    return true;
}

bool WriteTextures(const Options& options, const TextureSet& textures,
                    bool writeGlass, const LogFn& log, std::string& error) {
    const fs::path imageDirectory = options.outputRoot / "images";
    const std::array<std::pair<std::string, cod4::IwiKind>, 3> outputs{{
        {options.assetName + "_col.iwi", cod4::IwiKind::Color},
        {options.assetName + "_nml.iwi", cod4::IwiKind::Normal},
        {options.assetName + "_spc.iwi", cod4::IwiKind::SpecularGloss},
    }};
    const std::array<std::span<const std::uint8_t>, 3> pixels{{
        textures.color,
        textures.normal,
        textures.specularGloss,
    }};
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        if (!WriteAndValidateIwi(imageDirectory / outputs[index].first,
                                  textures, pixels[index], outputs[index].second,
                                  log, error)) {
            error = "IWI export failed: " + error;
            return false;
        }
    }
    if (writeGlass) {
        // Alpha present -> the writer picks DXT5 on its own.
        if (!WriteAndValidateIwi(imageDirectory / (options.assetName + "_glass_col.iwi"),
                                  textures, textures.glassColor, cod4::IwiKind::Color,
                                  log, error)) {
            error = "glass IWI export failed: " + error;
            return false;
        }
        LogPrintf(log, "Glass alpha from %s", textures.glassAlphaSource.c_str());
    }
    return true;
}

// The material a surface of the given collision type uses. Bullet-hole decals are
// gated by the RENDERED material's surface type (r_marks.cpp
// R_Mark_MaterialAllowsMarks: the receiver's type bits must contain the decal's),
// so a per-piece type only gets its holes when that piece's material carries the
// same type. Pieces of the (all) type use <asset>; every other type gets a clone
// <asset>_<type> over the same three images.
std::string MaterialNameForType(const Options& options, int surfaceType) {
    if (surfaceType == options.surfaceType) return options.assetName;
    std::string name = options.assetName + "_" + cod4::SurfaceTypeName(surfaceType);
    // Asset names are capped at 50 bytes (cod4_material.cpp); "_paintedmetal" on a
    // 46-byte asset would overflow, so fall back to "_s<index>" when needed.
    if (name.size() > 50u)
        name = options.assetName + "_s" + std::to_string(surfaceType);
    return name;
}

// Glass surfaces use the shipped glass family: l_sm_b0c0s0 (blended, specular,
// no normal slot), window sort key, glass surface type and contents. Their
// colour image is <asset>_glass_col (alpha = opacity); the specular map is shared.
bool IsGlassType(int surfaceType) { return surfaceType == cod4::kSurfaceTypeGlass; }

bool WriteMaterial(const Options& options, const fs::path& templatePath,
                    const std::string& materialName, int surfaceType,
                    const LogFn& log, std::string& error) {
    const bool glass = IsGlassType(surfaceType);
    cod4::MaterialEditOptions edits;
    edits.replaceColorMap = true;
    edits.colorMap = options.assetName + (glass ? "_glass_col" : "_col");
    edits.replaceNormalMap = !glass;          // the glass template has no normal slot
    edits.normalMap = options.assetName + "_nml";
    edits.replaceSpecularGlossMap = true;
    edits.specularGlossMap = options.assetName + "_spc";
    edits.surfaceTypeBits = surfaceType << cod4::kSurfaceTypeShift;
    cod4::MaterialWriteResult result;
    if (!cod4::WriteEditedMaterial(templatePath, options.outputRoot,
                                   materialName, edits, &result, error)) {
        error = "material export failed: " + error;
        return false;
    }
    if (!cod4::RoundTripMaterial(result.path, error)) {
        error = "material round-trip failed: " + error;
        return false;
    }
    LogPrintf(log, "Wrote + validated %s (template %s, technique %s, "
              "surface type %s)",
              result.path.string().c_str(),
              result.templatePath.string().c_str(),
              result.techniqueSet.c_str(),
              cod4::SurfaceTypeName(surfaceType));
    return true;
}

// `triangleTypes` and `triangleSlot` are aligned with `triangles` (one cod4 surface
// type and one texture slot each). Triangles are grouped by slot, then by type into
// separate surfaces: collision flags live on the surface, so that is what gives a
// single-texture model wooden posts and a metal rail.
bool WriteXModel(const Options& options, const std::vector<Options>& slots,
                  std::vector<xmodel::BakeTriangle>& triangles,
                  const std::vector<int>& triangleTypes,
                  const std::vector<std::size_t>& triangleSlot, ConvertResult& convertResult,
                  const LogFn& log, std::string& error) {
    xmodel::BakeLod lod;
    lod.distance = 0.0f;
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        for (int type = 0; type < cod4::kSurfaceTypeCount; ++type) {
            // The (all) type first so the model's first surface is the common one.
            const int order = type == 0 ? options.surfaceType : type;
            if (type != 0 && order == options.surfaceType) continue;
            xmodel::MaterialTriangles material;
            material.materialName = MaterialNameForType(slots[slot], order);
            material.collisionSurfaceFlags = order << cod4::kSurfaceTypeShift;
            for (std::size_t index = 0; index < triangles.size(); ++index) {
                const int triangleType =
                    index < triangleTypes.size() ? triangleTypes[index] : options.surfaceType;
                if (triangleType == order && triangleSlot[index] == slot)
                    material.triangles.push_back(triangles[index]);
            }
            if (material.triangles.empty()) continue;
            LogPrintf(log, "Collision surface %s (%s): %zu triangles",
                      cod4::SurfaceTypeName(order), material.materialName.c_str(),
                      material.triangles.size());
            lod.materials.push_back(std::move(material));
        }
    }
    triangles.clear();
    xmodel::BakeRequest request;
    request.outputRoot = options.outputRoot;
    request.modelName = options.assetName;
    request.originMode = options.originMode;
    request.lods.push_back(std::move(lod));
    request.collisionLod = 0;

    const xmodel::BakeResult result = xmodel::WriteRawXModelV25(request);
    LogPrintf(log, "XModel: %s", result.message.c_str());
    for (const fs::path& path : result.writtenPaths)
        LogPrintf(log, "  %s", path.string().c_str());
    LogPrintf(log,
              "XModel stats: %llu LOD, %llu surfaces, %llu verts, %llu tris; "
              "origin offset (in): %.3f %.3f %.3f",
              static_cast<unsigned long long>(result.lodCount),
              static_cast<unsigned long long>(result.surfaceCount),
              static_cast<unsigned long long>(result.vertexCount),
              static_cast<unsigned long long>(result.triangleCount),
              result.appliedOffsetInches[0], result.appliedOffsetInches[1],
              result.appliedOffsetInches[2]);
    if (!result.success) {
        error = result.message;
        return false;
    }
    convertResult.originOffsetInches = result.appliedOffsetInches;
    convertResult.vertexCount = result.vertexCount;
    convertResult.triangleCount = result.triangleCount;
    convertResult.surfaceCount = result.surfaceCount;
    convertResult.writtenPaths.insert(convertResult.writtenPaths.end(),
                                      result.writtenPaths.begin(),
                                      result.writtenPaths.end());
    return true;
}

int RunConvert(const Options& options, const ObjMesh& mesh,
               const std::vector<int>& pieceTypes, ConvertResult& result,
               const LogFn& log, std::string& error) {
    error.clear();
    LogPrintf(log,
              "modelimport: '%s' -> asset '%s' (units=%s, scale=%.9g, "
              "origin=%s, uv-flip=%s)",
              options.meshPath.string().c_str(), options.assetName.c_str(),
              options.units.c_str(), options.scale, options.origin.c_str(),
              options.flipUv ? "on" : "off");
    if (options.assetName != options.requestedName) {
        LogPrintf(log, "Sanitized asset name: '%s' -> '%s'",
                  options.requestedName.c_str(), options.assetName.c_str());
    }

    // KIWI: per-piece collision surface types come from the recipe.
    const pieces::PieceSet pieceSet = pieces::BuildPieces(ToPieceMeshImpl(mesh));
    pieces::Assignments assignments;
    assignments.pieceType.assign(pieceSet.pieces.size(), 0);
    if (!pieceTypes.empty() && pieceTypes.size() != pieceSet.pieces.size()) {
        error = "the recipe lists " + std::to_string(pieceTypes.size()) +
                " pieces but the mesh has " + std::to_string(pieceSet.pieces.size()) +
                "; re-inspect the source";
        return 1;
    }
    {
        std::size_t overridden = 0;
        for (std::size_t p = 0; p < pieceTypes.size(); ++p) {
            const int type = pieceTypes[p];
            if (type <= 0 || type >= cod4::kSurfaceTypeCount) continue;
            assignments.pieceType[p] = type;
            ++overridden;
            LogPrintf(log, "Piece '%s' (%u tris): %s", pieceSet.pieces[p].name.c_str(),
                      pieceSet.pieces[p].triangleCount, cod4::SurfaceTypeName(type));
        }
        if (overridden > 0)
            LogPrintf(log, "%zu of %zu pieces override the (all) surface type %s",
                      overridden, pieceSet.pieces.size(),
                      cod4::SurfaceTypeName(options.surfaceType));
    }
    if (mesh.materialNames.size() > 1u && options.materialTextures.size() <= 1u)
        LogPrintf(log,
                  "Warning: the source has %zu materials; they all bake with the one "
                  "texture set",
                  mesh.materialNames.size());
    const std::vector<int> meshTriangleTypes =
        pieces::ResolveTriangleTypes(pieceSet, assignments, options.surfaceType);

    Bounds bounds;
    std::vector<xmodel::BakeTriangle> triangles;
    std::vector<std::size_t> sourceIndices;
    if (!BuildBakeTriangles(mesh, options, triangles, bounds, log, error,
                            &sourceIndices))
        return 1;
    std::vector<int> triangleTypes(triangles.size(), options.surfaceType);
    for (std::size_t index = 0; index < sourceIndices.size(); ++index)
        triangleTypes[index] = meshTriangleTypes[sourceIndices[index]];
    PrintObjStats(mesh, bounds, log);

    // KIWI: one texture slot per source material when several sets were given:
    // slot 0 keeps <asset>, slot k writes <asset>_m<k> images and materials.
    std::vector<Options> slots;
    if (options.materialTextures.size() > 1u) {
        for (std::size_t k = 0; k < options.materialTextures.size(); ++k) {
            Options slot = options;
            if (k > 0) slot.assetName = options.assetName + "_m" + std::to_string(k);
            slot.textures = options.materialTextures[k];
            slots.push_back(std::move(slot));
            LogPrintf(log, "Material '%s' -> %s", mesh.materialNames[k].c_str(),
                      slots.back().assetName.c_str());
        }
    } else {
        slots.push_back(options);
    }
    std::vector<std::size_t> triangleSlot(triangles.size(), 0);
    if (slots.size() > 1u)
        for (std::size_t index = 0; index < sourceIndices.size(); ++index)
            if (sourceIndices[index] < mesh.triangleMaterial.size())
                triangleSlot[index] = std::min<std::size_t>(
                    mesh.triangleMaterial[sourceIndices[index]], slots.size() - 1u);

    fs::path materialTemplate;
    std::string templateError;
    if (!cod4::ResolveMaterialTemplate(
            options.templateRoot,
            cod4::MaterialTemplate::WorldLitNormalSpecular, materialTemplate,
            templateError)) {
        error = "could not resolve WorldLitNormalSpecular template: " +
                templateError;
        return 1;
    }
    bool usesGlass = IsGlassType(options.surfaceType);
    for (std::size_t index = 0; index < triangleTypes.size() && !usesGlass; ++index)
        usesGlass = IsGlassType(triangleTypes[index]);
    fs::path glassTemplate;
    if (usesGlass && !cod4::ResolveMaterialTemplate(options.templateRoot,
                                                    cod4::MaterialTemplate::WorldGlass,
                                                    glassTemplate, templateError)) {
        error = "could not resolve the WorldGlass template: " + templateError;
        return 1;
    }
    auto templateFor = [&](int type) -> const fs::path& {
        return IsGlassType(type) ? glassTemplate : materialTemplate;
    };
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        const Options& slotOptions = slots[slot];
        bool slotGlass = IsGlassType(options.surfaceType);
        for (std::size_t index = 0; index < triangleTypes.size() && !slotGlass; ++index)
            slotGlass = triangleSlot[index] == slot && IsGlassType(triangleTypes[index]);
        TextureSet textures;
        if (!PrepareTextures(slotOptions, textures, log, error) ||
            !WriteTextures(slotOptions, textures, slotGlass, log, error) ||
            !WriteMaterial(slotOptions, templateFor(options.surfaceType),
                           slotOptions.assetName, options.surfaceType, log, error)) {
            return 1;
        }
        // One extra material per overriding piece type, over the same images, so
        // bullet holes land on those pieces too (see MaterialNameForType); glass
        // pieces additionally switch template (see IsGlassType).
        for (int type = 1; type < cod4::kSurfaceTypeCount; ++type) {
            if (type == options.surfaceType) continue;
            bool used = false;
            for (std::size_t index = 0; index < triangleTypes.size() && !used; ++index)
                used = triangleSlot[index] == slot && triangleTypes[index] == type;
            if (!used) continue;
            if (!WriteMaterial(slotOptions, templateFor(type),
                               MaterialNameForType(slotOptions, type), type, log, error))
                return 1;
        }
    }
    if (!WriteXModel(options, slots, triangles, triangleTypes, triangleSlot, result, log,
                     error))
        return 1;

    LogPrintf(log,
              "Success: '%s' exported to '%s' as one LOD with three "
              "validated IWIs and material '%s'.",
              options.meshPath.string().c_str(),
              options.outputRoot.string().c_str(), options.assetName.c_str());
    return 0;
}

}  // namespace

bool LoadObj(const fs::path& path, bool flipWinding, ObjMesh& mesh, std::string& error) {
    return LoadObjImpl(path, flipWinding, mesh, error);
}

pieces::MeshInput ToPieceMesh(const ObjMesh& mesh) {
    return ToPieceMeshImpl(mesh);
}

namespace {

Options OptionsFromRequest(const ConvertRequest& request) {
    Options options;
    options.meshPath = request.sourcePath;
    options.outputRoot = request.outputRoot;
    options.templateRoot = request.templateRoot;
    options.requestedName = request.assetName;
    options.assetName = request.assetName;
    options.unitsFactor = request.unitsFactor;
    options.units = request.unitsFactor == kMetersPerInch ? "in"
                    : request.unitsFactor == 0.01         ? "cm"
                    : request.unitsFactor == 0.001        ? "mm"
                                                          : "m";
    options.scale = request.scale;
    options.originMode = request.originMode;
    options.origin = request.originMode == xmodel::OriginMode::BaseCenter     ? "base"
                     : request.originMode == xmodel::OriginMode::BoundsCenter ? "center"
                                                                              : "none";
    options.flipUv = request.flipUv;
    options.normalGreen = request.normalGreen;
    options.surfaceType = request.surfaceType;
    options.textures = request.textures;
    options.materialTextures = request.materialTextures;
    options.glassOpacity = request.glassOpacity;
    return options;
}

bool ValidateRequest(const ConvertRequest& request, std::string& error) {
    if (!request.mesh) {
        error = "no source mesh";
        return false;
    }
    if (request.assetName.empty() || request.assetName.size() > 46u ||
        xmodel::SanitizeModelName(request.assetName) != request.assetName) {
        error = "asset name '" + request.assetName +
                "' must be lowercase [a-z0-9_] and at most 46 characters";
        return false;
    }
    if (!(request.unitsFactor > 0.0) || !(request.scale > 0.0)) {
        error = "units and scale must be greater than zero";
        return false;
    }
    if (request.outputRoot.empty()) {
        error = "no output raw root";
        return false;
    }
    if (request.materialTextures.size() > 1u) {
        if (request.materialTextures.size() != request.mesh->materialNames.size()) {
            error = std::to_string(request.materialTextures.size()) +
                    " material texture sets for a source with " +
                    std::to_string(request.mesh->materialNames.size()) + " materials";
            return false;
        }
        const std::string last = request.assetName + "_m" +
                                 std::to_string(request.materialTextures.size() - 1u);
        if (last.size() > 46u) {
            error = "material asset name '" + last + "' is longer than 46 characters";
            return false;
        }
    }
    return true;
}

}  // namespace

bool Convert(const ConvertRequest& request, const LogFn& log, ConvertResult& result,
             std::string& error) {
    result = {};
    if (!ValidateRequest(request, error)) return false;
    const Options options = OptionsFromRequest(request);
    return RunConvert(options, *request.mesh, request.pieceType, result, log, error) == 0;
}

bool BuildModelSpaceMesh(const ConvertRequest& request, const pieces::PieceSet& pieceSet,
                         ModelSpaceMesh& out, std::string& error) {
    out = {};
    if (!request.mesh) {
        error = "no source mesh";
        return false;
    }
    const Options options = OptionsFromRequest(request);
    Bounds bounds;
    std::vector<xmodel::BakeTriangle> triangles;
    std::vector<std::size_t> sourceIndices;
    if (!BuildBakeTriangles(*request.mesh, options, triangles, bounds, LogFn{}, error,
                            &sourceIndices))
        return false;

    // xmodel_write.cpp's origin rebase: bounds of every kept corner, in inches.
    Vec3 mins{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max()};
    Vec3 maxs{-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
              -std::numeric_limits<double>::max()};
    bool populated = false;
    for (const xmodel::BakeTriangle& triangle : triangles)
        for (const xmodel::BakeVertex& corner : triangle.corners) {
            const Vec3 inches = Vec3{corner.positionMeters[0], corner.positionMeters[1],
                                     corner.positionMeters[2]} /
                                kMetersPerInch;
            if (!IsFinite(inches)) continue;
            mins = {std::min(mins.x, inches.x), std::min(mins.y, inches.y),
                    std::min(mins.z, inches.z)};
            maxs = {std::max(maxs.x, inches.x), std::max(maxs.y, inches.y),
                    std::max(maxs.z, inches.z)};
            populated = true;
        }
    Vec3 offset{};
    if (populated && options.originMode != xmodel::OriginMode::None) {
        offset = {(mins.x + maxs.x) * 0.5, (mins.y + maxs.y) * 0.5,
                  options.originMode == xmodel::OriginMode::BaseCenter
                      ? mins.z
                      : (mins.z + maxs.z) * 0.5};
    }
    out.originOffsetInches = {offset.x, offset.y, offset.z};

    out.positions.reserve(triangles.size() * 3u);
    out.uvs.reserve(triangles.size() * 3u);
    out.trianglePiece.reserve(triangles.size());
    std::array<float, 3> lo{std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()};
    std::array<float, 3> hi{-std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max()};
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        for (const xmodel::BakeVertex& corner : triangles[t].corners) {
            const std::array<float, 3> p{
                static_cast<float>(corner.positionMeters[0] / kMetersPerInch - offset.x),
                static_cast<float>(corner.positionMeters[1] / kMetersPerInch - offset.y),
                static_cast<float>(corner.positionMeters[2] / kMetersPerInch - offset.z)};
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
            out.positions.push_back(p);
            out.uvs.push_back({static_cast<float>(corner.uv[0]),
                               static_cast<float>(corner.uv[1])});
        }
        const std::size_t source = sourceIndices[t];
        out.trianglePiece.push_back(source < pieceSet.trianglePiece.size()
                                        ? pieceSet.trianglePiece[source]
                                        : 0u);
    }
    out.boundsMin = lo;
    out.boundsMax = hi;
    return true;
}

// KIWI GLASS
bool WriteImageMaterial(const ImageMaterialRequest& request, const LogFn& log,
                        std::vector<fs::path>& written, std::string& error) {
    if (request.color.empty()) {
        error = "a colour image is required";
        return false;
    }
    struct Slot {
        const fs::path* png;
        const char* suffix;
        cod4::IwiKind kind;
    };
    const Slot slots[3] = {
        {&request.color, "_col", cod4::IwiKind::Color},
        {&request.normal, "_nml", cod4::IwiKind::Normal},
        {&request.specularGloss, "_spc", cod4::IwiKind::SpecularGloss},
    };
    cod4::MaterialEditOptions edits;
    for (const Slot& slot : slots) {
        if (slot.png->empty()) continue;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
        if (!LoadRgba(*slot.png, width, height, pixels, error)) return false;
        if (slot.kind == cod4::IwiKind::Normal && request.normalGreen == NormalGreen::Flip)
            for (std::size_t offset = 1u; offset < pixels.size(); offset += 4u)
                pixels[offset] = static_cast<std::uint8_t>(255u - pixels[offset]);
        TextureSet textures;
        textures.width = static_cast<std::uint32_t>(width);
        textures.height = static_cast<std::uint32_t>(height);
        const std::string image = request.name + slot.suffix;
        const fs::path path = request.rawRoot / "images" / (image + ".iwi");
        if (!WriteAndValidateIwi(path, textures, pixels, slot.kind, log, error)) {
            error = "IWI export failed: " + error;
            return false;
        }
        written.push_back(path);
        if (slot.kind == cod4::IwiKind::Color) {
            edits.replaceColorMap = true;
            edits.colorMap = image;
        } else if (slot.kind == cod4::IwiKind::Normal) {
            edits.replaceNormalMap = true;
            edits.normalMap = image;
        } else {
            edits.replaceSpecularGlossMap = true;
            edits.specularGlossMap = image;
        }
    }
    if (request.surfaceType > 0)
        edits.surfaceTypeBits = request.surfaceType << cod4::kSurfaceTypeShift;
    cod4::MaterialWriteResult result;
    if (!cod4::WriteEditedMaterial(request.templateMaterial, request.rawRoot, request.name,
                                   edits, &result, error)) {
        error = "material export failed: " + error;
        return false;
    }
    if (!cod4::RoundTripMaterial(result.path, error)) {
        error = "material round-trip failed: " + error;
        return false;
    }
    written.push_back(result.path);
    LogPrintf(log, "Wrote + validated %s (template %s, technique %s)",
              result.path.string().c_str(), result.templatePath.string().c_str(),
              result.techniqueSet.c_str());
    return true;
}
// KIWI END

}  // namespace kimport
