#include "ki_pieces.h"

#include "cod4_material.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace pieces {
namespace {

struct PositionKey {
    std::array<float, 3> value{};
    bool operator==(const PositionKey& other) const {
        return std::memcmp(value.data(), other.value.data(), sizeof(value)) == 0;
    }
};

struct PositionKeyHash {
    std::size_t operator()(const PositionKey& key) const {
        std::uint32_t bits[3];
        std::memcpy(bits, key.value.data(), sizeof(bits));
        std::size_t hash = bits[0];
        hash ^= bits[1] + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        hash ^= bits[2] + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

struct UnionFind {
    std::vector<std::uint32_t> parent;
    explicit UnionFind(std::size_t count) : parent(count) {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    std::uint32_t Find(std::uint32_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    }
    void Union(std::uint32_t a, std::uint32_t b) {
        a = Find(a);
        b = Find(b);
        if (a != b) parent[b] = a;
    }
};

std::string Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return std::string(text);
}

}  // namespace

PieceSet BuildPieces(const MeshInput& mesh) {
    PieceSet result;
    const std::size_t triangleCount = mesh.triangles.size();
    result.trianglePiece.assign(triangleCount, 0u);
    if (triangleCount == 0) return result;

    // Weld by exact position so painter re-exports with split UV seams still
    // form one piece; -0.0f and 0.0f differ bitwise, so normalise those.
    std::vector<std::uint32_t> weld(mesh.positions.size());
    {
        std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> map;
        map.reserve(mesh.positions.size());
        for (std::size_t index = 0; index < mesh.positions.size(); ++index) {
            PositionKey key{mesh.positions[index]};
            for (float& component : key.value)
                if (component == 0.0f) component = 0.0f;
            const auto [found, inserted] =
                map.try_emplace(key, static_cast<std::uint32_t>(map.size()));
            weld[index] = found->second;
        }
    }
    const std::uint32_t weldCount =
        weld.empty() ? 0u : *std::max_element(weld.begin(), weld.end()) + 1u;
    const std::size_t groupCount =
        std::max<std::size_t>(mesh.groupNames.size(), 1u);

    // Vertex keys are (group, welded position) so touching bodies stay apart.
    UnionFind sets(static_cast<std::size_t>(weldCount) * groupCount);
    auto key = [&](std::size_t triangle, int corner) {
        const std::uint32_t group =
            triangle < mesh.triangleGroup.size() ? mesh.triangleGroup[triangle] : 0u;
        return static_cast<std::uint32_t>(group * weldCount +
                                          weld[mesh.triangles[triangle][corner]]);
    };
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
        sets.Union(key(triangle, 0), key(triangle, 1));
        sets.Union(key(triangle, 0), key(triangle, 2));
    }

    // Number pieces in first-triangle order.
    std::unordered_map<std::uint32_t, std::uint32_t> rootToPiece;
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
        const std::uint32_t root = sets.Find(key(triangle, 0));
        const auto [found, inserted] = rootToPiece.try_emplace(
            root, static_cast<std::uint32_t>(result.pieces.size()));
        if (inserted) {
            PieceInfo info;
            const std::uint32_t group =
                triangle < mesh.triangleGroup.size() ? mesh.triangleGroup[triangle] : 0u;
            info.name = group < mesh.groupNames.size() ? mesh.groupNames[group] : "";
            info.boundsMin = {std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::max()};
            info.boundsMax = {-std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::max()};
            result.pieces.push_back(std::move(info));
        }
        const std::uint32_t piece = found->second;
        result.trianglePiece[triangle] = piece;
        PieceInfo& info = result.pieces[piece];
        ++info.triangleCount;
        for (int corner = 0; corner < 3; ++corner) {
            const std::array<float, 3>& p =
                mesh.positions[mesh.triangles[triangle][corner]];
            for (int axis = 0; axis < 3; ++axis) {
                info.boundsMin[axis] = std::min(info.boundsMin[axis], p[axis]);
                info.boundsMax[axis] = std::max(info.boundsMax[axis], p[axis]);
            }
        }
    }

    // Name: the group name, "#n" when one group produced several islands,
    // "piece n" when the OBJ carried no names at all.
    std::unordered_map<std::string, int> nameUses;
    for (const PieceInfo& info : result.pieces) ++nameUses[info.name];
    std::unordered_map<std::string, int> nameSeen;
    for (std::size_t index = 0; index < result.pieces.size(); ++index) {
        PieceInfo& info = result.pieces[index];
        const std::string base = info.name;
        const int ordinal = ++nameSeen[base];
        if (base.empty()) {
            info.name = "piece " + std::to_string(index + 1);
        } else if (nameUses[base] > 1) {
            info.name = base + " #" + std::to_string(ordinal);
        }
    }
    return result;
}

