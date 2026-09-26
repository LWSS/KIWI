#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Construction selection is KIWI-owned; legacy selection sentinels are read only.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_conselect.h"
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_pick.h"
#include "kiwi_refimage.h"       // Unhide All also reveals hidden reference images
#include "kiwi_region.h"
#include "kiwi_selection.h"
#include "kiwi_camera.h"         // Extend: KiwiCam_RayAxis / AxisPortrayable / WorldPerPixel
#include "kiwi_lines.h"          // Extend: the preview pieces
#include "kiwi_units.h"          // Extend: the HUD length
#include "kiwi_vec.h"            // Extend: Dot3 / Sub3 / Norm3 / Mad3

#include <math.h>
#include <vector>

extern int  Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118
extern int  g_nUpdateBits;                       // 0x25D5A74
// 0x23F1864: selected_brushes is the display-list sentinel; read only here.
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
                                                 // mainfrm.cpp:1246

namespace
{
    std::vector<kconSelItem_t> s_sel;

    // Transform baselines store the object's plane origin, WORLD points, and (for
    // rotate/scale) the whole parametric frame at Begin.
    struct moveBase_t
    {
        int                object;
        float              origin[3];   // the object's plane origin at Begin
        std::vector<float> pts;         // its WORLD points at Begin
        kconPlane_t        plane;       // full frame at Begin (rotate needs u/v/normal)
        float              radius;      // parametric radius at Begin
    };
    std::vector<moveBase_t> s_moveBase;
    bool                    s_moveOpen = false;

    inline float PixelDist( float ax, float ay, float bx, float by )
    {
        const float dx = ax - bx, dy = ay - by;
        return sqrtf( dx * dx + dy * dy );
    }

    bool ItemEqual( const kconSelItem_t &a, const kconSelItem_t &b )
    {
        return a.object == b.object && a.kind == b.kind && a.index == b.index;
    }

    bool Contains( const kconSelItem_t &it )
    {
        for ( size_t i = 0; i < s_sel.size(); ++i )
            if ( ItemEqual( s_sel[i], it ) )
                return true;
        return false;
    }

    void Add( const kconSelItem_t &it )
    {
        if ( it.object < 0 || it.object >= KiwiCon_Count() )
            return;
        if ( !Contains( it ) )
            s_sel.push_back( it );
    }

    void Remove( const kconSelItem_t &it )
    {
        for ( size_t i = 0; i < s_sel.size(); ++i )
            if ( ItemEqual( s_sel[i], it ) )
            {
                s_sel.erase( s_sel.begin() + i );
                return;
            }
    }

    void Toggle( const kconSelItem_t &it )
    {
        if ( Contains( it ) ) Remove( it );
        else                  Add   ( it );
    }

    // Fine typed selections need not populate selected_brushes, so Move checks both.
    bool BrushSelectionEmpty()
    {
        if ( selected_brushes.next != &selected_brushes )
            return false;
        return KiwiSel().items.empty();
    }

    // Face and Object modes exclude construction lines. Exact equality preserves
    // Everything mode, whose combined mask must still admit them.
    bool ModeAllowsConstructionLines()
    {
        const sel_mask_t mask = KiwiSel_GetModeMask();
        return mask != SEL_MASK_FACE && mask != SEL_MASK_OBJECT;
    }

    // Match kiwi_boxselect.cpp's mask-priority order.
    kconSelKind_t KindForMode()
    {
        const sel_mask_t mask = KiwiSel_GetModeMask();
        if ( mask & SEL_MASK_OBJECT ) return KCONSEL_OBJECT;
        if ( mask & SEL_MASK_FACE )   return KCONSEL_OBJECT;
        if ( mask & SEL_MASK_EDGE )   return KCONSEL_SEGMENT;
        return KCONSEL_POINT;
    }

    // Liang-Barsky, matching kiwi_boxselect.cpp without widening its private API.
    bool SegHitsRect( float rx0, float ry0, float rx1, float ry1,
                      float ax, float ay, float bx, float by )
    {
        float t0 = 0.0f, t1 = 1.0f;
        const float dx = bx - ax, dy = by - ay;
        const float p[4] = { -dx, dx, -dy, dy };
        const float q[4] = { ax - rx0, rx1 - ax, ay - ry0, ry1 - ay };
        for ( int i = 0; i < 4; ++i )
        {
            if ( p[i] == 0.0f )
            {
                if ( q[i] < 0.0f )
                    return false;
                continue;
            }
            const float t = q[i] / p[i];
            if ( p[i] < 0.0f )
            {
                if ( t > t1 ) return false;
                if ( t > t0 ) t0 = t;
            }
            else
            {
                if ( t < t0 ) return false;
                if ( t < t1 ) t1 = t;
            }
        }
        return true;
    }

    inline bool PointIn( float rx0, float ry0, float rx1, float ry1, float x, float y )
    {
        return x >= rx0 && x <= rx1 && y >= ry0 && y <= ry1;
    }

    // The distinct OBJECTS the selection names, in ascending index order.
    void SelectedObjects( std::vector<int> *out )
    {
        out->clear();
        for ( size_t i = 0; i < s_sel.size(); ++i )
        {
            const int o = s_sel[i].object;
            if ( o < 0 || o >= KiwiCon_Count() )
                continue;
            bool dup = false;
            for ( size_t k = 0; k < out->size() && !dup; ++k )
                dup = ( (*out)[k] == o );
            if ( !dup )
                out->push_back( o );
        }
        for ( size_t i = 0; i + 1 < out->size(); ++i )
            for ( size_t j = i + 1; j < out->size(); ++j )
                if ( (*out)[j] < (*out)[i] )
                {
                    const int t = (*out)[i];
                    (*out)[i] = (*out)[j];
                    (*out)[j] = t;
                }
    }
}

// Selection list
int KiwiConSel_Count()
{
    return (int)s_sel.size();
}

const kconSelItem_t *KiwiConSel_At( int i )
{
    if ( i < 0 || i >= (int)s_sel.size() )
        return 0;
    return &s_sel[i];
}

void KiwiConSel_Clear()
{
    if ( s_sel.empty() )
        return;
    s_sel.clear();
    g_nUpdateBits |= 1;
}

// Paste replaces selection with whole objects, matching Map_ImportBuffer at 0x487C90.
void KiwiConSel_SelectObjects( const int *objects, int count )
{
    s_sel.clear();
    for ( int i = 0; i < count && objects; ++i )
    {
        kconSelItem_t it;
        it.object = objects[i];
        it.kind   = KCONSEL_OBJECT;
        it.index  = -1;
        Add( it );                      // Add range-checks and de-duplicates
    }
    g_nUpdateBits |= 1;
}

bool KiwiConSel_ObjectSelected( int object )
{
    for ( size_t i = 0; i < s_sel.size(); ++i )
        if ( s_sel[i].object == object )
            return true;
    return false;
}

bool KiwiConSel_PointSelected( int object, int pointIndex )
{
    for ( size_t i = 0; i < s_sel.size(); ++i )
    {
        if ( s_sel[i].object != object )
            continue;
        if ( s_sel[i].kind == KCONSEL_OBJECT )
            return true;                        // the whole object covers its points
        if ( s_sel[i].kind == KCONSEL_POINT && s_sel[i].index == pointIndex )
            return true;
    }
    return false;
}

bool KiwiConSel_SegmentSelected( int object, int segIndex )
{
    for ( size_t i = 0; i < s_sel.size(); ++i )
    {
        if ( s_sel[i].object != object )
            continue;
        if ( s_sel[i].kind == KCONSEL_OBJECT )
            return true;
        if ( s_sel[i].kind == KCONSEL_SEGMENT && s_sel[i].index == segIndex )
            return true;
    }
    return false;
}

