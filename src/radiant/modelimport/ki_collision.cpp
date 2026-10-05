// KIWI model importer - player collision hulls. See ki_collision.h.

#include "ki_collision.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numeric>

namespace kimport {
namespace {

namespace fs = std::filesystem;

using P3 = std::array<double, 3>;

P3 Add(const P3& a, const P3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
P3 Sub(const P3& a, const P3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
P3 Mul(const P3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double Dot3(const P3& a, const P3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
P3 Cross3(const P3& a, const P3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Len3(const P3& a) { return std::sqrt(Dot3(a, a)); }
P3 Unit(const P3& a) {
    const double l = Len3(a);
    return l > 1e-12 ? Mul(a, 1.0 / l) : P3{0.0, 0.0, 1.0};
}

constexpr double kClipEpsilon = 1e-6;
constexpr double kHuge = 262144.0;   // larger than any model, inches

std::vector<P3> BaseWinding(const P3& n, double d) {
    P3 up{0.0, 0.0, 1.0};
    if (std::abs(n[2]) > std::abs(n[0]) && std::abs(n[2]) > std::abs(n[1])) up = {1.0, 0.0, 0.0};
    P3 vup = Unit(Sub(up, Mul(n, Dot3(up, n))));
    P3 vright = Cross3(vup, n);
    const P3 org = Mul(n, d);
    vup = Mul(vup, kHuge);
    vright = Mul(vright, kHuge);
    return {Add(Sub(org, vright), vup), Add(Add(org, vright), vup),
            Sub(Add(org, vright), vup), Sub(Sub(org, vright), vup)};
}

// Keeps the part of `w` where n.p <= d.
std::vector<P3> ClipBack(const std::vector<P3>& w, const P3& n, double d) {
    std::vector<P3> out;
    if (w.empty()) return out;
    const std::size_t count = w.size();
    std::vector<double> dist(count);
    for (std::size_t i = 0; i < count; ++i) dist[i] = Dot3(w[i], n) - d;
    for (std::size_t i = 0; i < count; ++i) {
        const P3& a = w[i];
        const P3& b = w[(i + 1) % count];
        const double da = dist[i];
        const double db = dist[(i + 1) % count];
        if (da <= kClipEpsilon) out.push_back(a);
        if ((da > kClipEpsilon && db < -kClipEpsilon) || (da < -kClipEpsilon && db > kClipEpsilon)) {
            const double t = da / (da - db);
            out.push_back(Add(a, Mul(Sub(b, a), t)));
        }
    }
    return out.size() >= 3 ? out : std::vector<P3>{};
}

P3 PlaneNormal(const HullPlane& plane) { return {plane[0], plane[1], plane[2]}; }

// Symmetric 3x3 Jacobi eigen-decomposition; columns of `vectors` = eigenvectors.
void Jacobi3(double a[3][3], double vectors[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) vectors[i][j] = i == j ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 32; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-20) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-30) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) /
                                 (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = vectors[k][p];
                    const double vkq = vectors[k][q];
                    vectors[k][p] = c * vkp - s * vkq;
                    vectors[k][q] = s * vkp + c * vkq;
                }
            }
    }
}

struct Frame {
    std::array<P3, 3> axis{P3{1, 0, 0}, P3{0, 1, 0}, P3{0, 0, 1}};
    bool zUp = true;
};

struct Extents {
    P3 lo{};
    P3 hi{};
    double Volume() const {
        return std::max(hi[0] - lo[0], 1e-3) * std::max(hi[1] - lo[1], 1e-3) *
               std::max(hi[2] - lo[2], 1e-3);
    }
};

Extents MeasureIn(const std::vector<P3>& points, const Frame& frame) {
    Extents e;
    for (int a = 0; a < 3; ++a) {
        e.lo[a] = std::numeric_limits<double>::max();
        e.hi[a] = -std::numeric_limits<double>::max();
    }
    for (const P3& p : points)
        for (int a = 0; a < 3; ++a) {
            const double v = Dot3(p, frame.axis[a]);
            e.lo[a] = std::min(e.lo[a], v);
            e.hi[a] = std::max(e.hi[a], v);
        }
    return e;
}

Frame YawFrame(double degrees) {
    const double r = degrees * 3.14159265358979323846 / 180.0;
    Frame f;
    f.axis = {P3{std::cos(r), std::sin(r), 0.0}, P3{-std::sin(r), std::cos(r), 0.0},
              P3{0.0, 0.0, 1.0}};
    f.zUp = true;
    return f;
}

// The frame whose box is smallest: world axes, z-up yaw sweep (1 degree, then
// 0.05 refinement), and the PCA axes. A z-up frame within 2% of the best wins,
// so props stay upright-aligned instead of tilting by noise.
Frame BestFrame(const std::vector<P3>& points, bool coarse = false) {
    Frame bestUp = YawFrame(0.0);
    double bestUpVolume = MeasureIn(points, bestUp).Volume();
    double bestYaw = 0.0;
    for (int deg = 1; deg < 90; deg += coarse ? 5 : 1) {
        const Frame f = YawFrame(deg);
        const double v = MeasureIn(points, f).Volume();
        if (v < bestUpVolume) {
            bestUpVolume = v;
            bestUp = f;
            bestYaw = deg;
        }
    }
    for (double deg = bestYaw - 1.0; deg <= bestYaw + 1.0 && !coarse; deg += 0.05) {
        const Frame f = YawFrame(deg);
        const double v = MeasureIn(points, f).Volume();
        if (v < bestUpVolume) {
            bestUpVolume = v;
            bestUp = f;
        }
    }

    // PCA
    P3 mean{};
    for (const P3& p : points) mean = Add(mean, p);
    mean = Mul(mean, 1.0 / std::max<std::size_t>(points.size(), 1));
    double cov[3][3] = {};
    for (const P3& p : points) {
        const P3 d = Sub(p, mean);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov[i][j] += d[i] * d[j];
    }
    double vectors[3][3];
    Jacobi3(cov, vectors);
    Frame pca;
    for (int a = 0; a < 3; ++a) pca.axis[a] = Unit(P3{vectors[0][a], vectors[1][a], vectors[2][a]});
    pca.axis[2] = Unit(Cross3(pca.axis[0], pca.axis[1]));
    pca.axis[1] = Unit(Cross3(pca.axis[2], pca.axis[0]));
    pca.zUp = false;
    const double pcaVolume = MeasureIn(points, pca).Volume();
    return pcaVolume * 1.02 < bestUpVolume ? pca : bestUp;
}

Hull BoxHull(const std::vector<P3>& points, const Frame& frame, double minThickness) {
    Extents e = MeasureIn(points, frame);
    for (int a = 0; a < 3; ++a) {
        const double size = e.hi[a] - e.lo[a];
        if (size < minThickness) {
            const double mid = (e.hi[a] + e.lo[a]) * 0.5;
            e.lo[a] = mid - minThickness * 0.5;
            e.hi[a] = mid + minThickness * 0.5;
        }
    }
    Hull hull;
    hull.kind = "box";
    for (int a = 0; a < 3; ++a) {
        const P3& n = frame.axis[a];
        hull.planes.push_back({n[0], n[1], n[2], e.hi[a]});
        hull.planes.push_back({-n[0], -n[1], -n[2], -e.lo[a]});
    }
    return hull;
}

// 26-DOP in the frame: 3 axes, 6 edge diagonals, 4 corner diagonals, both signs.
Hull KdopHull(const std::vector<P3>& points, const Frame& frame) {
    const P3& a = frame.axis[0];
    const P3& b = frame.axis[1];
    const P3& c = frame.axis[2];
    const std::array<P3, 13> directions{
        a, b, c,
        Unit(Add(a, b)), Unit(Sub(a, b)), Unit(Add(a, c)), Unit(Sub(a, c)),
        Unit(Add(b, c)), Unit(Sub(b, c)),
        Unit(Add(Add(a, b), c)), Unit(Sub(Add(a, b), c)), Unit(Add(Sub(a, b), c)),
        Unit(Sub(Sub(a, b), c))};
    Hull hull;
    hull.kind = "kdop";
    for (const P3& dir : directions) {
        for (double sign : {1.0, -1.0}) {
            const P3 n = Mul(dir, sign);
            double d = -std::numeric_limits<double>::max();
            for (const P3& p : points) d = std::max(d, Dot3(p, n));
            hull.planes.push_back({n[0], n[1], n[2], d});
        }
    }
    CleanHull(hull);
    return hull;
}

bool Inside(const Hull& hull, const P3& p, double epsilon) {
    for (const HullPlane& plane : hull.planes)
        if (Dot3(PlaneNormal(plane), p) > plane[3] + epsilon) return false;
    return true;
}

// The fewest decimals that read back (atof, as cod4map / Radiant) as the same float - "%.4f"
// re-rounded a hand-edited master's plane points every time its variants were written.
std::string FormatNumber(double value) {
    if (std::abs(value) < 1e-6) value = 0.0;   // hull arithmetic noise
    const float f = static_cast<float>(value);
    char buffer[64];
    for (int decimals = 1; decimals <= 12; ++decimals) {
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(f));
        if (static_cast<float>(std::strtod(buffer, nullptr)) == f) break;
    }
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text == "-0") text = "0";
    return text;
}

}  // namespace

