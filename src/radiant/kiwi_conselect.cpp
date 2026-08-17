#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_conselect.cpp — SHAKEOUT F implementation.  See kiwi_conselect.h for the
// parallel-selection ruling, the click arbitration rule and the undo shape.
//
// NEW code over the KIWI layers.  It touches NO ported state: the construction
// store is KIWI's own (kiwi_construct.cpp), the chain walker is KIWI's own
// (kiwi_region.cpp) and the only ported things read here are the two selection
// sentinels — read, never written — to answer "is anything brush-side selected".
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_conselect.h"
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_pick.h"
#include "kiwi_region.h"
#include "kiwi_selection.h"

#include <math.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int  Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:112  int Sys_Printf(const char*,...)
extern int  g_nUpdateBits;                       // engine_stubs.cpp:773  int g_nUpdateBits = 0  (0x25D5A74)
// `selected_brushes` is the DISPLAY-list sentinel: declared in qe3.h:1054
// (`extern selbrush_t selected_brushes;  // 0x23f1864`), defined in
// engine_stubs.cpp:697 (`selbrush_t selected_brushes{};`).  READ ONLY here — the
// only question asked of it is "is anything brush-side selected".
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
                                                 // mainfrm.cpp:1246  bool Radiant_RegisterCommand(const char*,byte,byte,int)

namespace
{
    std::vector<kconSelItem_t> s_sel;

    // ── the MOVE gesture's baseline (kiwi_conselect.h UNDO) ─────────────────
    // SHAKEOUT H: the baseline is now the object's WORLD POINTS as well as its
    // plane origin.  A translate used to move only `plane.origin` and leave the
    // plane-space points alone, which was the only spelling that could not lose
    // coplanarity back when points WERE plane-space.  With world points that
    // shortcut moves nothing at all — the points would stay exactly where they
    // were — so the translate is now what it says: every point plus the origin.
    struct moveBase_t
    {
        int                object;
        float              origin[3];   // the object's plane origin at Begin
        std::vector<float> pts;         // its WORLD points at Begin
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

    // Nothing at all is selected brush-side.  BOTH halves are asked because they
    // can legitimately disagree: an EDGE-only selection lives in KiwiSel() with an
    // EMPTY legacy list since shakeout D (kiwi_selection.h Sel_NoteLegacyDeselect
    // explains why).
    // KIWI-UX (CLEANUP, A-9): the one caller is KiwiConSel_CanMove.  G's
    // construction arm must not run while ANY brush-side item is selected —
    // including the fine kinds (edges, faces) that never reach selected_brushes —
    // so Move needs both halves.  Delete uses the sentinel alone — see OwnsDelete.
    bool BrushSelectionEmpty()
    {
        if ( selected_brushes.next != &selected_brushes )
            return false;
        return KiwiSel().items.empty();
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AA, ITEM 7) — THE TWO EXCLUSIVE MODES PICK NO LINES
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "Make pick mode 4 a brush-only pick mode.  Also for
    // some reason lines are clickable in mode 3.  It should only be faces and
    // construction faces."
    //
    // Shakeout F's answer to "lines aren't selectable with ANY mode" was to make
    // every mode pick them (KindForMode below folded Face and Object into the
    // whole-object arm, and its comment argued that answering "…except mode 3"
    // would be answering half the report).  That was right at the time and is
    // wrong now: the report has been made from the other side.  The modes are a
    // FILTER, and a filter that never excludes anything is not one.
    //
    // The rule, and it is exactly two lines of it:
    //   mode 3 (mask == SEL_MASK_FACE  ) — faces.  Brush faces and construction
    //           REGION faces, which ARE faces (kiwi_region.cpp owns them and
    //           ClickSelect picks them on their own arm).  A construction LINE is
    //           not a face and never will be — that half of shakeout F's sentence
    //           was always true, it was just being used to argue the opposite.
    //   mode 4 (mask == SEL_MASK_OBJECT) — brush / entity objects.  Nothing else,
    //           which is the whole content of "brush-only".  Region faces go too.
    //
    // EXACT EQUALITY, not a bit test.  Mode 5 (Everything) has the OBJECT and FACE
    // bits set as well, and mode 5 must keep picking everything — it is the mode
    // whose entire job is not filtering.  A bit test would silence construction in
    // mode 5 as collateral, which is why this cannot be folded into KindForMode's
    // priority chain (that chain is a "which granularity" question and this is a
    // "whether at all" question; two questions, two predicates).
    //
    // AND THIS RETIRES THE 14-PIXEL LINE-BEATS-FACE RULE IN THOSE MODES BY
    // CONSTRUCTION.  KCON_CLICK_PIXELS (kiwi_construct.h:233) is only reachable
    // through PickAt, and ClickSelect's arbitration (`brushPointish` — an area hit
    // always loses to a construction line, kiwi_boxselect.cpp) only runs when
    // PickAt answered.  With PickAt refusing outright in modes 3 and 4, a line can
    // no longer beat the face it is drawn on top of there, and the rule keeps
    // working unchanged everywhere it was wanted.  No constant changed.
    bool ModeAllowsConstructionLines()
    {
        const sel_mask_t mask = KiwiSel_GetModeMask();
        return mask != SEL_MASK_FACE && mask != SEL_MASK_OBJECT;
    }

