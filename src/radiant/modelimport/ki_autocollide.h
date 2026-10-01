#pragma once

// KIWI model importer - collision for already-built xmodels (no recipe needed).
//
// Classes, from the model's own surfaces (raw\materials surface types) and name:
//   vegetation  grass / herbs / scrub / bushes / boughs / ground cover: NO collision
//   tree        a wood (bark) surface on a tree: TRUNK ONLY - the main stem traced up
//               from the ground slice by slice (so leaning trunks follow), wrapped in
//               stacked round k-DOP sections; branches and canopy get nothing
//   prop        everything else: the same automatic hulls as an imported model
// (user, 2026-10-01: "vegetation should have no collision except trees, and only on the
// trunk for those").

#include "ki_collision.h"
#include "ki_xmodel_read.h"

#include <string>
#include <vector>

namespace kimport {

enum class AutoClass { Prop, Tree, Vegetation };

const char* AutoClassName(AutoClass value);

AutoClass ClassifyXModel(const RawXModel& model);

struct TrunkSettings {
    double sliceHeight = 12.0;       // inches per tracking slice
    double maxHeight = 256.0;        // trunk collision stops this far above the base
    double sectionHeight = 48.0;     // one k-DOP per this much trunk
    double minDiameter = 1.5;        // thinner stems (twigs, whips) get nothing
};

// Hulls for one built model by its class. `note` says what was done (for reports).
void AutoHullsForXModel(const RawXModel& model, AutoClass cls,
                        const CollisionSettings& settings, const TrunkSettings& trunk,
                        std::vector<Hull>& hulls, std::string& note, const LogFn& log);

}  // namespace kimport
