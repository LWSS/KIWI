#pragma once

// KIWI model importer - reads a BUILT raw xmodel (v25: xmodel\<name> +
// xmodelsurfs\<lod0>) back into triangles, so collision can be made for models that
// have no recipe (anything already in raw\, whichever tool wrote it). Mirrors the
// engine loader: XModelLoadConfigFile / XModelLoadCollData / XModelReadSurface
// (src\xanim\xmodel_load_obj.cpp).

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kimport {

struct RawXModelSurface {
    std::string material;
    int surfaceType = 0;            // from raw\materials\<material> (0 when unreadable)
    std::uint32_t gameFlags = 0;
    std::string techniqueSet;
    std::uint32_t firstTriangle = 0;
    std::uint32_t triangleCount = 0;
};

struct RawXModel {
    std::string name;
    std::vector<std::array<float, 3>> positions;   // 3 per triangle, model space inches
    std::vector<std::uint32_t> triangleSurface;    // per triangle
    std::vector<RawXModelSurface> surfaces;        // LOD0
    bool multiBone = false;                         // positions are bone-local: not usable
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
};

bool ReadRawXModel(const std::filesystem::path& rawRoot, const std::string& name,
                   RawXModel& model, std::string& error);

}  // namespace kimport
