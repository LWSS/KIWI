#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// The retained scene - kiwi_scene.h has the model.

#include "stdafx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include "qe3.h"                  // selbrush_t, brush_t, patch_t, entity_s, the display lists
#include "kiwi_ported.h"          // Sys_Printf, Materialdef_GetName, world_entity
#include "kiwi_walkcache.h"       // KiwiWalkCache_StructEpoch (entity class changes)
#include "kiwi_str.h"             // KiwiStr_ContainsNoCase
#include "kiwi_gpu.h"
#include "kiwi_scene.h"
#include <universal/profile.h>

extern void Brush_UpdateLayerVis( selbrush_t *b, const char *layerPrefix );   // layers.cpp

// r_ed_scene.cpp: queue one retained batch into the open editor flush window - over an editor
// VB page, or over a vertex buffer of the scene's own.
void Editor_AddBatchCmd( Material *material, int techType, int sortKey, unsigned vbPage,
                         const KiwiGpuIndexBuffer *ib, int firstIndex, int triCount, int maxVert );
void Editor_AddVertexBatchCmd( Material *material, int techType, int sortKey,
                               const KiwiGpuVertexBuffer *vb, int vertDecl, int stride,
                               const KiwiGpuIndexBuffer *ib, int firstIndex, int triCount,
                               int maxVert );

unsigned Editor_ModelInstVersion( int instanceHandle );   // r_ed_scene.cpp

// A run of scene-owned vertices (KiwiScene_AddRun), owned by the record whose describe made it.
struct ksRun_t
{
    ksRunKey_t key;
    int        page;
    int        first;
    int        count;
};

struct ksBrushOut_t
{
    std::vector<ksLayer_t> layers;
    std::vector<int>       perFrame;
    std::vector<ksRun_t>   runs;                  // the runs this describe asked for
    std::vector<ksRun_t>  *previous = nullptr;    // the record's runs from its last describe
    bool                   hasBounds = false;     // KiwiScene_SetBounds
    float                  mins[3];
    float                  maxs[3];
    std::vector<uint8_t>   lines;                 // KiwiScene_AddLines
    std::vector<uint8_t>   lineWidths;
    std::vector<uint8_t>   lineDims;
    int                    lineSegs  = 0;
};

void KiwiScene_AddLines( ksBrushOut_t *out, const void *verts, int stride, const uint8_t *widths,
                         const uint8_t *dimensions, int segCount )
{
    if ( segCount <= 0 || !verts || stride <= 0 || !widths || !dimensions )
        return;
    const uint8_t *src = (const uint8_t *)verts;
    out->lines.insert( out->lines.end(), src, src + (size_t)stride * 2 * (size_t)segCount );
    out->lineWidths.insert( out->lineWidths.end(), widths, widths + segCount );
    out->lineDims.insert( out->lineDims.end(), dimensions, dimensions + segCount );
    out->lineSegs += segCount;
}

void KiwiScene_SetBounds( ksBrushOut_t *out, const float mins[3], const float maxs[3] )
{
    out->hasBounds = true;
    for ( int k = 0; k < 3; ++k )
    {
        out->mins[k] = mins[k];
        out->maxs[k] = maxs[k];
    }
}

void KiwiScene_AddLayer( ksBrushOut_t *out, const ksLayer_t &layer )
{
    out->layers.push_back( layer );
}

void KiwiScene_AddPerFrame( ksBrushOut_t *out, int faceIndex )
{
    out->perFrame.push_back( faceIndex );
}

namespace
{
    const float KS_CELL_SIZE     = 1024.0f;
    // brushFlags bits FilterBrush, DrawBrush and the outliner read: filtered, layer-hidden,
    // hidden, fdhide, fdshow, frozen.
    const int   KS_FLAG_MASK     = 0x3F;
    const int   KS_HIDDEN_BIT    = 4;
    // One draw's triangle ceiling, well inside every D3D9 device's MaxPrimitiveCount.
    const int   KS_MAX_DRAW_TRIS = 1 << 19;
    // A batch whose visible cells would split into more runs than this draws whole: a draw
    // call costs more than the triangles the cull saves.
    const int   KS_MAX_RUNS_PER_BATCH = 8;

    enum ksList_t { KS_LIST_ACTIVE, KS_LIST_SELECTED, KS_LIST_FILTERED, KS_LIST_COUNT };

    selbrush_t *ListHead( int list )
    {
        switch ( list )
        {
        case KS_LIST_ACTIVE:   return &active_brushes;
        case KS_LIST_SELECTED: return &selected_brushes;
        default:               return &filtered_brushes;
        }
    }

    // What the per-frame scan compares: the node's own fields plus its def's version - the
    // pair Brush_CheckBuildFaceVis keys on - and the patch versions.  Equal = the brush's
    // description, facts and cell still stand.
    struct ksStamp_t
    {
        const brush_t   *def;
        const faceVis_s *faces;
        const patch_t   *patch;
        const entity_s  *owner;
        int              faceCount;
        int              flags;
        short            instVersion;
        short            defVersion;
        short            patchVersion;
        short            patchDefVersion;
        // A model entity's mesh: its instance, that instance's placement version, and the
        // entity def's model version (Model_SetModel's gate) - a model or angle key edit
        // moves no brush version.
        int              modelInst;
        unsigned         modelInstVersion;
        unsigned short   entModelVersion;
        // A placed prefab's contents (kept whole by the camera's describe).  Its spawnflags
        // move the def version (SetKeyValue).
        const void      *prefab;

        bool operator==( const ksStamp_t &o ) const
        {
            return def == o.def && faces == o.faces && patch == o.patch && owner == o.owner
                && faceCount == o.faceCount && flags == o.flags && instVersion == o.instVersion
                && defVersion == o.defVersion && patchVersion == o.patchVersion
                && patchDefVersion == o.patchDefVersion && modelInst == o.modelInst
                && modelInstVersion == o.modelInstVersion && entModelVersion == o.entModelVersion
                && prefab == o.prefab;
        }
    };

    void StampOf( const selbrush_t *b, ksStamp_t *s )
    {
        const brush_t *def = b->def;
        s->def             = def;
        s->faces           = b->faces;
        s->patch           = b->patch;
        s->owner           = b->owner;
        s->faceCount       = b->faceCount;
        s->flags           = b->brushFlags & KS_FLAG_MASK;
        s->instVersion     = b->version;
        s->defVersion      = def ? def->version : 0;
        s->patchVersion    = b->patch ? b->patch->version : 0;
        s->patchDefVersion = ( b->patch && b->patch->def ) ? b->patch->def->version : 0;
        // Worldspawn's def version moves with layer and group edits that change no world
        // brush's drawing; only an entity's own brushes read it.
        const entity_s     *owner = b->owner != world_entity ? b->owner : nullptr;
        const entity_s_def *eDef  = owner ? (const entity_s_def *)owner->def : nullptr;
        s->modelInst        = owner ? owner->modelInst : 0;
        s->modelInstVersion = s->modelInst ? Editor_ModelInstVersion( s->modelInst ) : 0;
        s->entModelVersion  = eDef ? (unsigned short)eDef->version_prob_wrong : 0;
        s->prefab           = owner ? owner->prefab : nullptr;
    }

    // The fields every edit path changes TOGETHER with a version, so the scan leaves them to
    // the verifier (RADIANT_SCENE_VERIFY=1), and the entity class, which changes only with a
    // structural edit (kiwi_walkcache.h) and is compared when the structure epoch moves.
    struct ksDeep_t
    {
        const face_t       *defFaces;
        int                 defFaceCount;
        const void         *patchVis;
        const eclass_t     *eclass;
        unsigned long long  handles;    // the resident vertex handles the batches index
    };