std::filesystem::path SidecarPath(const std::filesystem::path& objPath) {
    std::filesystem::path result = objPath;
    result += ".surfaces.txt";
    return result;
}

bool LoadAssignments(const std::filesystem::path& path, const PieceSet& pieces,
                     Assignments& out, std::vector<std::string>& warnings,
                     std::string& error) {
    out.pieceType.assign(pieces.pieces.size(), 0);
    std::error_code fsError;
    if (!std::filesystem::exists(path, fsError)) return true;
    std::ifstream input(path);
    if (!input) {
        error = "cannot open '" + path.string() + "'";
        return false;
    }
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;
        std::istringstream parser(trimmed);
        std::string record, typeName;
        long long index = -1, triangles = -1;
        if (!(parser >> record >> index >> triangles >> typeName) ||
            record != "piece") {
            warnings.push_back(path.filename().string() + " line " +
                               std::to_string(lineNumber) + ": unreadable, skipped");
            continue;
        }
        if (index < 0 || static_cast<std::size_t>(index) >= pieces.pieces.size()) {
            warnings.push_back(path.filename().string() + " line " +
                               std::to_string(lineNumber) + ": piece " +
                               std::to_string(index) + " does not exist any more, skipped");
            continue;
        }
        const PieceInfo& info = pieces.pieces[static_cast<std::size_t>(index)];
        if (triangles != static_cast<long long>(info.triangleCount)) {
            warnings.push_back(path.filename().string() + " line " +
                               std::to_string(lineNumber) + ": piece " +
                               std::to_string(index) + " (" + info.name + ") has " +
                               std::to_string(info.triangleCount) +
                               " triangles now, file says " +
                               std::to_string(triangles) +
                               "; the OBJ changed, entry skipped");
            continue;
        }
        int type = 0;
        if (typeName == "default" || typeName == "0") {
            type = 0;
        } else if (!cod4::ParseSurfaceType(typeName, type)) {
            warnings.push_back(path.filename().string() + " line " +
                               std::to_string(lineNumber) + ": unknown surface type '" +
                               typeName + "', skipped");
            continue;
        }
        out.pieceType[static_cast<std::size_t>(index)] = type;
    }
    return true;
}

bool SaveAssignments(const std::filesystem::path& path, const PieceSet& pieces,
                     const Assignments& assignments, std::string& error) {
    bool any = false;
    for (std::size_t index = 0;
         index < assignments.pieceType.size() && index < pieces.pieces.size(); ++index)
        any = any || assignments.pieceType[index] != 0;
    std::error_code fsError;
    if (!any) {
        std::filesystem::remove(path, fsError);
        if (fsError) {
            error = "could not remove '" + path.string() + "': " + fsError.message();
            return false;
        }
        return true;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "cannot write '" + path.string() + "'";
        return false;
    }
    output << "# obj2xmodel per-piece collision surface types\n"
              "# piece <index> <triangles> <surface>   # name\n"
              "# Pieces not listed follow the converter's (all) surface type.\n";
    for (std::size_t index = 0;
         index < assignments.pieceType.size() && index < pieces.pieces.size(); ++index) {
        const int type = assignments.pieceType[index];
        if (type == 0) continue;
        output << "piece " << index << ' ' << pieces.pieces[index].triangleCount << ' '
               << cod4::SurfaceTypeName(type) << "   # " << pieces.pieces[index].name
               << '\n';
    }
    output.flush();
    if (!output) {
        error = "failed while writing '" + path.string() + "'";
        return false;
    }
    return true;
}

std::vector<int> ResolveTriangleTypes(const PieceSet& pieces,
                                      const Assignments& assignments,
                                      int defaultType) {
    std::vector<int> result(pieces.trianglePiece.size(), defaultType);
    for (std::size_t triangle = 0; triangle < result.size(); ++triangle) {
        const std::uint32_t piece = pieces.trianglePiece[triangle];
        if (piece < assignments.pieceType.size() && assignments.pieceType[piece] != 0)
            result[triangle] = assignments.pieceType[piece];
    }
    return result;
}

}  // namespace pieces