void KiwiConSel_NoteStoreReplaced()
{
    // A live move retains index selection while its own cancel restores the store.
    if ( s_moveOpen )
        return;
    KiwiConSel_Clear();
}

// Picking
bool KiwiConSel_PickAt( int imgX, int imgY, kconSelItem_t *out, float *outPixels )
{
    if ( !out )
        return false;
    if ( !KiwiCon_ShowConstruction() )
        return false;                           // hidden geometry is not clickable
    if ( !ModeAllowsConstructionLines() )       // Face/Object modes exclude lines.
        return false;

    const kconSelKind_t want = KindForMode();
    const float curX = (float)imgX;
    const float curY = (float)imgY;

    kconSelItem_t best;
    float         bestDist = 0.0f;
    bool          have     = false;

    const int count = KiwiCon_Count();
    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        if ( !o || o->hidden )        // Hidden objects are inert.
            continue;

        if ( want == KCONSEL_POINT )
        {
            const int anchors = KiwiCon_AnchorCount( *o );
            for ( int a = 0; a < anchors; ++a )
            {
                float w[3], px, py;
                if ( !KiwiCon_AnchorWorld( *o, a, w ) )
                    break;
                if ( !Pick_WorldToImage( w, &px, &py ) )
                    continue;
                const float d = PixelDist( px, py, curX, curY );
                if ( d > PICK_VERT_PIXELS )
                    continue;
                if ( have && d >= bestDist )
                    continue;
                have = true;  bestDist = d;
                best.object = i;  best.kind = KCONSEL_POINT;  best.index = a;
            }
            continue;
        }

        // SEGMENT and OBJECT share the segment scan — the difference is only what
        // the winning candidate is RECORDED as.
        const int segs = KiwiCon_SegmentCount( *o );
        for ( int s = 0; s < segs; ++s )
        {
            float wa[3], wb[3], ax, ay, bx, by;
            if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                break;
            if ( !Pick_WorldToImage( wa, &ax, &ay ) || !Pick_WorldToImage( wb, &bx, &by ) )
                continue;                       // an end behind the eye — skip whole
            const float d = Pick_SegDist2D( curX, curY, ax, ay, bx, by, 0 );
            // Clicks use 14 px for access; continuous hover/snap stays 10 px to limit noise.
            if ( d > KCON_CLICK_PIXELS )
                continue;
            if ( have && d >= bestDist )
                continue;
            have = true;  bestDist = d;
            best.object = i;
            best.kind   = want;                 // KCONSEL_SEGMENT or KCONSEL_OBJECT
            best.index  = ( want == KCONSEL_SEGMENT ) ? s : -1;
        }
    }

    if ( !have )
        return false;
    *out = best;
    if ( outPixels )
        *outPixels = bestDist;
    return true;
}

void KiwiConSel_ApplyClick( const kconSelItem_t &it, bool shift, bool ctrl )
{
    if ( it.object < 0 )
    {
        if ( !shift && !ctrl )
            KiwiConSel_Clear();
        return;
    }
    if ( ctrl )
    {
        Toggle( it );
    }
    else if ( shift )
    {
        Add( it );
    }
    else
    {
        s_sel.clear();
        Add( it );
    }
    g_nUpdateBits |= 1;
}

void KiwiConSel_ApplyRect( float x0, float y0, float x1, float y1,
                           bool crossing, bool shift, bool ctrl )
{
    if ( !KiwiCon_ShowConstruction() )
        return;
    // Filter before clearing: excluded modes do not alter construction selection.
    if ( !ModeAllowsConstructionLines() )
        return;
    if ( !shift && !ctrl )
        s_sel.clear();

    const kconSelKind_t want = KindForMode();
    const int count = KiwiCon_Count();

    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        if ( !o || o->hidden )        // Hidden objects are inert.
            continue;

        if ( want == KCONSEL_POINT )
        {
            const int anchors = KiwiCon_AnchorCount( *o );
            for ( int a = 0; a < anchors; ++a )
            {
                float w[3], px, py;
                if ( !KiwiCon_AnchorWorld( *o, a, w ) )
                    break;
                if ( !Pick_WorldToImage( w, &px, &py ) )
                    continue;
                if ( !PointIn( x0, y0, x1, y1, px, py ) )
                    continue;
                kconSelItem_t it;
                it.object = i;  it.kind = KCONSEL_POINT;  it.index = a;
                if ( ctrl ) Remove( it );
                else        Add( it );
            }
            continue;
        }

        // For a WHOLE-OBJECT rect the object's own accumulator decides once:
        // containment = every segment endpoint inside, crossing = any segment
        // touching.  Same two rules kiwi_boxselect.cpp applies to a brush.
        bool objAny = false;
        bool objAll = true;
        bool objAnyPoint = false;
        const int segs = KiwiCon_SegmentCount( *o );
        for ( int s = 0; s < segs; ++s )
        {
            float wa[3], wb[3], ax, ay, bx, by;
            if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                break;
            const bool pa = Pick_WorldToImage( wa, &ax, &ay );
            const bool pb = Pick_WorldToImage( wb, &bx, &by );
            if ( !pa || !pb )
            {
                objAll = false;                 // behind the eye: cannot be contained
                continue;
            }
            const bool inA = PointIn( x0, y0, x1, y1, ax, ay );
            const bool inB = PointIn( x0, y0, x1, y1, bx, by );
            if ( !inA || !inB )
                objAll = false;
            if ( inA || inB )
                objAnyPoint = true;
            const bool hit = crossing ? SegHitsRect( x0, y0, x1, y1, ax, ay, bx, by )
                                      : ( inA && inB );
            if ( hit )
                objAny = true;

            if ( want == KCONSEL_SEGMENT && hit )
            {
                kconSelItem_t it;
                it.object = i;  it.kind = KCONSEL_SEGMENT;  it.index = s;
                if ( ctrl ) Remove( it );
                else        Add( it );
            }
        }

        if ( want == KCONSEL_OBJECT && segs > 0 )
        {
            const bool take = crossing ? ( objAny || objAnyPoint ) : objAll;
            if ( take )
            {
                kconSelItem_t it;
                it.object = i;  it.kind = KCONSEL_OBJECT;  it.index = -1;
                if ( ctrl ) Remove( it );
                else        Add( it );
            }
        }
    }
    g_nUpdateBits |= 1;
}

// Join
bool KiwiConSel_CanJoin()
{
    std::vector<int> objs;
    SelectedObjects( &objs );
    if ( objs.size() < 2 )
        return false;
    // At least two OPEN objects: a closed object is already a region and has no
    // free end to chain onto.
    int open = 0;
    for ( size_t i = 0; i < objs.size(); ++i )
    {
        const kconObject_t *o = KiwiCon_At( objs[i] );
        if ( !o )
            continue;
        const bool closed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed;
        if ( !closed )
            ++open;
    }
    return open >= 2;
}