    // Every editor-VB handle the brush's resident geometry lives at.  A batch indexes these
    // slots, so they must not move without the stamp moving (the verifier checks it).
    unsigned long long HandlesOf( const selbrush_t *b )
    {
        unsigned long long h = 1469598103934665603ull;
        if ( b->faces )
            for ( int f = 0; f < b->faceCount; ++f )
            {
                const faceVis_s &fv = ( (const faceVis_s *)b->faces )[f];
                for ( int i = 0; i < fv.visCount && fv.visArray; ++i )
                    h = ( h ^ (unsigned)fv.visArray[i].vertHandle ) * 1099511628211ull;
            }
        if ( b->patch && b->patch->visArray )
            for ( int i = 0; i < b->patch->visCount; ++i )
                h = ( h ^ (unsigned)b->patch->visArray[i].vertHandle ) * 1099511628211ull;
        return h;
    }

    const eclass_t *EClassOf( const selbrush_t *b )
    {
        const entity_s_def *eDef = b->owner ? (const entity_s_def *)b->owner->def : nullptr;
        return eDef ? eDef->eclass : nullptr;
    }

    void DeepOf( const selbrush_t *b, ksDeep_t *d )
    {
        d->defFaces     = b->def ? b->def->faces : nullptr;
        d->defFaceCount = b->def ? b->def->faceCount : 0;
        d->patchVis     = b->patch ? b->patch->visArray : nullptr;
        d->eclass       = EClassOf( b );
        d->handles      = HandlesOf( b );
    }

    struct ksFacts_t
    {
        const entity_s *owner  = nullptr;
        unsigned        cls    = 0;
        bool            hidden = false;
    };

    struct ksRec_t
    {
        selbrush_t      *node     = nullptr;   // nullptr = dropped, waiting for Resync to free it
        int              list     = KS_LIST_ACTIVE;
        int              slot     = -1;        // its entry in s_slots
        ksDeep_t         deep     = {};
        ksFacts_t        facts;
        bool             counted  = false;     // `facts` are in the entity counters
        int              cell     = -1;
        int              cellSlot = -1;        // index in that cell's member list
        bool             dirty    = false;
        unsigned         seen     = 0;         // the Resync pass that last found it
        std::vector<int> perFrame;             // face indices; -1 = the whole brush
        unsigned         windows  = 0;         // bit per ksWindow_t its last describe fed
        std::vector<ksRun_t> runs;             // the scene-owned vertices its layers use
        bool             drawBounds = false;   // its describe stated what it draws spans
        float            drawMins[3] = {};
        float            drawMaxs[3] = {};
        std::vector<uint8_t> lines;            // its lines (KiwiScene_AddLines)
        std::vector<uint8_t> lineWidths;
        std::vector<uint8_t> lineDims;
        int              lineSegs   = 0;
    };

    // The list-ordered copy the scan walks: contiguous, one entry per node.
    struct ksSlot_t
    {
        selbrush_t *node;
        int         rec;
        int         list;
        ksStamp_t   stamp;
    };

    // One batch's share of one cell: absolute VB indices.
    struct ksPart_t
    {
        int                   batch;
        std::vector<uint16_t> indices;
        int                   maxVert;
    };

    struct ksCell_t
    {
        std::vector<int>      members;         // record ids
        std::vector<ksPart_t> parts;
        float                 mins[3] = { 0.0f, 0.0f, 0.0f };
        float                 maxs[3] = { 0.0f, 0.0f, 0.0f };
        bool                  hasBounds   = false;
        bool                  boundsDirty = true;    // RefreshBounds before reading mins/maxs
        bool                  dirty = false;         // the camera re-describes it (RebuildCell)
        unsigned              gen   = 1;             // moves whenever a member or the set does
        unsigned long long    morton = 0;            // its grid position on a Z-order curve
    };

    struct ksBatchKey_t
    {
        int       window;
        Material *material;
        int       techType;
        int       sortKey;
        unsigned  vbPage;

        bool operator==( const ksBatchKey_t &o ) const
        {
            return window == o.window && material == o.material && techType == o.techType
                && sortKey == o.sortKey && vbPage == o.vbPage;
        }
    };

    struct ksBatchKeyHash
    {
        size_t operator()( const ksBatchKey_t &k ) const
        {
            size_t h = (size_t)(uintptr_t)k.material;
            h = h * 31u + (size_t)k.window;
            h = h * 31u + (size_t)k.techType;
            h = h * 31u + (size_t)k.sortKey;
            h = h * 31u + (size_t)k.vbPage;
            return h;
        }
    };

    // Where one cell's indices sit inside its batch's buffer.
    struct ksRange_t
    {
        int cell;
        int first;
        int count;
    };

    struct ksBatch_t
    {
        ksBatchKey_t           key;
        std::vector<int>       cells;          // cells holding a part of this batch
        std::vector<ksRange_t> ranges;         // in buffer order
        bool                   dirty      = false;
        KiwiGpuIndexBuffer    *ib         = nullptr;
        int                    indexCount = 0;
        int                    maxVert    = 0;
    };

    std::vector<ksRec_t>   s_recs;
    std::vector<int>       s_freeRecs;
    std::vector<ksSlot_t>  s_slots;
    std::vector<int>       s_dirtyRecs;
    std::unordered_map<const selbrush_t *, int> s_recByNode;
    unsigned               s_resyncPass = 0;
    bool                   s_recordsFresh = false;   // SyncRecords ran this editor frame...
    unsigned               s_scannedEpoch = 0;       // ...at this structure epoch

    std::unordered_map<const entity_s *, ksEntityCounts_t> s_counts;
    unsigned               s_factsGen = 1;

    std::vector<ksCell_t>  s_cells;
    std::unordered_map<unsigned long long, int> s_cellIndex;
    std::vector<int>       s_dirtyCells;

    std::vector<ksBatch_t> s_batches;
    std::unordered_map<ksBatchKey_t, int, ksBatchKeyHash> s_batchIndex;
    std::vector<int>       s_dirtyBatches;
    std::vector<int>       s_windowBatches[KS_WINDOW_COUNT];   // non-empty batches, by window
    bool                   s_windowsDirty = false;

    std::vector<int>       s_perFrameRecs;     // records with per-frame entries, in list order
    bool                   s_perFrameDirty = false;

    unsigned long long     s_sceneKey    = 0;
    bool                   s_haveKey     = false;
    unsigned               s_structEpoch = 0;
    // Moves on every InvalidateAll: cell ids and generations restart, so no region
    // generation taken before can match one taken after.
    unsigned long long     s_resetSerial = 0;

    std::vector<uint16_t>  s_staging;

    // ── scene-owned vertex pages (KiwiScene_AddRun) ──────────────────────────
    // A page holds one 16-bit index window, like an editor VB page.  A run handle sets
    // KS_RUN_HANDLE over (page << 16 | first); a batch over a scene page sets KS_SCENE_PAGE in
    // its key's vbPage, so it never merges with an editor page of the same number.
    const int      KS_PAGE_VERTS = 65536;
    const unsigned KS_RUN_HANDLE = 0x80000000u;
    const unsigned KS_SCENE_PAGE = 0x80000000u;

