#pragma once
// The retained scene.
//
// One record per brush instance on the three display lists (active, selected, filtered).
// Once per editor frame the scene walks those lists beside a compact list-ordered copy of
// what it last saw - each node's own version, face array, patch, filter/hide bits, owner and
// its def's version - so ANY edit path and any list relink is noticed without hooks in the
// editing code.  Brush_Free drops its record at once, so a recycled address never inherits
// one.  RADIANT_SCENE_VERIFY=1 also compares the deep fields the fast stamp leaves out.
//
// What the scene keeps for the views:
//   facts     each brush's owner entity, class (terrain / lightgrid) and hidden bit, with
//             per-entity counters - the outliner's counts and eye states (no map walks)
//   cells     active-list brushes grouped on a world grid, with their bounds
//   batches   every face layer sharing (window, material, technique, sort key, VB page),
//             concatenated across cells into one static index buffer (kiwi_gpu.h), drawn
//             one range per run of visible cells
//
// What a brush DRAWS is the camera's rule (camwnd.cpp's describe function): the scene stores
// the answer and knows nothing about materials or graphics APIs.  Faces whose look depends on
// the frame (sky film, missing resident geometry) go on the per-frame list instead.

#include <stdint.h>
#include <vector>

struct selbrush_t;
struct entity_s;
struct Material;

// ── records and facts ────────────────────────────────────────────────────────

// Called once per editor frame, before anything reads the scene.
void KiwiScene_BeginFrame();

// Bring the records, facts, counters and cell bounds up to date.  The first call each frame
// scans; later calls return at once unless a structural edit (kiwi_walkcache.h) happened
// since, so a pick right after a click still sees the lists as they are.  The camera's
// KiwiScene_Sync calls it; so does any reader.
void KiwiScene_SyncRecords();

enum ksBrushClass_t
{
    KS_CLASS_TERRAIN   = 1,     // a terrain patch (PATCH_TERRAIN)
    KS_CLASS_LIGHTGRID = 2,     // a brush with any lightgrid face
    KS_CLASS_FIXEDSIZE = 4,     // owned by a fixed-size (point / model / prefab) entity
    KS_CLASS_ENTITY    = 8,     // owned by an entity other than worldspawn
};

enum ksBrushFilter_t
{
    KS_FILTER_ALL,              // every brush
    KS_FILTER_PLAIN,            // neither terrain nor lightgrid (fixed-size or not)
    KS_FILTER_TERRAIN,
    KS_FILTER_LIGHTGRID,
    KS_FILTER_COUNT
};

struct ksEntityCounts_t
{
    int brushes[KS_FILTER_COUNT];
    int hidden[KS_FILTER_COUNT];
};

// The class of a brush, computed (no record needed).
unsigned KiwiScene_ClassifyBrush( const selbrush_t *b );
// The recorded class when the brush has a record, else KiwiScene_ClassifyBrush.
unsigned KiwiScene_BrushClass( const selbrush_t *b );
bool     KiwiScene_BrushPasses( const selbrush_t *b, ksBrushFilter_t filter );
// An entity's brush counts over the three lists; nullptr = no recorded brushes.
const ksEntityCounts_t *KiwiScene_EntityCounts( const entity_s *e );
// Moves whenever a record appears, disappears, or its owner / class / hidden bit changes.
unsigned KiwiScene_FactsGeneration();

// A value that changes whenever anything in the cells reaching the box changes (a brush
// added, removed, edited, hidden...).  Selected brushes are not in cells.
unsigned long long KiwiScene_RegionGeneration( const float mins[3], const float maxs[3] );

// The active-list brushes whose class has any bit of `classMask`, in list order.
void KiwiScene_ActiveOfClass( unsigned classMask, std::vector<selbrush_t *> &out );
// The same, leaving out the brushes whose whole main-window draw the scene keeps
// (KiwiScene_BrushRetained( b, KS_WINDOW_MAIN ) or KiwiScene_BrushLinesRetained).
void KiwiScene_ActiveOfClassLive( unsigned classMask, std::vector<selbrush_t *> &out );

// The active-list brushes in the cells whose bounds pass `test`, in list order (so a walker
// that keeps the first of equal hits behaves exactly as when it walked the whole list).
// Selected and filtered brushes are never in cells; walk those lists directly.
// `keepEntityBrushes` also returns every brush owned by an entity other than worldspawn,
// whatever its cell - for a caller that draws things around an entity beyond its box.
typedef bool ( *ksBoundsTestFn )( const float mins[3], const float maxs[3], void *ctx );
void KiwiScene_QueryActive( ksBoundsTestFn test, void *ctx, std::vector<selbrush_t *> &out,
                            bool keepEntityBrushes = false );

// Brush_Free, before the instance is deleted: its record goes now, so an instance later
// allocated at the same address can never inherit it.
void KiwiScene_NodeFreed( const selbrush_t *b );

// Forget everything; the next sync rebuilds from the lists.  Map teardown, device reset.
void KiwiScene_InvalidateAll();