    // The granularity the CURRENT selection mode asks for.  Point mode picks
    // anchors, Edge mode picks segments, and everything else picks WHOLE OBJECTS.
    // (Face and Object still resolve to KCONSEL_OBJECT here — they simply never
    // reach this function any more, because ModeAllowsConstructionLines gates the
    // two entry points above it.  Left as-is so a future mask with the FACE bit
    // set ALONGSIDE others still resolves sanely.)
    kconSelKind_t KindForMode()
    {
        const sel_mask_t mask = KiwiSel_GetModeMask();
        // Tested in the same priority order kiwi_boxselect.cpp's RectKind uses, so
        // a mask with several bits resolves the same way in both files.
        if ( mask & SEL_MASK_OBJECT ) return KCONSEL_OBJECT;
        if ( mask & SEL_MASK_FACE )   return KCONSEL_OBJECT;
        if ( mask & SEL_MASK_EDGE )   return KCONSEL_SEGMENT;
        return KCONSEL_POINT;
    }

    // Liang-Barsky, the same test kiwi_boxselect.cpp's marquee uses on brush
    // windings.  Duplicated rather than exported: it is twenty lines of pure
    // geometry in that file's anonymous namespace, and giving §12's private helper
    // a public header would be a wider change than the copy.
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

// ─── the list ────────────────────────────────────────────────────────────────
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

// ── KIWI-UX (ROUND AR, ITEM 1): SELECT AN EXACT SET OF WHOLE OBJECTS ────────
// The paste's "and now these are selected" step (kiwi_conclip.h).  It REPLACES —
// a paste's selection is the pasted set and nothing else, which is what the brush
// clipboard does too (Map_ImportBuffer opens with Select_Deselect(1), map.cpp
// 0x487C90) and what makes the Move that follows unambiguous.
//
// Whole objects only, at KCONSEL_OBJECT granularity, regardless of the current
// selection mode: a freshly pasted object has no meaningful point or segment the
// user has aimed at, and G's construction arm is whole-object anyway
// (kiwi_transform.cpp's arm, v1 note).
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
    // A move in flight owns the store; it re-stamps its own state on commit or
    // cancel, so dropping the selection under it would leave the gesture pointing
    // at objects it can no longer name.
    if ( s_moveOpen )
        return;
    KiwiConSel_Clear();
}

// ─── picking ─────────────────────────────────────────────────────────────────
bool KiwiConSel_PickAt( int imgX, int imgY, kconSelItem_t *out, float *outPixels )
{
    if ( !out )
        return false;
    if ( !KiwiCon_ShowConstruction() )
        return false;                           // hidden geometry is not clickable
    if ( !ModeAllowsConstructionLines() )       // ROUND AA, ITEM 7 — modes 3 and 4
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
        if ( !o || o->hidden )        // ROUND U — a hidden object is not clickable
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
            // KIWI-UX (CLEANUP, A-12): one spelling, kiwi_pick.h.
            const float d = Pick_SegDist2D( curX, curY, ax, ay, bx, by, 0 );
            // KIWI-UX (ROUND R): the CLICK box is KCON_CLICK_PIXELS (14), NOT the
            // hover/snap radius KCON_LINE_PIXELS (10).  "Lines are still hard to
            // click" is a report about this one test, and only this one — see
            // kiwi_construct.h for why the two radii separate and for the
            // arbitration re-check (a construction line beats a brush FACE at any
            // distance, so a wider box cannot lose to the face it is drawn on).
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
    // ROUND AA, ITEM 7 — modes 3 and 4 do not marquee construction either.  The
    // RETURN IS BEFORE THE CLEAR on purpose: a face- or object-mode marquee is a
    // statement about brushes and has no opinion about the construction selection,
    // so it must not silently drop one the user made in another mode.
    if ( !ModeAllowsConstructionLines() )
        return;
    if ( !shift && !ctrl )
        s_sel.clear();

    const kconSelKind_t want = KindForMode();
    const int count = KiwiCon_Count();

    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        if ( !o || o->hidden )        // ROUND U — a hidden object is not marquee-able
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

// ─── JOIN ────────────────────────────────────────────────────────────────────
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

    // ── SHAKEOUT H: JOIN NO LONGER REQUIRES ONE PLANE ───────────────────────
    // It used to refuse a selection whose objects were not coplanar, because the
    // merged polyline had to be written in ONE plane's basis and there was no
    // honest choice of basis for a non-planar chain (kiwi_construct.h ruling 3 as
    // it stood).  With a world-space store that constraint is gone: a 3D chain is
    // a perfectly legal polyline, it just may not become a REGION — and whether it
    // does is the region layer's question to answer, from the merged points, on the
    // next frame.  Refusing here would have been this file deciding it on stale
    // information.
    //
    // The old test survives where it belongs, in kiwi_region.cpp's PASS 2, which
    // fits a plane through the chain and rejects it if the points do not lie on
    // one.  So "anything that joins can become a region" becomes the honest "a join
    // that IS planar becomes a region", which is what the user can see on screen.
    std::vector<kchainStep_t> steps;
    bool closed = false;
    if ( !KiwiRegion_ChainWalk( &objs[0], (int)objs.size(), &steps, &closed )
      || steps.size() < 2
      // EVERY selected object must be IN the chain.  The walker legitimately DROPS
      // a member whose own two ends coincide (it closes on itself and is not a
      // chain link — kiwi_region.h), and a walker that returned a chain of the
      // others would leave this function deleting an object it never merged.
      || steps.size() != objs.size() )
    {
        Sys_Printf( "Join: the selected lines do not form a single chain "
                    "(ends must meet, no end may be shared by three lines, and "
                    "every selected line must be part of the chain).\n" );
        return false;
    }

    // Concatenate the walk into one WORLD polyline (shakeout H: no projection, no
    // basis, nothing to lose).  RAW first — the weld cannot be chosen until the
    // whole point sequence exists (see the note under the loop).
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

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AR, ITEM 2) — JOIN MUST WELD AT THE TOLERANCE THAT
    //                               AUTHORISED THE JOIN
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "why does this bool diff fail? … The only different
    // is I joined the polyline on the 2nd one.  When i bool it into the pyramid it
    // fails!"
    //
    // THE DEFECT.  The chain that authorised this join was found by
    // KiwiRegion_ChainWalk, which welds two ends into ONE NODE at
    // `KiwiRegion_WeldDist()` — grid-scaled since round AF, `clamp( grid * 0.25,
    // 0.5, 16 )` (kiwi_region.cpp, NodeFor / the two call sites either side of it).
    // The concatenation below, and the closing-vertex drop under it, both used the
    // bare KREG_JOIN_DIST floor of 0.5 instead.  At any grid of 4 or coarser —
    // i.e. every grid anyone maps at — the walker therefore declares two ends
    // IDENTICAL and this function then stores BOTH of them:
    //
    //   * a STUB EDGE at every junction, up to 16 world units long, and
    //   * a DUPLICATE CLOSING VERTEX, because the ring's last point is within the
    //     weld of its first but further than 0.5 from it.
    //
    // Neither is a shape the user drew, and both are exactly the input §23's prism
    // builder turns into two nearly-identical side planes (§19 V5 "duplicate
    // plane") and the boolean then turns into a near-tangent cut plane.  The
    // UNJOINED outline never acquires them: nothing bakes the walker's weld into
    // the store, so the loop the region layer stitches is cleaned by AcceptLoop and
    // that is the end of it.  **That is a join changing geometry, which it must
    // never do.**
    //
    // THE RULE IS NOW THE RING'S OWN, verbatim (kiwi_region.h A WELD MAY NEVER
    // EXCEED THE GEOMETRY IT IS WELDING): the weld is the grid-scaled distance
    // BOUNDED by the finest real edge in the chain, via KiwiRegion_WeldFor, which
    // is bit-for-bit what AcceptLoop's DedupLoop uses on the same points.  So the
    // stored polyline is the ring the region layer would have derived anyway.
    //
    // WHY THE STORE'S OWN NormalizePoints IS *NOT* CHANGED WITH IT.  It runs on
    // every KiwiCon_Add, including a polyline the user drew point by point, and its
    // hard 0.5 is right there: it must never eat a point the user placed.  The
    // difference is that here the walker has ALREADY RULED that these two points
    // are one, on the record, at that tolerance.
    // KIWI-UX (CLEANUP, A-11): the scan was written out here, in
    // kiwi_region.cpp's FinestLoopEdge and in kiwi_arrange.cpp — and this copy
    // already cited kiwi_region.h as the authority for it.  Same walk, same
    // KREG_JOIN_DIST floor, same open-or-closed wrap; only the stride differed.
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

