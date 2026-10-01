#pragma once

// Per-piece collision surface types for the KIWI model importer (from obj2xmodel).
//
// A CoD4 xmodel keys bullet impacts / penetration on the COLLISION SURFACE's
// surfFlags, not on the material, and one material may back any number of
// surfaces. So a single-material InstaMAT bake can still be "wood here, metal
// there": the triangles are grouped by surface type into separate surfaces
// sharing the one material, each with its own collision flags.
//
// A "piece" is a connected component of the OBJ (triangles welded by exact
// position), never merged across `o` / `g` records, so every Plasticity body
// comes out as its own piece even when two bodies touch. Pieces are numbered in
// first-triangle order, which is stable for an unchanged OBJ; the sidecar that
// stores the assignments also records each piece's triangle count so a stale
// file is detected instead of silently mislabelling.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pieces {

struct MeshInput {
    std::vector<std::array<float, 3>> positions;     // OBJ order, OBJ units
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::uint32_t> triangleGroup;        // index into groupNames
    std::vector<std::string> groupNames;             // "" when the OBJ had none
};

struct PieceInfo {
    std::string name;            // group name, "#n" suffixed when a group split
    std::uint32_t triangleCount = 0;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
};

struct PieceSet {
    std::vector<std::uint32_t> trianglePiece;   // aligned with MeshInput::triangles
    std::vector<PieceInfo> pieces;
};

PieceSet BuildPieces(const MeshInput& mesh);

// pieceType[i] is a cod4 surface type index (1..28) or 0 = "follow the (all)
// default". Sized to the piece count.
struct Assignments {
    std::vector<int> pieceType;
};

// <mesh.obj>.surfaces.txt beside the OBJ.
std::filesystem::path SidecarPath(const std::filesystem::path& objPath);

// Missing file: returns true with everything at default. Entries whose piece
// index or triangle count no longer match are reported in `warnings` and
// skipped. Unknown type names are warnings too.
bool LoadAssignments(const std::filesystem::path& path, const PieceSet& pieces,
                     Assignments& out, std::vector<std::string>& warnings,
                     std::string& error);

// Writes only the non-default entries; removes the file when there are none.
bool SaveAssignments(const std::filesystem::path& path, const PieceSet& pieces,
                     const Assignments& assignments, std::string& error);

// Effective per-triangle surface type (defaultType where the piece is 0).
std::vector<int> ResolveTriangleTypes(const PieceSet& pieces,
                                      const Assignments& assignments,
                                      int defaultType);

}  // namespace pieces
