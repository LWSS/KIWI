#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Editor-only construction geometry lives outside selection_t and map data. A
// parallel KIWI selection owns its picks and edits; R/S still do not transform it.
// Points and segments participate in snapping.
//
// Construction snapshots remain separate from the legacy brush/entity undo store,
// but kiwi_undo orders both stores on one journal timeline.
//
// LINE/POLYLINE/RECT points are stored as WORLD-SPACE xyz triples. Their planes are
// derived and cached; a legal non-planar chain simply cannot form a region.
// CIRCLE/ARC are intrinsically planar and keep an authoritative parametric plane.
//
// Persistence uses a versioned `<mapname>.kiwi` sidecar. KIWI1 plane-space points
// still load; only KIWI2 world-space points are written. Unknown optional fields
// are skipped, and malformed sidecars warn without blocking the map load.

#include <string>
#include <vector>

struct ray_t;                       // kiwi_pick.h
struct pick_result_t;               // kiwi_pick.h
struct selbrush_t;                  // qe3.h:31 (the brush INSTANCE / list node)
class  KiwiEditorCommand;           // kiwi_command.h

// Tessellation and store budgets.
#define KCON_SEGS_PER_UNIT   0.25f  // 32 segments at radius 128
#define KCON_SEGS_MIN        8
#define KCON_SEGS_MAX        64
// Typed circle/arc counts allow 3, but cardinal anchoring rounds them to at
// least 4; polygons retain their independent three-side minimum.
#define KCON_SIDES_MIN       3
#define KCON_MAX_POINTS      256    // per object; a polyline that long is a mistake
#define KCON_UNDO_DEPTH      KUNDO_DOMAIN_DEPTH     // whole-store snapshot depth (kiwi_undo.h)
#define KCON_DRAW_SEGMENTS   1600   // kiwi_lines budget for the whole construction pass
#define KCON_ANGLE_STEP      15.0f  // bearing snap increment, degrees, in-plane
#define KCON_JOIN_PIXELS     10.0f  // "click near the first point" closes a polyline

// Double-click timing and per-axis pixel slop used by KiwiDrawTool::Click.
#define KCON_DBLCLICK_MS     400u
#define KCON_DBLCLICK_SLOP_PX 4     // per axis, in pixels

// Shared spellings prevent precision drift between construction paths.
#define KCON_PI              3.14159265358979f
#define KCON_TWO_PI          6.283185307179586f
#define KCON_DEG2RAD         0.01745329252f
#define KCON_RAD2DEG         57.29577951308232f

// Working-plane indicator dimensions are screen-space, not world-space.
#define KCON_PLANE_HALF_PIXELS  110.0f
// Grazing fade floor and anchor-cross fraction for the plane indicator.
#define KCON_PLANE_FADE_MIN     0.06f
#define KCON_PLANE_CROSS_FRAC   0.18f

// WORLD-SPACE coplanarity tolerance; deliberately matches KREG_JOIN_DIST.
#define KCON_PLANE_FIT_DIST  0.5f
// WORLD-SPACE segment intersection tolerance, stricter than endpoint welding.
#define KCON_ISECT_DIST      0.25f

// Fixed PLANE-SPACE weld epsilon for fillet/offset solves. Do not replace it
// with the grid-scaled region weld without reevaluating behavior.
#define KCON_WELD_2D         0.01f

// Shared construction preview colors; bevel uses a different palette.
extern const float KCON_PREVIEW_OK[3];
extern const float KCON_PREVIEW_BAD[3];
// Continuous construction-segment tests use 10 px without widening brush picks.
#define KCON_LINE_PIXELS     10.0f
// Discrete selection gets a wider radius than continuous hover/snap tests.
#define KCON_CLICK_PIXELS    14.0f

// Polygon and spline tessellations are stored as KCON_POLYLINE, avoiding a
// sidecar-format change. The 64-side cap stays below the extrusion profile cap.
#define KCON_POLY_SIDES_MIN  3
// Half the maximum extrusion profile, leaving every generated n-gon extrudable.
#define KCON_POLY_SIDES_MAX  64     // == the extruder's comfort zone, half of KEXT_MAX_PROFILE
#define KCON_POLY_SIDES_DEF  6
// Spline segments per control-point span; emission is capped at KCON_MAX_POINTS.
#define KCON_SPLINE_SEGS     8