    // A CLOSED chain ends where it began; the store's convention is that a closed
    // polyline does NOT repeat its first point (the wrap is implicit), so drop it.
    // ROUND AR, ITEM 2: at the SAME weld, for the same reason — this is the
    // duplicate closing vertex, and it is the one the extruder trips over.
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

    // KIWI-UX (ROUND R): report the count the STORE ended up with, not the count
    // this function assembled.  KiwiCon_Add normalises the point list (the seam
    // rule, kiwi_construct.h), so the two can legitimately differ — and the console
    // line that said "5 points" for a shape the store held as 4 is precisely what
    // sent this round's investigation to the wrong place.
    const kconObject_t *stored = ( added >= 0 ) ? KiwiCon_At( added ) : 0;
    Sys_Printf( "Join: %i lines -> one %s polyline (%i points).\n",
                (int)objs.size(), closed ? "CLOSED" : "open",
                stored ? (int)( stored->pts.size() / 3 ) : (int)( pts.size() / 3 ) );
    g_nUpdateBits |= 1;
    return true;
}

// ─── DELETE ──────────────────────────────────────────────────────────────────
namespace
{
    // ── ROUND U: how many BRUSH-side items a construction delete will skip ───
    // Only the FINE kinds are counted, because they are exactly the ones the
    // ported Delete Selection cannot act on: since shakeout D neither a face nor
    // an edge nor a vertex puts its owner on `selected_brushes`
    // (kiwi_selection.cpp pass 1), so Cmd_OnSelectionDelete — which walks that
    // sentinel — is a NO-OP for them.  A whole-object selection is a different
    // matter and is deliberately NOT counted here: see OwnsDelete.
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

// ── KIWI-UX (ROUND U): THE MIXED-SELECTION DELETE ───────────────────────────
// USER DIRECTIVE, verbatim: "I also want you to fix the box select so I can press
// 2, box, and delete all lines (without trying to delete edges from a brush cuz
// they are un-deletable this way)".
//
// WHAT WENT WRONG, exactly.  A mode-2 marquee runs BOTH collectors (kiwi_boxselect
// KiwiBox_End: Collect for brushes, then KiwiConSel_ApplyRect), so a box drawn over
// scaffolding that overlaps a wall yields construction items AND brush SEL_EDGE
// items.  Shakeout F's gate was `construction selected AND NOTHING brush-side`,
// with "nothing brush-side" reading BOTH the legacy sentinel and KiwiSel().  The
// edge items made the second half false, the funnel arm declined, and the key fell
// through to the ported 33003 — which walks `selected_brushes`, finds it empty
// (fine kinds do not promote since shakeout D) and does NOTHING.  Net effect for
// the user: Delete silently did nothing at all.
//
// THE WIDENED RULE, and why it is still safe:
//   * construction items selected AND `selected_brushes` EMPTY   -> ours.
//     Any face / edge / vertex items in the selection are SKIPPED and counted, and
//     skipping them costs nothing because the classic delete could not have acted
//     on them either.
//   * `selected_brushes` NON-EMPTY (whole objects / entities selected) -> NOT ours.
//     There the classic delete has a real, unambiguous job and must keep it, so
//     the funnel declines and the key falls through exactly as it always did.
// The legacy sentinel is now the WHOLE test, which is also why it is the only one
// this function reads.
bool KiwiConSel_OwnsDelete()
{
    if ( s_sel.empty() )
        return false;
    return selected_brushes.next == &selected_brushes;
}

// KIWI-UX (CLEANUP, A-36): deliberately NOT routed through OwnsDelete — see the
// block above it.  CanDelete is "is there anything of ours to delete" (the
// palette greys on it); OwnsDelete additionally asks whether the classic delete
// has a job, and only the funnel may ask that.  It shares its body with
// CanHide/OwnsHide by coincidence, not by rule, so it keeps its own.
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

// ─── ROUND U: HIDE (kiwi_conselect.h) ────────────────────────────────────────
// USER DIRECTIVE, verbatim: "When box selecting with 2, the idea is to grab all
// the temporary lines you used and delete/hide them.  We dont have a hide mechanic
// right now, you should add that(H) and add (unhide all) in the search menu".
//
// THE H DISPATCH, stated once and implemented in KiwiUX_KeyFunnel:
//   construction selection only   -> this, and the key is CONSUMED
//   brush selection only          -> untouched: the ported Hide Selected (32923),
//                                    which round J left on bare H
//   BOTH                          -> this runs AND the funnel returns false, so
//                                    32923 runs too.  One key, both halves, which
//                                    is what a user who selected both means.
// Shift+H (isolate), Alt+H (show hidden) and Ctrl+H (invert) are NOT touched —
// only the bare chord is arbitrated, so every other member of the family is
// exactly what round J wired.
// KIWI-UX (CLEANUP, A-36): CanHide, OwnsHide and CanDelete were three
// byte-identical `return !s_sel.empty();` bodies.  The header argues why
// OwnsHide is weaker than OwnsDelete and why CanDelete is weaker again, but it
// never distinguishes CanHide from OwnsHide — they are the same question asked
// through two doors (the palette's canExecute and the H funnel).  Both names
// stay, because the two doors are real; the ANSWER is written once so a future
// change to one cannot silently miss the other.
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