bool KiwiConSel_Join()
{
    std::vector<int> all;
    SelectedObjects( &all );

    // Only OPEN objects take part; a closed one cannot chain and is left alone.
    std::vector<int> objs;
    for ( size_t i = 0; i < all.size(); ++i )
    {
        const kconObject_t *o = KiwiCon_At( all[i] );
        if ( !o )
            continue;
        const bool closed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed;
        if ( !closed && KiwiCon_VertCount( *o ) >= 2 )
            objs.push_back( all[i] );
    }
    if ( objs.size() < 2 )
    {
        Sys_Printf( "Join: select two or more open construction lines first.\n" );
        return false;
    }

    // The world-space store permits 3D joins; region fitting later rejects
    // non-planar loops.
    std::vector<kchainStep_t> steps;
    bool closed = false;
    if ( !KiwiRegion_ChainWalk( &objs[0], (int)objs.size(), &steps, &closed )
      || steps.size() < 2
      // Never delete a selected object that the walker omitted from the chain.
      || steps.size() != objs.size() )
    {
        Sys_Printf( "Join: the selected lines do not form a single chain "
                    "(ends must meet, no end may be shared by three lines, and "
                    "every selected line must be part of the chain).\n" );
        return false;
    }

    // Gather a raw world-space polyline before choosing its finest-edge weld.
    std::vector<float> raw;
    for ( size_t s = 0; s < steps.size(); ++s )
    {
        const kconObject_t *o = KiwiCon_At( steps[s].object );
        if ( !o )
            continue;
        const int n = KiwiCon_VertCount( *o );
        for ( int k = 0; k < n; ++k )
        {
            float w[3];
            if ( !KiwiCon_VertWorld( *o, steps[s].forward ? k : ( n - 1 - k ), w ) )
                break;
            raw.push_back( w[0] );
            raw.push_back( w[1] );
            raw.push_back( w[2] );
        }
    }

    // Bound endpoint collapse by the finest real edge, matching region sanitization.
    // The store's 0.5 normalizer stays conservative for user-placed points.
    const float finest = KiwiRegion_FinestEdge(
        raw.empty() ? 0 : &raw[0], (int)( raw.size() / 3 ), 3, closed );
    const float weld = KiwiRegion_WeldFor( finest );

    std::vector<float> pts;
    {
        const int n = (int)( raw.size() / 3 );
        for ( int i = 0; i < n; ++i )
        {
            const int have = (int)( pts.size() / 3 );
            if ( have > 0 )
            {
                const float dx = raw[(size_t)i * 3 + 0] - pts[(size_t)( have - 1 ) * 3 + 0];
                const float dy = raw[(size_t)i * 3 + 1] - pts[(size_t)( have - 1 ) * 3 + 1];
                const float dz = raw[(size_t)i * 3 + 2] - pts[(size_t)( have - 1 ) * 3 + 2];
                if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= weld )
                    continue;
            }
            pts.push_back( raw[(size_t)i * 3 + 0] );
            pts.push_back( raw[(size_t)i * 3 + 1] );
            pts.push_back( raw[(size_t)i * 3 + 2] );
        }
    }

    // Closed chains store an implicit wrap; drop the repeated endpoint at the same weld.
    if ( closed && (int)( pts.size() / 3 ) >= 2 )
    {
        const int n = (int)( pts.size() / 3 );
        const float dx = pts[0] - pts[( n - 1 ) * 3 + 0];
        const float dy = pts[1] - pts[( n - 1 ) * 3 + 1];
        const float dz = pts[2] - pts[( n - 1 ) * 3 + 2];
        if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= weld )
            pts.resize( (size_t)( n - 1 ) * 3 );
    }

    if ( (int)( pts.size() / 3 ) < 2 )
    {
        Sys_Printf( "Join: the chain collapsed to nothing.\n" );
        return false;
    }
    if ( (int)( pts.size() / 3 ) > KCON_MAX_POINTS )
    {
        Sys_Printf( "Join: the chain would be %i points, the limit is %i.\n",
                    (int)( pts.size() / 3 ), KCON_MAX_POINTS );
        return false;
    }

    kconObject_t merged;
    merged.type   = KCON_POLYLINE;
    merged.plane  = KiwiCon_At( objs[0] )->plane;   // seed only; KiwiCon_Add refits
    merged.pts    = pts;
    merged.closed = closed;

    // ONE store-undo snapshot for the whole operation (kiwi_conselect.h UNDO).
    KiwiCon_UndoPush();
    s_sel.clear();                              // the indices are about to move
    // Remove HIGH index first so the lower ones stay valid while removing.
    for ( int i = (int)objs.size() - 1; i >= 0; --i )
        KiwiCon_RemoveAt( objs[i] );
    const int added = KiwiCon_Add( merged );
    if ( added >= 0 )
    {
        kconSelItem_t it;
        it.object = added;  it.kind = KCONSEL_OBJECT;  it.index = -1;
        s_sel.push_back( it );
    }

    // Report the normalized store count, which may differ after seam removal.
    const kconObject_t *stored = ( added >= 0 ) ? KiwiCon_At( added ) : 0;
    Sys_Printf( "Join: %i lines -> one %s polyline (%i points).\n",
                (int)objs.size(), closed ? "CLOSED" : "open",
                stored ? (int)( stored->pts.size() / 3 ) : (int)( pts.size() / 3 ) );
    g_nUpdateBits |= 1;
    return true;
}

// Extend (KIWI 2026-09-25, user: "an 'extend line' command that takes a line/s and just
// extends them.  It's used to fill gaps between regions after lines are moved without
// changing their properties", then: "the extending should be an operation I control with a
// lollipop handle").  A modal command: the lollipop sits on the selected end nearest the
// cursor, pointing along its line; dragging it (or typing a length) moves every selected end
// that far along its OWN end segment, so an object keeps its direction, its other points,
// group, name and plane.  Nothing is written until Enter.
namespace
{
    const float KCON_EXTEND_MAX    = 4096.0f;   // WORLD UNITS an end may travel
    const float KCON_EXTEND_MAG_PX = 10.0f;     // the handle clicks onto a line crossing this close
    const float KCON_EXTEND_MIN    = 0.01f;     // below this an end has not moved

    struct extendEnd_t
    {
        int   object;
        int   vert;                             // the end's index in pts (0 or n-1)
        int   seg;                              // its end segment's index
        float p[3], dir[3];                     // the end and its outward unit direction
        float first;                            // "Stop at lines": where it must stop
        float t;                                // live travel
        int   hitObject, hitSeg;                // the line `first` belongs to (-1: none)
    };

    // A straight, open line / polyline Extend can lengthen.
    bool Extendable( const kconObject_t *o )
    {
        return o && !o->hidden && ( o->type == KCON_LINE || o->type == KCON_POLYLINE )
            && !o->closed && !KiwiCon_HasSmooth( *o ) && o->pts.size() >= 6;
    }

    // The ends of object `obj` (n points) the selection names: bit 0 start, bit 1 finish.
    // *whole = the object itself is selected (Begin then takes its FREE ends); an end point
    // (Point mode) or end segment names just that end.
    int SelectedEnds( int obj, int n, bool *whole )
    {
        int ends = 0;
        *whole = false;
        for ( size_t i = 0; i < s_sel.size(); ++i )
        {
            const kconSelItem_t &it = s_sel[i];
            if ( it.object != obj )
                continue;
            if ( it.kind == KCONSEL_OBJECT )
            {
                *whole = true;
                return 3;
            }
            const int last = ( it.kind == KCONSEL_POINT ) ? n - 1 : n - 2;
            if ( it.index == 0 )    ends |= 1;
            if ( it.index == last ) ends |= 2;
        }
        return ends;
    }

    float PointSegDist2( const float *p, const float *a, const float *b )
    {
        float ab[3], ap[3], d[3];
        Sub3( b, a, ab );
        Sub3( p, a, ap );
        const float len2 = Dot3( ab, ab );
        float s = len2 > 0.0f ? Dot3( ap, ab ) / len2 : 0.0f;
        s = s < 0.0f ? 0.0f : ( s > 1.0f ? 1.0f : s );
        for ( int k = 0; k < 3; ++k )
            d[k] = ap[k] - ab[k] * s;
        return Dot3( d, d );
    }