std::vector<std::vector<std::array<double, 3>>> HullWindings(const Hull& hull) {
    std::vector<std::vector<P3>> faces(hull.planes.size());
    for (std::size_t i = 0; i < hull.planes.size(); ++i) {
        std::vector<P3> w = BaseWinding(PlaneNormal(hull.planes[i]), hull.planes[i][3]);
        for (std::size_t j = 0; j < hull.planes.size() && !w.empty(); ++j) {
            if (j == i) continue;
            w = ClipBack(w, PlaneNormal(hull.planes[j]), hull.planes[j][3]);
        }
        faces[i] = std::move(w);
    }
    return faces;
}

double HullVolume(const Hull& hull) {
    const auto faces = HullWindings(hull);
    P3 centre{};
    std::size_t count = 0;
    for (const auto& face : faces)
        for (const P3& p : face) {
            centre = Add(centre, p);
            ++count;
        }
    if (count == 0) return 0.0;
    centre = Mul(centre, 1.0 / static_cast<double>(count));
    double volume = 0.0;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto& face = faces[i];
        if (face.size() < 3) continue;
        P3 area{};
        for (std::size_t k = 1; k + 1 < face.size(); ++k)
            area = Add(area, Cross3(Sub(face[k], face[0]), Sub(face[k + 1], face[0])));
        const double faceArea = 0.5 * Len3(area);
        const double height = hull.planes[i][3] - Dot3(PlaneNormal(hull.planes[i]), centre);
        volume += faceArea * height / 3.0;
    }
    return volume;
}