    struct ksPage_t
    {
        KiwiGpuVertexBuffer             *vb       = nullptr;
        int                              stride   = 0;
        int                              vertDecl = 0;
        int                              top      = 0;     // first never-used vertex
        std::vector<std::pair<int, int>> free;             // {first, count}, sorted by first
    };
    std::vector<ksPage_t>  s_pages;
    std::vector<uint8_t>   s_runStaging;

    bool AllocRun( int stride, int vertDecl, int count, int *outPage, int *outFirst )
    {
        if ( count <= 0 || count > KS_PAGE_VERTS )
            return false;
        for ( int p = 0; p < (int)s_pages.size(); ++p )
        {
            ksPage_t &pg = s_pages[p];
            if ( pg.stride != stride || pg.vertDecl != vertDecl )
                continue;
            for ( size_t k = 0; k < pg.free.size(); ++k )
                if ( pg.free[k].second >= count )
                {
                    *outPage  = p;
                    *outFirst = pg.free[k].first;
                    pg.free[k].first  += count;
                    pg.free[k].second -= count;
                    if ( !pg.free[k].second )
                        pg.free.erase( pg.free.begin() + (ptrdiff_t)k );
                    return true;
                }
            if ( pg.top + count <= KS_PAGE_VERTS )
            {
                *outPage  = p;
                *outFirst = pg.top;
                pg.top   += count;
                return true;
            }
        }
        if ( (int)s_pages.size() >= 0x7FFF )
            return false;
        ksPage_t pg;
        pg.vb = KiwiGpu_CreateVertexBuffer( nullptr, stride, KS_PAGE_VERTS );
        if ( !pg.vb )
            return false;
        pg.stride   = stride;
        pg.vertDecl = vertDecl;
        pg.top      = count;
        s_pages.push_back( pg );
        *outPage  = (int)s_pages.size() - 1;
        *outFirst = 0;
        return true;
    }

    void FreeRun( const ksRun_t &run )
    {
        if ( run.count <= 0 || run.page < 0 || run.page >= (int)s_pages.size() )
            return;
        std::vector<std::pair<int, int>> &free = s_pages[run.page].free;
        std::vector<std::pair<int, int>>::iterator it =
            std::lower_bound( free.begin(), free.end(), std::make_pair( run.first, 0 ) );
        it = free.insert( it, std::make_pair( run.first, run.count ) );
        // Merge with the neighbours it touches.
        std::vector<std::pair<int, int>>::iterator next = it + 1;
        if ( next != free.end() && it->first + it->second == next->first )
        {
            it->second += next->second;
            free.erase( next );
        }
        if ( it != free.begin() )
        {
            std::vector<std::pair<int, int>>::iterator prev = it - 1;
            if ( prev->first + prev->second == it->first )
            {
                prev->second += it->second;
                free.erase( it );
            }
        }
    }

    void ReleasePages()
    {
        for ( ksPage_t &pg : s_pages )
            KiwiGpu_ReleaseVertexBuffer( pg.vb );
        s_pages.clear();
    }
    std::vector<signed char> s_cellVisible;    // KiwiScene_Emit's per-cell answer

    bool VerifyEnabled()
    {
        static const bool s_on = []{ const char *e = getenv( "RADIANT_SCENE_VERIFY" );
                                     return e && *e && *e != '0'; }();
        return s_on;
    }

    // ── facts ────────────────────────────────────────────────────────────────

    ksFacts_t FactsOf( const selbrush_t *b )
    {
        ksFacts_t f;
        f.owner  = b->owner;
        f.cls    = KiwiScene_ClassifyBrush( b );
        f.hidden = ( b->brushFlags & KS_HIDDEN_BIT ) != 0;
        return f;
    }

    void CountFacts( const ksFacts_t &f, int sign )
    {
        if ( !f.owner )
            return;
        ksEntityCounts_t &c = s_counts[f.owner];   // value-initialised to zeros when new
        const bool terrain = ( f.cls & KS_CLASS_TERRAIN ) != 0;
        const bool grid    = ( f.cls & KS_CLASS_LIGHTGRID ) != 0;
        const bool pass[KS_FILTER_COUNT] = { true, !terrain && !grid, terrain, grid };
        for ( int k = 0; k < KS_FILTER_COUNT; ++k )
            if ( pass[k] )
            {
                c.brushes[k] += sign;
                if ( f.hidden )
                    c.hidden[k] += sign;
            }
    }

    void UpdateFacts( ksRec_t &r )
    {
        const ksFacts_t now = FactsOf( r.node );
        if ( r.counted && now.owner == r.facts.owner && now.cls == r.facts.cls
          && now.hidden == r.facts.hidden )
            return;
        if ( r.counted )
            CountFacts( r.facts, -1 );
        CountFacts( now, +1 );
        r.facts   = now;
        r.counted = true;
        ++s_factsGen;
    }

    // ── records ──────────────────────────────────────────────────────────────

    void MarkRecDirty( int id )
    {
        if ( !s_recs[id].dirty )
        {
            s_recs[id].dirty = true;
            s_dirtyRecs.push_back( id );
        }
    }

    void MarkCellDirty( int c )
    {
        ++s_cells[c].gen;
        s_cells[c].boundsDirty = true;
        if ( !s_cells[c].dirty )
        {
            s_cells[c].dirty = true;
            s_dirtyCells.push_back( c );
        }
    }

    bool BoundsOf( const selbrush_t *b, float mins[3], float maxs[3] );

    // The union of the members' boxes.  A member with no box makes the cell unbounded (every
    // test passes it): nothing is ever culled on a box nobody could compute.
    void RefreshBounds( ksCell_t &cell )
    {
        if ( !cell.boundsDirty )
            return;
        cell.boundsDirty = false;
        cell.hasBounds   = false;
        bool first = true;
        for ( int id : cell.members )
        {
            float mins[3], maxs[3];
            const ksRec_t &r = s_recs[id];
            if ( r.drawBounds )
            {
                // A model's mesh reaches past its entity box (KiwiScene_SetBounds).
                for ( int k = 0; k < 3; ++k )
                {
                    mins[k] = r.drawMins[k];
                    maxs[k] = r.drawMaxs[k];
                }
            }
            else if ( !BoundsOf( r.node, mins, maxs ) )
                return;                         // unbounded
            for ( int k = 0; k < 3; ++k )
            {
                if ( first || mins[k] < cell.mins[k] ) cell.mins[k] = mins[k];
                if ( first || maxs[k] > cell.maxs[k] ) cell.maxs[k] = maxs[k];
            }
            first = false;
        }
        cell.hasBounds = !first;
    }

    void MarkBatchDirty( int id )
    {
        if ( !s_batches[id].dirty )
        {
            s_batches[id].dirty = true;
            s_dirtyBatches.push_back( id );
        }
    }