    // How far along the path p + dir*t (t > tol) it comes within tol of segment a-b.  A
    // parallel segment counts only when it lies ON the path (a collinear piece left behind
    // by a move): the path runs into its nearer end.
    bool PathMeetsSegment( const float *p, const float *dir, const float *a, const float *b,
                           float tol, float *outT )
    {
        float e[3], w[3], r[3];
        Sub3( b, a, e );
        Sub3( p, a, w );
        const float ee = Dot3( e, e );
        if ( ee < 1.0e-8f )
            return false;
        const float de = Dot3( dir, e ), dw = Dot3( dir, w ), ew = Dot3( e, w );
        const float den = ee - de * de;
        if ( den <= 1.0e-6f * ee )
        {
            const float *ends[2] = { a, b };
            bool  have = false;
            float best = 0.0f;
            for ( int n = 0; n < 2; ++n )
            {
                Sub3( ends[n], p, r );
                const float t = Dot3( r, dir );
                if ( t <= tol )
                    continue;
                for ( int k = 0; k < 3; ++k )
                    r[k] -= dir[k] * t;
                if ( Dot3( r, r ) > tol * tol )
                    continue;
                if ( !have || t < best )
                {
                    best = t;
                    have = true;
                }
            }
            if ( have )
                *outT = best;
            return have;
        }
        float s = ( ew - de * dw ) / den;
        s = s < 0.0f ? 0.0f : ( s > 1.0f ? 1.0f : s );
        for ( int k = 0; k < 3; ++k )
            r[k] = a[k] + e[k] * s - p[k];
        const float t = Dot3( r, dir );
        if ( t <= tol )
            return false;
        for ( int k = 0; k < 3; ++k )
            r[k] -= dir[k] * t;
        if ( Dot3( r, r ) > tol * tol )
            return false;
        *outT = t;
        return true;
    }

    // Is the end already on another visible line (so a region closes there already)?
    bool EndTouches( const extendEnd_t &e, float tol )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !o || o->hidden )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float a[3], b[3];
                if ( i == e.object && s == e.seg )
                    continue;
                if ( !KiwiCon_SegmentWorld( *o, s, a, b ) )
                    break;
                if ( PointSegDist2( e.p, a, b ) <= tol * tol )
                    return true;
            }
        }
        return false;
    }

    // The first visible line ahead of the end within KCON_EXTEND_MAX.
    bool FirstBoundary( const extendEnd_t &e, float tol, float *outT, int *outObject, int *outSeg )
    {
        bool  have = false;
        float best = KCON_EXTEND_MAX;
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !o || o->hidden )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float a[3], b[3], t;
                if ( i == e.object && s == e.seg )
                    continue;
                if ( !KiwiCon_SegmentWorld( *o, s, a, b ) )
                    break;
                if ( PathMeetsSegment( e.p, e.dir, a, b, tol, &t ) && t <= best )
                {
                    best       = t;
                    have       = true;
                    *outObject = i;
                    *outSeg    = s;
                }
            }
        }
        *outT = best;
        return have;
    }

    // Where two free ends' paths cross, each within (tol, KCON_EXTEND_MAX]: a corner.
    bool PathsMeet( const extendEnd_t &a, const extendEnd_t &b, float tol, float *ta, float *tb )
    {
        const float dd  = Dot3( a.dir, b.dir );
        const float den = 1.0f - dd * dd;
        if ( den <= 1.0e-6f )
            return false;
        float w[3], gap[3];
        Sub3( a.p, b.p, w );
        const float da = Dot3( a.dir, w ), db = Dot3( b.dir, w );
        const float t = ( dd * db - da ) / den;
        const float s = ( db - dd * da ) / den;
        if ( t <= tol || s <= tol || t > KCON_EXTEND_MAX || s > KCON_EXTEND_MAX )
            return false;
        for ( int k = 0; k < 3; ++k )
            gap[k] = ( a.p[k] + a.dir[k] * t ) - ( b.p[k] + b.dir[k] * s );
        if ( Dot3( gap, gap ) > tol * tol )
            return false;
        *ta = t;
        *tb = s;
        return true;
    }
}

bool KiwiConSel_CanExtend()
{
    std::vector<int> objs;
    SelectedObjects( &objs );
    for ( size_t i = 0; i < objs.size(); ++i )
    {
        const kconObject_t *o = KiwiCon_At( objs[i] );
        bool whole;
        if ( Extendable( o ) && SelectedEnds( objs[i], (int)( o->pts.size() / 3 ), &whole ) )
            return true;
    }
    return false;
}

