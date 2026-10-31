#include "xmodel_write.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <mikktspace/mikktspace.h>

namespace xmodel {
namespace {

constexpr double kMetersPerInch = 0.0254;
constexpr size_t kMaxModelNameBytes = 51;
constexpr size_t kMaxMaterialNameBytes = 252;
constexpr uint32_t kMaxLods = 4;
constexpr uint32_t kMaxSurfaces = 255;
constexpr uint32_t kMaxSurfaceElements = std::numeric_limits<uint16_t>::max();
// The engine's MODELSURF_MAX_VERTICES is 0x8000, but stock cod4rad's
// model-collision transport builder faults near ~10320 verts per collision
// surface (see kMaxTrianglesPerSurface note). A UV-seam-heavy surface can carry
// far more verts than triangles, so guard verts independently at a value under
// that edge. Normal meshes (verts <= tris) never reach this via the 8192 tri
// cap; a pathological high-vert surface errors clearly instead of shipping a
// stock-crashing asset.
constexpr uint32_t kMaxVerticesPerSurface = 10000;
// Per-surface caps for stock-toolchain safety. Two distinct retail limits:
//
//  (1) cod4rad's BuildStaticModelCollisionMesh copies a surface's indices into
//      a fixed 0x8000-short stack buffer -> a hard 32768/3 = 10922-triangle
//      ceiling; one triangle more smashes the stack.
//  (2) BELOW that, stock cod4rad's model-collision light-transport builder
//      (retail 0x41A590 -> 0x4091E0) heap-corrupts and access-violates for a
//      single collision surface past ~10905 triangles / ~10320 vertices
//      (empirically bisected: 10900t/10317v OK, 10912t/10322v faults). The
//      exact internal limit is undocumented, so we stay well clear of it.
//
// 8192 gives a wide margin under the crash edge while still exceeding the
// heaviest stock surface ever shipped (6720 tris) — so we are no more split
// than a real AAA asset. Must stay EVEN: the v25 loader pads an odd raw count
// with a degenerate triangle.
constexpr size_t kMaxTrianglesPerSurface = 8192;
// Contents match raw/material_properties/$default3d (solid). The surface type
// deliberately does NOT: type 0 "default" gives Bullet_Trace penetration depth
// 0 and no impact FX, so bullets stop silently and the prop reads as having no
// collision (the concretefence_wall* case, 2026-09-15). Concrete
// (SURF_TYPE_CONCRETE = 5 << 20) is the fallback when a material carries none.
constexpr int32_t kCollisionContentsSolid = 1;
constexpr int32_t kCollisionSurfaceFlagsDefault = 0x00500000;

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(const Vec3& v, double scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

Vec3 operator/(const Vec3& v, double scale) {
    return {v.x / scale, v.y / scale, v.z / scale};
}

double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

double length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

bool finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 fromArray(const std::array<double, 3>& value) {
    return {value[0], value[1], value[2]};
}

std::array<float, 3> toArray(const Vec3& value) {
    return {static_cast<float>(value.x), static_cast<float>(value.y),
            static_cast<float>(value.z)};
}

struct Bounds {
    Vec3 mins{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max()};
    Vec3 maxs{-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
              -std::numeric_limits<double>::max()};
    bool populated = false;

    void add(const Vec3& p) {
        mins.x = std::min(mins.x, p.x);
        mins.y = std::min(mins.y, p.y);
        mins.z = std::min(mins.z, p.z);
        maxs.x = std::max(maxs.x, p.x);
        maxs.y = std::max(maxs.y, p.y);
        maxs.z = std::max(maxs.z, p.z);
        populated = true;
    }
};

class ByteWriter {
public:
    void u8(uint8_t value) { bytes.push_back(value); }

    void u16(uint16_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void i16(int16_t value) { u16(static_cast<uint16_t>(value)); }

    void u32(uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 24));
    }

    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }

    void f32(float value) { u32(std::bit_cast<uint32_t>(value)); }

    void vec3(const std::array<float, 3>& value) {
        f32(value[0]);
        f32(value[1]);
        f32(value[2]);
    }

    void vec4(const std::array<float, 4>& value) {
        f32(value[0]);
        f32(value[1]);
        f32(value[2]);
        f32(value[3]);
    }

    void string(const std::string& value) {
        bytes.insert(bytes.end(), value.begin(), value.end());
        bytes.push_back(0);
    }

