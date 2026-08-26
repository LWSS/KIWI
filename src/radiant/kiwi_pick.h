#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Unified picking over ported Test_Ray surface hits and projected vertex/edge hits.
// Vertex and edge tolerances are camera-image pixels (8 px / 6 px), never world
// units. Candidates are filtered active_brushes first, then selected_brushes.
//
// "Screen" and "pixel" mean camera-RTT-relative TOP-LEFT coordinates, as reported
// by ImGuiShell_CameraPaintCursor. Flip against camera_s.height, kept aligned by
// CamWnd_RenderToRT, not the hidden native child's client rect.
//
// Brush indices follow kiwi_selection.h DESIGN NOTE 3: edge (face, e) is
// w->p[e] → w->p[(e+1)%n], vertex (face, v) is w->p[v], and a patch point is
// (-1, col * height + row). Same-brush corner ties choose the lowest face index.
//
// PICKF_EXCLUDE_SELECTED snapshots selected_brushes plus every KiwiSel() brush
// before both passes; face selections live only in KiwiSel(). This prevents dragged
// geometry from picking or snapping to itself. PICKF_NONE preserves ordinary picks.
// With exclusion enabled, sub_48D460 skips BRUSHFLAG_SELECTED (0x80) nodes and the
// post-filter handles face selections. Test_Ray retains only its nearest hit, so a
// rejected hit cannot expose the surface behind; snapping falls through to the grid.

#include "kiwi_selection.h"

#include <math.h>

// Pixel tolerances (spec §2).
#define PICK_VERT_PIXELS 8.0f
#define PICK_EDGE_PIXELS 6.0f

// 0 keeps ordinary picking; exclusions are opt-in.
#define PICKF_NONE              0u
#define PICKF_EXCLUDE_SELECTED  1u

struct ray_t
{
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    float dir[3]    = { 0.0f, 0.0f, 0.0f };   // normalised
};

struct pick_result_t
{
    bool       valid      = false;
    sel_item_t item;                          // what was hit, at the filter's granularity
    float      point[3]   = { 0.0f, 0.0f, 0.0f };   // world hit point
    float      screenDist = 0.0f;             // px from the cursor (0 for area hits)
    // Area hits copy edTrace_t's validated unit normal. False for misses and
    // screen-space hits; consumers must gate normal reads on haveNormal.
    bool       haveNormal = false;
    float      normal[3]  = { 0.0f, 0.0f, 1.0f };
};

// Build a ray from TOP-LEFT camera-RTT coordinates; false before viewport sizing.
// The CameraCalcRayDir forwarder keeps its direction identical to Drag_Begin's.
bool Pick_RayFromImagePos( int imgX, int imgY, ray_t *out );

// Same, for wherever the shell last saw the cursor over the camera image
// (ImGuiShell_CameraPaintCursor).  False when the cursor is not over it.
bool Pick_RayFromCursor( ray_t *out );

// World → TOP-LEFT camera-image pixels, inverse to CameraCalcRayDir. False for an
// invalid viewport, or for perspective points at or behind the eye plane.
bool Pick_WorldToImage( const float *world, float *outX, float *outY );

// Camera-image pixel distance to a segment. Optional outT returns the clamped
// parameter for mapping the screen result onto the world segment; callers own
// acceptance radii and behind-eye policy.
inline float Pick_SegDist2D( float px, float py, float ax, float ay,
                             float bx, float by, float *outT )
{
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if ( len2 > 1e-6f )
    {
        t = ( ( px - ax ) * dx + ( py - ay ) * dy ) / len2;
        if ( t < 0.0f )      t = 0.0f;
        else if ( t > 1.0f ) t = 1.0f;
    }
    const float ex = px - ( ax + dx * t );
    const float ey = py - ( ay + dy * t );
    if ( outT )
        *outT = t;
    return sqrtf( ex * ex + ey * ey );
}

// Shared with box selection to keep its candidate set aligned with click picking;
// mirrors the ported sub_48D460 admission rules.
bool Pick_BrushPickable( selbrush_t *b );

// The Test_Ray `contents` mask a 3D-camera-viewport pick uses — the same bits
// Drag_Begin computes for viewz == 2 (prefs gates + the 0x1000 camera-view flag),
// minus the light-preview bit.
int Pick_CameraContents();

// kindMask filters candidates at pick time, never by post-converting an object hit.
// Resolution order is vertex → edge → area within their respective pixel radii.
pick_result_t Pick( const ray_t &ray, sel_mask_t kindMask, unsigned pickFlags = PICKF_NONE );