bool CleanHull(Hull& hull) {
    const auto faces = HullWindings(hull);
    std::vector<HullPlane> kept;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (faces[i].size() < 3) continue;
        bool duplicate = false;
        for (const HullPlane& k : kept)
            duplicate |= Dot3(PlaneNormal(k), PlaneNormal(hull.planes[i])) > 0.99999 &&
                         std::abs(k[3] - hull.planes[i][3]) < 1e-4;
        if (!duplicate) kept.push_back(hull.planes[i]);
    }
    hull.planes = std::move(kept);
    // Sliver faces (two k-DOP planes grazing one corner) are a fraction of a square
    // inch; written as three rounded points they turn collinear and the brush
    // breaks. Dropping one grows the hull by less than the sliver itself.
    constexpr double kMinFaceArea = 0.25;   // square inches
    for (;;) {
        const auto current = HullWindings(hull);
        std::size_t smallest = current.size();
        double smallestArea = kMinFaceArea;
        for (std::size_t i = 0; i < current.size(); ++i) {
            const auto& face = current[i];
            double area = 0.0;
            if (face.size() >= 3) {
                P3 sum{};
                for (std::size_t k = 1; k + 1 < face.size(); ++k)
                    sum = Add(sum, Cross3(Sub(face[k], face[0]), Sub(face[k + 1], face[0])));
                area = 0.5 * Len3(sum);
            }
            if (area < smallestArea) {
                smallestArea = area;
                smallest = i;
            }
        }
        if (smallest == current.size() || hull.planes.size() <= 4) break;
        hull.planes.erase(hull.planes.begin() + static_cast<std::ptrdiff_t>(smallest));
    }
    if (hull.planes.size() < 4) return false;
    // Unbounded: some face still reaches the base-winding size.
    for (const auto& face : HullWindings(hull))
        for (const P3& p : face)
            if (std::abs(p[0]) > kHuge * 0.5 || std::abs(p[1]) > kHuge * 0.5 ||
                std::abs(p[2]) > kHuge * 0.5)
                return false;
    return true;
}

