// KIWI model importer - ray picking. See ki_pick.h.

#include "ki_pick.h"

#include <cmath>
#include <limits>

namespace kimport {

bool PickPiece(const ModelSpaceMesh& mesh, const float origin[3], const float dir[3], int& piece,
               int& triangle, float& t) {
    float best = std::numeric_limits<float>::max();
    int bestTriangle = -1;
    const std::size_t count = mesh.trianglePiece.size();
    for (std::size_t i = 0; i < count; ++i) {
        // Moller-Trumbore, double-sided.
        const auto& a = mesh.positions[i * 3 + 0];
        const auto& b = mesh.positions[i * 3 + 1];
        const auto& c = mesh.positions[i * 3 + 2];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float p[3] = {dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
                            dir[0] * e2[1] - dir[1] * e2[0]};
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-12f) continue;
        const float inv = 1.0f / det;
        const float s[3] = {origin[0] - a[0], origin[1] - a[1], origin[2] - a[2]};
        const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                            s[0] * e1[1] - s[1] * e1[0]};
        const float v = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float hit = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (hit > 1e-4f && hit < best) {
            best = hit;
            bestTriangle = static_cast<int>(i);
        }
    }
    if (bestTriangle < 0) return false;
    triangle = bestTriangle;
    piece = static_cast<int>(mesh.trianglePiece[bestTriangle]);
    t = best;
    return true;
}

}  // namespace kimport