namespace
{
    // Every line the end's path crosses within KCON_EXTEND_MAX: the handle's magnets.
    void PathCrossings( const extendEnd_t &e, float tol, std::vector<float> *out )
    {
        out->clear();
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !o || o->hidden )
                continue;
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float a[3], b[3], t;
                if ( i == e.object && s == e.seg )
                    continue;
                if ( !KiwiCon_SegmentWorld( *o, s, a, b ) )
                    break;
                if ( PathMeetsSegment( e.p, e.dir, a, b, tol, &t ) && t <= KCON_EXTEND_MAX )
                    out->push_back( t );
            }
        }
    }

    // Screen distance from the cursor to world point p (FLT_MAX when either is unknown).
    float CursorPixels( const float *p, bool haveCursor, int cx, int cy )
    {
        float x, y;
        if ( !haveCursor || !Pick_WorldToImage( p, &x, &y ) )
            return 3.0e38f;
        return PixelDist( x, y, (float)cx, (float)cy );
    }

    bool s_extendStop = true;                   // "Stop at the first line" (F), kept across runs

    class KiwiExtendCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Extend Lines"; }
        bool CanExecute() override { return KiwiConSel_CanExtend(); }

        // Construction grammar: the handle snaps by default, Ctrl frees it.
        kiwiSnapCtx_t SnapContext() const override { return KSNAPCTX_CONSTRUCT; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t stop[] = { { "F", "Pass lines" } };
            static const kiwiPrompt_t pass[] = { { "F", "Stop at lines" } };
            *out = s_extendStop ? stop : pass;
            return 1;
        }

        int CommandOptions( const kiwiOption_t **out ) const override
        {
            static const kiwiOption_t options[] =
                { { "Stop at the first line", KOPT_TOGGLE, nullptr, 0, 0, 0, 1, -1 } };
            *out = options;
            return 1;
        }
        int OptionValue( int opt ) const override { return opt == 0 ? (int)s_extendStop : 0; }
        void OptionChanged( int opt, int value ) override
        {
            if ( opt != 0 )
                return;
            s_extendStop = value != 0;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( vk != 'F' || mods )
                return false;
            OptionChanged( 0, !s_extendStop );
            return true;
        }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out || m_ends.empty() )
                return false;
            *out = m_ends[m_primary].t;
            return true;
        }
        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( m_ends.empty() || !out3 )
                return false;
            const extendEnd_t &e = m_ends[m_primary];
            Mad3( e.p, e.dir, e.t, out3 );
            return true;
        }

        // The handle rides the primary end, pointing on along its line.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_ends.empty() || !outAnchor || !outDir )
                return false;
            const extendEnd_t &e = m_ends[m_primary];
            Mad3( e.p, e.dir, e.t, outAnchor );
            Copy3( e.dir, outDir );
            return true;
        }

        // A grab drops travel the stop swallowed, so the handle answers from where it shows.
        void Rebase() override
        {
            if ( !m_ends.empty() )
                m_dist = m_ends[m_primary].t;
            LatchStart();
        }

        bool Begin() override
        {
            m_ends.clear();
            m_cross.clear();
            m_primary   = 0;
            m_dist      = 0.0f;
            m_haveStart = false;
            m_hasNum    = false;
            m_onLine    = false;
            m_hud[0]    = '\0';
            const float tol = KiwiRegion_WeldDist();
            int  cx = 0, cy = 0;
            const bool haveCursor = KiwiCmd_LastCursor( &cx, &cy );

            std::vector<int> objs;
            SelectedObjects( &objs );
            for ( size_t i = 0; i < objs.size(); ++i )
            {
                const kconObject_t *o = KiwiCon_At( objs[i] );
                if ( !Extendable( o ) )
                    continue;
                const int n = (int)( o->pts.size() / 3 );
                bool whole;
                const int want = SelectedEnds( objs[i], n, &whole );
                extendEnd_t cand[2];
                bool ok[2] = { false, false }, freeEnd[2] = { false, false };
                for ( int side = 0; side < 2; ++side )
                {
                    if ( !( want & ( 1 << side ) ) )
                        continue;
                    extendEnd_t &e = cand[side];
                    e.object    = objs[i];
                    e.vert      = side ? n - 1 : 0;
                    e.seg       = side ? n - 2 : 0;
                    e.first     = KCON_EXTEND_MAX;
                    e.t         = 0.0f;
                    e.hitObject = e.hitSeg = -1;
                    const int nb = side ? n - 2 : 1;
                    float q[3];
                    for ( int k = 0; k < 3; ++k )
                    {
                        e.p[k] = o->pts[(size_t)e.vert * 3 + k];
                        q[k]   = o->pts[(size_t)nb * 3 + k];
                    }
                    Sub3( e.p, q, e.dir );
                    if ( !Norm3( e.dir, 1.0e-4f ) )
                        continue;
                    ok[side]      = true;
                    freeEnd[side] = !EndTouches( e, tol );
                }
                // A whole object extends its FREE ends; with neither free, the one nearer the
                // cursor.  Named ends (Point mode / end segments) extend as named.
                if ( whole )
                {
                    if ( ok[0] && ok[1] && !freeEnd[0] && !freeEnd[1] )
                    {
                        const bool startNearer = CursorPixels( cand[0].p, haveCursor, cx, cy )
                                              <= CursorPixels( cand[1].p, haveCursor, cx, cy );
                        ok[startNearer ? 1 : 0] = false;
                    }
                    else
                        for ( int side = 0; side < 2; ++side )
                            if ( !freeEnd[side] )
                                ok[side] = false;
                }
                for ( int side = 0; side < 2; ++side )
                    if ( ok[side] )
                        m_ends.push_back( cand[side] );
            }
            if ( m_ends.empty() )
            {
                Sys_Printf( "Extend: select open construction lines (or their end points in Point mode) first.\n" );
                return false;
            }

            // Where each end stops with "Stop at the first line": the first line ahead, or
            // where another selected end's path crosses first (a corner a move opened).
            for ( size_t i = 0; i < m_ends.size(); ++i )
                FirstBoundary( m_ends[i], tol, &m_ends[i].first, &m_ends[i].hitObject, &m_ends[i].hitSeg );
            for ( size_t i = 0; i < m_ends.size(); ++i )
                for ( size_t j = i + 1; j < m_ends.size(); ++j )
                {
                    float ti, tj;
                    if ( !PathsMeet( m_ends[i], m_ends[j], tol, &ti, &tj ) )
                        continue;
                    if ( ti < m_ends[i].first ) m_ends[i].first = ti;
                    if ( tj < m_ends[j].first ) m_ends[j].first = tj;
                }
            // Collinear pieces facing each other across a gap meet halfway.
            for ( size_t i = 0; i < m_ends.size(); ++i )
                for ( size_t j = i + 1; j < m_ends.size(); ++j )
                {
                    extendEnd_t &a = m_ends[i], &b = m_ends[j];
                    if ( a.hitObject == b.object && a.hitSeg == b.seg
                      && b.hitObject == a.object && b.hitSeg == a.seg
                      && Dot3( a.dir, b.dir ) < -0.999f )
                        a.first = b.first = a.first * 0.5f;
                }

            // The handle goes on the end nearest the cursor.
            float bestPx = 3.0e38f;
            for ( size_t i = 0; i < m_ends.size(); ++i )
            {
                const float px = CursorPixels( m_ends[i].p, haveCursor, cx, cy );
                if ( px < bestPx )
                {
                    bestPx    = px;
                    m_primary = i;
                }
            }
            PathCrossings( m_ends[m_primary], tol, &m_cross );
            LatchStart();
            Recompute();
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        void Commit() override
        {
            int moved = 0;
            for ( size_t i = 0; i < m_ends.size(); ++i )
                if ( m_ends[i].t > KCON_EXTEND_MIN )
                    ++moved;
            if ( !moved )
            {
                Sys_Printf( "Extend: zero length - nothing changed.\n" );
                m_ends.clear();
                return;
            }
            // ONE store-undo snapshot; the objects keep their indices, so the selection stays.
            KiwiCon_UndoPush();
            for ( size_t i = 0; i < m_ends.size(); ++i )
            {
                const extendEnd_t &e = m_ends[i];
                if ( e.t <= KCON_EXTEND_MIN )
                    continue;
                kconObject_t *o = KiwiCon_MutableAt( e.object );
                if ( !o || o->pts.size() < (size_t)( e.vert + 1 ) * 3 )
                    continue;
                for ( int k = 0; k < 3; ++k )
                    o->pts[(size_t)e.vert * 3 + k] = e.p[k] + e.dir[k] * e.t;
            }
            KiwiCon_NoteMutated();
            Sys_Printf( "Extend: %i end%s extended.\n", moved, moved == 1 ? "" : "s" );
            m_ends.clear();
            g_nUpdateBits |= 1;
        }

        void Cancel() override
        {
            m_ends.clear();
            g_nUpdateBits |= 1;
        }

        int LineBudget() const override { return (int)m_ends.size() + 96; }

        // The extension pieces in construction's in-progress amber; the lines themselves are
        // untouched until Enter.
        void DrawWorld() override
        {
            KiwiLines_Color( 1.00f, 0.85f, 0.45f );
            for ( size_t i = 0; i < m_ends.size(); ++i )
            {
                const extendEnd_t &e = m_ends[i];
                if ( e.t <= KCON_EXTEND_MIN )
                    continue;
                float q[3];
                Mad3( e.p, e.dir, e.t, q );
                if ( !KiwiLines_Add( e.p, q ) )
                    return;
            }
        }

    private:
        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            return KiwiCmd_LastCursor( &x, &y ) && Pick_RayFromImagePos( x, y, out );
        }

        // The cursor's travel along the primary path is measured from here (rebase = relatch).
        void LatchStart()
        {
            m_haveStart = false;
            if ( m_ends.empty() )
                return;
            const extendEnd_t &pe = m_ends[m_primary];
            ray_t ray;
            float q[3], rel[3];
            if ( !KiwiCam_AxisPortrayable( pe.dir ) || !CursorRay( &ray )
              || !KiwiCam_RayAxis( ray, pe.p, pe.dir, q ) )
                return;
            Sub3( q, pe.p, rel );
            m_start     = Dot3( rel, pe.dir ) - m_dist;
            m_haveStart = true;
        }

        // Cursor -> travel: a typed length wins; with snapping on, the path's line crossings
        // (magnetic), then an aimed point projected onto the path, then the soft lattice.
        void Recompute()
        {
            if ( m_ends.empty() )
                return;
            const extendEnd_t &pe = m_ends[m_primary];
            float d = m_dist;
            const bool canAxis = KiwiCam_AxisPortrayable( pe.dir );
            if ( !canAxis )
                m_haveStart = false;                   // looking down the line: hold
            else if ( !m_haveStart )
                LatchStart();
            ray_t ray;
            float q[3], rel[3];
            if ( canAxis && m_haveStart && CursorRay( &ray ) && KiwiCam_RayAxis( ray, pe.p, pe.dir, q ) )
            {
                Sub3( q, pe.p, rel );
                d = Dot3( rel, pe.dir ) - m_start;
            }

            m_onLine = false;
            if ( m_hasNum )
                d = m_numWorld;
            else if ( KiwiCmd_SnapEngaged() )
            {
                float bestGap = 3.0e38f;
                for ( size_t c = 0; c < m_cross.size(); ++c )
                {
                    float at[3];
                    Mad3( pe.p, pe.dir, m_cross[c], at );
                    const float gap = fabsf( d - m_cross[c] );
                    if ( gap <= KCON_EXTEND_MAG_PX * KiwiCam_WorldPerPixel( at ) && gap < bestGap )
                    {
                        bestGap  = gap;
                        m_onLine = true;
                        d        = m_cross[c];
                    }
                }
                float sd = 0.0f;
                if ( !m_onLine && m_snap.valid && KiwiSnap_IsGeometry( m_snap.type ) && m_snap.type != SNAP_FACE
                  && KiwiSnap_AxisDepth( m_snap, pe.p, pe.dir, &sd ) && sd > KCON_EXTEND_MIN )
                    d = sd;
                else if ( !m_onLine )
                {
                    bool major = false;
                    d = KiwiSnap_LatticeAxis( d, pe.p, pe.dir, false, &major );
                }
            }
            if ( d < 0.0f )            d = 0.0f;             // Extend never shortens (Trim does)
            if ( d > KCON_EXTEND_MAX ) d = KCON_EXTEND_MAX;
            m_dist = d;
            for ( size_t i = 0; i < m_ends.size(); ++i )
                m_ends[i].t = ( s_extendStop && d > m_ends[i].first ) ? m_ends[i].first : d;
            UpdateHud();
        }

        void UpdateHud()
        {
            char len[32];
            KiwiUnits_Format( len, sizeof( len ), m_ends.empty() ? 0.0f : m_ends[m_primary].t );
            int stopped = 0;
            for ( size_t i = 0; i < m_ends.size(); ++i )
                if ( s_extendStop && m_dist > m_ends[i].first )
                    ++stopped;
            const int n = (int)m_ends.size();
            if ( stopped )
                _snprintf( m_hud, sizeof( m_hud ), "extend %i end%s  %s  ·  %i stopped at a line",
                           n, n == 1 ? "" : "s", len, stopped );
            else
                _snprintf( m_hud, sizeof( m_hud ), "extend %i end%s  %s%s",
                           n, n == 1 ? "" : "s", len, m_onLine ? "  ·  on a line" : "" );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        std::vector<extendEnd_t> m_ends;
        std::vector<float>       m_cross;               // the primary path's line crossings
        size_t        m_primary   = 0;
        float         m_dist      = 0.0f;               // the handle's travel before the stops
        float         m_start     = 0.0f;
        bool          m_haveStart = false;
        bool          m_hasNum    = false;
        float         m_numWorld  = 0.0f;
        bool          m_onLine    = false;
        snap_result_t m_snap;
        char          m_hud[128]  = "";
    };

    KiwiExtendCommand s_extend;
}