    // ONE store snapshot for the whole act, taken BEFORE the first write, exactly
    // as Delete and Join do — so a single Ctrl+Z brings the scaffolding back
    // (kiwi_conselect.h UNDO; the store's undo is a whole-store snapshot, which
    // carries the hidden flags with it for free).
    KiwiCon_UndoPush();
    for ( size_t i = 0; i < objs.size(); ++i )
        KiwiCon_SetHidden( objs[i], true );
    // The selection goes with them: an object that is inert must not still be the
    // thing G moves or Delete deletes (kiwi_construct.h HIDDEN).
    s_sel.clear();

    Sys_Printf( "Construction: hid %i object%s (Unhide All restores them).\n",
                (int)objs.size(), ( objs.size() == 1 ) ? "" : "s" );
    g_nUpdateBits |= 1;
    return true;
}

// ─── ROUND K: BRUSH EDGES → CONSTRUCTION LINES (Shift+D) ─────────────────────
// The directive, the Plasticity source and every rule below are written out on
// the declarations in kiwi_conselect.h.  This is the assembly.
namespace
{
    // One duplicated edge, in world space.
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

    // KIWI-UX (CLEANUP, A-13): this copy WAS the strictest of the four (kind +
    // liveness + the MAX_POINTS_ON_WINDING bound), so it is the one that became
    // Sel_EdgeEnds in kiwi_selection.h — every guard here survived.  The call
    // site below takes the default checkLive = true, which is exactly what this
    // body did: it walks a cached selection, where a freed node is the case the
    // guard exists for.