    std::vector<uint8_t> bytes;
};

struct RawVertex {
    std::array<float, 3> normal{};
    std::array<uint8_t, 4> color{255, 255, 255, 255};
    std::array<float, 2> uv{};
    std::array<float, 3> binormal{};
    std::array<float, 3> tangent{};
    std::array<float, 3> position{};
};

struct VertexKey {
    std::array<uint32_t, 14> bits{};
    bool operator==(const VertexKey&) const = default;
};

struct VertexKeyHash {
    size_t operator()(const VertexKey& key) const noexcept {
        size_t hash = static_cast<size_t>(1469598103934665603ull);
        for (uint32_t word : key.bits) {
            hash ^= word;
            hash *= static_cast<size_t>(1099511628211ull);
        }
        return hash;
    }
};

float canonicalFloat(float value) {
    return value == 0.0f ? 0.0f : value;
}

VertexKey makeVertexKey(const RawVertex& vertex) {
    VertexKey key;
    size_t at = 0;
    auto append = [&](float value) {
        key.bits[at++] = std::bit_cast<uint32_t>(canonicalFloat(value));
    };
    for (float value : vertex.position) append(value);
    for (float value : vertex.normal) append(value);
    for (float value : vertex.uv) append(value);
    for (float value : vertex.binormal) append(value);
    for (float value : vertex.tangent) append(value);
    return key;
}

struct CollisionTriangle {
    std::array<float, 4> plane{};
    std::array<float, 4> svec{};
    std::array<float, 4> tvec{};
};

struct PreparedSurface {
    std::string materialName;
    std::vector<RawVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<CollisionTriangle> collision;
    Bounds bounds;
    int32_t surfaceFlags = kCollisionSurfaceFlagsDefault;
};

struct PreparedLod {
    float distance = 0.0f;
    std::string filename;
    std::vector<PreparedSurface> surfaces;
    Bounds bounds;
    double radius = 0.0;
};

bool checkedFloat(double value, float& out) {
    if (!std::isfinite(value) ||
        std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    out = canonicalFloat(static_cast<float>(value));
    return std::isfinite(out);
}

bool checkedVec3(const Vec3& value, std::array<float, 3>& out) {
    return checkedFloat(value.x, out[0]) && checkedFloat(value.y, out[1]) &&
           checkedFloat(value.z, out[2]);
}

bool validMaterialName(const std::string& name, std::string& error) {
    if (name.empty()) {
        error = "material name is empty";
        return false;
    }
    if (name.size() > kMaxMaterialNameBytes) {
        error = "material name exceeds 252 bytes";
        return false;
    }
    if (name.size() >= 3 && (name[0] == 'm' || name[0] == 'M') &&
        (name[1] == 'c' || name[1] == 'C') && name[2] == '/') {
        error = "material name must omit the loader-added mc/ prefix";
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c > 0x7e) {
            error = "material name must contain printable ASCII only";
            return false;
        }
    }
    return true;
}

std::array<double, 2> boxUv(const Vec3& p, int dominantAxis, double tileSize) {
    if (dominantAxis == 0) return {p.y / tileSize, p.z / tileSize};
    if (dominantAxis == 1) return {p.x / tileSize, p.z / tileSize};
    return {p.x / tileSize, p.y / tileSize};
}

Vec3 fallbackTangent(const Vec3& normal) {
    Vec3 basis;
    if (std::abs(normal.x) <= std::abs(normal.y) &&
        std::abs(normal.x) <= std::abs(normal.z))
        basis = {1.0, 0.0, 0.0};
    else if (std::abs(normal.y) <= std::abs(normal.z))
        basis = {0.0, 1.0, 0.0};
    else
        basis = {0.0, 0.0, 1.0};
    return cross(basis, normal) / length(cross(basis, normal));
}

// MikkTSpace wants faces counter-clockwise about their normals; the stored
// corners are clockwise (CoD4), so face vertex i reads stored corner
// kMikkCorner[i].
constexpr std::array<int, 3> kMikkCorner{0, 2, 1};

std::array<RawVertex, 3>& mikkTriangle(const SMikkTSpaceContext* context,
                                       int face) {
    auto& triangles =
        *static_cast<std::vector<std::array<RawVertex, 3>>*>(context->m_pUserData);
    return triangles[static_cast<size_t>(face)];
}

RawVertex& mikkCorner(const SMikkTSpaceContext* context, int face, int vert) {
    return mikkTriangle(context, face)[static_cast<size_t>(kMikkCorner[vert])];
}

int mikkNumFaces(const SMikkTSpaceContext* context) {
    return static_cast<int>(
        static_cast<std::vector<std::array<RawVertex, 3>>*>(context->m_pUserData)
            ->size());
}

int mikkNumVerticesOfFace(const SMikkTSpaceContext*, const int) {
    return 3;
}

void mikkPosition(const SMikkTSpaceContext* context, float out[], const int face,
                  const int vert) {
    const RawVertex& raw = mikkCorner(context, face, vert);
    out[0] = raw.position[0];
    out[1] = raw.position[1];
    out[2] = raw.position[2];
}

void mikkNormal(const SMikkTSpaceContext* context, float out[], const int face,
                const int vert) {
    const RawVertex& raw = mikkCorner(context, face, vert);
    out[0] = raw.normal[0];
    out[1] = raw.normal[1];
    out[2] = raw.normal[2];
}

void mikkTexCoord(const SMikkTSpaceContext* context, float out[], const int face,
                  const int vert) {
    const RawVertex& raw = mikkCorner(context, face, vert);
    out[0] = raw.uv[0];
    out[1] = raw.uv[1];
}

// Binormal = sign * cross(N, T), the same reconstruction the bakers use.
void mikkSetTSpaceBasic(const SMikkTSpaceContext* context, const float tangent[],
                        const float sign, const int face, const int vert) {
    RawVertex& raw = mikkCorner(context, face, vert);
    const Vec3 normal{raw.normal[0], raw.normal[1], raw.normal[2]};
    Vec3 t{tangent[0], tangent[1], tangent[2]};
    t = t - normal * dot(normal, t);
    double tangentLength = length(t);
    t = tangentLength > 1.0e-12 ? t / tangentLength : fallbackTangent(normal);
    const Vec3 binormal = cross(normal, t) * (sign < 0.0f ? -1.0 : 1.0);
    checkedVec3(t, raw.tangent);
    checkedVec3(binormal, raw.binormal);
}

bool computeMikkTangents(std::vector<std::array<RawVertex, 3>>& triangles) {
    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = mikkNumFaces;
    callbacks.m_getNumVerticesOfFace = mikkNumVerticesOfFace;
    callbacks.m_getPosition = mikkPosition;
    callbacks.m_getNormal = mikkNormal;
    callbacks.m_getTexCoord = mikkTexCoord;
    callbacks.m_setTSpaceBasic = mikkSetTSpaceBasic;
    SMikkTSpaceContext context{};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &triangles;
    return genTangSpaceDefault(&context) != 0;
}

// The translation-only origin rebase (rotation untouched): the pivot picked
// from the pre-pass bounds, subtracted from every vertex AFTER the meters ->
// inches conversion. Bounds, radius, box-projected UVs and collision below all
// read the rebased positions, so everything the files carry agrees with the shift.
bool prepareSurface(const MaterialTriangles& source, double tileSize,
                    const Vec3& originOffsetInches, PreparedSurface& output,
                    Bounds& modelBounds, double& radius,
                    size_t& degenerateSkips, std::string& error) {
    output.materialName = source.materialName;
    output.surfaceFlags = source.collisionSurfaceFlags >= 0
                              ? source.collisionSurfaceFlags
                              : kCollisionSurfaceFlagsDefault;
    if (source.triangles.size() > kMaxSurfaceElements) {
        error = "material '" + source.materialName + "' has " +
                std::to_string(source.triangles.size()) +
                " triangles; raw v25 allows at most 65535 per surface";
        return false;
    }

    output.indices.reserve(source.triangles.size() * 3u);
    output.collision.reserve(source.triangles.size());
    std::unordered_map<VertexKey, uint16_t, VertexKeyHash> vertexMap;
    vertexMap.reserve(std::min<size_t>(source.triangles.size() * 3u,
                                       kMaxSurfaceElements));
    // Triangles that survive the checks, corners in stored (clockwise) order.
    // Their tangent frames come from MikkTSpace after the loop, then they weld.
    std::vector<std::array<RawVertex, 3>> kept;
    kept.reserve(source.triangles.size());

    for (size_t triangleIndex = 0; triangleIndex < source.triangles.size();
         ++triangleIndex) {
        const BakeTriangle& sourceTriangle = source.triangles[triangleIndex];
        std::array<Vec3, 3> positions;
        std::array<Vec3, 3> normals;
        std::array<std::array<double, 2>, 3> uv{};
        bool hasExplicitUv = sourceTriangle.corners[0].hasUv;
        for (size_t corner = 0; corner < 3; ++corner) {
            if (sourceTriangle.corners[corner].hasUv != hasExplicitUv) {
                error = "material '" + source.materialName + "' triangle " +
                        std::to_string(triangleIndex) +
                        " mixes explicit and box-projected UV corners";
                return false;
            }
            positions[corner] = fromArray(sourceTriangle.corners[corner].positionMeters) /
                                    kMetersPerInch -
                                originOffsetInches;
            normals[corner] = fromArray(sourceTriangle.corners[corner].normal);
            if (hasExplicitUv) uv[corner] = sourceTriangle.corners[corner].uv;
            if (!finite(positions[corner]) || !finite(normals[corner])) {
                error = "material '" + source.materialName + "' triangle " +
                        std::to_string(triangleIndex) + " contains a non-finite value";
                return false;
            }
            if (hasExplicitUv &&
                (!std::isfinite(uv[corner][0]) ||
                 !std::isfinite(uv[corner][1]))) {
                error = "material '" + source.materialName + "' triangle " +
                        std::to_string(triangleIndex) +
                        " contains a non-finite explicit UV";
                return false;
            }
            double normalLength = length(normals[corner]);
            if (!(normalLength > 1.0e-12)) {
                error = "material '" + source.materialName + "' triangle " +
                        std::to_string(triangleIndex) + " has a zero bridge normal";
                return false;
            }
            normals[corner] = normals[corner] / normalLength;
        }

        Vec3 edge1 = positions[1] - positions[0];
        Vec3 edge2 = positions[2] - positions[0];
        Vec3 geometric = cross(edge1, edge2);
        double geometricLength = length(geometric);
        double edgeScale = length(edge1) * length(edge2);
        if (!(edgeScale > 0.0) || geometricLength <= edgeScale * 1.0e-12) {
            error = "material '" + source.materialName + "' triangle " +
                    std::to_string(triangleIndex) + " is degenerate";
            return false;
        }

        Vec3 normalSum = normals[0] + normals[1] + normals[2];
        double normalSumLength = length(normalSum);
        if (!(normalSumLength > 1.0e-12)) {
            normalSum = normals[0];
            normalSumLength = 1.0;
        }
        double orientation = dot(geometric, normalSum);
        if (std::abs(orientation) <= geometricLength * normalSumLength * 1.0e-6) {
            error = "material '" + source.materialName + "' triangle " +
                    std::to_string(triangleIndex) +
                    " bridge normals are perpendicular to its face";
            return false;
        }

        // CoD4 stock raw xmodels are clockwise when viewed along their stored
        // outward normals. Plastic's bridge pipeline culls neither winding, so
        // establish the engine convention from the CAD normals here.
        if (orientation > 0.0) {
            std::swap(positions[1], positions[2]);
            std::swap(normals[1], normals[2]);
            std::swap(uv[1], uv[2]);
            edge1 = positions[1] - positions[0];
            edge2 = positions[2] - positions[0];
            geometric = cross(edge1, edge2);
            geometricLength = length(geometric);
        }
        Vec3 planeNormal = geometric * (-1.0 / geometricLength);

        if (!hasExplicitUv) {
            int dominantAxis = 0;
            if (std::abs(planeNormal.y) > std::abs(planeNormal.x))
                dominantAxis = 1;
            if (std::abs(planeNormal.z) >
                (dominantAxis == 0 ? std::abs(planeNormal.x)
                                   : std::abs(planeNormal.y))) {
                dominantAxis = 2;
            }
            for (size_t corner = 0; corner < 3; ++corner)
                uv[corner] = boxUv(positions[corner], dominantAxis, tileSize);
        }

        double du1 = uv[1][0] - uv[0][0];
        double dv1 = uv[1][1] - uv[0][1];
        double du2 = uv[2][0] - uv[0][0];
        double dv2 = uv[2][1] - uv[0][1];
        double uvDet = du1 * dv2 - du2 * dv1;
        if (!std::isfinite(uvDet) || uvDet == 0.0) {
            error = "material '" + source.materialName + "' triangle " +
                    std::to_string(triangleIndex) +
                    (hasExplicitUv ? " has degenerate explicit UVs"
                                   : " produced a degenerate box projection");
            return false;
        }
        std::array<RawVertex, 3> cornerRaw{};
        for (size_t corner = 0; corner < 3; ++corner) {
            RawVertex& raw = cornerRaw[corner];  // tangent/binormal: MikkTSpace, after the loop
            if (!checkedVec3(positions[corner], raw.position) ||
                !checkedVec3(normals[corner], raw.normal) ||
                !checkedFloat(uv[corner][0], raw.uv[0]) ||
                !checkedFloat(uv[corner][1], raw.uv[1])) {
                error = "material '" + source.materialName + "' triangle " +
                        std::to_string(triangleIndex) +
                        " exceeds raw v25 float range";
                return false;
            }
        }

        // cod4rad rebuilds collision from the stored FLOAT32 vertices and
        // asserts each triangle has positive projected area (collvec.c
        // scale > 0). A sliver with area at double precision can collapse
        // after the float32 store — or weld two float-identical corners into
        // one index — so re-test degeneracy on exactly the values the file
        // will carry and skip such triangles.
        {
            const Vec3 p0{cornerRaw[0].position[0], cornerRaw[0].position[1],
                          cornerRaw[0].position[2]};
            const Vec3 p1{cornerRaw[1].position[0], cornerRaw[1].position[1],
                          cornerRaw[1].position[2]};
            const Vec3 p2{cornerRaw[2].position[0], cornerRaw[2].position[1],
                          cornerRaw[2].position[2]};
            const Vec3 fEdge1 = p1 - p0;
            const Vec3 fEdge2 = p2 - p0;
            const double fCrossLength = length(cross(fEdge1, fEdge2));
            const double fEdgeScale = length(fEdge1) * length(fEdge2);
            if (!(fEdgeScale > 0.0) || fCrossLength <= fEdgeScale * 1.0e-6) {
                ++degenerateSkips;
                continue;
            }
        }

        kept.push_back(cornerRaw);
        for (size_t corner = 0; corner < 3; ++corner) {
            output.bounds.add(positions[corner]);
            modelBounds.add(positions[corner]);
            radius = std::max(radius, length(positions[corner]));
        }

        double d00 = dot(edge1, edge1);
        double d01 = dot(edge1, edge2);
        double d11 = dot(edge2, edge2);
        double baryDet = d00 * d11 - d01 * d01;
        if (!(baryDet > 0.0)) {
            error = "material '" + source.materialName + "' triangle " +
                    std::to_string(triangleIndex) +
                    " cannot form collision barycentrics";
            return false;
        }
        Vec3 s = (edge1 * d11 - edge2 * d01) / baryDet;
        Vec3 t = (edge2 * d00 - edge1 * d01) / baryDet;
        CollisionTriangle collision;
        std::array<float, 3> plane3, s3, t3;
        float planeDist = 0.0f, sOrigin = 0.0f, tOrigin = 0.0f;
        if (!checkedVec3(planeNormal, plane3) || !checkedVec3(s, s3) ||
            !checkedVec3(t, t3) ||
            !checkedFloat(dot(planeNormal, positions[0]), planeDist) ||
            !checkedFloat(dot(s, positions[0]), sOrigin) ||
            !checkedFloat(dot(t, positions[0]), tOrigin)) {
            error = "material '" + source.materialName + "' triangle " +
                    std::to_string(triangleIndex) +
                    " collision data exceeds raw v25 float range";
            return false;
        }
        collision.plane = {plane3[0], plane3[1], plane3[2], planeDist};
        collision.svec = {s3[0], s3[1], s3[2], sOrigin};
        collision.tvec = {t3[0], t3[1], t3[2], tOrigin};
        output.collision.push_back(collision);
    }

    if (kept.empty()) {
        error = "material '" + source.materialName +
                "' has no triangles left after skipping float-degenerate ones";
        return false;
    }

    // Normal maps are baked (Blender, Substance, InstaMAT, xNormal) against
    // MikkTSpace tangents; any other basis turns the bake's broad corrections
    // into visible facets and lines on low-poly meshes, so use the same code.
    if (!computeMikkTangents(kept)) {
        error = "material '" + source.materialName +
                "' MikkTSpace tangent generation failed";
        return false;
    }

    // Weld on the full vertex, tangent frame included: MikkTSpace gives every
    // corner with equal position/normal/uv the same frame unless it must split
    // it (mirrored UVs), so this keeps the old vertex counts.
    for (const std::array<RawVertex, 3>& triangle : kept) {
        for (const RawVertex& raw : triangle) {
            VertexKey key = makeVertexKey(raw);
            auto found = vertexMap.find(key);
            if (found == vertexMap.end()) {
                if (output.vertices.size() >= kMaxVerticesPerSurface) {
                    // The vertex-aware chunker upstream keeps every surface's
                    // welded vertex count under kMaxVerticesPerSurface, so this
                    // is a defensive guard that should be unreachable.
                    error = "material '" + source.materialName + "' exceeded " +
                            std::to_string(kMaxVerticesPerSurface) +
                            " welded vertices in one split surface after "
                            "vertex-aware splitting (internal invariant)";
                    return false;
                }
                uint16_t index = static_cast<uint16_t>(output.vertices.size());
                output.vertices.push_back(raw);
                found = vertexMap.emplace(key, index).first;
            }
            output.indices.push_back(found->second);
        }
    }
    return true;
}

void writeConfig(ByteWriter& writer, const std::vector<PreparedLod>& lods,
                 float radius, uint32_t collisionLod) {
    writer.i16(25);
    writer.u8(0);  // XModel flags
    for (int axis = 0; axis < 3; ++axis) writer.f32(-radius);
    for (int axis = 0; axis < 3; ++axis) writer.f32(radius);
    writer.string("");  // physics preset
    for (uint32_t lod = 0; lod < kMaxLods; ++lod) {
        if (lod < lods.size()) {
            writer.f32(lods[lod].distance);
            writer.string(lods[lod].filename);
        } else {
            writer.f32(0.0f);
            writer.string("");
        }
    }
    writer.i32(static_cast<int32_t>(collisionLod));
}

void writeCollision(ByteWriter& writer, const std::vector<PreparedSurface>& surfaces) {
    writer.i32(static_cast<int32_t>(surfaces.size()));
    for (const PreparedSurface& surface : surfaces) {
        writer.i32(static_cast<int32_t>(surface.collision.size()));
        for (const CollisionTriangle& triangle : surface.collision) {
            writer.vec4(triangle.plane);
            writer.vec4(triangle.svec);
            writer.vec4(triangle.tvec);
        }
        writer.vec3(toArray(surface.bounds.mins));
        writer.vec3(toArray(surface.bounds.maxs));
        writer.i32(0);  // bone index: the sole root bone
        writer.i32(kCollisionContentsSolid);
        writer.i32(surface.surfaceFlags);
    }
}

ByteWriter makeModelFile(const std::vector<PreparedLod>& lods,
                         const Bounds& modelBounds, float radius,
                         uint32_t collisionLod) {
    ByteWriter writer;
    writeConfig(writer, lods, radius, collisionLod);
    writeCollision(writer, lods[collisionLod].surfaces);
    for (const PreparedLod& lod : lods) {
        writer.u16(static_cast<uint16_t>(lod.surfaces.size()));
        for (const PreparedSurface& surface : lod.surfaces)
            writer.string(surface.materialName);
    }
    writer.vec3(toArray(modelBounds.mins));
    writer.vec3(toArray(modelBounds.maxs));
    return writer;
}

ByteWriter makePartsFile() {
    ByteWriter writer;
    writer.u16(25);
    writer.u16(0);  // child bones
    writer.u16(1);  // root bones
    writer.string("tag_origin");
    writer.u8(0);  // part classification: none
    writer.u8(1);  // use bones
    return writer;
}

ByteWriter makeSurfsFile(const std::vector<PreparedSurface>& surfaces) {
    ByteWriter writer;
    writer.i16(25);
    writer.i16(static_cast<int16_t>(surfaces.size()));
    for (const PreparedSurface& surface : surfaces) {
        writer.u8(0);   // tileMode
        writer.u16(0);  // ignored/opaque in the v25 loader
        writer.u16(static_cast<uint16_t>(surface.vertices.size()));
        writer.u16(static_cast<uint16_t>(surface.collision.size()));
        writer.u16(static_cast<uint16_t>(surface.vertices.size()));
        writer.u16(0);  // local root-bone index
        writer.u16(0);  // rigid-list terminator
        for (const RawVertex& vertex : surface.vertices) {
            writer.vec3(vertex.normal);
            for (uint8_t channel : vertex.color) writer.u8(channel);
            writer.f32(vertex.uv[0]);
            writer.f32(vertex.uv[1]);
            writer.vec3(vertex.binormal);
            writer.vec3(vertex.tangent);
            writer.vec3(vertex.position);
        }
        for (uint16_t index : surface.indices) writer.u16(index);
    }
    return writer;
}

bool writeFile(const std::filesystem::path& path, const ByteWriter& contents,
               std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open '" + path.string() + "' for writing";
        return false;
    }
    if (!contents.bytes.empty())
        file.write(reinterpret_cast<const char*>(contents.bytes.data()),
                   static_cast<std::streamsize>(contents.bytes.size()));
    file.close();
    if (!file) {
        error = "failed while writing '" + path.string() + "'";
        return false;
    }
    return true;
}