KiwiEditorCommand *KiwiConSel_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_CONSTRUCT_EXTEND ) ? &s_extend : 0;
}

// KIWI (2026-09-25, user: "make the key (E), but only for lines"): E is Extrude's key; with
// construction lines selected - no brush selection, no region selected - it extends them.
// Faces (even one merely under the cursor) and regions keep E = Extrude.
int KiwiConSel_ContextE( int commandId )
{
    if ( commandId != KIWI_CMD_EXTRUDE_FACE || !BrushSelectionEmpty()
      || KiwiRegion_HasSelection() || !KiwiConSel_CanExtend() )
        return commandId;
    return KIWI_CMD_CONSTRUCT_EXTEND;
}

// Delete
namespace
{
    // Count fine brush items that construction Delete leaves selected.
    int BrushFineItemCount()
    {
        int n = 0;
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
            if ( sel.items[i].kind != SEL_OBJECT )
                ++n;
        return n;
    }
}

// Construction owns Delete only when the legacy whole-object list is empty.
// Fine typed brush items remain selected; whole objects keep classic Delete.
bool KiwiConSel_OwnsDelete()
{
    if ( s_sel.empty() )
        return false;
    return selected_brushes.next == &selected_brushes;
}

// Palette availability does not use the key's legacy-selection arbitration.
bool KiwiConSel_CanDelete()
{
    return !s_sel.empty();
}

bool KiwiConSel_DeleteSelected()
{
    std::vector<int> objs;
    SelectedObjects( &objs );
    if ( objs.empty() )
        return false;

    const int skipped = BrushFineItemCount();

    KiwiCon_UndoPush();
    s_sel.clear();
    for ( int i = (int)objs.size() - 1; i >= 0; --i )
        KiwiCon_RemoveAt( objs[i] );

    Sys_Printf( "Construction: deleted %i object%s.\n",
                (int)objs.size(), ( objs.size() == 1 ) ? "" : "s" );
    if ( skipped )
        Sys_Printf( "  (%i brush point/edge/face item%s left alone — brush geometry "
                    "is not deletable this way.)\n",
                    skipped, ( skipped == 1 ) ? "" : "s" );
    g_nUpdateBits |= 1;
    return true;
}

// Hide
// Bare H hides both halves of a mixed selection; other H modifiers are untouched.
// Palette availability and key ownership intentionally share one predicate.
bool KiwiConSel_OwnsHide()
{
    return !s_sel.empty();
}

bool KiwiConSel_CanHide()
{
    return KiwiConSel_OwnsHide();
}

bool KiwiConSel_HideSelected()
{
    std::vector<int> objs;
    SelectedObjects( &objs );
    if ( objs.empty() )
        return false;

    // One pre-mutation store snapshot makes the whole hide undoable.
    KiwiCon_UndoPush();
    for ( size_t i = 0; i < objs.size(); ++i )
        KiwiCon_SetHidden( objs[i], true );
    // Hidden objects are inert, so they cannot remain selected.
    s_sel.clear();

    Sys_Printf( "Construction: hid %i object%s (Unhide All restores them).\n",
                (int)objs.size(), ( objs.size() == 1 ) ? "" : "s" );
    g_nUpdateBits |= 1;
    return true;
}

// Duplicate brush edges as construction lines
namespace
{
    // One duplicated edge in world space.
    struct kedge_t
    {
        float a[3], b[3];
        bool  used;                 // consumed by a polyline run
    };

    inline float EdgeDist3( const float *p, const float *q )
    {
        const float dx = p[0]-q[0], dy = p[1]-q[1], dz = p[2]-q[2];
        return sqrtf( dx * dx + dy * dy + dz * dz );
    }

    inline bool SamePoint( const float *p, const float *q )
    {
        return EdgeDist3( p, q ) <= KREG_JOIN_DIST;
    }