void TranslateHull(Hull& hull, const std::array<double, 3>& delta) {
    for (HullPlane& plane : hull.planes) plane[3] += Dot3(PlaneNormal(plane), delta);
}

namespace {

using Triangle = std::array<P3, 3>;

std::vector<P3> UniquePoints(const std::vector<Triangle>& triangles) {
    std::vector<P3> points;
    points.reserve(triangles.size() * 3u);
    for (const Triangle& t : triangles) points.insert(points.end(), t.begin(), t.end());
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

// One convex hull for a set of points: the tightest box, or the 26-DOP when it is
// clearly tighter (round pipes), thin hulls thickened to minThickness.
Hull FitHull(const std::vector<P3>& points, const CollisionSettings& settings) {
    const Frame frame = BestFrame(points);
    Hull box = BoxHull(points, frame, settings.minThickness);
    if (settings.shape == "box") return box;
    const Extents e = MeasureIn(points, frame);
    const bool thin = (e.hi[0] - e.lo[0]) < settings.minThickness ||
                      (e.hi[1] - e.lo[1]) < settings.minThickness ||
                      (e.hi[2] - e.lo[2]) < settings.minThickness;
    if (thin) return box;
    Hull kdop = KdopHull(points, frame);
    const double kv = HullVolume(kdop);
    const double bv = HullVolume(box);
    if (kv > 0.0 && (settings.shape == "kdop" || kv < settings.kdopRatio * bv)) return kdop;
    return box;
}

struct HullCandidate {
    Hull hull;
    std::vector<P3> points;
};

// Greedy convex decomposition: split the triangle set (by centroid) with the axis
// plane that most reduces the summed box volume of the two halves; recurse while
// the gain is worth it. Splitting whole triangles keeps it simple and leaves the
// halves overlapping a little at the cut, which is what collision wants.
void FitRecursive(const std::vector<Triangle>& triangles, const CollisionSettings& settings,
                  int depth, std::vector<HullCandidate>& out) {
    const std::vector<P3> points = UniquePoints(triangles);
    if (points.size() < 4) return;
    const Frame frame = BestFrame(points);
    const Extents whole = MeasureIn(points, frame);
    const double wholeVolume = whole.Volume();
    double largest = 0.0;
    for (int a = 0; a < 3; ++a) largest = std::max(largest, whole.hi[a] - whole.lo[a]);

    if (depth < settings.maxDepth && largest > 2.0 * settings.minPieceSize &&
        triangles.size() >= 4) {
        std::vector<P3> centroids(triangles.size());
        for (std::size_t i = 0; i < triangles.size(); ++i)
            centroids[i] = Mul(Add(Add(triangles[i][0], triangles[i][1]), triangles[i][2]),
                               1.0 / 3.0);
        double bestSum = wholeVolume * settings.splitGain;
        P3 bestAxis{};
        double bestCut = 0.0;
        bool found = false;
        std::array<P3, 6> axes{frame.axis[0], frame.axis[1], frame.axis[2],
                               P3{1, 0, 0}, P3{0, 1, 0}, P3{0, 0, 1}};
        for (const P3& axis : axes) {
            std::vector<double> keys(centroids.size());
            for (std::size_t i = 0; i < centroids.size(); ++i) keys[i] = Dot3(centroids[i], axis);
            std::vector<double> sorted = keys;
            std::sort(sorted.begin(), sorted.end());
            for (int q = 1; q < 16; ++q) {
                const double cut = sorted[sorted.size() * q / 16];
                std::vector<P3> left;
                std::vector<P3> right;
                for (std::size_t i = 0; i < triangles.size(); ++i) {
                    auto& side = keys[i] < cut ? left : right;
                    side.insert(side.end(), triangles[i].begin(), triangles[i].end());
                }
                if (left.size() < 9 || right.size() < 9) continue;
                // Each half in its own best frame: a bent gutter's halves only
                // look tight once each is measured along its own run.
                const double sum = MeasureIn(left, BestFrame(left, true)).Volume() +
                                   MeasureIn(right, BestFrame(right, true)).Volume();
                if (sum < bestSum) {
                    bestSum = sum;
                    bestAxis = axis;
                    bestCut = cut;
                    found = true;
                }
            }
        }
        if (found) {
            std::vector<Triangle> left;
            std::vector<Triangle> right;
            for (std::size_t i = 0; i < triangles.size(); ++i)
                (Dot3(centroids[i], bestAxis) < bestCut ? left : right).push_back(triangles[i]);
            FitRecursive(left, settings, depth + 1, out);
            FitRecursive(right, settings, depth + 1, out);
            return;
        }
    }
    HullCandidate candidate;
    candidate.hull = FitHull(points, settings);
    if (!CleanHull(candidate.hull)) return;
    candidate.points = points;
    out.push_back(std::move(candidate));
}

}  // namespace

void GenerateHulls(const ModelSpaceMesh& mesh, const pieces::PieceSet& pieceSet,
                   const CollisionSettings& settings, std::vector<Hull>& hulls,
                   const LogFn& log) {
    hulls.clear();
    std::vector<std::vector<Triangle>> pieceTriangles(pieceSet.pieces.size());
    for (std::size_t t = 0; t < mesh.trianglePiece.size(); ++t) {
        const std::uint32_t piece = mesh.trianglePiece[t];
        if (piece >= pieceTriangles.size()) continue;
        Triangle triangle;
        for (int c = 0; c < 3; ++c) {
            const auto& p = mesh.positions[t * 3 + c];
            triangle[c] = {p[0], p[1], p[2]};
        }
        pieceTriangles[piece].push_back(triangle);
    }
    std::size_t small = 0;
    std::size_t splitPieces = 0;
    std::vector<HullCandidate> candidates;
    for (std::size_t piece = 0; piece < pieceTriangles.size(); ++piece) {
        const std::vector<Triangle>& triangles = pieceTriangles[piece];
        if (triangles.empty()) continue;
        P3 lo = triangles.front()[0];
        P3 hi = lo;
        for (const Triangle& t : triangles)
            for (const P3& p : t)
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], p[a]);
                    hi[a] = std::max(hi[a], p[a]);
                }
        const double largest = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        if (largest < settings.minPieceSize) {
            ++small;
            continue;
        }
        const std::size_t before = candidates.size();
        FitRecursive(triangles, settings, 0, candidates);
        for (std::size_t i = before; i < candidates.size(); ++i)
            candidates[i].hull.piece = static_cast<int>(piece);
        splitPieces += candidates.size() - before > 1u;
    }
    for (HullCandidate& candidate : candidates) hulls.push_back(candidate.hull);

    // Hulls wholly inside a bigger hull (bolts on a bracket, inner liners) add
    // nothing a player can touch.
    std::vector<std::size_t> order(hulls.size());
    std::iota(order.begin(), order.end(), 0u);
    std::vector<double> volume(hulls.size());
    for (std::size_t i = 0; i < hulls.size(); ++i) volume[i] = HullVolume(hulls[i]);
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return volume[a] > volume[b]; });
    std::vector<bool> drop(hulls.size(), false);
    std::size_t contained = 0;
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        const std::size_t i = order[oi];
        const std::vector<P3>& points = candidates[i].points;
        for (std::size_t oj = 0; oj < oi; ++oj) {
            const std::size_t j = order[oj];
            if (drop[j]) continue;
            bool all = true;
            for (const P3& p : points)
                if (!Inside(hulls[j], p, 0.25)) {
                    all = false;
                    break;
                }
            if (all) {
                drop[i] = true;
                ++contained;
                break;
            }
        }
    }
    std::vector<Hull> kept;
    for (std::size_t i = 0; i < hulls.size(); ++i)
        if (!drop[i]) kept.push_back(std::move(hulls[i]));
    hulls = std::move(kept);
    if (log) {
        std::size_t boxes = 0;
        for (const Hull& h : hulls) boxes += h.kind == "box";
        log("Collision: " + std::to_string(hulls.size()) + " hulls (" +
            std::to_string(boxes) + " box, " + std::to_string(hulls.size() - boxes) +
            " k-DOP) from " + std::to_string(pieceSet.pieces.size()) + " pieces (" +
            std::to_string(splitPieces) + " concave, split); " +
            std::to_string(small) + " under " + FormatNumber(settings.minPieceSize) +
            " in skipped, " + std::to_string(contained) + " inside another hull");
    }
}