BakeResult failure(std::string message) {
    BakeResult result;
    result.message = "Bake error: " + std::move(message);
    return result;
}

bool containsLineBreak(const std::string& value) {
    return value.find('\n') != std::string::npos || value.find('\r') != std::string::npos;
}

std::string lodFilename(const std::string& modelName, size_t lodIndex,
                        size_t lodCount) {
    if (lodCount == 1) return modelName;

    // This is the dominant stock raw-export convention: foo_lod01,
    // foo_lod11, ... . Keep enough headroom for both the suffix and the
    // loader's 64-byte "xmodelsurfs/<name>" prefixed path buffer.
    const std::string suffix = "_lod" + std::to_string(lodIndex) + "1";
    size_t baseBytes = kMaxModelNameBytes - suffix.size();
    return modelName.substr(0, baseBytes) + suffix;
}

}  // namespace

std::string SanitizeModelName(std::string_view name) {
    std::string sanitized;
    sanitized.reserve(std::min(name.size(), kMaxModelNameBytes));
    for (unsigned char c : name) {
        if (sanitized.size() == kMaxModelNameBytes) break;
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            sanitized.push_back(static_cast<char>(c));
        else
            sanitized.push_back('_');
    }
    return sanitized;
}

bool LoadBakeSettings(const std::filesystem::path& path, BakeSettings& settings,
                      std::string& error) {
    error.clear();
    std::error_code filesystemError;
    bool exists = std::filesystem::exists(path, filesystemError);
    if (filesystemError) {
        error = "cannot inspect bake settings '" + path.string() + "': " +
                filesystemError.message();
        return false;
    }
    if (!exists) return true;

    std::ifstream file(path);
    if (!file) {
        error = "cannot read bake settings '" + path.string() + "'";
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        if (key == "model_name") {
            std::string sanitized = SanitizeModelName(value);
            if (!sanitized.empty()) settings.modelName = std::move(sanitized);
        } else if (key == "output_root" && !value.empty()) {
            settings.outputRoot = std::move(value);
        } else if (key == "origin_mode") {
            if (value == "none")
                settings.originMode = OriginMode::None;
            else if (value == "bounds_center")
                settings.originMode = OriginMode::BoundsCenter;
            else if (value == "base_center")
                settings.originMode = OriginMode::BaseCenter;
            // unknown values keep the default
        } else if (key == "uv_tile_size_inches") {
            std::istringstream parser(value);
            double parsed = 0.0;
            char trailing = 0;
            if ((parser >> parsed) && !(parser >> trailing) && std::isfinite(parsed) &&
                parsed > 0.0) {
                settings.uvTileSizeInches = parsed;
            }
        } else if (key == "uv_mode") {
            if (value == "tiling_box")
                settings.uvMode = UvMode::TilingBox;
            else if (value == "unique_atlas")
                settings.uvMode = UvMode::UniqueAtlas;
        } else if (key == "texture_size") {
            std::istringstream parser(value);
            uint32_t parsed = 0;
            char trailing = 0;
            if ((parser >> parsed) && !(parser >> trailing) &&
                (parsed == 256u || parsed == 512u || parsed == 1024u ||
                 parsed == 2048u)) {
                settings.textureSize = parsed;
            }
        }
    }
    if (!file.eof()) {
        error = "failed while reading bake settings '" + path.string() + "'";
        return false;
    }
    return true;
}