    // A brush's world box.  A patch's comes from what is DRAWN: its tessellated render mesh
    // (curveDef), else its control points.  The symbiont brush's own bounds are not the
    // mesh's - on london.map terrain meshes reached well outside them, and culling on them
    // dropped visible ground.  False = no usable box (no windings or mesh yet).
    bool BoundsOf( const selbrush_t *b, float mins[3], float maxs[3] )
    {
        const brush_t *def = b->def;
        if ( !def )
            return false;
        const patchMesh_t *pm = def->patch;
        const curvePatchDef_t *mesh = pm ? pm->curveDef : nullptr;
        if ( mesh && mesh->verts && mesh->width > 0 && mesh->height > 0 )
        {
            const int n = mesh->width * mesh->height;
            for ( int k = 0; k < 3; ++k )
                mins[k] = maxs[k] = mesh->verts[0].xyz[k];
            for ( int i = 1; i < n; ++i )
                for ( int k = 0; k < 3; ++k )
                {
                    const float v = mesh->verts[i].xyz[k];
                    if ( v < mins[k] ) mins[k] = v;
                    if ( v > maxs[k] ) maxs[k] = v;
                }
            return true;
        }
        if ( pm )
        {
            if ( pm->width <= 0 || pm->height <= 0 || pm->width > 16 || pm->height > 16 )
                return false;                   // no mesh and no readable control grid
            for ( int k = 0; k < 3; ++k )
            {
                mins[k] = pm->ctrl[0][0].xyz[k];
                maxs[k] = pm->ctrl[0][0].xyz[k];
            }
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                    for ( int k = 0; k < 3; ++k )
                    {
                        const float v = pm->ctrl[col][row].xyz[k];
                        if ( v < mins[k] ) mins[k] = v;
                        if ( v > maxs[k] ) maxs[k] = v;
                    }
            return true;
        }
        if ( def->mins[0] > def->maxs[0] || def->mins[1] > def->maxs[1] || def->mins[2] > def->maxs[2] )
            return false;
        for ( int k = 0; k < 3; ++k )
        {
            mins[k] = def->mins[k];
            maxs[k] = def->maxs[k];
        }
        return true;
    }

    // The cell holding the centre of the brush bounds.
    int CellFor( const selbrush_t *b )
    {
        float c[3] = { 0.0f, 0.0f, 0.0f };
        float mins[3], maxs[3];
        if ( BoundsOf( b, mins, maxs ) )
        {
            for ( int k = 0; k < 3; ++k )
                c[k] = 0.5f * ( mins[k] + maxs[k] );
        }
        unsigned long long key = 0, morton = 0;
        for ( int k = 0; k < 3; ++k )
        {
            const long long q = (long long)floorf( c[k] / KS_CELL_SIZE );
            const unsigned long long u = (unsigned long long)( ( q + 0x100000 ) & 0x1FFFFF );
            key |= u << ( 21 * k );
            for ( int bit = 0; bit < 21; ++bit )             // interleave: x0 y0 z0 x1 y1 z1 ...
                morton |= ( ( u >> bit ) & 1ull ) << ( 3 * bit + k );
        }
        std::unordered_map<unsigned long long, int>::const_iterator it = s_cellIndex.find( key );
        if ( it != s_cellIndex.end() )
            return it->second;
        s_cells.emplace_back();
        const int id = (int)s_cells.size() - 1;
        s_cells[id].morton = morton;
        s_cellIndex[key] = id;
        return id;
    }

    void CellAdd( int recId, int c )
    {
        ksRec_t &r = s_recs[recId];
        r.cell     = c;
        r.cellSlot = (int)s_cells[c].members.size();
        s_cells[c].members.push_back( recId );
        MarkCellDirty( c );
    }

    void CellRemove( int recId )
    {
        ksRec_t &r = s_recs[recId];
        if ( r.cell < 0 )
            return;
        std::vector<int> &m = s_cells[r.cell].members;
        const int last = m.back();
        m[r.cellSlot] = last;
        s_recs[last].cellSlot = r.cellSlot;
        m.pop_back();
        MarkCellDirty( r.cell );
        r.cell     = -1;
        r.cellSlot = -1;
    }

    int NewRec( selbrush_t *b )
    {
        int id;
        if ( !s_freeRecs.empty() )
        {
            id = s_freeRecs.back();
            s_freeRecs.pop_back();
        }
        else
        {
            s_recs.emplace_back();
            id = (int)s_recs.size() - 1;
        }
        s_recs[id] = ksRec_t();
        s_recs[id].node = b;
        s_recByNode[b] = id;
        return id;
    }

    // Detach a record from its node, cell and counters.  Its id stays out of the free list
    // until the next Resync drops it from the slots, so an id is never listed twice.
    void DropRec( int id )
    {
        ksRec_t &r = s_recs[id];
        CellRemove( id );
        for ( const ksRun_t &run : r.runs )
            FreeRun( run );
        r.runs.clear();
        r.lines.clear();
        r.lineWidths.clear();
        r.lineDims.clear();
        r.lineSegs = 0;
        if ( r.counted )
        {
            CountFacts( r.facts, -1 );
            ++s_factsGen;
        }
        if ( !r.perFrame.empty() )
            s_perFrameDirty = true;
        if ( r.node )
            s_recByNode.erase( r.node );
        // Poison its slot: a new instance linked at the same address and list position must
        // still read as "the list changed", never as this record.
        if ( r.slot >= 0 && r.slot < (int)s_slots.size() && s_slots[r.slot].rec == id )
            s_slots[r.slot].node = nullptr;
        r = ksRec_t();
    }

