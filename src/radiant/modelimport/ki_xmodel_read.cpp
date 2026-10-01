// KIWI model importer - raw xmodel v25 reader. See ki_xmodel_read.h.

#include "ki_xmodel_read.h"

#include "cod4_material.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace kimport {
namespace {

namespace fs = std::filesystem;

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t at = 0;
    bool ok = true;

    bool Need(std::size_t count) {
        if (!ok || at + count > bytes.size()) {
            ok = false;
            return false;
        }
        return true;
    }
    template <typename T>
    T Get() {
        T value{};
        if (!Need(sizeof(T))) return value;
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    void Skip(std::size_t count) {
        if (Need(count)) at += count;
    }
    std::string String() {
        std::string text;
        while (Need(1)) {
            const char c = static_cast<char>(bytes[at++]);
            if (!c) return text;
            text.push_back(c);
        }
        return text;
    }
};

bool ReadFile(const fs::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

}  // namespace

bool ReadRawXModel(const fs::path& rawRoot, const std::string& name, RawXModel& model,
                   std::string& error) {
    model = {};
    model.name = name;
    std::vector<std::uint8_t> bytes;
    if (!ReadFile(rawRoot / "xmodel" / name, bytes)) {
        error = "cannot read xmodel " + name;
        return false;
    }
    Reader r{bytes};
    // XModelLoadConfigFile
    if (r.Get<std::int16_t>() != 25) {
        error = name + ": not a raw v25 xmodel";
        return false;
    }
    r.Get<std::uint8_t>();   // flags
    r.Skip(24);              // mins, maxs
    r.String();              // physics preset
    std::array<std::string, 4> lodFiles;
    for (std::string& file : lodFiles) {
        r.Get<float>();
        file = r.String();
    }
    r.Get<std::int32_t>();   // collLod
    // XModelLoadCollData
    const std::int32_t collSurfs = r.Get<std::int32_t>();
    if (collSurfs < 0 || collSurfs > 4096) {
        error = name + ": bad collision surface count";
        return false;
    }
    for (std::int32_t s = 0; s < collSurfs && r.ok; ++s) {
        const std::int32_t tris = r.Get<std::int32_t>();
        if (tris < 0) r.ok = false;
        r.Skip(static_cast<std::size_t>(tris) * 48u + 24u + 12u);
    }
    // per-LOD material names
    std::vector<std::string> lod0Materials;
    for (int lod = 0; lod < 4 && r.ok; ++lod) {
        if (lodFiles[lod].empty()) continue;
        const std::uint16_t count = r.Get<std::uint16_t>();
        for (std::uint16_t i = 0; i < count; ++i) {
            std::string material = r.String();
            if (lod == 0) lod0Materials.push_back(std::move(material));
        }
    }
    if (!r.ok || lodFiles[0].empty()) {
        error = name + ": truncated xmodel";
        return false;
    }

    // XModelReadSurface, LOD0 only.
    std::vector<std::uint8_t> surfBytes;
    if (!ReadFile(rawRoot / "xmodelsurfs" / lodFiles[0], surfBytes)) {
        error = name + ": cannot read xmodelsurfs " + lodFiles[0];
        return false;
    }
    Reader s{surfBytes};
    if (s.Get<std::int16_t>() != 25) {
        error = name + ": xmodelsurfs is not v25";
        return false;
    }
    const std::int16_t surfCount = s.Get<std::int16_t>();
    if (surfCount <= 0 || static_cast<std::size_t>(surfCount) != lod0Materials.size()) {
        error = name + ": surface count does not match the material list";
        return false;
    }
    std::array<float, 3> lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()};
    std::array<float, 3> hi{-std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max()};
    for (std::int16_t surf = 0; surf < surfCount && s.ok; ++surf) {
        s.Get<std::uint8_t>();    // tileMode
        s.Get<std::uint16_t>();   // opaque
        const std::uint16_t vertCount = s.Get<std::uint16_t>();
        const std::uint16_t triCount = s.Get<std::uint16_t>();
        int vertListCount = 0;
        int rigidVerts = 0;
        for (;;) {
            const std::uint16_t listVerts = s.Get<std::uint16_t>();
            if (!s.ok || listVerts == 0) break;
            s.Get<std::uint16_t>();   // bone
            rigidVerts += listVerts;
            ++vertListCount;
        }
        const bool deformed = rigidVerts != vertCount;
        if (deformed) vertListCount = 0;
        if (vertListCount != 1) {
            s.Get<std::uint16_t>();   // numblends
            model.multiBone = true;
        }
        std::vector<std::array<float, 3>> verts(vertCount);
        for (std::uint16_t v = 0; v < vertCount && s.ok; ++v) {
            s.Skip(12 + 4 + 8 + 12 + 12);   // normal, color, uv, binormal, tangent
            std::uint8_t weights = 0;
            if (vertListCount != 1) {
                weights = s.Get<std::uint8_t>();
                s.Get<std::uint16_t>();     // blend bone
            }
            for (int a = 0; a < 3; ++a) verts[v][a] = s.Get<float>();
            s.Skip(static_cast<std::size_t>(weights) * 4u);
        }
        RawXModelSurface info;
        info.material = lod0Materials[surf];
        info.firstTriangle = static_cast<std::uint32_t>(model.triangleSurface.size());
        for (std::uint16_t t = 0; t < triCount && s.ok; ++t) {
            std::uint16_t index[3];
            for (std::uint16_t& i : index) i = s.Get<std::uint16_t>();
            if (index[0] >= vertCount || index[1] >= vertCount || index[2] >= vertCount) {
                s.ok = false;
                break;
            }
            for (std::uint16_t i : index) {
                model.positions.push_back(verts[i]);
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], verts[i][a]);
                    hi[a] = std::max(hi[a], verts[i][a]);
                }
            }
            model.triangleSurface.push_back(static_cast<std::uint32_t>(surf));
        }
        info.triangleCount =
            static_cast<std::uint32_t>(model.triangleSurface.size()) - info.firstTriangle;
        cod4::MaterialInfo material;
        std::string materialError;
        if (cod4::LoadMaterialInfo(rawRoot / "materials" / info.material, material,
                                   materialError)) {
            info.surfaceType = (static_cast<std::uint32_t>(material.surfaceFlags) >> 20) & 0x1f;
            info.gameFlags = material.gameFlags;
            info.techniqueSet = material.techniqueSet;
        }
        model.surfaces.push_back(std::move(info));
    }
    if (!s.ok) {
        error = name + ": truncated xmodelsurfs";
        return false;
    }
    model.boundsMin = lo;
    model.boundsMax = hi;
    return true;
}

}  // namespace kimport