    // Every UNIQUE selected edge, coincident duplicates dropped (see the header:
    // a brush edge belongs to two faces, so the same world segment arrives twice
    // whenever both of its faces contributed).
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

    // Grow one RUN out of `edges` starting at `seed`: keep extending from either
    // free end with any unused edge whose own end lands on it, stopping when the
    // run closes on itself or nothing else touches.  Greedy on purpose — the
    // AMBIGUOUS case (three edges meeting at a point, which is every brush corner
    // when three faces' edges are selected at once) has no right answer, and
    // KiwiRegion_ChainWalk rejects it outright for exactly that reason.  Here a
    // wrong guess costs a polyline split into two, not a wrong region: the §8 pass
    // re-chains whatever this leaves as separate objects, and ROUND K's arrangement
    // pass does not care about object boundaries at all.
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
            // KIWI-UX (CLEANUP, A-34): BY VALUE.  These used to be raw pointers into
            // `*pts`, which the loop below push_backs and inserts into — either can
            // reallocate.  It only worked because every mutating branch sets `grew`
            // and ends the scan before the next read; copying makes that an ordinary
            // fact rather than an unwritten invariant a future branch can break.
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
        // One last close test, for a run that finished on its own start.
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

    // ONE push for the whole press (kiwi_undo.h): a duplicate that took three
    // Ctrl+Z presses to take back would be three edits, and it is one act.
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
        // A two-point run IS a line; anything longer is a polyline.  Same typing
        // the drawing tools use, so nothing downstream has to special-case these.
        // KIWI-UX (CLEANUP, A-37): asked in POINTS, not in the float count — the
        // rule is "two points" and "three points", and the bare 6 and 9 this used
        // to spell hid that behind the three-floats-per-point stride.
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

// ─── MOVE ────────────────────────────────────────────────────────────────────
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

