#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Keep seams welded - see kiwi_weld.h.
//
// One graph over the borders of every visible terrain patch:
//   * a VERTEX is a border point, or all the border points of other patches within 1 unit of
//     it in XY whose heights chain within KWELD_MAX_DZ (sorted by height, cut at a bigger
//     step - a wall's top and bottom stay apart);
//   * a vertex lying part-way along another patch's border segment is that segment's
//     DEPENDENT: it belongs on the line between that patch's own copies of the segment ends;
//   * a vertex is OPEN when its copies differ, or a dependent sits off its segment.
// Welding an open vertex puts every copy at the copies' mean (the locked copies' mean when a
// copy is locked), then the dependents go onto their segments - Gauss-Seidel, so chains and
// edge densities that do not nest settle together.  A dependent whose segment end moves
// follows it, open or not, so welding never opens the next seam along.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include "kiwi_ported.h"
#include "kiwi_weld.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    const char *KWELD_PROFILE = "KiwiWeld";
    const float KWELD_XY      = 1.0f;       // border points this close in XY are one point (kiwi_terrain KTER_WELD)
    const float KWELD_MAX_DZ  = 32.0f;      // heights further apart are a wall / a second layer (kiwi_terrain KTER_WELD_Z)
    const float KWELD_BUCKET  = 64.0f;      // border-point hash cell
    const float KWELD_OPEN    = 0.05f;      // a seam wider than this is open
    const float KWELD_SETTLE  = 1.0e-4f;    // the dependents' solve stops when nothing moves more
    const int   KWELD_PASSES  = 512;

    bool  s_keep   = true;      // Keep seams welded: the terrain ops close the seams they open
    bool  s_loaded = false;

    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded = true;
        s_keep = Radiant_ProfileGetInt( KWELD_PROFILE, "KeepWelded", 1 ) != 0;
    }

    // ── the graph ────────────────────────────────────────────────────────────
    struct kwPatch_t
    {
        patchMesh_t *def;
        bool         scope;         // its seams may be welded
        int          ring0, ringN;  // its border points: points[ring0 ..], in ring order
    };

    struct kwPoint_t
    {
        int patch, i, j;
        int vert;
    };

    struct kwVert_t
    {
        float pos[3];               // the weld target (the copies' mean, then the solve)
        int   ref0, refN;           // copies: refs[ref0 ..] (point indices)
        bool  locked;               // a copy is locked (turned_edge & 2)
        int   host;                 // dependent: the patch whose border segment it lies on; -1 = none
        int   hostA, hostB;         // that patch's own copies (point indices) of the segment ends
        float hostT;
        float gap;                  // widest distance between its copies, or off its segment
        bool  active;
    };

    struct kwGraph_t
    {
        std::vector<kwPatch_t> patches;
        std::vector<kwPoint_t> points;
        std::vector<kwVert_t>  verts;
        std::vector<int>       refs;
    };

    bool Eligible( selbrush_t *b )
    {
        return b && b->patch && b->def && b->patch->def && !FilterBrush( b, 0 ) && ( b->brushFlags & 0x20 ) == 0
            && ( b->patch->def->type & PATCH_TERRAIN ) != 0
            && b->patch->def->width >= 2 && b->patch->def->height >= 2;
    }

    template <class F> void ForEachTerrain( F fn )
    {
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( Eligible( b ) )
                    fn( b );
        }
    }

    long long BucketKey( int bx, int by )
    {
        return ( (long long)bx << 32 ) ^ (long long)(unsigned int)by;
    }

    int Cell( float v )
    {
        return (int)floorf( v / KWELD_BUCKET );
    }

    float *PointXyz( const kwGraph_t &g, int p )
    {
        const kwPoint_t &q = g.points[p];
        return g.patches[q.patch].def->ctrl[q.i][q.j].xyz;
    }

    float Dist3( const float *a, const float *b )
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return sqrtf( dx * dx + dy * dy + dz * dz );
    }

    int UfFind( std::vector<int> &uf, int x )
    {
        while ( uf[x] != x )
        {
            uf[x] = uf[uf[x]];
            x = uf[x];
        }
        return x;
    }

    // Walk a patch's border as consecutive (i,j) pairs (kiwi_terrain BorderRing).
    int BorderRing( const patchMesh_t *def, int ii[64], int jj[64] )
    {
        int n = 0;
        const int w = def->width, h = def->height;
        for ( int i = 0; i < w && n < 64; ++i )         { ii[n] = i;     jj[n] = 0;     ++n; }
        for ( int j = 1; j < h && n < 64; ++j )         { ii[n] = w - 1; jj[n] = j;     ++n; }
        for ( int i = w - 2; i >= 0 && n < 64; --i )    { ii[n] = i;     jj[n] = h - 1; ++n; }
        for ( int j = h - 2; j >= 1 && n < 64; --j )    { ii[n] = 0;     jj[n] = j;     ++n; }
        return n;
    }

    // `scope` = the patches whose seams may be welded; nullptr = all of them.
    void Build( kwGraph_t &g, const std::set<const patchMesh_t *> *scope )
    {
        g.patches.clear();
        g.points.clear();
        g.verts.clear();
        g.refs.clear();
        ForEachTerrain( [&]( selbrush_t *b )
        {
            patchMesh_t *def = b->patch->def;
            kwPatch_t p = { def, !scope || scope->count( def ) != 0, (int)g.points.size(), 0 };
            int ii[64], jj[64];
            p.ringN = BorderRing( def, ii, jj );
            for ( int k = 0; k < p.ringN; ++k )
            {
                const kwPoint_t q = { (int)g.patches.size(), ii[k], jj[k], -1 };
                g.points.push_back( q );
            }
            g.patches.push_back( p );
        } );
        const int np = (int)g.points.size();
        if ( !np )
            return;

        // 1. XY groups: border points of different patches within KWELD_XY of each other
        std::unordered_map<long long, std::vector<int> > buckets;
        for ( int p = 0; p < np; ++p )
        {
            const float *x = PointXyz( g, p );
            buckets[BucketKey( Cell( x[0] ), Cell( x[1] ) )].push_back( p );
        }
        std::vector<int> uf( np );
        for ( int p = 0; p < np; ++p )
            uf[p] = p;
        for ( int p = 0; p < np; ++p )
        {
            const float *x = PointXyz( g, p );
            const int bx = Cell( x[0] ), by = Cell( x[1] );
            for ( int dx = -1; dx <= 1; ++dx )
                for ( int dy = -1; dy <= 1; ++dy )
                {
                    const std::unordered_map<long long, std::vector<int> >::const_iterator it =
                        buckets.find( BucketKey( bx + dx, by + dy ) );
                    if ( it == buckets.end() )
                        continue;
                    for ( size_t c = 0; c < it->second.size(); ++c )
                    {
                        const int q = it->second[c];
                        if ( q <= p || g.points[q].patch == g.points[p].patch )
                            continue;
                        const float *y = PointXyz( g, q );
                        if ( fabsf( x[0] - y[0] ) > KWELD_XY || fabsf( x[1] - y[1] ) > KWELD_XY )
                            continue;
                        uf[UfFind( uf, p )] = UfFind( uf, q );
                    }
                }
        }

        // 2. vertices: each XY group sorted by height, cut where a step exceeds KWELD_MAX_DZ
        std::vector<int> root( np ), order( np );
        for ( int p = 0; p < np; ++p )
        {
            root[p]  = UfFind( uf, p );
            order[p] = p;
        }
        std::sort( order.begin(), order.end(), [&]( int a, int b )
        {
            if ( root[a] != root[b] )
                return root[a] < root[b];
            return PointXyz( g, a )[2] < PointXyz( g, b )[2];
        } );
        for ( int k = 0; k < np; ++k )
        {
            const int p = order[k];
            if ( k == 0 || root[p] != root[order[k - 1]]
              || PointXyz( g, p )[2] - PointXyz( g, order[k - 1] )[2] > KWELD_MAX_DZ )
            {
                kwVert_t v;
                memset( &v, 0, sizeof( v ) );
                v.ref0 = (int)g.refs.size();
                v.host = -1;
                g.verts.push_back( v );
            }
            kwVert_t &v = g.verts.back();
            g.points[p].vert = (int)g.verts.size() - 1;
            g.refs.push_back( p );
            ++v.refN;
        }
        const int nv = (int)g.verts.size();
        for ( int vi = 0; vi < nv; ++vi )
        {
            kwVert_t &v = g.verts[vi];
            float sum[3] = { 0.0f, 0.0f, 0.0f }, lockSum[3] = { 0.0f, 0.0f, 0.0f };
            int locks = 0;
            for ( int r = 0; r < v.refN; ++r )
            {
                const int p = g.refs[v.ref0 + r];
                const float *x = PointXyz( g, p );
                const kwPoint_t &q = g.points[p];
                const bool locked = ( g.patches[q.patch].def->ctrl[q.i][q.j].turned_edge & 2 ) != 0;
                for ( int a = 0; a < 3; ++a )
                {
                    sum[a] += x[a];
                    if ( locked )
                        lockSum[a] += x[a];
                }
                locks += locked ? 1 : 0;
                for ( int e = 0; e < r; ++e )
                {
                    const float d = Dist3( x, PointXyz( g, g.refs[v.ref0 + e] ) );
                    if ( d > v.gap )
                        v.gap = d;
                }
            }
            v.locked = locks > 0;
            for ( int a = 0; a < 3; ++a )
                v.pos[a] = locks ? lockSum[a] / (float)locks : sum[a] / (float)v.refN;
        }

        // 3. dependents: a vertex strictly inside another patch's border segment
        std::unordered_map<long long, std::vector<int> > vb;
        for ( int vi = 0; vi < nv; ++vi )
            vb[BucketKey( Cell( g.verts[vi].pos[0] ), Cell( g.verts[vi].pos[1] ) )].push_back( vi );
        std::vector<float> hostErr( nv, FLT_MAX );
        for ( int pi = 0; pi < (int)g.patches.size(); ++pi )
        {
            const kwPatch_t &P = g.patches[pi];
            for ( int k = 0; k < P.ringN; ++k )
            {
                const int pa = P.ring0 + k, pb = P.ring0 + ( k + 1 ) % P.ringN;
                const int a = g.points[pa].vert, b = g.points[pb].vert;
                if ( a == b )
                    continue;
                const float *A = PointXyz( g, pa ), *B = PointXyz( g, pb );    // this patch's real edge
                const float dx = B[0] - A[0], dy = B[1] - A[1], dz = B[2] - A[2];
                const float len2 = dx * dx + dy * dy;
                if ( len2 <= 4.0f * KWELD_XY * KWELD_XY )
                    continue;
                const float len = sqrtf( len2 );
                const int bx0 = Cell( ( dx < 0.0f ? B[0] : A[0] ) - KWELD_XY ), bx1 = Cell( ( dx < 0.0f ? A[0] : B[0] ) + KWELD_XY );
                const int by0 = Cell( ( dy < 0.0f ? B[1] : A[1] ) - KWELD_XY ), by1 = Cell( ( dy < 0.0f ? A[1] : B[1] ) + KWELD_XY );
                for ( int bx = bx0; bx <= bx1; ++bx )
                    for ( int by = by0; by <= by1; ++by )
                    {
                        const std::unordered_map<long long, std::vector<int> >::const_iterator it =
                            vb.find( BucketKey( bx, by ) );
                        if ( it == vb.end() )
                            continue;
                        for ( size_t c = 0; c < it->second.size(); ++c )
                        {
                            const int vi = it->second[c];
                            kwVert_t &V = g.verts[vi];
                            if ( vi == a || vi == b )
                                continue;
                            const float t = ( ( V.pos[0] - A[0] ) * dx + ( V.pos[1] - A[1] ) * dy ) / len2;
                            if ( t * len <= KWELD_XY || ( 1.0f - t ) * len <= KWELD_XY )
                                continue;                   // off the segment, or at an end
                            const float ex = V.pos[0] - ( A[0] + dx * t ), ey = V.pos[1] - ( A[1] + dy * t );
                            const float e2 = ex * ex + ey * ey;
                            if ( e2 > KWELD_XY * KWELD_XY || e2 >= hostErr[vi]
                              || fabsf( V.pos[2] - ( A[2] + dz * t ) ) > KWELD_MAX_DZ )
                                continue;
                            bool own = false;               // a point of this very patch: a degenerate border
                            for ( int r = 0; r < V.refN && !own; ++r )
                                own = g.points[g.refs[V.ref0 + r]].patch == pi;
                            if ( own )
                                continue;
                            hostErr[vi] = e2;
                            V.host  = pi;
                            V.hostA = pa;
                            V.hostB = pb;
                            V.hostT = t;
                        }
                    }
            }
        }
        for ( int vi = 0; vi < nv; ++vi )
        {
            kwVert_t &V = g.verts[vi];
            if ( V.host < 0 )
                continue;
            const float *A = PointXyz( g, V.hostA ), *B = PointXyz( g, V.hostB );
            float on[3];
            for ( int a = 0; a < 3; ++a )
                on[a] = A[a] + ( B[a] - A[a] ) * V.hostT;
            for ( int r = 0; r < V.refN; ++r )
            {
                const float d = Dist3( PointXyz( g, g.refs[V.ref0 + r] ), on );
                if ( d > V.gap )
                    V.gap = d;
            }
        }
    }

    bool InScope( const kwGraph_t &g, const kwVert_t &v )
    {
        if ( v.host >= 0 && g.patches[v.host].scope )
            return true;
        for ( int r = 0; r < v.refN; ++r )
            if ( g.patches[g.points[g.refs[v.ref0 + r]].patch].scope )
                return true;
        return false;
    }

    // Open in-scope vertices (minus `skip`), then every dependent whose segment end is welded.
    void Activate( kwGraph_t &g, const std::vector<char> *skip )
    {
        const int nv = (int)g.verts.size();
        for ( int vi = 0; vi < nv; ++vi )
        {
            kwVert_t &v = g.verts[vi];
            v.active = v.gap > KWELD_OPEN && InScope( g, v ) && !( skip && ( *skip )[vi] );
        }
        for ( bool grew = true; grew; )
        {
            grew = false;
            for ( int vi = 0; vi < nv; ++vi )
            {
                kwVert_t &v = g.verts[vi];
                if ( v.active || v.host < 0 || ( skip && ( *skip )[vi] ) )
                    continue;
                if ( g.verts[g.points[v.hostA].vert].active || g.verts[g.points[v.hostB].vert].active )
                {
                    v.active = true;
                    grew = true;
                }
            }
        }
    }

    // A segment end as the solve sees it: the weld target when welded, else the host's own copy.
    const float *HostEnd( const kwGraph_t &g, int point )
    {
        const kwVert_t &v = g.verts[g.points[point].vert];
        return v.active ? v.pos : PointXyz( g, point );
    }

    void Solve( kwGraph_t &g )
    {
        for ( int pass = 0; pass < KWELD_PASSES; ++pass )
        {
            float most = 0.0f;
            for ( size_t vi = 0; vi < g.verts.size(); ++vi )
            {
                kwVert_t &v = g.verts[vi];
                if ( !v.active || v.host < 0 || v.locked )
                    continue;
                const float *A = HostEnd( g, v.hostA ), *B = HostEnd( g, v.hostB );
                float on[3];
                for ( int a = 0; a < 3; ++a )
                    on[a] = A[a] + ( B[a] - A[a] ) * v.hostT;
                const float d = Dist3( on, v.pos );
                if ( d > most )
                    most = d;
                memcpy( v.pos, on, sizeof( on ) );
            }
            if ( most < KWELD_SETTLE )
                break;
        }
    }

    struct kwResult_t
    {
        int points;                 // control points moved
        int seams;                  // patch pairs whose seam closed
    };

    // Does the weld move anything?
    bool AnyMove( const kwGraph_t &g )
    {
        for ( size_t vi = 0; vi < g.verts.size(); ++vi )
        {
            const kwVert_t &v = g.verts[vi];
            if ( !v.active )
                continue;
            for ( int r = 0; r < v.refN; ++r )
                if ( Dist3( PointXyz( g, g.refs[v.ref0 + r] ), v.pos ) > 1.0e-5f )
                    return true;
        }
        return false;
    }

    // Writes the weld targets to every copy, `mark`ing each patch before its first change.
    kwResult_t Apply( kwGraph_t &g, void ( *mark )( patchMesh_t * ) )
    {
        kwResult_t res = { 0, 0 };
        std::set<patchMesh_t *> marked;
        std::vector<patchMesh_t *> changed;
        std::set<std::pair<patchMesh_t *, patchMesh_t *> > seams;
        std::vector<patchMesh_t *> around;
        for ( size_t vi = 0; vi < g.verts.size(); ++vi )
        {
            const kwVert_t &v = g.verts[vi];
            if ( !v.active )
                continue;
            bool moved = false;
            for ( int r = 0; r < v.refN; ++r )
            {
                const int p = g.refs[v.ref0 + r];
                float *x = PointXyz( g, p );
                if ( Dist3( x, v.pos ) <= 1.0e-5f )
                    continue;
                patchMesh_t *def = g.patches[g.points[p].patch].def;
                if ( marked.insert( def ).second )
                {
                    mark( def );
                    changed.push_back( def );
                }
                memcpy( x, v.pos, sizeof( v.pos ) );
                ++res.points;
                moved = true;
            }
            if ( !moved )
                continue;
            around.clear();
            for ( int r = 0; r < v.refN; ++r )
                around.push_back( g.patches[g.points[g.refs[v.ref0 + r]].patch].def );
            if ( v.host >= 0 )
                around.push_back( g.patches[v.host].def );
            for ( size_t m = 0; m < around.size(); ++m )
                for ( size_t o = m + 1; o < around.size(); ++o )
                    if ( around[m] != around[o] )
                        seams.insert( around[m] < around[o] ? std::make_pair( around[m], around[o] )
                                                            : std::make_pair( around[o], around[m] ) );
        }
        for ( size_t k = 0; k < changed.size(); ++k )
        {
            Patch_Rebuild( changed[k], 1 );
            ++changed[k]->version;
        }
        res.seams = (int)seams.size();
        if ( res.points )
            g_nUpdateBits = -1;
        return res;
    }

    // Inside a terrain op's record: Undo_AddBrush saves each patch once and skips the ones the
    // record created (Undo_KiwiMarkCreated stamped them).
    void MarkRecord( patchMesh_t *def )
    {
        if ( def->pSymbiot )
            Undo_AddBrush( def->pSymbiot );
    }

    // Keep seams welded: where seams were open when the op began.
    std::vector<float> s_preOpen;                               // x y pairs
    std::unordered_map<long long, std::vector<int> > s_preBuckets;
    bool s_guardOpen = false;

    bool WasOpen( const float *pos )
    {
        const int bx = Cell( pos[0] ), by = Cell( pos[1] );
        for ( int dx = -1; dx <= 1; ++dx )
            for ( int dy = -1; dy <= 1; ++dy )
            {
                const std::unordered_map<long long, std::vector<int> >::const_iterator it =
                    s_preBuckets.find( BucketKey( bx + dx, by + dy ) );
                if ( it == s_preBuckets.end() )
                    continue;
                for ( size_t c = 0; c < it->second.size(); ++c )
                {
                    const float *q = &s_preOpen[2 * it->second[c]];
                    if ( fabsf( q[0] - pos[0] ) <= KWELD_XY && fabsf( q[1] - pos[1] ) <= KWELD_XY )
                        return true;
                }
            }
        return false;
    }
}