    // A list was relinked (select, deselect, create, delete, undo, prefab enter/leave, a map
    // swap): match nodes to records by address, add the new ones, drop the ones that left.
    // A node that was FREED lost its record in KiwiScene_NodeFreed already, so a new
    // instance at a recycled address always starts a new record.
    void Resync()
    {
        PROF_SCOPED( "scene resync" );
        const unsigned pass = ++s_resyncPass;
        std::vector<ksSlot_t> slots;
        slots.reserve( s_slots.size() + 64 );
        for ( int list = 0; list < KS_LIST_COUNT; ++list )
        {
            selbrush_t *head = ListHead( list );
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                Brush_UpdateLayerVis( b, "" );
                ksSlot_t s;
                s.node = b;
                s.list = list;
                std::unordered_map<const selbrush_t *, int>::const_iterator it = s_recByNode.find( b );
                if ( it != s_recByNode.end() && s_recs[it->second].seen != pass )
                {
                    s.rec   = it->second;
                    s.stamp = s_slots[s_recs[s.rec].slot].stamp;
                    ksStamp_t now;
                    StampOf( b, &now );
                    if ( !( now == s.stamp ) || s_recs[s.rec].list != list )
                        MarkRecDirty( s.rec );
                }
                else
                {
                    s.rec = NewRec( b );
                    StampOf( b, &s.stamp );
                    MarkRecDirty( s.rec );
                }
                ksRec_t &r = s_recs[s.rec];
                r.seen = pass;
                r.list = list;
                r.slot = (int)slots.size();
                slots.push_back( s );
            }
        }
        // Records no list holds any more (unlinked, or dropped by NodeFreed) are released.
        for ( const ksSlot_t &old : s_slots )
            if ( s_recs[old.rec].seen != pass )
            {
                DropRec( old.rec );
                s_freeRecs.push_back( old.rec );
            }
        s_slots.swap( slots );
        s_perFrameDirty = true;
    }

    // Walk the lists beside the slots.  A node out of step means a list changed; Resync
    // takes over.  Both refresh each brush's layer bits first (a compare against the layer
    // generation when nothing moved): the binary's XY DrawBrush did that for every brush
    // each frame, every FilterBrush caller relies on it, and XY_DrawBrushes now culls.
    void ScanLists()
    {
        PROF_SCOPED( "scene scan" );
        const unsigned structEpoch = KiwiWalkCache_StructEpoch();
        const bool readClass = structEpoch != s_structEpoch;
        s_structEpoch = structEpoch;
        const bool verify = VerifyEnabled();

        size_t i = 0;
        for ( int list = 0; list < KS_LIST_COUNT; ++list )
        {
            selbrush_t *head = ListHead( list );
            for ( selbrush_t *b = head->next; b && b != head; b = b->next, ++i )
            {
                if ( i >= s_slots.size() || s_slots[i].node != b || s_slots[i].list != list )
                {
                    Resync();
                    return;
                }
                const ksSlot_t &s = s_slots[i];
                Brush_UpdateLayerVis( b, "" );
                ksStamp_t now;
                StampOf( b, &now );
                if ( !( now == s.stamp ) )
                {
                    MarkRecDirty( s.rec );
                    continue;
                }
                if ( !readClass && !verify )
                    continue;
                ksRec_t &r = s_recs[s.rec];
                ksDeep_t deep;
                DeepOf( b, &deep );
                if ( deep.eclass != r.deep.eclass )
                    MarkRecDirty( s.rec );
                else if ( verify && ( deep.defFaces != r.deep.defFaces
                                   || deep.defFaceCount != r.deep.defFaceCount
                                   || deep.patchVis != r.deep.patchVis
                                   || deep.handles != r.deep.handles ) )
                {
                    Sys_Printf( "KiwiScene verify: brush %p (list %d) changed with no version "
                                "change: def faces %p -> %p, face count %d -> %d, patch visuals "
                                "%p -> %p, VB handles %s (versions def %d inst %d).\n",
                                (const void *)b, list, (const void *)r.deep.defFaces,
                                (const void *)deep.defFaces, r.deep.defFaceCount,
                                deep.defFaceCount, r.deep.patchVis, deep.patchVis,
                                deep.handles != r.deep.handles ? "MOVED" : "same",
                                (int)now.defVersion, (int)now.instVersion );
                    MarkRecDirty( s.rec );
                }
            }
        }
        if ( i != s_slots.size() )
            Resync();
    }

    // ── the camera world ────────────────────────────────────────────────────

    int BatchFor( const ksBatchKey_t &key )
    {
        std::unordered_map<ksBatchKey_t, int, ksBatchKeyHash>::const_iterator it = s_batchIndex.find( key );
        if ( it != s_batchIndex.end() )
            return it->second;
        s_batches.emplace_back();
        const int id = (int)s_batches.size() - 1;
        s_batches[id].key = key;
        s_batchIndex[key] = id;
        return id;
    }

    // A refused layer is dropped and reported, as the old path drops and reports a bad mesh
    // surf (Editor_MeshSurfBadIndex): drawing it would be worse than not drawing it.
    const int KS_MAX_REFUSAL_REPORTS = 8;
    int       s_refusalReports       = 0;

    void RefuseLayer( const ksLayer_t &l, const char *why )
    {
        if ( s_refusalReports >= KS_MAX_REFUSAL_REPORTS )
            return;
        ++s_refusalReports;
        Sys_Printf( "KiwiScene: layer refused (%s): handle %08x, %d verts, %d indices\n", why,
                    l.vertHandle, l.vertCount, l.indexCount );
    }

    // A layer's indices, rebased onto its VB page.  Refuses anything the run cannot hold: an
    // index outside [0, vertCount) would draw a triangle into unrelated geometry sharing the
    // page (r_ed_scene.cpp, "THE MESH-SURF INDEX INVARIANT").
    void AppendLayer( ksCell_t &cell, const ksLayer_t &l )
    {
        const bool     sceneRun = ( l.vertHandle & KS_RUN_HANDLE ) != 0;
        const unsigned page     = sceneRun ? ( KS_SCENE_PAGE | ( ( l.vertHandle >> 16 ) & 0x7FFFu ) )
                                           : ( l.vertHandle >> 16 );
        const int      first    = (int)( l.vertHandle & 0xFFFF );
        if ( !l.material || !page || l.vertCount < 3 || first + l.vertCount - 1 > 0xFFFF )
        {
            RefuseLayer( l, "no run" );
            return;
        }
        if ( l.indices )
        {
            if ( l.indexCount < 3 || l.indexCount % 3 )
            {
                RefuseLayer( l, "index count" );
                return;
            }
            for ( int k = 0; k < l.indexCount; ++k )
                if ( (int)l.indices[k] >= l.vertCount )
                {
                    RefuseLayer( l, "index outside the run" );
                    return;
                }
        }

        ksBatchKey_t key;
        key.window   = (int)l.window;
        key.material = l.material;
        key.techType = l.techType;
        key.sortKey  = l.sortKey;
        key.vbPage   = page;
        const int batch = BatchFor( key );

        ksPart_t *part = nullptr;
        for ( ksPart_t &p : cell.parts )
            if ( p.batch == batch )
            {
                part = &p;
                break;
            }
        if ( !part )
        {
            cell.parts.emplace_back();
            part = &cell.parts.back();
            part->batch   = batch;
            part->maxVert = 0;
        }

        std::vector<uint16_t> &out = part->indices;
        if ( l.indices )
        {
            for ( int k = 0; k < l.indexCount; ++k )
                out.push_back( (uint16_t)( first + l.indices[k] ) );
        }
        else
        {
            // Editor_AddGeoFace's front fan: triangle t = { t+1, t+2, 0 }.
            for ( int t = 0; t + 2 < l.vertCount; ++t )
            {
                out.push_back( (uint16_t)( first + t + 1 ) );
                out.push_back( (uint16_t)( first + t + 2 ) );
                out.push_back( (uint16_t)first );
            }
        }
        if ( first + l.vertCount - 1 > part->maxVert )
            part->maxVert = first + l.vertCount - 1;
    }

    void RebuildCell( int c, ksDescribeFn describe )
    {
        std::vector<ksPart_t> oldParts;
        oldParts.swap( s_cells[c].parts );
        s_cells[c].dirty       = false;
        s_cells[c].boundsDirty = true;          // the describe may build bounds it lacked

        ksBrushOut_t out;
        for ( int id : s_cells[c].members )
        {
            ksRec_t &r = s_recs[id];
            out.layers.clear();
            out.perFrame.clear();
            out.runs.clear();
            out.previous  = &r.runs;
            out.hasBounds = false;
            out.lines.clear();
            out.lineWidths.clear();
            out.lineDims.clear();
            out.lineSegs  = 0;
            describe( r.node, &out );
            r.lines.swap( out.lines );
            r.lineWidths.swap( out.lineWidths );
            r.lineDims.swap( out.lineDims );
            r.lineSegs = out.lineSegs;
            r.drawBounds = out.hasBounds;
            for ( int k = 0; k < 3 && out.hasBounds; ++k )
            {
                r.drawMins[k] = out.mins[k];
                r.drawMaxs[k] = out.maxs[k];
            }
            // The runs this describe did not ask for again are free; the rest are its now.
            for ( const ksRun_t &run : r.runs )
                FreeRun( run );
            r.runs.swap( out.runs );
            out.previous = nullptr;
            // After the describe: it builds the brush's resident geometry, which syncs the
            // instance version the slot's stamp records - and a patch's first visuals, which
            // appear with no version change (Patch_Fill_SyncVersion builds on a null array).
            StampOf( r.node, &s_slots[r.slot].stamp );
            DeepOf( r.node, &r.deep );
            if ( r.perFrame != out.perFrame )
            {
                r.perFrame = out.perFrame;
                s_perFrameDirty = true;
            }
            r.windows = 0;
            for ( const ksLayer_t &l : out.layers )
            {
                AppendLayer( s_cells[c], l );
                r.windows |= 1u << l.window;
            }
        }

        // Batch membership: every batch this cell fed before or feeds now is rebuilt.
        for ( const ksPart_t &p : oldParts )
        {
            bool kept = false;
            for ( const ksPart_t &q : s_cells[c].parts )
                if ( q.batch == p.batch )
                {
                    kept = true;
                    break;
                }
            if ( !kept )
            {
                std::vector<int> &cells = s_batches[p.batch].cells;
                for ( size_t k = 0; k < cells.size(); ++k )
                    if ( cells[k] == c )
                    {
                        cells[k] = cells.back();
                        cells.pop_back();
                        break;
                    }
            }
            MarkBatchDirty( p.batch );
        }
        for ( const ksPart_t &q : s_cells[c].parts )
        {
            bool had = false;
            for ( const ksPart_t &p : oldParts )
                if ( p.batch == q.batch )
                {
                    had = true;
                    break;
                }
            if ( !had )
                s_batches[q.batch].cells.push_back( c );
            MarkBatchDirty( q.batch );
        }
    }

    // Concatenate the batch's parts, one range per cell, and upload the buffer.  Cells go in
    // Z-order, so the cells a view sees tend to sit next to each other in the buffer and
    // draw as few runs.
    void RebuildBatch( int id )
    {
        ksBatch_t &bt = s_batches[id];
        bt.dirty = false;
        std::sort( bt.cells.begin(), bt.cells.end(),
                   []( int a, int b ) { return s_cells[a].morton < s_cells[b].morton; } );
        bt.ranges.clear();
        s_staging.clear();
        int maxVert = 0;
        for ( int c : bt.cells )
            for ( const ksPart_t &p : s_cells[c].parts )
            {
                if ( p.batch != id || p.indices.empty() )
                    continue;
                ksRange_t r;
                r.cell  = c;
                r.first = (int)s_staging.size();
                r.count = (int)p.indices.size();
                bt.ranges.push_back( r );
                s_staging.insert( s_staging.end(), p.indices.begin(), p.indices.end() );
                if ( p.maxVert > maxVert )
                    maxVert = p.maxVert;
            }

        const bool wasEmpty = bt.indexCount == 0;
        if ( s_staging.empty() )
        {
            KiwiGpu_ReleaseIndexBuffer( bt.ib );
            bt.ib         = nullptr;
            bt.indexCount = 0;
        }
        else
        {
            bt.ib         = KiwiGpu_ReplaceIndexBuffer( bt.ib, s_staging.data(), (int)s_staging.size() );
            bt.indexCount = bt.ib ? (int)s_staging.size() : 0;
            bt.maxVert    = maxVert;
        }
        if ( wasEmpty != ( bt.indexCount == 0 ) )
            s_windowsDirty = true;
    }

    bool CellVisible( int c, const ksFrustum_t *frustum )
    {
        signed char &v = s_cellVisible[c];
        if ( v >= 0 )
            return v != 0;
        ksCell_t &cell = s_cells[c];
        RefreshBounds( cell );
        bool inside = true;
        if ( cell.hasBounds )
        {
            for ( int p = 0; p < frustum->planeCount && inside; ++p )
            {
                const float *pl = frustum->planes[p];
                // The box corner furthest along the plane normal.
                const float x = pl[0] >= 0.0f ? cell.maxs[0] : cell.mins[0];
                const float y = pl[1] >= 0.0f ? cell.maxs[1] : cell.mins[1];
                const float z = pl[2] >= 0.0f ? cell.maxs[2] : cell.mins[2];
                if ( pl[0] * x + pl[1] * y + pl[2] * z < pl[3] )
                    inside = false;
            }
        }
        v = inside ? 1 : 0;
        return inside;
    }

    void EmitRun( const ksBatch_t &bt, int first, int count )
    {
        const int maxIndices = 3 * KS_MAX_DRAW_TRIS;
        for ( int done = 0; done < count; done += maxIndices )
        {
            const int n = ( count - done < maxIndices ) ? count - done : maxIndices;
            if ( bt.key.vbPage & KS_SCENE_PAGE )
            {
                const ksPage_t &pg = s_pages[bt.key.vbPage & ~KS_SCENE_PAGE];
                Editor_AddVertexBatchCmd( bt.key.material, bt.key.techType, bt.key.sortKey, pg.vb,
                                          pg.vertDecl, pg.stride, bt.ib, first + done, n / 3,
                                          bt.maxVert );
            }
            else
                Editor_AddBatchCmd( bt.key.material, bt.key.techType, bt.key.sortKey,
                                    bt.key.vbPage, bt.ib, first + done, n / 3, bt.maxVert );
        }
    }
}

