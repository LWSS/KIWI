#pragma once

// KIWI model importer - player collision hulls.
//
// CoD4 xmodel collision (the collision LOD obj2xmodel bakes) is hit only by point
// traces: bullets. Players, AI and vehicles collide with BRUSHES. IW hand-built
// clip-brush hulls per model (`collmaps\<model>.map`) and cod4map stamps them on
// every placed misc_model when run with -staticModelCollMaps (map.cpp:1664). This
// module builds those hulls automatically - one convex brush per mesh piece - and
// writes the collmap. Hulls are plane sets in MODEL space (inches, origin rebased),
// so the editor, the recipe and every texture variant share them.

#include "ki_convert.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace kimport {

using HullPlane = std::array<double, 4>;   // nx ny nz d; inside where n.p <= d

struct Hull {
    int piece = -1;              // source piece, -1 = added by hand
    std::string kind = "box";    // box | kdop | manual
    std::vector<HullPlane> planes;
};

struct CollisionSettings {
    std::string shape = "auto";  // auto | box | kdop
    double minPieceSize = 2.0;   // inches: pieces whose largest extent is smaller get no hull
    double minThickness = 1.0;   // inches: thinner hulls are thickened about their middle
    double kdopRatio = 0.85;     // auto picks the k-DOP when its volume < ratio * box volume
    // Concave pieces (an L-bent pipe) are split by a plane while that shrinks the
    // summed box volume below splitGain * the whole; maxDepth bounds the recursion
    // (at most 2^maxDepth hulls per piece).
    double splitGain = 0.7;
    int maxDepth = 4;
    // KIWI (2026-10-22): one clip box around the whole model instead of per-piece hulls.
    bool wholeBox = false;
};

// One hull per piece (pieces small, or inside another piece's hull, are skipped).
void GenerateHulls(const ModelSpaceMesh& mesh, const pieces::PieceSet& pieceSet,
                   const CollisionSettings& settings, std::vector<Hull>& hulls,
                   const LogFn& log);

// Face polygons of a hull (one per plane; empty for a redundant plane).
std::vector<std::vector<std::array<double, 3>>> HullWindings(const Hull& hull);

double HullVolume(const Hull& hull);

// Drops planes that do not bound the hull. False when the hull is empty/unbounded.
bool CleanHull(Hull& hull);

// Translates hull planes by `delta` inches (p' = p + delta).
void TranslateHull(Hull& hull, const std::array<double, 3>& delta);

// One convex hull around a point cloud (box, or 26-DOP per settings.shape / auto).
Hull HullAroundPoints(const std::vector<std::array<double, 3>>& points,
                      const CollisionSettings& settings);

// The first-line marker of every collmap this tool writes. A collmap without it was
// hand-made (Radiant re-save, IW's vehicles) and batch tools must not overwrite it.
constexpr const char* kGeneratedCollmapMarker = "// KIWI-modelimport generated";

// Writes `collmaps\<model>.map`: one brush per hull in `material`. `note` goes on
// the marker line (e.g. "auto prop", "auto tree trunk").
bool WriteCollmap(const std::filesystem::path& path, const std::vector<Hull>& hulls,
                  const std::string& material, std::string& error,
                  const std::string& note = "auto");

// True when the file exists and carries kGeneratedCollmapMarker.
bool IsGeneratedCollmap(const std::filesystem::path& path);

// Reads every brush of a collmap (any entity) back as hulls (kind "manual"); the
// material of the first face is returned. Brushes hand-edited in Radiant come
// back exactly as cod4map will see them.
bool ReadCollmap(const std::filesystem::path& path, std::vector<Hull>& hulls,
                 std::string& material, std::string& error);

}  // namespace kimport