bool SaveBakeSettings(const std::filesystem::path& path, const BakeSettings& settings,
                      std::string& error) {
    error.clear();
    std::string modelName = SanitizeModelName(settings.modelName);
    if (modelName.empty()) {
        error = "cannot persist an empty model name";
        return false;
    }
    if (settings.outputRoot.empty() || containsLineBreak(settings.outputRoot)) {
        error = "cannot persist an empty or multiline output root";
        return false;
    }
    if (!std::isfinite(settings.uvTileSizeInches) || settings.uvTileSizeInches <= 0.0) {
        error = "cannot persist a non-positive UV tile size";
        return false;
    }
    if (settings.textureSize != 256u && settings.textureSize != 512u &&
        settings.textureSize != 1024u && settings.textureSize != 2048u) {
        error = "cannot persist an unsupported atlas texture size";
        return false;
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        error = "cannot write bake settings '" + path.string() + "'";
        return false;
    }
    file << "model_name=" << modelName << '\n';
    file << "output_root=" << settings.outputRoot << '\n';
    file << "origin_mode="
         << (settings.originMode == OriginMode::None
                 ? "none"
                 : settings.originMode == OriginMode::BoundsCenter ? "bounds_center"
                                                                    : "base_center")
         << '\n';
    file << "uv_tile_size_inches=" << settings.uvTileSizeInches << '\n';
    file << "uv_mode="
         << (settings.uvMode == UvMode::UniqueAtlas ? "unique_atlas"
                                                    : "tiling_box")
         << '\n';
    file << "texture_size=" << settings.textureSize << '\n';
    file.close();
    if (!file) {
        error = "failed while writing bake settings '" + path.string() + "'";
        return false;
    }
    return true;
}