// ── public: records and facts ────────────────────────────────────────────────

unsigned KiwiScene_ClassifyBrush( const selbrush_t *b )
{
    if ( !b || !b->def )
        return 0;
    unsigned cls = 0;
    const eclass_t *ec = EClassOf( b );
    if ( ec && *(const int *)&ec->fixedsize )
        cls |= KS_CLASS_FIXEDSIZE;
    if ( b->owner && b->owner != world_entity )
        cls |= KS_CLASS_ENTITY;
    const brush_t *def = b->def;
    if ( def->patch )
        return cls | ( ( def->patch->type & PATCH_TERRAIN ) ? KS_CLASS_TERRAIN : 0u );
    if ( !def->faces )
        return cls;
    // A brush with any lightgrid face (lightgrid_volume and friends; the same substring the
    // camera's tool colours and kiwi_material.cpp's tool list use).
    for ( int f = 0; f < def->faceCount; ++f )
    {
        MaterialDef *md = const_cast<MaterialDef *>( &def->faces[f].mtldef[0] );
        if ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) != 1 )
            continue;                       // a half-built face has no name to read
        const char *name = (const char *)Materialdef_GetName( md );
        if ( name && KiwiStr_ContainsNoCase( name, "lightgrid" ) )
            return cls | KS_CLASS_LIGHTGRID;
    }
    return cls;
}

unsigned KiwiScene_BrushClass( const selbrush_t *b )
{
    std::unordered_map<const selbrush_t *, int>::const_iterator it = s_recByNode.find( b );
    if ( it != s_recByNode.end() && s_recs[it->second].counted && !s_recs[it->second].dirty )
        return s_recs[it->second].facts.cls;
    return KiwiScene_ClassifyBrush( b );
}

bool KiwiScene_BrushPasses( const selbrush_t *b, ksBrushFilter_t filter )
{
    const unsigned cls = KiwiScene_BrushClass( b );
    switch ( filter )
    {
    case KS_FILTER_PLAIN:     return ( cls & ( KS_CLASS_TERRAIN | KS_CLASS_LIGHTGRID ) ) == 0;
    case KS_FILTER_TERRAIN:   return ( cls & KS_CLASS_TERRAIN ) != 0;
    case KS_FILTER_LIGHTGRID: return ( cls & KS_CLASS_LIGHTGRID ) != 0;
    default:                  return true;
    }
}

const ksEntityCounts_t *KiwiScene_EntityCounts( const entity_s *e )
{
    std::unordered_map<const entity_s *, ksEntityCounts_t>::const_iterator it = s_counts.find( e );
    return it != s_counts.end() ? &it->second : nullptr;
}

unsigned KiwiScene_FactsGeneration()
{
    return s_factsGen;
}

void KiwiScene_BeginFrame()
{
    s_recordsFresh = false;
}

