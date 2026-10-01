// KIWI model importer - collision for already-built xmodels. See ki_autocollide.h.

#include "ki_autocollide.h"

#include "cod4_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace kimport {
namespace {

using P3 = std::array<double, 3>;

std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool NameHas(const std::string& lowerName, std::initializer_list<const char*> words) {
    for (const char* word : words)
        if (lowerName.find(word) != std::string::npos) return true;
    return false;
}

int TypeIndex(const char* name) {
    int index = 0;
    return cod4::ParseSurfaceType(name, index) ? index : -1;
}

bool IsFoliageType(int type) {
    return type > 0 && (type == TypeIndex("foliage") || type == TypeIndex("grass"));
}

bool IsWoodType(int type) {
    return type > 0 && (type == TypeIndex("wood") || type == TypeIndex("bark"));
}

double Percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t at = std::min(values.size() - 1,
                                    static_cast<std::size_t>(fraction * (values.size() - 1) + 0.5));
    return values[at];
}

struct Slice {
    double cx = 0.0;
    double cy = 0.0;
    double radius = 0.0;
    std::vector<P3> points;
};

// Points of `candidates` within `reach` (XY) of (cx, cy) -> a ring: centre, MEDIAN
// radius (a trunk ring's vertices all sit at one radius; branch stubs caught in the
// same slice are a minority and do not move the median), then only the points on
// that ring are kept, so stubs never reach the hull.
bool FitSlice(const std::vector<P3>& candidates, double cx, double cy, double reach,
              Slice& out) {
    out = {};
    std::vector<P3> near;
    for (const P3& p : candidates)
        if (std::hypot(p[0] - cx, p[1] - cy) <= reach) near.push_back(p);
    if (near.size() < 3) return false;
    // Centre on the median (robust to a stub on one side), then the ring radius.
    std::vector<double> xs;
    std::vector<double> ys;
    for (const P3& p : near) {
        xs.push_back(p[0]);
        ys.push_back(p[1]);
    }
    out.cx = Percentile(xs, 0.5);
    out.cy = Percentile(ys, 0.5);
    std::vector<double> distances;
    for (const P3& p : near) distances.push_back(std::hypot(p[0] - out.cx, p[1] - out.cy));
    out.radius = Percentile(distances, 0.5);
    const double keep = out.radius * 1.3 + 0.5;
    for (const P3& p : near)
        if (std::hypot(p[0] - out.cx, p[1] - out.cy) <= keep) out.points.push_back(p);
    return out.points.size() >= 3;
}