// Orthonormal right-handed basis: u x v == normal.
struct kconPlane_t
{
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 0.0f, 1.0f };
    float u[3]      = { 1.0f, 0.0f, 0.0f };
    float v[3]      = { 0.0f, 1.0f, 0.0f };
};

enum kconType_t
{
    KCON_LINE = 0,      // 2 points
    KCON_POLYLINE,      // n points, `closed` says whether it wraps
    KCON_RECT,          // 4 points, axis-aligned in-plane, always closed
    KCON_CIRCLE,        // parametric, always closed
    KCON_ARC,           // parametric, never closed
    KCON_TYPE_COUNT
};

// LINE/POLYLINE/RECT use WORLD-SPACE `pts`; CIRCLE/ARC use the parametric payload.
// For parametric objects `plane` is authoritative. For point objects it is a
// checked cache maintained by Add/NoteMutated; use KiwiCon_ObjectPlane.
struct kconObject_t
{
    kconType_t         type   = KCON_LINE;
    kconPlane_t        plane;
    bool               planeValid = false;        // see above; always true for CIRCLE/ARC
    std::vector<float> pts;                       // 3 floats per point, WORLD SPACE
    bool               closed = false;
    float              centre[2] = { 0.0f, 0.0f };// CIRCLE / ARC, plane space
    float              radius    = 0.0f;
    float              ang0      = 0.0f;          // degrees, CCW about `plane.normal`
    float              ang1      = 360.0f;
    // Per-object tessellation override; 0 selects the radius-driven rule.
    // Arcs apply the full-circle count pro rata over their sweep.
    int                segs      = 0;             // 0 = automatic (radius-driven)
    // Hidden objects are inert: not drawn, picked, snapped, or used for regions.
    // Hiding also clears their construction selection.
    bool               hidden = false;
    // Flat group handle: -1 is ungrouped; nonnegative ids are never renumbered.
    int                group = -1;
    // Empty means unnamed; names are sidecar-only labels and need not be unique.
    std::string        name;
    // KIWI (2026-09-10, user: "S key in the line mode to start a spline curve"): per
    // control point, 1 = the span LEAVING point i (to i+1, wrapping when closed) is a
    // spline span.  LINE/POLYLINE only; a shorter vector reads as straight.  The
    // control points stay in `pts` (anchors, moves, sidecar); VertCount/VertWorld hand
    // out the Catmull-Rom tessellation through the smooth runs, so drawing, picking,
    // snapping and the Plasticity export see the curve.  Regions, extrusion, trim,
    // fillet, offset and arrangement refuse objects with spline spans: they are 2D
    // layout curves and never a brush edge.
    std::vector<unsigned char> smooth;
    // Tessellation cache for smooth chains (store generation + shape fingerprint).
    mutable std::vector<float> tessCache;
    mutable unsigned           tessGen  = 0;
    mutable unsigned long long tessKey  = 0;
};

// True for a LINE/POLYLINE with at least three points and one spline span.
bool KiwiCon_HasSmooth( const kconObject_t &o );
// The tessellated polyline of a smooth chain (WORLD xyz triples); `closed` mirrors
// o.closed.  Used by the draw tool preview as well as the store accessors.
void KiwiCon_TessellateMixed( const std::vector<float> &ctrl,
                              const std::vector<unsigned char> &smooth, bool closed,
                              std::vector<float> *out );

// A circle/arc: parametric, intrinsically planar, `plane` authoritative.
inline bool KiwiCon_IsParametric( const kconObject_t &o )
{
    return o.type == KCON_CIRCLE || o.type == KCON_ARC;
}