void KiwiScene_SyncRecords()
{
    if ( s_recordsFresh && KiwiWalkCache_StructEpoch() == s_scannedEpoch )
        return;
    if ( !active_brushes.next || !selected_brushes.next || !filtered_brushes.next )
    {
        KiwiScene_InvalidateAll();               // no lists: nothing may stay drawable
        return;
    }
    s_recordsFresh = true;
    s_scannedEpoch = KiwiWalkCache_StructEpoch();
    PROF_SCOPED( "scene records" );
    ScanLists();

    // Changed records: new facts, and the cell their bounds now name (both cells re-describe).
    for ( int id : s_dirtyRecs )
    {
        ksRec_t &r = s_recs[id];
        if ( !r.dirty )
            continue;                           // dropped and recycled within this sync
        r.dirty = false;
        StampOf( r.node, &s_slots[r.slot].stamp );
        DeepOf( r.node, &r.deep );
        UpdateFacts( r );
        if ( r.list == KS_LIST_ACTIVE )
        {
            const int c = CellFor( r.node );
            if ( c != r.cell )
            {
                CellRemove( id );
                CellAdd( id, c );
            }
            else
                MarkCellDirty( c );
        }
        else
        {
            CellRemove( id );
            if ( !r.perFrame.empty() )
            {
                r.perFrame.clear();
                s_perFrameDirty = true;
            }
        }
    }
    s_dirtyRecs.clear();
}

unsigned long long KiwiScene_RegionGeneration( const float mins[3], const float maxs[3] )
{
    KiwiScene_SyncRecords();
    unsigned long long h = 1469598103934665603ull;
    for ( int c = 0; c < (int)s_cells.size(); ++c )
    {
        ksCell_t &cell = s_cells[c];
        RefreshBounds( cell );
        if ( cell.hasBounds
          && ( cell.maxs[0] < mins[0] || cell.mins[0] > maxs[0]
            || cell.maxs[1] < mins[1] || cell.mins[1] > maxs[1]
            || cell.maxs[2] < mins[2] || cell.mins[2] > maxs[2] ) )
            continue;
        if ( !cell.hasBounds && cell.members.empty() )
            continue;
        h = ( h ^ (unsigned long long)c ) * 1099511628211ull;
        h = ( h ^ (unsigned long long)cell.gen ) * 1099511628211ull;
    }
    return h ^ s_resetSerial;
}

void KiwiScene_ActiveOfClass( unsigned classMask, std::vector<selbrush_t *> &out )
{
    out.clear();
    KiwiScene_SyncRecords();
    for ( const ksSlot_t &s : s_slots )
    {
        if ( s.list != KS_LIST_ACTIVE )
            break;                              // the active list comes first in the slots
        if ( s.node && ( s_recs[s.rec].facts.cls & classMask ) != 0 )
            out.push_back( s.node );
    }
}

void KiwiScene_ActiveOfClassLive( unsigned classMask, std::vector<selbrush_t *> &out )
{
    out.clear();
    KiwiScene_SyncRecords();
    for ( const ksSlot_t &s : s_slots )
    {
        if ( s.list != KS_LIST_ACTIVE )
            break;
        const ksRec_t &r = s_recs[s.rec];
        if ( !s.node || ( r.facts.cls & classMask ) == 0 )
            continue;
        const bool kept = !r.dirty && r.cell >= 0 && !s_cells[r.cell].dirty
                       && ( ( r.windows >> KS_WINDOW_MAIN & 1u ) != 0 || r.lineSegs > 0 );
        if ( !kept )
            out.push_back( s.node );
    }
}

void KiwiScene_QueryActive( ksBoundsTestFn test, void *ctx, std::vector<selbrush_t *> &out,
                            bool keepEntityBrushes )
{
    out.clear();
    KiwiScene_SyncRecords();
    static std::vector<int> slots;
    slots.clear();
    for ( ksCell_t &cell : s_cells )
    {
        if ( cell.members.empty() )
            continue;
        RefreshBounds( cell );
        if ( !cell.hasBounds || test( cell.mins, cell.maxs, ctx ) )
        {
            for ( int id : cell.members )
                slots.push_back( s_recs[id].slot );
        }
        else if ( keepEntityBrushes )
        {
            for ( int id : cell.members )
                if ( s_recs[id].facts.cls & KS_CLASS_ENTITY )
                    slots.push_back( s_recs[id].slot );
        }
    }
    std::sort( slots.begin(), slots.end() );
    out.reserve( slots.size() );
    for ( int s : slots )
        out.push_back( s_slots[s].node );
}

void KiwiScene_NodeFreed( const selbrush_t *b )
{
    std::unordered_map<const selbrush_t *, int>::const_iterator it = s_recByNode.find( b );
    if ( it != s_recByNode.end() )
        DropRec( it->second );
}

void KiwiScene_InvalidateAll()
{
    for ( ksBatch_t &bt : s_batches )
        KiwiGpu_ReleaseIndexBuffer( bt.ib );
    ReleasePages();
    s_recs.clear();
    s_freeRecs.clear();
    s_slots.clear();
    s_dirtyRecs.clear();
    s_recByNode.clear();
    s_counts.clear();
    ++s_factsGen;
    s_cells.clear();
    s_cellIndex.clear();
    s_dirtyCells.clear();
    s_batches.clear();
    s_batchIndex.clear();
    s_dirtyBatches.clear();
    for ( std::vector<int> &w : s_windowBatches )
        w.clear();
    s_windowsDirty = false;
    s_perFrameRecs.clear();
    s_perFrameDirty = false;
    s_haveKey = false;
    s_recordsFresh = false;
    s_resetSerial = s_resetSerial * 6364136223846793005ull + 1442695040888963407ull;
}

// ── public: the camera world ─────────────────────────────────────────────────

void KiwiScene_Sync( unsigned long long sceneKey, ksDescribeFn describe )
{
    PROF_SCOPED( "scene sync" );
    KiwiScene_SyncRecords();
    if ( !describe || !s_recordsFresh )
        return;

    if ( !s_haveKey || sceneKey != s_sceneKey )
    {
        s_sceneKey = sceneKey;
        s_haveKey  = true;
        for ( int c = 0; c < (int)s_cells.size(); ++c )
            MarkCellDirty( c );
    }
    {
        PROF_SCOPED( "scene cells" );
        for ( size_t i = 0; i < s_dirtyCells.size(); ++i )
            RebuildCell( s_dirtyCells[i], describe );
        s_dirtyCells.clear();
    }
    {
        PROF_SCOPED( "scene batches" );
        for ( int id : s_dirtyBatches )
            RebuildBatch( id );
        s_dirtyBatches.clear();
    }
    if ( s_windowsDirty )
    {
        s_windowsDirty = false;
        for ( std::vector<int> &w : s_windowBatches )
            w.clear();
        for ( int id = 0; id < (int)s_batches.size(); ++id )
            if ( s_batches[id].indexCount > 0 )
                s_windowBatches[s_batches[id].key.window].push_back( id );
        // In the editor flush comparator's order (Editor_SurfCompare: sortKey, technique,
        // then material and VB page for a batch; a scene page binds as page 0), so a window
        // opened with these batches already has a sorted head (KiwiEdScene_StampSortedHead).
        for ( std::vector<int> &w : s_windowBatches )
            std::sort( w.begin(), w.end(), []( int a, int b )
            {
                const ksBatchKey_t &ka = s_batches[a].key, &kb = s_batches[b].key;
                if ( ka.sortKey != kb.sortKey )
                    return ka.sortKey < kb.sortKey;
                if ( ka.techType != kb.techType )
                    return ka.techType < kb.techType;
                if ( ka.material != kb.material )
                    return (uintptr_t)ka.material < (uintptr_t)kb.material;
                const unsigned pa = ( ka.vbPage & KS_SCENE_PAGE ) ? 0u : ka.vbPage;
                const unsigned pb = ( kb.vbPage & KS_SCENE_PAGE ) ? 0u : kb.vbPage;
                return pa < pb;
            } );
    }
}