BakeResult WriteRawXModelV25(const BakeRequest& request) {
    try {
        std::string modelName = SanitizeModelName(request.modelName);
        if (modelName.empty()) return failure("model name is empty after sanitization");
        if (request.outputRoot.empty()) return failure("output root is empty");
        if (!std::isfinite(request.uvTileSizeInches) || request.uvTileSizeInches <= 0.0)
            return failure("UV tile size must be a finite value greater than zero inches");

        if (request.lods.empty()) return failure("no LOD meshes to bake");
        if (request.lods.size() > kMaxLods)
            return failure("raw v25 supports at most four LODs");
        if (request.collisionLod >= request.lods.size())
            return failure("collision LOD is outside the enabled LOD range");

        for (size_t lodIndex = 0; lodIndex < request.lods.size(); ++lodIndex) {
            float distance = request.lods[lodIndex].distance;
            if (!std::isfinite(distance) || distance < 0.0f)
                return failure("LOD" + std::to_string(lodIndex) +
                               " distance must be finite and non-negative");
            if (lodIndex + 1 < request.lods.size() && distance == 0.0f)
                return failure("distance 0 is only valid on the final/coarsest LOD");
            if (lodIndex > 0 && distance != 0.0f &&
                distance <= request.lods[lodIndex - 1].distance) {
                return failure("non-zero LOD distances must increase from fine to coarse");
            }
        }

        // Split each material into surfaces bounded by BOTH the triangle cap
        // and the welded-vertex cap. Splitting on triangle count alone assumes
        // ~1 vertex per triangle, which holds for clean unwraps but not for a
        // seam-heavy tool export (InstaMAT hard-surface-axial can weld barely
        // better than 3 verts/triangle) — such a surface would blow the engine
        // per-surface vertex limit. The key mirrors prepareSurface's weld
        // (position/normal/uv) at double precision, which distinguishes at
        // least as finely as the file's float32 weld, so this estimate never
        // undercounts and the hard cap in prepareSurface stays a dead guard.
        struct ChunkKey {
            std::array<double, 8> v{};
            bool operator==(const ChunkKey& other) const { return v == other.v; }
        };
        struct ChunkKeyHash {
            size_t operator()(const ChunkKey& key) const noexcept {
                size_t hash = 1469598103934665603ull;
                for (double component : key.v) {
                    if (component == 0.0) component = 0.0;  // fold -0.0 into +0.0
                    hash ^= std::hash<double>{}(component) +
                            0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
                }
                return hash;
            }
        };
        const auto cornerKey = [](const BakeVertex& corner) {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            // A UV-less corner (legacy box-projection path) gets a NaN uv so it
            // never welds — an over-count that can only split earlier, never
            // overflow.
            return ChunkKey{{corner.positionMeters[0], corner.positionMeters[1],
                             corner.positionMeters[2], corner.normal[0],
                             corner.normal[1], corner.normal[2],
                             corner.hasUv ? corner.uv[0] : nan,
                             corner.hasUv ? corner.uv[1] : nan}};
        };

        std::vector<std::vector<MaterialTriangles>> groupedLods(request.lods.size());
        for (size_t lodIndex = 0; lodIndex < request.lods.size(); ++lodIndex) {
            std::vector<MaterialTriangles>& grouped = groupedLods[lodIndex];
            for (const MaterialTriangles& material : request.lods[lodIndex].materials) {
                if (material.triangles.empty()) continue;
                std::string materialError;
                if (!validMaterialName(material.materialName, materialError))
                    return failure("LOD" + std::to_string(lodIndex) + ": " +
                                   materialError);

                bool hasOpen = false;
                size_t openChunk = 0;
                size_t openTriCount = 0;
                std::unordered_set<ChunkKey, ChunkKeyHash> openVertKeys;
                for (const BakeTriangle& triangle : material.triangles) {
                    const std::array<ChunkKey, 3> keys = {
                        cornerKey(triangle.corners[0]),
                        cornerKey(triangle.corners[1]),
                        cornerKey(triangle.corners[2])};
                    size_t newVerts = 0;
                    for (size_t i = 0; i < 3; ++i) {
                        bool duplicateInTriangle = false;
                        for (size_t j = 0; j < i; ++j) {
                            if (keys[j] == keys[i]) {
                                duplicateInTriangle = true;
                                break;
                            }
                        }
                        if (duplicateInTriangle) continue;
                        if (!hasOpen ||
                            openVertKeys.find(keys[i]) == openVertKeys.end())
                            ++newVerts;
                    }
                    if (!hasOpen || openTriCount >= kMaxTrianglesPerSurface ||
                        openVertKeys.size() + newVerts > kMaxVerticesPerSurface) {
                        if (grouped.size() == kMaxSurfaces) {
                            return failure("LOD" + std::to_string(lodIndex) +
                                           " needs more than 255 split material "
                                           "surfaces; simplify the mesh or its "
                                           "UV unwrap");
                        }
                        MaterialTriangles chunk;
                        chunk.materialName = material.materialName;
                        chunk.collisionSurfaceFlags =
                            material.collisionSurfaceFlags;
                        grouped.push_back(std::move(chunk));
                        openChunk = grouped.size() - 1;
                        openTriCount = 0;
                        openVertKeys.clear();
                        hasOpen = true;
                    }
                    grouped[openChunk].triangles.push_back(triangle);
                    ++openTriCount;
                    for (const ChunkKey& key : keys) openVertKeys.insert(key);
                }
            }
            if (grouped.empty())
                return failure("LOD" + std::to_string(lodIndex) +
                               " has no Solid or Sheet triangles to bake");
            if (grouped.size() > kMaxSurfaces)
                return failure("LOD" + std::to_string(lodIndex) +
                               " has more than 255 material surfaces");
        }

        // Origin rebase pre-pass: Plasticity scenes carry a large world offset,
        // so pick the pivot from the source bounds (inches) and shift every
        // vertex onto the origin.  Translation only — rotation is untouched.
        // Non-finite values are skipped here; prepareSurface rejects them with
        // the precise triangle later.
        Vec3 originOffsetInches{0.0, 0.0, 0.0};
        if (request.originMode != OriginMode::None) {
            Bounds sourceBounds;
            // Every LOD uses the one translation chosen from LOD0.
            for (const MaterialTriangles& material : groupedLods[0])
                for (const BakeTriangle& triangle : material.triangles)
                    for (const BakeVertex& corner : triangle.corners) {
                        Vec3 inches = fromArray(corner.positionMeters) / kMetersPerInch;
                        if (finite(inches)) sourceBounds.add(inches);
                    }
            if (sourceBounds.populated) {
                originOffsetInches = {
                    (sourceBounds.mins.x + sourceBounds.maxs.x) * 0.5,
                    (sourceBounds.mins.y + sourceBounds.maxs.y) * 0.5,
                    request.originMode == OriginMode::BaseCenter
                        ? sourceBounds.mins.z
                        : (sourceBounds.mins.z + sourceBounds.maxs.z) * 0.5,
                };
            }
        }

        std::vector<PreparedLod> lods(request.lods.size());
        double radius = 0.0;
        uint64_t totalSurfaces = 0;
        uint64_t totalVertices = 0;
        uint64_t totalTriangles = 0;
        size_t degenerateSkips = 0;
        for (size_t lodIndex = 0; lodIndex < lods.size(); ++lodIndex) {
            PreparedLod& lod = lods[lodIndex];
            lod.distance = request.lods[lodIndex].distance;
            lod.filename = lodFilename(modelName, lodIndex, lods.size());
            lod.surfaces.resize(groupedLods[lodIndex].size());
            for (size_t surfaceIndex = 0;
                 surfaceIndex < groupedLods[lodIndex].size(); ++surfaceIndex) {
                std::string prepareError;
                if (!prepareSurface(groupedLods[lodIndex][surfaceIndex],
                                    request.uvTileSizeInches, originOffsetInches,
                                    lod.surfaces[surfaceIndex], lod.bounds, lod.radius,
                                    degenerateSkips, prepareError)) {
                    return failure("LOD" + std::to_string(lodIndex) + ": " +
                                   std::move(prepareError));
                }
                totalVertices += lod.surfaces[surfaceIndex].vertices.size();
                totalTriangles += lod.surfaces[surfaceIndex].collision.size();
            }
            totalSurfaces += lod.surfaces.size();
            radius = std::max(radius, lod.radius);
        }
        const Bounds& modelBounds = lods[0].bounds;
        float radiusFloat = 0.0f;
        if (!modelBounds.populated || !checkedFloat(radius, radiusFloat))
            return failure("model bounds exceed raw v25 float range");
        std::array<float, 3> checkedBounds;
        if (!checkedVec3(modelBounds.mins, checkedBounds) ||
            !checkedVec3(modelBounds.maxs, checkedBounds))
            return failure("model bounds exceed raw v25 float range");

        ByteWriter modelFile = makeModelFile(lods, modelBounds, radiusFloat,
                                             request.collisionLod);
        ByteWriter partsFile = makePartsFile();
        std::vector<ByteWriter> surfsFiles;
        surfsFiles.reserve(lods.size());
        for (const PreparedLod& lod : lods)
            surfsFiles.push_back(makeSurfsFile(lod.surfaces));

        std::filesystem::path modelDirectory = request.outputRoot / "xmodel";
        std::filesystem::path partsDirectory = request.outputRoot / "xmodelparts";
        std::filesystem::path surfsDirectory = request.outputRoot / "xmodelsurfs";
        for (const std::filesystem::path* directory :
             {&modelDirectory, &partsDirectory, &surfsDirectory}) {
            std::error_code filesystemError;
            std::filesystem::create_directories(*directory, filesystemError);
            if (filesystemError)
                return failure("cannot create '" + directory->string() + "': " +
                               filesystemError.message());
        }

        std::vector<std::filesystem::path> paths;
        paths.reserve(2u + lods.size());
        paths.push_back(modelDirectory / modelName);
        // XModelLoadFile precaches parts using config entry zero's filename.
        paths.push_back(partsDirectory / lods[0].filename);
        for (const PreparedLod& lod : lods)
            paths.push_back(surfsDirectory / lod.filename);
        std::string writeError;
        if (!writeFile(paths[0], modelFile, writeError) ||
            !writeFile(paths[1], partsFile, writeError))
            return failure(std::move(writeError));
        for (size_t lodIndex = 0; lodIndex < surfsFiles.size(); ++lodIndex)
            if (!writeFile(paths[lodIndex + 2u], surfsFiles[lodIndex], writeError))
                return failure(std::move(writeError));

        BakeResult result;
        result.success = true;
        result.writtenPaths = paths;
        result.lodCount = lods.size();
        result.surfaceCount = totalSurfaces;
        result.vertexCount = totalVertices;
        result.triangleCount = totalTriangles;
        result.appliedOffsetInches = {originOffsetInches.x, originOffsetInches.y,
                                      originOffsetInches.z};
        std::ostringstream message;
        message << "Wrote ";
        for (size_t index = 0; index < paths.size(); ++index) {
            if (index != 0) message << ", ";
            message << paths[index].string();
        }
        message << " (" << result.lodCount << " LOD, " << result.surfaceCount
                << " surf, " << result.vertexCount << " verts, "
                << result.triangleCount << " visual tris; collision LOD"
                << request.collisionLod << ")";
        if (originOffsetInches.x != 0.0 || originOffsetInches.y != 0.0 ||
            originOffsetInches.z != 0.0) {
            message << "; origin rebase shifted by (" << -originOffsetInches.x << ", "
                    << -originOffsetInches.y << ", " << -originOffsetInches.z
                    << ") inches";
        }
        if (degenerateSkips > 0) {
            message << "; skipped " << degenerateSkips
                    << " float32-degenerate triangles (cod4rad collision safety)";
        }
        result.message = message.str();
        return result;
    } catch (const std::exception& exception) {
        return failure(exception.what());
    } catch (...) {
        return failure("unknown writer failure");
    }
}

}  // namespace xmodel
