#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Loft bridges two planar brush faces without modifying the source brushes.
// Brush modes emit convex solids; CURVE emits open, non-colliding q3 patch strips.

class KiwiEditorCommand;

// Rings are bounded like the other profile tools in this layer.
#define KLOFT_MAX_RING     64
#define KLOFT_MIN_SEGS      1
#define KLOFT_MAX_SEGS    128
#define KLOFT_DEF_SEGS_RULED    1     // a straight bridge needs one solid
#define KLOFT_DEF_SEGS_TANGENT  8     // enough facets to read as curved
#define KLOFT_MIN_SPAN      1.0f      // world units between face centroids
#define KLOFT_MIN_TENSION   0.05f
#define KLOFT_MAX_TENSION   4.0f
#define KLOFT_DEF_TENSION   0.5f      // tangent magnitude relative to span

// Ring construction invariants:
// - rewind both source windings CCW about A->B;
// - equalize counts by splitting longest edges, preserving every source corner;
// - match and blend vertices in rotation-minimising frames so sections do not twist or pinch.
// Brush stations are planarised, then every unlinked segment is rebuilt and validity-gated.

// A patch grid is at most 15 columns: 2 * 7 spans + 1.
// Longer strips are chunked at shared stations; global tangents keep chunk joins G1.
#define KLOFT_CURVE_SPANS_PER_PATCH  7
#define KLOFT_MAX_CURVE_SEGS    56
#define KLOFT_MAX_CURVE_PATCHES 256   // per loft, mirrored copies included
#define KLOFT_DEF_SEGS_CURVE    2     // one span per 45 degrees of a right-angle bend
#define KLOFT_CURVE_ROWS        3     // edge endpoints and their midpoint

enum kiwiLoftBridge_t
{
    KLOFT_BRIDGE_RULED = 0,
    KLOFT_BRIDGE_TANGENT,
    KLOFT_BRIDGE_CURVE,
    KLOFT_BRIDGE_COUNT
};

// CURVE continuity is per end: G0 follows the chord, G1 the source-face normal,
// and G2 circular-arc-fits the end span (it is not a true curvature match).
// Patch normals are cross(dCol, dRow); reversing rows flips facing.
#define KLOFT_SECTION        "KiwiLoft"
#define KLOFT_ARC_MIN_COS    0.05f   // reject sideways/backward arc-fit tangents
#define KLOFT_CURVE_PREVIEW  6       // line subdivisions per curve span

enum kiwiLoftCont_t
{
    KLOFT_CONT_G0 = 0,
    KLOFT_CONT_G1,
    KLOFT_CONT_G2,
    KLOFT_CONT_COUNT
};

void KiwiLoft_RegisterCommands();
KiwiEditorCommand *KiwiLoft_CommandForId( int commandId );

// One usable selected brush face is enough; the command can pick the second.
bool KiwiLoft_CanExecute();