void KiwiWeld_DrawPanel()
{
    Load();
    if ( ImGui::Checkbox( "Keep seams welded", &s_keep ) )
        Radiant_ProfileSetInt( KWELD_PROFILE, "KeepWelded", s_keep ? 1 : 0 );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Strokes, Split and Tessellate close every seam between chunks they would open\n"
                           "(a finer chunk beside a coarser one, densities that do not line up, a new chunk\n"
                           "beside old terrain) in the same undo step. A crack that was already there is\n"
                           "left as it was." );
}

void KiwiWeld_GuardBegin()
{
    Load();
    s_preOpen.clear();
    s_preBuckets.clear();
    s_guardOpen = s_keep;
    if ( !s_keep )
        return;
    kwGraph_t g;
    Build( g, nullptr );
    for ( size_t vi = 0; vi < g.verts.size(); ++vi )
    {
        const kwVert_t &v = g.verts[vi];
        if ( v.gap <= KWELD_OPEN )
            continue;
        s_preBuckets[BucketKey( Cell( v.pos[0] ), Cell( v.pos[1] ) )].push_back( (int)( s_preOpen.size() / 2 ) );
        s_preOpen.push_back( v.pos[0] );
        s_preOpen.push_back( v.pos[1] );
    }
}

int KiwiWeld_GuardEnd( patchMesh_t *const *touched, int count, void ( *mark )( patchMesh_t * ) )
{
    const bool open = s_guardOpen;
    s_guardOpen = false;
    if ( !open || !s_keep || count <= 0 )
    {
        s_preOpen.clear();
        s_preBuckets.clear();
        return 0;
    }
    std::set<const patchMesh_t *> scope( touched, touched + count );
    kwGraph_t g;
    Build( g, &scope );
    std::vector<char> skip( g.verts.size(), 0 );
    for ( size_t vi = 0; vi < g.verts.size(); ++vi )
        skip[vi] = WasOpen( g.verts[vi].pos ) ? 1 : 0;
    s_preOpen.clear();
    s_preBuckets.clear();
    Activate( g, &skip );
    Solve( g );
    if ( !AnyMove( g ) )
        return 0;
    const kwResult_t r = Apply( g, mark ? mark : MarkRecord );
    Sys_Printf( "Terrain: kept %i seam%s welded (%i point%s moved).\n",
                r.seams, r.seams == 1 ? "" : "s", r.points, r.points == 1 ? "" : "s" );
    return r.points;
}

float KiwiWeld_TestMaxGap()
{
    Load();
    kwGraph_t g;
    Build( g, nullptr );
    float worst = 0.0f;
    for ( size_t vi = 0; vi < g.verts.size(); ++vi )
        if ( g.verts[vi].gap > worst )
            worst = g.verts[vi].gap;
    return worst;
}