    // Sel_EdgeEnds includes kind, liveness, and winding-bound checks.
    // Drop coincident duplicates because adjacent faces can contribute one edge twice.
    void GatherSelectedEdges( std::vector<kedge_t> *out )
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            kedge_t e;
            if ( !Sel_EdgeEnds( sel.items[i], e.a, e.b ) )
                continue;
            if ( EdgeDist3( e.a, e.b ) <= KREG_JOIN_DIST )
                continue;                       // a degenerate winding edge
            bool dup = false;
            for ( size_t k = 0; k < out->size() && !dup; ++k )
                dup = ( SamePoint( ( *out )[k].a, e.a ) && SamePoint( ( *out )[k].b, e.b ) )
                   || ( SamePoint( ( *out )[k].a, e.b ) && SamePoint( ( *out )[k].b, e.a ) );
            if ( dup )
                continue;
            e.used = false;
            out->push_back( e );
        }
    }

    // Greedily extend both ends. At branch points, leftover edges become later
    // runs and region assembly may re-chain them.
    void GrowRun( std::vector<kedge_t> &edges, size_t seed,
                  std::vector<float> *pts, bool *closed )
    {
        edges[seed].used = true;
        pts->clear();
        for ( int k = 0; k < 3; ++k ) pts->push_back( edges[seed].a[k] );
        for ( int k = 0; k < 3; ++k ) pts->push_back( edges[seed].b[k] );
        *closed = false;

        bool grew = true;
        while ( grew )
        {
            grew = false;
            const size_t n = pts->size();
            // Copy endpoints before vector growth can invalidate element pointers.
            float head[3], tail[3];
            for ( int k = 0; k < 3; ++k ) head[k] = ( *pts )[k];
            for ( int k = 0; k < 3; ++k ) tail[k] = ( *pts )[n - 3 + k];
            if ( SamePoint( head, tail ) )
            {
                pts->resize( n - 3 );           // drop the repeated closing point
                *closed = true;
                return;
            }
            for ( size_t i = 0; i < edges.size() && !grew; ++i )
            {
                if ( edges[i].used )
                    continue;
                const float *ea = edges[i].a;
                const float *eb = edges[i].b;
                if ( SamePoint( tail, ea ) )
                {
                    for ( int k = 0; k < 3; ++k ) pts->push_back( eb[k] );
                    edges[i].used = true;  grew = true;
                }
                else if ( SamePoint( tail, eb ) )
                {
                    for ( int k = 0; k < 3; ++k ) pts->push_back( ea[k] );
                    edges[i].used = true;  grew = true;
                }
                else if ( SamePoint( head, ea ) )
                {
                    pts->insert( pts->begin(), eb, eb + 3 );
                    edges[i].used = true;  grew = true;
                }
                else if ( SamePoint( head, eb ) )
                {
                    pts->insert( pts->begin(), ea, ea + 3 );
                    edges[i].used = true;  grew = true;
                }
            }
        }
        // A run may close on the final extension.
        const size_t n = pts->size();
        if ( n >= 9 && SamePoint( &( *pts )[0], &( *pts )[n - 3] ) )
        {
            pts->resize( n - 3 );
            *closed = true;
        }
    }
}

bool KiwiConSel_CanDuplicateEdges()
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( sel.items[i].kind == SEL_EDGE && Sel_BrushLive( sel.items[i].brush ) )
            return true;
    return false;
}

bool KiwiConSel_DuplicateEdgesAsLines()
{
    std::vector<kedge_t> edges;
    GatherSelectedEdges( &edges );
    if ( edges.empty() )
    {
        Sys_Printf( "Duplicate: no brush edges are selected.\n" );
        return false;
    }

    // One snapshot covers every object created by this command.
    KiwiCon_UndoPush();

    int added = 0, rings = 0;
    for ( size_t i = 0; i < edges.size(); ++i )
    {
        if ( edges[i].used )
            continue;
        std::vector<float> pts;
        bool closed = false;
        GrowRun( edges, i, &pts, &closed );
        if ( (int)( pts.size() / 3 ) < 2 )
            continue;

        kconObject_t o;
        // Two points form a line; longer runs use the ordinary polyline type.
        const int nPts = (int)( pts.size() / 3 );
        o.type   = ( nPts == 2 ) ? KCON_LINE : KCON_POLYLINE;
        o.closed = closed && ( nPts >= 3 );
        o.pts.swap( pts );
        if ( KiwiCon_Add( o ) < 0 )
            continue;
        ++added;
        if ( o.closed )
            ++rings;
    }

    if ( added <= 0 )
    {
        Sys_Printf( "Duplicate: nothing was created from the selected edges.\n" );
        return false;
    }
    Sys_Printf( "Duplicate: %i construction object%s from %i brush edge%s%s.\n",
                added, ( added == 1 ) ? "" : "s",
                (int)edges.size(), ( edges.size() == 1 ) ? "" : "s",
                rings ? " (a closed ring is now an extrudable region)" : "" );
    g_nUpdateBits |= 1;
    return true;
}

// Move
bool KiwiConSel_CanMove()
{
    return !s_sel.empty() && BrushSelectionEmpty();
}

bool KiwiConSel_MoveBegin( float outRef[3] )
{
    s_moveBase.clear();
    s_moveOpen = false;

    std::vector<int> objs;
    SelectedObjects( &objs );
    if ( objs.empty() )
        return false;

    // Use defining anchors for the centroid so tessellation density cannot bias it.
    float sum[3] = { 0.0f, 0.0f, 0.0f };
    int   n      = 0;
    for ( size_t i = 0; i < objs.size(); ++i )
    {
        const kconObject_t *o = KiwiCon_At( objs[i] );
        if ( !o )
            continue;
        moveBase_t b;
        b.object = objs[i];
        for ( int k = 0; k < 3; ++k )
            b.origin[k] = o->plane.origin[k];
        b.pts    = o->pts;
        b.plane  = o->plane;
        b.radius = o->radius;
        s_moveBase.push_back( b );

        const int anchors = KiwiCon_AnchorCount( *o );
        for ( int a = 0; a < anchors; ++a )
        {
            float w[3];
            if ( !KiwiCon_AnchorWorld( *o, a, w ) )
                break;
            for ( int k = 0; k < 3; ++k )
                sum[k] += w[k];
            ++n;
        }
    }
    if ( s_moveBase.empty() || n < 1 )
        return false;

    if ( outRef )
        for ( int k = 0; k < 3; ++k )
            outRef[k] = sum[k] / (float)n;

    // The pre-mutation snapshot is both cancel state and the undo record.
    KiwiCon_UndoPush();
    s_moveOpen = true;
    return true;
}

void KiwiConSel_MoveApply( const float delta[3] )
{
    if ( !s_moveOpen || !delta )
        return;
    // Apply absolute world-space deltas from the baseline. plane.origin carries
    // parametric shapes and remains useful when a point-shape refit fails.
    // Check point counts before acquiring mutable pointers so cancel can restore safely.
    for ( size_t i = 0; i < s_moveBase.size(); ++i )
    {
        const kconObject_t *chk = KiwiCon_At( s_moveBase[i].object );
        if ( chk && chk->pts.size() != s_moveBase[i].pts.size() )
        {
            Sys_Printf( "Move: the construction store changed under the gesture — "
                        "move abandoned.\n" );
            KiwiConSel_MoveCancel();
            return;
        }
    }

    for ( size_t i = 0; i < s_moveBase.size(); ++i )
    {
        const moveBase_t &b = s_moveBase[i];
        kconObject_t *o = KiwiCon_MutableAt( b.object );
        if ( !o )
            continue;
        for ( int k = 0; k < 3; ++k )
            o->plane.origin[k] = b.origin[k] + delta[k];
        if ( o->pts.size() == b.pts.size() )
            for ( size_t k = 0; k + 2 < b.pts.size(); k += 3 )
            {
                o->pts[k + 0] = b.pts[k + 0] + delta[0];
                o->pts[k + 1] = b.pts[k + 1] + delta[1];
                o->pts[k + 2] = b.pts[k + 2] + delta[2];
            }
    }
    KiwiCon_NoteMutated();
}

namespace
{
    // The baseline must still describe the store; otherwise abandon the gesture.
    bool BaselineValid( const char *what )
    {
        for ( size_t i = 0; i < s_moveBase.size(); ++i )
        {
            const kconObject_t *chk = KiwiCon_At( s_moveBase[i].object );
            if ( chk && chk->pts.size() != s_moveBase[i].pts.size() )
            {
                Sys_Printf( "%s: the construction store changed under the gesture — "
                            "%s abandoned.\n", what, what );
                KiwiConSel_MoveCancel();
                return false;
            }
        }
        return true;
    }
}