void TrunkHulls(const RawXModel& model, const CollisionSettings& settings,
                const TrunkSettings& trunk, std::vector<Hull>& hulls, std::string& note) {
    // Bark triangles welded into connected pieces. Generated trees (KIWI's pine/v100/big
    // generators, the foliage_kiwi set) build the trunk as ONE tube from the ground up and
    // every branch and root as its own tube (measured 2026-10-01 on pines, oaks, birches,
    // spruces, saplings, dead pines), so the trunk is simply the tallest piece that
    // starts at the ground - lean and curve come for free.
    pieces::MeshInput barkMesh;
    std::map<std::array<float, 3>, std::uint32_t> weld;
    for (std::size_t t = 0; t < model.triangleSurface.size(); ++t) {
        if (!IsWoodType(model.surfaces[model.triangleSurface[t]].surfaceType)) continue;
        std::array<std::uint32_t, 3> tri{};
        for (int c = 0; c < 3; ++c) {
            const auto& p = model.positions[t * 3 + c];
            const auto [it, inserted] =
                weld.try_emplace(p, static_cast<std::uint32_t>(barkMesh.positions.size()));
            if (inserted) barkMesh.positions.push_back(p);
            tri[c] = it->second;
        }
        barkMesh.triangles.push_back(tri);
        barkMesh.triangleGroup.push_back(0);
    }
    if (barkMesh.triangles.size() < 4) {
        note = "tree without bark: no collision";
        return;
    }
    barkMesh.groupNames.push_back("");
    const pieces::PieceSet barkPieces = pieces::BuildPieces(barkMesh);
    std::vector<std::vector<P3>> piecePoints(barkPieces.pieces.size());
    std::vector<P3> bark;
    for (std::size_t t = 0; t < barkMesh.triangles.size(); ++t)
        for (std::uint32_t v : barkMesh.triangles[t]) {
            const auto& p = barkMesh.positions[v];
            const P3 point{p[0], p[1], p[2]};
            piecePoints[barkPieces.trianglePiece[t]].push_back(point);
            bark.push_back(point);
        }
    auto dedupe = [](std::vector<P3>& points) {
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
    };
    dedupe(bark);
    for (std::vector<P3>& points : piecePoints) dedupe(points);
    double zmin = bark.front()[2];
    double zmax = zmin;
    for (const P3& p : bark) {
        zmin = std::min(zmin, p[2]);
        zmax = std::max(zmax, p[2]);
    }
    const double top = std::min(zmax, zmin + trunk.maxHeight);
    auto inBand = [&](double lo, double hi) {
        std::vector<P3> band;
        for (const P3& p : bark)
            if (p[2] >= lo && p[2] < hi) band.push_back(p);
        return band;
    };

    struct Ring {
        Slice slice;
        double z = 0.0;
    };
    std::vector<Ring> rings;
    std::vector<double> radii;

    // The trunk piece: among pieces starting within 6 in of the lowest bark, the tallest.
    int trunkPiece = -1;
    double tallest = 0.0;
    for (std::size_t piece = 0; piece < piecePoints.size(); ++piece) {
        if (piecePoints[piece].size() < 6) continue;
        double lo = piecePoints[piece].front()[2];
        double hi = lo;
        for (const P3& p : piecePoints[piece]) {
            lo = std::min(lo, p[2]);
            hi = std::max(hi, p[2]);
        }
        if (lo <= zmin + 6.0 && hi - lo > tallest) {
            tallest = hi - lo;
            trunkPiece = static_cast<int>(piece);
        }
    }
    bool fromPiece = false;
    if (trunkPiece >= 0) {
        // Its vertices clustered by height are the rings (a gap over 1.5 in starts a
        // new ring); one ring past the cap so the top section reaches it.
        std::vector<P3> trunkPoints = piecePoints[trunkPiece];
        std::sort(trunkPoints.begin(), trunkPoints.end(),
                  [](const P3& a, const P3& b) { return a[2] < b[2]; });
        std::vector<P3> cluster;
        auto flush = [&]() {
            Slice slice;
            if (cluster.size() >= 3) {
                double cx = 0.0;
                double cy = 0.0;
                for (const P3& p : cluster) {
                    cx += p[0];
                    cy += p[1];
                }
                cx /= static_cast<double>(cluster.size());
                cy /= static_cast<double>(cluster.size());
                if (FitSlice(cluster, cx, cy, 1e9, slice)) rings.push_back({slice, cluster.front()[2]});
            }
            cluster.clear();
        };
        for (const P3& p : trunkPoints) {
            if (!cluster.empty() && p[2] - cluster.back()[2] > 1.5) {
                flush();
                if (!rings.empty() && rings.back().z > top) break;
            }
            cluster.push_back(p);
        }
        if (rings.empty() || rings.back().z <= top) flush();
        for (std::size_t i = 1; i < rings.size(); ++i) radii.push_back(rings[i].slice.radius);
        if (radii.empty() && !rings.empty()) radii.push_back(rings.front().slice.radius);
        // A ring far wider than the stem means branches are welded into this piece:
        // fall back to tracking the stem through all the bark.
        const double stem = Percentile(radii, 0.5);
        bool welded = false;
        for (std::size_t i = 1; i < rings.size(); ++i)
            welded |= rings[i].slice.radius > std::max(stem * 3.0, stem + 24.0);
        fromPiece = !welded && rings.size() >= 2;
        if (!fromPiece) {
            rings.clear();
            radii.clear();
        }
    }

    // Fallback tracker. Low-poly trunks are cylinders with vertices only at ring heights
    // and nothing in between, so it walks from ring to ring (empty slices are skipped,
    // not an end). The base ring is centred on its own points; every later ring keeps
    // only the bark near the predicted centre (branch stubs drop out), and a ring that
    // balloons (a whorl, a fork) is re-fit to the stem's own width.
    if (!fromPiece)
    for (double z = zmin; z < top; z += trunk.sliceHeight) {
        const std::vector<P3> band = inBand(z, z + trunk.sliceHeight);
        if (band.size() < 3) continue;
        Slice slice;
        if (rings.empty()) {
            double cx = 0.0;
            double cy = 0.0;
            for (const P3& p : band) {
                cx += p[0];
                cy += p[1];
            }
            cx /= static_cast<double>(band.size());
            cy /= static_cast<double>(band.size());
            if (!FitSlice(band, cx, cy, 1e9, slice)) continue;
        } else {
            const Ring& prev = rings.back();
            // A long bare gap between rings is still trunk; past ~13 ft of nothing the
            // stem is gone (a snag top, a crown with no bark rings).
            if (z - prev.z > 160.0) break;
            // The stem's own width so far (low percentile: whorl rings run wide).
            const double stem = Percentile(radii, 0.3);
            // Predict the centre by carrying the lean of the last two rings forward:
            // sparse rings on a leaning trunk sit far apart sideways, branches do not
            // follow the lean.
            const double dz = z - prev.z;
            double px = prev.slice.cx;
            double py = prev.slice.cy;
            bool haveLean = false;
            if (rings.size() >= 2) {
                const Ring& before = rings[rings.size() - 2];
                const double span = prev.z - before.z;
                if (span > 1e-6) {
                    px += (prev.slice.cx - before.slice.cx) / span * dz;
                    py += (prev.slice.cy - before.slice.cy) / span * dz;
                    haveLean = true;
                }
            }
            // Trunks curve (lean grows with height), so the prediction gets slack.
            const double leanSlack = dz * (haveLean ? 0.15 : 0.35);
            if (!FitSlice(band, px, py, prev.slice.radius * 1.25 + 3.0 + leanSlack, slice))
                continue;   // only branch bark in this slice: keep climbing
            if (slice.radius > std::max(stem * 1.8, stem + 6.0) &&
                (!FitSlice(band, px, py, stem * 1.3 + 1.0, slice) ||
                 slice.radius > std::max(stem * 1.8, stem + 6.0)))
                continue;   // a whorl or fork, not the stem
            // A trunk neither jumps sideways off its lean nor halves between rings; a
            // branch stub's ring does both. Skipping it keeps the tracker on the stem.
            if (std::hypot(slice.cx - px, slice.cy - py) >
                    std::max(std::min(prev.slice.radius * 0.5, 8.0), 2.0) + leanSlack ||
                slice.radius < prev.slice.radius * 0.5)
                continue;
        }
        rings.push_back({slice, z});
        if (rings.size() > 1) radii.push_back(slice.radius);   // the base flare is not the stem
        if (radii.empty()) radii.push_back(slice.radius);
    }
    if (rings.empty()) {
        note = "tree: no trunk found";
        return;
    }

    // Sections of consecutive rings spanning about sectionHeight, each sharing its
    // first ring with the previous section's last so the stack has no gaps. A section
    // is the convex hull of its rings: the trunk frustum, following any lean.
    CollisionSettings round = settings;
    round.shape = "kdop";
    std::size_t thin = 0;
    std::size_t start = 0;
    while (start + 1 < rings.size() || (rings.size() == 1 && start == 0)) {
        std::size_t end = start + 1;
        while (end + 1 < rings.size() && rings[end].z - rings[start].z < trunk.sectionHeight) ++end;
        if (end >= rings.size()) end = rings.size() - 1;
        std::vector<P3> points;
        double diameter = 0.0;
        for (std::size_t i = start; i <= end; ++i) {
            points.insert(points.end(), rings[i].slice.points.begin(), rings[i].slice.points.end());
            diameter = std::max(diameter, rings[i].slice.radius * 2.0);
        }
        if (diameter < trunk.minDiameter || points.size() < 6) {
            ++thin;
        } else {
            Hull hull = HullAroundPoints(points, round);
            if (hull.planes.size() >= 4) {
                hull.kind = "trunk";
                hulls.push_back(std::move(hull));
            }
        }
        if (end == start) break;
        start = end;
    }
    char text[160];
    std::snprintf(text, sizeof(text),
                  "tree trunk (%s): %zu sections, %zu rings over %.0f in, stem %.1f in wide%s",
                  fromPiece ? "trunk piece" : "tracked", hulls.size(), rings.size(),
                  rings.back().z - rings.front().z, Percentile(radii, 0.5) * 2.0,
                  thin ? " (thin parts skipped)" : "");
    note = hulls.empty() ? "tree: trunk thinner than the minimum, no collision" : text;
}

