#pragma once

// KIWI model importer - picking a piece of a model-space mesh with a ray (the Model
// Import window's click-to-select).

#include "ki_convert.h"

namespace kimport {

// Nearest triangle the ray (origin + t * dir, t > 0) hits, both sides. `piece` and
// `t` are set on a hit; `triangle` is the ModelSpaceMesh triangle index.
bool PickPiece(const ModelSpaceMesh& mesh, const float origin[3], const float dir[3], int& piece,
               int& triangle, float& t);

}  // namespace kimport