Hull HullAroundPoints(const std::vector<std::array<double, 3>>& points,
                      const CollisionSettings& settings) {
    Hull hull = FitHull(points, settings);
    CleanHull(hull);
    return hull;
}

bool IsGeneratedCollmap(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    for (int i = 0; i < 3 && std::getline(in, line); ++i)
        if (line.rfind(kGeneratedCollmapMarker, 0) == 0) return true;
    return false;
}

bool WriteCollmap(const fs::path& path, const std::vector<Hull>& hulls,
                  const std::string& material, std::string& error, const std::string& note) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    out << "iwmap 4\n" << kGeneratedCollmapMarker << " (" << note
        << "); a Radiant re-save drops this line and marks the hulls hand-edited\n"
        << "// entity 0\n{\n\"classname\" \"worldspawn\"\n";
    int brushIndex = 0;
    for (const Hull& hull : hulls) {
        const auto faces = HullWindings(hull);
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const auto& w = faces[i];
            if (w.size() < 3) continue;
            // Three well-spread points: w[0] and the pair with the largest triangle.
            std::size_t bj = 1;
            std::size_t bk = 2;
            double best = -1.0;
            for (std::size_t j = 1; j < w.size(); ++j)
                for (std::size_t k = j + 1; k < w.size(); ++k) {
                    const double area = Len3(Cross3(Sub(w[j], w[0]), Sub(w[k], w[0])));
                    if (area > best) {
                        best = area;
                        bj = j;
                        bk = k;
                    }
                }
            P3 p0 = w[0];
            P3 p1 = w[bj];
            P3 p2 = w[bk];
            // Radiant/cod4map plane: normal = (p0 - p1) x (p2 - p1), outward.
            if (Dot3(Cross3(Sub(p0, p1), Sub(p2, p1)), PlaneNormal(hull.planes[i])) < 0.0)
                std::swap(p0, p2);
            std::string line = " ";
            for (const P3* p : {&p0, &p1, &p2})
                line += "( " + FormatNumber((*p)[0]) + " " + FormatNumber((*p)[1]) + " " +
                        FormatNumber((*p)[2]) + " ) ";
            line += material + " 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n";
            lines.push_back(std::move(line));
        }
        if (lines.size() < 4) continue;
        out << "// brush " << brushIndex++ << "\n{\n";
        for (const std::string& line : lines) out << line;
        out << "}\n";
    }
    out << "}\n";
    if (!out) {
        error = "write failed: " + path.string();
        return false;
    }
    return true;
}