void KiwiScene_Emit( ksWindow_t window, const ksFrustum_t *frustum )
{
    if ( (int)window < 0 || window >= KS_WINDOW_COUNT )
        return;
    if ( frustum )
        s_cellVisible.assign( s_cells.size(), (signed char)-1 );
    for ( int id : s_windowBatches[window] )
    {
        const ksBatch_t &bt = s_batches[id];
        if ( !frustum )
        {
            EmitRun( bt, 0, bt.indexCount );
            continue;
        }
        // Consecutive visible cells are consecutive in the buffer: one draw per run.
        ksRange_t runs[KS_MAX_RUNS_PER_BATCH];
        int  runCount  = 0;
        bool fragmented = false;
        int  runFirst = -1, runEnd = 0;
        for ( const ksRange_t &r : bt.ranges )
        {
            if ( CellVisible( r.cell, frustum ) )
            {
                if ( runFirst < 0 )
                    runFirst = r.first;
                runEnd = r.first + r.count;
                continue;
            }
            if ( runFirst < 0 )
                continue;
            if ( runCount == KS_MAX_RUNS_PER_BATCH )
            {
                fragmented = true;
                break;
            }
            runs[runCount].first = runFirst;
            runs[runCount].count = runEnd - runFirst;
            ++runCount;
            runFirst = -1;
        }
        if ( !fragmented && runFirst >= 0 )
        {
            if ( runCount == KS_MAX_RUNS_PER_BATCH )
                fragmented = true;
            else
            {
                runs[runCount].first = runFirst;
                runs[runCount].count = runEnd - runFirst;
                ++runCount;
            }
        }
        if ( fragmented )
            EmitRun( bt, 0, bt.indexCount );
        else
            for ( int k = 0; k < runCount; ++k )
                EmitRun( bt, runs[k].first, runs[k].count );
    }
}

bool KiwiScene_HasBatches( ksWindow_t window )
{
    return (int)window >= 0 && window < KS_WINDOW_COUNT && !s_windowBatches[window].empty();
}

unsigned KiwiScene_AddRun( ksBrushOut_t *out, int stride, int vertDecl, const ksRunKey_t &key,
                           ksFillFn fill, void *ctx )
{
    // The same vertices another window of this describe already asked for: share them.
    for ( const ksRun_t &run : out->runs )
        if ( s_pages[run.page].stride == stride && s_pages[run.page].vertDecl == vertDecl
          && run.key.source == key.source && run.key.count == key.count
          && !memcmp( run.key.placement, key.placement, sizeof( key.placement ) ) )
            return KS_RUN_HANDLE | ( (unsigned)run.page << 16 ) | (unsigned)run.first;
    // The same vertices as last describe: keep them where they are.
    if ( out->previous )
        for ( ksRun_t &old : *out->previous )
            if ( old.count > 0 && s_pages[old.page].stride == stride
              && s_pages[old.page].vertDecl == vertDecl && old.key.source == key.source
              && old.key.count == key.count
              && !memcmp( old.key.placement, key.placement, sizeof( key.placement ) ) )
            {
                out->runs.push_back( old );
                old.count = 0;                     // moved: RebuildCell must not free it
                const ksRun_t &run = out->runs.back();
                return KS_RUN_HANDLE | ( (unsigned)run.page << 16 ) | (unsigned)run.first;
            }
    ksRun_t run;
    run.key   = key;
    run.count = key.count;
    if ( !AllocRun( stride, vertDecl, key.count, &run.page, &run.first ) )
        return 0;
    s_runStaging.resize( (size_t)stride * (size_t)key.count );
    fill( s_runStaging.data(), ctx );
    ksPage_t &pg = s_pages[run.page];
    KiwiGpuVertexBuffer *vb = KiwiGpu_WriteVertexBuffer( pg.vb, run.first * stride,
                                                         s_runStaging.data(),
                                                         (int)s_runStaging.size() );
    if ( !vb )
    {
        FreeRun( run );
        return 0;
    }
    pg.vb = vb;
    out->runs.push_back( run );
    return KS_RUN_HANDLE | ( (unsigned)run.page << 16 ) | (unsigned)run.first;
}

void KiwiScene_KeepNothing( ksBrushOut_t *out )
{
    // The runs it took over from the last describe are its own now, like the new ones.
    for ( const ksRun_t &run : out->runs )
        FreeRun( run );
    out->runs.clear();
    out->layers.clear();
    out->perFrame.clear();
    out->hasBounds = false;
    out->lines.clear();
    out->lineWidths.clear();
    out->lineDims.clear();
    out->lineSegs  = 0;
}

void KiwiScene_EmitBox( ksWindow_t window, const float mins[3], const float maxs[3] )
{
    // The box as six inward planes: a cell passes when it reaches into it.
    ksFrustum_t box;
    box.planeCount = 6;
    for ( int k = 0; k < 3; ++k )
    {
        float *lo = box.planes[2 * k];
        float *hi = box.planes[2 * k + 1];
        lo[0] = lo[1] = lo[2] = 0.0f;
        hi[0] = hi[1] = hi[2] = 0.0f;
        lo[k] = 1.0f;
        lo[3] = mins[k];
        hi[k] = -1.0f;
        hi[3] = -maxs[k];
    }
    KiwiScene_Emit( window, &box );
}

void KiwiScene_EmitLines( const ksFrustum_t *frustum, ksLinesFn fn, void *ctx )
{
    if ( frustum )
        s_cellVisible.assign( s_cells.size(), (signed char)-1 );
    for ( int c = 0; c < (int)s_cells.size(); ++c )
    {
        const ksCell_t &cell = s_cells[c];
        if ( cell.dirty || cell.members.empty() || ( frustum && !CellVisible( c, frustum ) ) )
            continue;
        for ( int id : cell.members )
        {
            const ksRec_t &r = s_recs[id];
            if ( r.lineSegs > 0 )
                fn( r.lines.data(), r.lineWidths.data(), r.lineDims.data(), r.lineSegs, ctx );
        }
    }
}

bool KiwiScene_BrushLinesRetained( const selbrush_t *b )
{
    std::unordered_map<const selbrush_t *, int>::const_iterator it = s_recByNode.find( b );
    if ( it == s_recByNode.end() )
        return false;
    const ksRec_t &r = s_recs[it->second];
    return r.list == KS_LIST_ACTIVE && !r.dirty && r.cell >= 0 && !s_cells[r.cell].dirty
        && r.lineSegs > 0;
}

bool KiwiScene_BrushRetained( const selbrush_t *b, ksWindow_t window )
{
    std::unordered_map<const selbrush_t *, int>::const_iterator it = s_recByNode.find( b );
    if ( it == s_recByNode.end() )
        return false;
    const ksRec_t &r = s_recs[it->second];
    return r.list == KS_LIST_ACTIVE && !r.dirty && r.cell >= 0 && !s_cells[r.cell].dirty
        && ( r.windows >> window & 1u ) != 0;
}

void KiwiScene_ForEachPerFrame( ksPerFrameFn fn, void *ctx )
{
    if ( s_perFrameDirty )
    {
        s_perFrameDirty = false;
        s_perFrameRecs.clear();
        for ( const ksSlot_t &s : s_slots )
            if ( s.list == KS_LIST_ACTIVE && !s_recs[s.rec].perFrame.empty() )
                s_perFrameRecs.push_back( s.rec );
    }
    for ( int id : s_perFrameRecs )
    {
        const ksRec_t &r = s_recs[id];
        for ( int face : r.perFrame )
            fn( r.node, face, ctx );
    }
}