// Plane math shared by construction, regions, and extrusion.
void KiwiCon_PlaneToWorld( const kconPlane_t &p, const float uv[2], float out[3] );
void KiwiCon_WorldToPlane( const kconPlane_t &p, const float world[3], float outUV[2] );
// Strict ray/plane hit: no clamp; false for parallel rays or hits behind the eye.
bool KiwiCon_RayPlane( const kconPlane_t &p, const ray_t &ray, float outWorld[3] );

// Finite ray/plane placement. Forward hits clamp in plane-space to
// KCON_PLANE_REACH; parallel or rearward rays pin to that square's edge.
// Returns false only for a degenerate plane.
bool KiwiCon_RayPlaneBounded( const kconPlane_t &p, const ray_t &ray, float outWorld[3] );

// WORLD-UNIT half-extent of the finite plane, measured from p.origin in (u,v).
#define KCON_PLANE_REACH     16384.0f

// Plane equivalence, minimum camera facing, and selected-face reach tolerances.
// The 85-degree facing cutoff prevents edge-on faces from capturing placement;
// face reach allows one face extent of cursor slack.
#define KCON_PLANE_PARALLEL     0.999f
#define KCON_PLANE_FACING_MIN   0.0872f    // cos(85 degrees)
#define KCON_FACE_PLANE_SLACK   1.0f
#define KCON_FACE_PLANE_MINGROW 16.0f      // world units, so a tiny face still has reach
// Build an orthonormal basis, preferring hintU; false for a degenerate normal.
bool KiwiCon_MakePlane( const float origin[3], const float normal[3],
                        const float *hintU, kconPlane_t *out );

// Bearing axes derive from the normal, never face winding order. Non-ground
// planes use u=normalize(worldZ x n), v=n x u; ground-ish planes use world +X.
// Thus 0 degrees is level on a surface and 90 degrees points up it. Callers use
// the snap result's resolved plane normal, with the point latch only as fallback.
// Bearings are normalized to [0, 360).
void KiwiCon_BearingBasis( const float normal[3], float outU[3], float outV[3] );
// Absolute bearing of `dir` in that basis, degrees in [0, 360).  False when the
// direction has no usable in-plane component (`inPlaneOut`, when given, receives
// the in-plane length so the caller can apply its own omission gate).
bool KiwiCon_BearingOf( const float normal[3], const float dir[3], float *outDeg,
                        float *inPlaneOut, float *outOfPlaneOut );
// The inverse: the unit in-plane direction a typed bearing names.
void KiwiCon_BearingDir( const float normal[3], float deg, float out[3] );
// Degrees wrapped into [0, 360).
float KiwiCon_WrapDeg( float deg );
// Snap (u,v) on a lattice anchored at the WORLD ORIGIN's plane projection, not
// at p.origin. The latter may be an arbitrary off-grid face hit and would shift
// every later snap; the normal component remains unchanged.
void KiwiCon_SnapUV( const kconPlane_t &p, float uv[2] );

// Fit a plane to WORLD-SPACE xyz triples with Newell's method. False for fewer
// than three points, degenerate area, or deviation above KCON_PLANE_FIT_DIST.
bool KiwiCon_FitPlane( const float *worldPts, int count, kconPlane_t *out );

// Parametric objects return their authoritative plane; point objects return the
// checked cached fit. Do not read a point object's `plane` without planeValid.
bool KiwiCon_ObjectPlane( const kconObject_t &o, kconPlane_t *out );

// Closest 3D segment approach; outMid is the midpoint and outTa/outTb are the
// clamped segment parameters. The return value is their distance.
float KiwiCon_SegSegClosest( const float a0[3], const float a1[3],
                             const float b0[3], const float b1[3],
                             float *outTa, float *outTb, float outMid[3] );

// Tessellated geometry used by drawing, snapping, regions, and extrusion.
int  KiwiCon_VertCount   ( const kconObject_t &o );
bool KiwiCon_VertWorld   ( const kconObject_t &o, int i, float out[3] );
int  KiwiCon_SegmentCount( const kconObject_t &o );
bool KiwiCon_SegmentWorld( const kconObject_t &o, int i, float a[3], float b[3] );