void PropHulls(const RawXModel& model, const CollisionSettings& settings,
               std::vector<Hull>& hulls, std::string& note, const LogFn& log) {
    // Weld the triangle soup so pieces are connected components, as for a source.
    pieces::MeshInput input;
    std::map<std::array<float, 3>, std::uint32_t> weld;
    for (std::size_t t = 0; t < model.triangleSurface.size(); ++t) {
        std::array<std::uint32_t, 3> tri{};
        for (int c = 0; c < 3; ++c) {
            const auto& p = model.positions[t * 3 + c];
            const auto [it, inserted] = weld.try_emplace(p, static_cast<std::uint32_t>(input.positions.size()));
            if (inserted) input.positions.push_back(p);
            tri[c] = it->second;
        }
        input.triangles.push_back(tri);
        input.triangleGroup.push_back(0);
    }
    input.groupNames.push_back("");
    const pieces::PieceSet pieceSet = pieces::BuildPieces(input);
    ModelSpaceMesh mesh;
    mesh.positions = model.positions;
    mesh.trianglePiece = pieceSet.trianglePiece;
    GenerateHulls(mesh, pieceSet, settings, hulls, log);
    note = "prop: " + std::to_string(hulls.size()) + " hulls from " +
           std::to_string(pieceSet.pieces.size()) + " pieces";
}

}  // namespace