bool ReadCollmap(const fs::path& path, std::vector<Hull>& hulls, std::string& material,
                 std::string& error) {
    hulls.clear();
    material.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open collmap " + path.string();
        return false;
    }
    // Brace depth: 1 = inside an entity, 2 = inside a brush (or patch, rejected).
    int depth = 0;
    bool inBrush = false;
    Hull current;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const std::string text = line.substr(first);
        if (text.rfind("//", 0) == 0) continue;
        if (text == "{") {
            ++depth;
            if (depth == 2) {
                inBrush = true;
                current = {};
                current.kind = "manual";
            }
            continue;
        }
        if (text == "}") {
            if (depth == 2 && inBrush) {
                if (current.planes.size() >= 4) hulls.push_back(current);
                inBrush = false;
            }
            --depth;
            continue;
        }
        if (!inBrush) continue;
        if (text.rfind("curve", 0) == 0 || text.rfind("mesh", 0) == 0) {
            error = "collmaps may hold brushes only (found a patch in " + path.string() + ")";
            return false;
        }
        if (text[0] != '(') continue;   // "contents detail;" and the like
        P3 p[3];
        char materialName[128] = {};
        if (std::sscanf(text.c_str(),
                        "( %lf %lf %lf ) ( %lf %lf %lf ) ( %lf %lf %lf ) %127s",
                        &p[0][0], &p[0][1], &p[0][2], &p[1][0], &p[1][1], &p[1][2],
                        &p[2][0], &p[2][1], &p[2][2], materialName) != 10)
            continue;
        // Radiant/cod4map: normal = (p0 - p1) x (p2 - p1), outward.
        const P3 n = Cross3(Sub(p[0], p[1]), Sub(p[2], p[1]));
        const double length = Len3(n);
        if (!(length > 1e-9)) continue;
        const P3 unit = Mul(n, 1.0 / length);
        current.planes.push_back({unit[0], unit[1], unit[2], Dot3(unit, p[1])});
        if (material.empty()) material = materialName;
    }
    return true;
}

}  // namespace kimport