    // The reference point: the centroid of every selected object's ANCHORS.  Not
    // the tessellated vertices — a 64-segment circle would otherwise drag the
    // centroid onto itself and swamp a line sharing the selection.
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
        b.pts = o->pts;
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

    // ONE snapshot, taken BEFORE anything moves.  It is both the cancel and the
    // undo record (kiwi_conselect.h UNDO).
    KiwiCon_UndoPush();
    s_moveOpen = true;
    return true;
}

void KiwiConSel_MoveApply( const float delta[3] )
{
    if ( !s_moveOpen || !delta )
        return;
    // SHAKEOUT H: a whole-object translate moves EVERY WORLD POINT, plus the plane
    // origin.  Absolute from the baseline, never incremental (kiwi_transform.h
    // rule 1), so a drag that wanders and comes back lands exactly where it started.
    //
    // THE ORIGIN WRITE IS NOT DEAD, and it is worth saying which case needs it:
    //   * CIRCLE / ARC   `pts` is EMPTY and the plane IS the shape's frame, so this
    //                    line is the entire move.  RefitPlane leaves a parametric
    //                    object's plane alone (kiwi_construct.cpp), so nothing
    //                    overwrites it afterwards.
    //   * LINE / …       `pts` is the shape and the plane is a cache.  The write is
    //                    then usually superseded by NoteMutated's refit — but only
    //                    USUALLY: a non-planar chain's refit FAILS and leaves the
    //                    cache alone, and a translated origin is a better thing to
    //                    leave behind than an untranslated one.
    // ── KIWI-UX (CLEANUP, A-33): THE BASELINE MUST STILL DESCRIBE THE OBJECT ───
    // The point loop below is guarded on the baseline and the live object having
    // the same point count, but the ORIGIN write above it is not — so a mismatch
    // used to translate the cached plane origin while leaving the geometry where it
    // was, with no console line, and then commit that state as an undo record.
    // Asked as a PRE-PASS so nothing has moved yet when the answer is no: cancelling
    // pops the store wholesale (KiwiCon_UndoPop), which would invalidate any
    // kconObject_t* the apply loop was holding.  Abandoning is what
    // kiwi_transform.cpp does when the selection changes under a live drag.
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

void KiwiConSel_MoveCancel()
{
    if ( !s_moveOpen )
        return;
    s_moveBase.clear();
    // The snapshot MoveBegin pushed IS the pre-gesture store, so popping it is an
    // exact restore and simultaneously drops the record — a cancelled gesture must
    // leave nothing on the undo stack.
    //
    // s_moveOpen is cleared AFTER the pop, deliberately: UndoPop notifies
    // KiwiConSel_NoteStoreReplaced, which drops the selection unless a move owns
    // the store.  The pop restores the very snapshot this gesture took, so every
    // index the selection holds is still correct and the user keeps their
    // selection across a cancelled drag — which is what "cancel" should mean.
    KiwiCon_UndoPop();
    s_moveOpen = false;
}

// ── KIWI-UX (ROUND AP, ITEM 2): the self-snap mute (kiwi_conselect.h) ────────
// s_moveBase IS the authoritative "what is this gesture moving" list — it is what
// MoveApply writes through — so asking it is asking the one source of truth rather
// than re-deriving the selection, which could drift from it.
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

// ─── commands ────────────────────────────────────────────────────────────────
void KiwiConSel_RegisterCommands()
{
    // Unbound here — these are the CLASSIC-profile bindings; kiwi_keymap.cpp puts
    // Join on Ctrl+J in the modern profile.  Delete has NO row of its own on
    // purpose: it must not become a second thing competing for VK_DELETE with
    // 33003 (the funnel arbitrates instead — KiwiConSel_OwnsDelete).
    Radiant_RegisterCommand( "KiwiConstructJoin",   0, 0, KIWI_CMD_CONSTRUCT_JOIN );
    Radiant_RegisterCommand( "KiwiConstructDelete", 0, 0, KIWI_CMD_CONSTRUCT_DELETE );
    // ROUND U: both UNBOUND, for the same reason Delete has no row of its own —
    // H is SHARED with the ported Hide Selected (32923) and is arbitrated in the
    // key funnel (KiwiConSel_OwnsHide), so a second row competing for the chord
    // would make which one runs depend on table order.  Unhide All has no chord in
    // Plasticity either and is a palette/search verb by the directive's own words
    // ("add (unhide all) in the search menu").
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
        if ( n > 0 ) Sys_Printf( "Construction: unhid %i object%s.\n",
                                 n, ( n == 1 ) ? "" : "s" );
        else         Sys_Printf( "Construction: nothing is hidden.\n" );
        g_nUpdateBits |= 1;
        return true;
    }
    default:
        return false;
    }
}