void KiwiConSel_RotateApply( const float pivot[3], int axis, float degrees )
{
    if ( axis < 0 || axis > 2 )
        return;
    // Negative: the ported Select_RotateAxis turns the other way for the same degrees,
    // and the ring feeds both, so construction must spin like brushes do.
    const float rad = -degrees * KCON_DEG2RAD;
    const float c = cosf( rad ), sn = sinf( rad );
    const int i = ( axis + 1 ) % 3, j = ( axis + 2 ) % 3;
    float m[3][3];
    for ( int a = 0; a < 3; ++a )
        for ( int k = 0; k < 3; ++k )
            m[a][k] = ( a == k ) ? 1.0f : 0.0f;
    m[i][i] = c;  m[i][j] = -sn;
    m[j][i] = sn; m[j][j] = c;
    KiwiConSel_RotateApplyMatrix( pivot, m );
}

namespace
{
    void MatVec( const float m[3][3], const float in[3], float out[3] )
    {
        for ( int i = 0; i < 3; ++i )
            out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
    }

    void MatAbout( const float pivot[3], const float m[3][3], const float in[3], float out[3] )
    {
        float rel[3], turned[3];
        for ( int k = 0; k < 3; ++k ) rel[k] = in[k] - pivot[k];
        MatVec( m, rel, turned );
        for ( int k = 0; k < 3; ++k ) out[k] = pivot[k] + turned[k];
    }
}

void KiwiConSel_RotateApplyMatrix( const float pivot[3], const float m[3][3] )
{
    if ( !s_moveOpen || !pivot || !m )
        return;
    if ( !BaselineValid( "Rotate" ) )
        return;
    for ( size_t i = 0; i < s_moveBase.size(); ++i )
    {
        const moveBase_t &b = s_moveBase[i];
        kconObject_t *o = KiwiCon_MutableAt( b.object );
        if ( !o )
            continue;
        // Rigid frame rotation keeps circles/arcs parametric: the origin orbits the
        // pivot and the basis vectors turn with it (centre/radius/angles unchanged).
        MatAbout( pivot, m, b.plane.origin, o->plane.origin );
        MatVec( m, b.plane.normal, o->plane.normal );
        MatVec( m, b.plane.u,      o->plane.u );
        MatVec( m, b.plane.v,      o->plane.v );
        if ( o->pts.size() == b.pts.size() )
            for ( size_t k = 0; k + 2 < b.pts.size(); k += 3 )
                MatAbout( pivot, m, &b.pts[k], &o->pts[k] );
    }
    KiwiCon_NoteMutated();
}

void KiwiConSel_ScaleApply( const float pivot[3], const float factor[3] )
{
    if ( !s_moveOpen || !pivot || !factor )
        return;
    if ( !BaselineValid( "Scale" ) )
        return;
    for ( size_t i = 0; i < s_moveBase.size(); ++i )
    {
        const moveBase_t &b = s_moveBase[i];
        kconObject_t *o = KiwiCon_MutableAt( b.object );
        if ( !o )
            continue;
        for ( int k = 0; k < 3; ++k )
            o->plane.origin[k] = pivot[k] + ( b.plane.origin[k] - pivot[k] ) * factor[k];
        if ( o->pts.size() == b.pts.size() )
            for ( size_t k = 0; k + 2 < b.pts.size(); k += 3 )
                for ( int a = 0; a < 3; ++a )
                    o->pts[k + a] = pivot[a] + ( b.pts[k + a] - pivot[a] ) * factor[a];
        if ( KiwiCon_IsParametric( *o ) )
        {
            // A circle cannot become an ellipse: use the mean factor of the two world
            // axes that carry most of the plane (weighted by the basis vectors), so a
            // uniform scale is exact and an axis scale still grows the ring sensibly.
            float wsum = 0.0f, fsum = 0.0f;
            for ( int a = 0; a < 3; ++a )
            {
                const float w = b.plane.u[a] * b.plane.u[a] + b.plane.v[a] * b.plane.v[a];
                wsum += w;
                fsum += w * factor[a];
            }
            const float f = wsum > 1.0e-6f ? fsum / wsum : factor[0];
            o->radius = b.radius * ( f > 0.0f ? f : -f );
            if ( o->radius < 1.0e-3f )
                o->radius = 1.0e-3f;
        }
    }
    KiwiCon_NoteMutated();
}

void KiwiConSel_MoveCancel()
{
    if ( !s_moveOpen )
        return;
    s_moveBase.clear();
    // Pop while s_moveOpen is true so store replacement preserves the restored,
    // index-valid selection.
    KiwiCon_UndoPop();
    s_moveOpen = false;
}

// The latched move baseline is the authoritative self-snap exclusion set.
bool KiwiConSel_SnapMuted( int objectIndex )
{
    if ( !s_moveOpen || objectIndex < 0 )
        return false;
    for ( size_t i = 0; i < s_moveBase.size(); ++i )
        if ( s_moveBase[i].object == objectIndex )
            return true;
    return false;
}

void KiwiConSel_MoveCommit()
{
    if ( !s_moveOpen )
        return;
    s_moveOpen = false;
    s_moveBase.clear();
    // The snapshot STAYS: it is this gesture's undo record.
    KiwiCon_NoteMutated();
}

// Commands
void KiwiConSel_RegisterCommands()
{
    // Keep registrations unbound: profiles bind Join, while shared Delete/H chords
    // are arbitrated by KiwiUX_KeyFunnel.
    Radiant_RegisterCommand( "KiwiConstructJoin",   0, 0, KIWI_CMD_CONSTRUCT_JOIN );
    Radiant_RegisterCommand( "KiwiConstructExtend", 0, 0, KIWI_CMD_CONSTRUCT_EXTEND );
    Radiant_RegisterCommand( "KiwiConstructDelete", 0, 0, KIWI_CMD_CONSTRUCT_DELETE );
    // Unhide All remains palette-only.
    Radiant_RegisterCommand( "KiwiConstructHide",      0, 0, KIWI_CMD_CONSTRUCT_HIDE );
    Radiant_RegisterCommand( "KiwiConstructUnhideAll", 0, 0, KIWI_CMD_CONSTRUCT_UNHIDE );
}

bool KiwiConSel_DispatchInstant( unsigned int cmdId )
{
    switch ( cmdId )
    {
    case KIWI_CMD_CONSTRUCT_JOIN:
        KiwiConSel_Join();
        return true;
    case KIWI_CMD_CONSTRUCT_DELETE:
        if ( !KiwiConSel_DeleteSelected() )
            Sys_Printf( "Construction: nothing selected.\n" );
        return true;
    case KIWI_CMD_CONSTRUCT_HIDE:
        if ( !KiwiConSel_HideSelected() )
            Sys_Printf( "Construction: nothing selected.\n" );
        return true;
    case KIWI_CMD_CONSTRUCT_UNHIDE:
    {
        const int n = KiwiCon_UnhideAll();
        int images = 0;
        for ( int i = 0; i < KiwiRefImage_Count(); ++i )
        {
            const krefImage_t *img = KiwiRefImage_At( i );
            if ( img && img->hidden && KiwiRefImage_SetHidden( i, false ) )
                ++images;
        }
        if ( n > 0 || images > 0 )
            Sys_Printf( "Construction: unhid %i object%s and %i reference image%s.\n",
                        n, ( n == 1 ) ? "" : "s", images, ( images == 1 ) ? "" : "s" );
        else
            Sys_Printf( "Construction: nothing is hidden.\n" );
        g_nUpdateBits |= 1;
        return true;
    }
    default:
        return false;
    }
}