// ── the camera world ─────────────────────────────────────────────────────────

// The flush window a batch belongs to: MAIN is the camera's world flush, TOOL the see-through
// tool-volume flush drawn after it, LIGHT_* a previewed light's lit pass at its technique.
enum ksWindow_t
{
    KS_WINDOW_MAIN,
    KS_WINDOW_TOOL,
    KS_WINDOW_LIGHT_OMNI,
    KS_WINDOW_LIGHT_SPOT,
    KS_WINDOW_COUNT
};

// One retained draw of a face or patch layer, exactly as the camera would queue it.
struct ksLayer_t
{
    ksWindow_t      window;
    Material       *material;     // the material the draw binds
    int             techType;
    int             sortKey;
    unsigned        vertHandle;   // editor VB handle: HIWORD = page (1-based), LOWORD = first vertex;
                                  // or a KiwiScene_AddRun handle
    int             vertCount;
    const uint16_t *indices;      // run-relative; nullptr = a fan over vertCount (Editor_AddGeoFace's)
    int             indexCount;
};

// What the describe function hands back for one active brush.
struct ksBrushOut_t;
void KiwiScene_AddLayer( ksBrushOut_t *out, const ksLayer_t &layer );

// Vertices the scene itself owns (a model's mesh in world space), for layers whose vertices
// live in no editor VB page.  `key` states what the vertices are - the placement and the
// source surface - and a run the brush's previous describe made with the same key is kept
// without rewriting; otherwise `fill` writes `count` vertices of `stride` bytes once.  The
// run belongs to the brush: it goes when a describe no longer asks for it or the brush goes.
// Returns the vertHandle its layers use (0 = no room: draw nothing from it).
struct ksRunKey_t
{
    float       placement[8];
    const void *source;
    int         count;
};
typedef void ( *ksFillFn )( void *dst, void *ctx );
unsigned KiwiScene_AddRun( ksBrushOut_t *out, int stride, int vertDecl, const ksRunKey_t &key,
                           ksFillFn fill, void *ctx );

// What the brush draws spans this box (a model's mesh reaches past its entity box); the cell
// culls on it instead of the brush's own bounds.
void KiwiScene_SetBounds( ksBrushOut_t *out, const float mins[3], const float maxs[3] );

// Lines the brush draws (an entity's wireframe box, its facing arrow): `segCount` segments
// of two vertices, `stride` bytes each, with each segment's width and dimension, kept as
// given.  KiwiScene_EmitLines hands them back for the cells in view;
// KiwiScene_BrushLinesRetained says a brush's lines are kept.
void KiwiScene_AddLines( ksBrushOut_t *out, const void *verts, int stride, const uint8_t *widths,
                         const uint8_t *dimensions, int segCount );
// faceIndex -1 = the whole brush is drawn per frame.
void KiwiScene_AddPerFrame( ksBrushOut_t *out, int faceIndex );
// The describe gives up part-way: everything it added goes (its runs freed), so the brush
// keeps nothing and its pass draws it live.
void KiwiScene_KeepNothing( ksBrushOut_t *out );

typedef void ( *ksDescribeFn )( selbrush_t *b, ksBrushOut_t *out );

// SyncRecords, then re-describe the cells that changed.  `sceneKey` folds every global input
// the describe function reads (layer, technique, filter generation...): a different key
// re-describes everything.
void KiwiScene_Sync( unsigned long long sceneKey, ksDescribeFn describe );

// A convex view volume: a point p is inside when dot(n, p) >= d for every plane {n, d}.
struct ksFrustum_t
{
    int   planeCount;
    float planes[6][4];
};

// Queue `window`'s batches into the open editor flush window (R_SortMaterials ...
// R_AddEditorSurfsCmd), only the cells inside `frustum` (nullptr = all).
void KiwiScene_Emit( ksWindow_t window, const ksFrustum_t *frustum );
bool KiwiScene_HasBatches( ksWindow_t window );

// The same, for the cells reaching the box [mins, maxs] (a light's reach).
void KiwiScene_EmitBox( ksWindow_t window, const float mins[3], const float maxs[3] );

// Every kept line run (KiwiScene_AddLines) in the cells inside `frustum` (nullptr = all).
typedef void ( *ksLinesFn )( const void *verts, const uint8_t *widths, const uint8_t *dimensions,
                             int segCount, void *ctx );
void KiwiScene_EmitLines( const ksFrustum_t *frustum, ksLinesFn fn, void *ctx );
bool KiwiScene_BrushLinesRetained( const selbrush_t *b );

// True when every layer `b` draws in `window` comes from the batches: an active brush whose
// describe gave that window layers.  A describe adds a brush to a LIGHT_* window whole or
// not at all, so a false answer means the caller draws the brush itself.
bool KiwiScene_BrushRetained( const selbrush_t *b, ksWindow_t window );

// The per-frame entries, in list order.
typedef void ( *ksPerFrameFn )( selbrush_t *b, int faceIndex, void *ctx );
void KiwiScene_ForEachPerFrame( ksPerFrameFn fn, void *ctx );