// Return the nearest visible tessellated segment within KCON_LINE_PIXELS,
// independent of selection mode. Any out-parameter may be null.
bool KiwiCon_PickSegmentAt( int imgX, int imgY, float outA[3], float outB[3],
                            int *outObj, int *outSeg, float *outPixels );

// Snap anchors are defining points, not all tessellated vertices. Circles expose
// center/quadrants; arcs expose center/endpoints to avoid swamping snap ranking.
int  KiwiCon_AnchorCount( const kconObject_t &o );
bool KiwiCon_AnchorWorld( const kconObject_t &o, int i, float out[3] );

// Store entry normalizes point objects: consecutive near-duplicates collapse,
// and closed chains do not retain a repeated last/first seam. The implicit wrap
// must not become a zero-length extrusion edge.
int                 KiwiCon_Count();
const kconObject_t *KiwiCon_At( int index );
int                 KiwiCon_Add( const kconObject_t &o );   // index, or -1 when rejected
// Validate before pushing undo so a rejected object creates no empty undo step.
int                 KiwiCon_AddWithUndo( const kconObject_t &o );
bool                KiwiCon_RemoveAt( int index );
// Also resets the active plane and explicit latch even when the store is empty;
// document-global plane state must not survive a scene clear.
void                KiwiCon_ClearAll();

// New-document reset: ClearAll plus construction undo/redo and unified journal.
void                KiwiCon_ResetForNewMap();

// Index-safe: an invalid index reads as hidden.
bool                KiwiCon_Hidden( int index );

// Invalid/unnamed objects read as "". Names reject quotes for sidecar safety.
const char         *KiwiCon_Name( int index );
void                KiwiCon_SetName( int index, const char *name );
#define KCON_NAME_MAX 64
// SetHidden does not push undo; callers bracket compound hide/selection changes.
void                KiwiCon_SetHidden( int index, bool hidden );
bool                KiwiCon_HasHidden();
// Returns the reveal count and brackets its own undo snapshot.
int                 KiwiCon_UnhideAll();

// Group access is index-safe and does not push undo; callers bracket regrouping.
int         KiwiCon_Group   ( int index );
void        KiwiCon_SetGroup( int index, int group );
// Mint a stable id; a null/empty name becomes "Group N".
int         KiwiCon_NewGroup( const char *name );
// Declared groups stay addressable even when empty.
int         KiwiCon_GroupCount();
int         KiwiCon_GroupIdAt( int i );
bool        KiwiCon_GroupExists( int group );
const char *KiwiCon_GroupName( int group );          // "" for an unknown id
void        KiwiCon_SetGroupName( int group, const char *name );
// Dissolve a group and return all members to ungrouped.
bool        KiwiCon_RemoveGroup( int group );
// Number of objects assigned to the group.
int         KiwiCon_GroupMemberCount( int group );

// Mutable pointers expire on structural store changes. Call NoteMutated after
// editing: it bumps generation and refits every point object's cached plane.
kconObject_t       *KiwiCon_MutableAt( int index );
void                KiwiCon_NoteMutated();

// Bumped on every store change; region caches may not span generations.
unsigned KiwiCon_Generation();

// Push before mutation. Snapshots stay construction-local, while kiwi_undo
// journals their order with legacy edits. Undo/redo replace the whole store.
void KiwiCon_UndoPush();
bool KiwiCon_UndoPop();
bool KiwiCon_RedoPop();
void KiwiCon_ClearRedo();
int  KiwiCon_UndoDepth();