const char* AutoClassName(AutoClass value) {
    switch (value) {
    case AutoClass::Tree: return "tree";
    case AutoClass::Vegetation: return "vegetation";
    default: return "prop";
    }
}

AutoClass ClassifyXModel(const RawXModel& model) {
    const std::string name = Lower(model.name);
    bool foliage = false;
    bool wood = false;
    for (const RawXModelSurface& surface : model.surfaces) {
        foliage |= IsFoliageType(surface.surfaceType);
        wood |= IsWoodType(surface.surfaceType);
    }
    // Plants that are never trees, whatever their surfaces say.
    if (NameHas(name, {"grass", "herb", "scrub", "bush", "bough", "ground", "fern", "weed",
                       "ivy", "shrub", "flower", "leaf", "moss", "reed", "plant", "hedge"}))
        return AutoClass::Vegetation;
    const bool treeName =
        NameHas(name, {"tree", "pine", "spruce", "fir_", "_fir", "oak", "birch", "snag",
                       "sapling", "maple", "willow", "elm_", "cedar", "palm", "poplar", "beech",
                       "larch", "trunk"});
    if (wood && (treeName || foliage)) return AutoClass::Tree;
    if (foliage) return AutoClass::Vegetation;
    return AutoClass::Prop;
}

void AutoHullsForXModel(const RawXModel& model, AutoClass cls, const CollisionSettings& settings,
                        const TrunkSettings& trunk, std::vector<Hull>& hulls, std::string& note,
                        const LogFn& log) {
    hulls.clear();
    if (cls == AutoClass::Vegetation) {
        note = "vegetation: no collision";
        return;
    }
    if (cls == AutoClass::Tree) {
        TrunkHulls(model, settings, trunk, hulls, note);
        return;
    }
    PropHulls(model, settings, hulls, note, log);
}

}  // namespace kimport