// Free LINE/POLYLINE/SPLINE points resolve verbatim: engaged snap, else surface
// ray hit, else grid-snapped world ground. They never read the active plane.
// The plane API is only for intrinsically planar tools/primitives and explicit
// plane consumers such as face focus, entity drop, and offset.
const kconPlane_t &KiwiCon_ActivePlane();
void KiwiCon_SetActivePlane( const kconPlane_t &p );
// `axis` is the plane normal: 2=XY, 1=XZ, 0=YZ.
void KiwiCon_SetPlaneAxis( int axis );
void KiwiCon_SetPlaneFromView();
bool KiwiCon_SetPlaneFromCursorFace();          // false when no face is under the cursor
// Use exactly one selected face, and only when ArmSelectedFacePlane enabled this
// one tool start; ordinary face selection must not create a persistent plane.
bool KiwiCon_SetPlaneFromSelectedFace();
// Build from winding center/normal/longest edge. `requireReach` rejects a face
// outside cursor reach. The caller separately decides whether to mark explicit.
bool KiwiCon_SetPlaneFromFace( selbrush_t *node, int faceIndex, bool requireReach );
// Quantize the view direction to a world-axis plane at the current offset.
// This named operation is not an AutoPlaneForTool rung.
bool KiwiCon_SetPlaneFromViewDominantAxis();
// Tool-start invariant: an EXPLICIT plane stands; an armed single selected face
// is consumed once and marked explicit; otherwise derive a zero-height default.
// Never inherit an ambient face/object/view offset: an invisible persistent plane
// silently distorts every later planar placement.
void KiwiCon_AutoPlaneForTool();

// Arm selected-face adoption for exactly the next AutoPlaneForTool call.
void KiwiCon_ArmSelectedFacePlane();

// Axis views use their world-axis plane; perspective uses world ground. The plane
// always passes through the WORLD ORIGIN and inherits no prior elevation.
void KiwiCon_DefaultPlaneForView();

// Only an explicit user gesture may latch a plane. Installing transient geometry
// and marking intent are separate operations so tool state cannot latch a plane.
void        KiwiCon_MarkPlaneExplicit( const char *desc );
bool        KiwiCon_PlaneIsExplicit();
const char *KiwiCon_PlaneDesc();          // "" when the plane is the default
// Live description is the explicit label or the named default plane.
const char *KiwiCon_PlaneDescLive();
// Drop the latch and re-derive the default; Space over empty space uses this.
void        KiwiCon_ClearPlaneToDefault();

// Draw construction lines and points; region fills use the command-overlay slot.
void KiwiCon_DrawWorld();

// Remembered round-tool count: 0=automatic, otherwise clamped and persisted.
int  KiwiCon_ToolSides();
void KiwiCon_SetToolSides( int sides );

bool KiwiCon_ShowConstruction();
void KiwiCon_SetShowConstruction( bool on );

// True for any active drawing chain; plane-aware snapping uses PlanePlacement.
bool KiwiCon_ToolActive();

// PlanePlacement is true only for shapes that must remain planar: planar draw
// tools and planar primitives. Free line/polyline/spline tools keep it false.
// Set and clear it for the full gesture so every plane-borne snap branch agrees.
void KiwiCon_SetPlanePlacement( bool on );
bool KiwiCon_PlanePlacement();      // a PLANAR draw tool OR a plane-placing command

// Last placed tool point; SNAP_ANGLE's origin. False before the first point.
bool KiwiCon_ToolAnchor( float out[3] );

// Previous committed point; relative-angle snapping's base direction.
bool KiwiCon_ToolPrevAnchor( float out[3] );

// Every placed WORLD point of the open chain, oldest first (index Count-1 is the anchor).
// Snapping derives its parallel / perpendicular guides from the chain's own segments.
int  KiwiCon_ToolChainCount();
bool KiwiCon_ToolChainPoint( int i, float out[3] );

// Expose an open chain's first point as a snap target once at least three points
// exist, keeping visual closure and stored closure identical.
bool KiwiCon_ToolLoopStart( float out[3] );

// `mapPath` names the .map; persistence replaces its extension with `.kiwi`.
// Bad sidecars warn but never fail map load.
bool KiwiCon_SaveSidecar( const char *mapPath );
bool KiwiCon_LoadSidecar( const char *mapPath );

// Command and panel integration.
void KiwiCon_RegisterCommands();
KiwiEditorCommand *KiwiCon_CommandForId( int commandId );   // the 9 drawing tools
bool KiwiCon_DispatchInstant( unsigned int commandId );     // the plane / clear commands
bool KiwiCon_HasObjects();                                  // palette canExecute
// View-panel Construct items.
void KiwiCon_MenuItems();
