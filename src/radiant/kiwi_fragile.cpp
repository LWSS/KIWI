#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Fragile-corner list (2026-10-04; user: "flash the face that needs to be deleted", then
// "attempt to solve it for us, with an undo record of course, and make it per-issue, and an
// all button").  The entries come from KIWI-cod4map's .errlog (brush.cpp
// ReportKeptBoundingSide), which names the face by its plane normal and the action in fixed
// wording (ParseAction).  The blink is the engine's out-of-bounds cadence and colours: r_clear
// "blink" swaps r_clearColor (0.5 0.75 1) and r_clearColor2 (1 0.5 0) on
// `Sys_Milliseconds() & 0x200` (gfx_d3d/r_utils.cpp R_ClearScreen), every 512 ms.
//
// Identity is geometric.  At load each entry's brush is found and its planes at the corner are
// captured; the issue stays open while some brush still carries those planes there.  Undo, redo
// and map reloads replace brush nodes (undo.cpp Undo_Undo frees and re-adds them), so cached
// selbrush_t pointers are only hints, dereferenced after Sel_BrushesLive and re-found by a
// throttled walk when they stop matching.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include "xywnd.h"                  // ED_VIEW_*, Ed_ActiveXY
#include <gfx_d3d/r_rendercmds.h>   // GfxPointVertex, GfxColor, R_AddCmd_Line3D / Line3DNoDepth
#include <imgui/imgui.h>

#include "kiwi_command.h"           // KiwiCmd_Undo*
#include "kiwi_fragile.h"
#include "kiwi_launch.h"            // KiwiLaunch_SamePath
#include "kiwi_ported.h"
#include "kiwi_selection.h"         // Sel_*, KiwiSel*
#include "kiwi_validity.h"          // KiwiValid_*
#include "kiwi_vec.h"
#include "kiwi_viewdirty.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <string>
#include <vector>

// Ported entry points.
extern void  Radiant_VecToAngles( float *ang, const float *vec );                   // engine_stubs.cpp 0x4a5020
extern face_t *Face_Alloc( brush_t *b, face_t *f );                                 // brush.cpp 0x471500
extern bool  Map_WriteSnapshot( const char *path );                                 // map.cpp (KIWI)

namespace
{
    enum fragileAction_t
    {
        FRAG_DELETE,        // a shallow crease inside the brush: delete the narrow face
        FRAG_ALIGN,         // a face almost on another brush's face: line the two up
        FRAG_NUDGE          // no single face to blame: no automatic fix
    };

    enum rescan_t
    {
        RESCAN_NO,          // draw paths: pointers and planes only
        RESCAN_THROTTLED,   // the pump tick: walk the brushes at most every KFRAG_RESCAN_MS
        RESCAN_NOW          // before acting (Show / Fix)
    };

    // A Radiant face plane captured at load (Radiant's own numbers, so later matches are exact).
    struct facePlane_t
    {
        float normal[3] = {};
        float dist = 0.0f;
        bool  set = false;
    };

    struct fragileMark_t
    {
        int             ent = 0, brush = 0;     // .errlog entity / brush numbers (Select_ByEntityNumber order)
        float           spot[3] = {};           // the flat corner (errlog origin is 16 units out along -dir)
        float           normal[3] = {};         // cod4map's normal of the face to act on (minus errlog dir)
        fragileAction_t action = FRAG_NUDGE;
        int             neighbourNum = -1;      // FRAG_ALIGN: the other brush's number, -1 when unknown
        float           degrees = 0.0f;
        std::string     faceName, partnerName;  // material names from the message
        std::string     message;

        facePlane_t     target;                 // the face to act on
        facePlane_t     partner;                // FRAG_DELETE: the face it creases against
        facePlane_t     other;                  // FRAG_ALIGN: the neighbour's face
        bool            moveNeighbour = false;  // FRAG_ALIGN: the neighbour is the smaller brush - it turns

        // RefreshLive's results; the pointers are hints, dereferenced only after Sel_BrushesLive
        selbrush_t     *inst = nullptr;
        selbrush_t     *neighbour = nullptr;
        int             targetFace = -1;
        int             otherFace = -1;
        bool            found = false;          // a brush carries `target` (and `partner`) at the spot
        bool            otherFound = false;     // a brush carries `other` at the spot
        bool            fixedByUs = false;      // the last Fix of this entry succeeded
    };

    std::vector<fragileMark_t> s_marks;
    std::string s_mapName;          // currentmap when the marks were loaded
    int  s_lastPhase = -1;
    DWORD s_lastRescan = 0;
    uint64_t s_rescanLists = 0;     // Sel_BrushesLive's list hash at the last walk
    int  s_current = -1;            // the entry last picked with Show
    bool s_liveEnabled = true;      // the live check (bottom of this namespace)

    const float KFRAG_XY_DEPTH = 131072.0f;     // 2D overlay depth, same as kiwi_entarrow.cpp KENTARROW_XY_DEPTH
    const float KFRAG_BLUE[3]   = { 0.5f, 0.75f, 1.0f };    // r_clearColor
    const float KFRAG_ORANGE[3] = { 1.0f, 0.5f,  0.0f };    // r_clearColor2
    const float KFRAG_DIM[3]    = { 0.55f, 0.55f, 0.55f };  // the rest of the brush, for context
    const float KFRAG_SAME_FACE_DOT = 0.9999f;  // cod4map's normal vs Radiant's: within ~0.8 deg
    const float KFRAG_PARALLEL_DOT  = 0.98f;    // cos 11.5 deg: cod4map's FRAGILE_CREASE_DEGREES (10) plus slack
    const float KFRAG_ON_PLANE      = 2.0f;     // the corner's faces pass this close to the spot
    const float KFRAG_PLANE_NORMAL_EPS = 1e-4f; // Radiant plane vs captured plane, per component
    const float KFRAG_PLANE_DIST_EPS   = 0.05f;
    const float KFRAG_MAX_MOVE      = 16.0f;    // Fix refuses to swing brush corners further
    const DWORD KFRAG_RESCAN_MS     = 250;
    const float KFRAG_CORNER_RADIUS = 8.0f;     // brush corners this close to the spot get markers
    const float KFRAG_SHOW_DISTANCE = 24.0f;    // Show: closest camera distance / 2D span

    // Brush numbers only mean something in the map the compile came from.
    bool MarksForThisMap()
    {
        return !s_marks.empty() && _stricmp( currentmap, s_mapName.c_str() ) == 0;
    }

    int BlinkPhase() { return ( ::GetTickCount() & 0x200 ) ? 1 : 0; }

    unsigned int PackColor( const float *rgb )
    {
        float rgba[4] = { rgb[0], rgb[1], rgb[2], 1.0f };
        GfxColor c;
        Byte4PackPixelColor( rgba, &c );
        return c.packed;
    }

    // The Select_ByEntityNumber walk (win_dlg.cpp:45), returning NULL instead of asserting.
    selbrush_t *ResolveBrush( int entIdx, int brushIdx )
    {
        if ( entIdx < 0 || brushIdx < 0 )
            return nullptr;
        entity_s *ent = entityInsts.next;
        if ( ent == &entityInsts )
            return nullptr;
        for ( ; entIdx > 0; --entIdx )
        {
            ent = ent->next;
            if ( ent == &entityInsts )
                return nullptr;
        }
        entity_s_def *def = (entity_s_def *)ent->def;
        if ( !def )
            return nullptr;
        brush_t *bdef = (brush_t *)def->brushes.prev;
        void    *defSentinel = (void *)&def->def;
        if ( (void *)bdef == defSentinel )
            return nullptr;
        for ( ; brushIdx > 0; --brushIdx )
        {
            bdef = bdef->onext;
            if ( (void *)bdef == defSentinel )
                return nullptr;
        }
        for ( selbrush_t *inst = ent->brushes.ownerNext; inst && inst != &( ent->brushes ); inst = inst->ownerNext )
            if ( inst->def == bdef )
                return inst;
        return nullptr;
    }

    bool UsableBrush( const selbrush_t *inst )
    {
        return inst && inst->def && !inst->patch && inst->def->faces;
    }

    bool Covers( const brush_t *def, const float *spot )
    {
        for ( int a = 0; a < 3; ++a )
            if ( spot[a] < def->mins[a] - KFRAG_ON_PLANE || spot[a] > def->maxs[a] + KFRAG_ON_PLANE )
                return false;
        return true;
    }

    float PlaneOffset( const plane_t &pl, const float *p ) { return Dot3( pl.normal, p ) - pl.dist; }

    float Extent( const brush_t *def )
    {
        const float d[3] = { def->maxs[0] - def->mins[0], def->maxs[1] - def->mins[1], def->maxs[2] - def->mins[2] };
        return sqrtf( Dot3( d, d ) );
    }

    void Capture( facePlane_t &out, const plane_t &pl )
    {
        for ( int a = 0; a < 3; ++a )
            out.normal[a] = pl.normal[a];
        out.dist = pl.dist;
        out.set = true;
    }

    // The face on exactly the captured plane, or -1.
    int FindPlaneFace( const brush_t *def, const facePlane_t &q )
    {
        int   best = -1;
        float bestErr = KFRAG_PLANE_NORMAL_EPS * 3.0f;
        for ( int f = 0; q.set && f < def->faceCount; ++f )
        {
            const plane_t &pl = def->faces[f].plane;
            if ( fabsf( pl.dist - q.dist ) > KFRAG_PLANE_DIST_EPS )
                continue;
            float err = 0.0f;
            bool  match = true;
            for ( int a = 0; a < 3 && match; ++a )
            {
                const float e = fabsf( pl.normal[a] - q.normal[a] );
                match = e <= KFRAG_PLANE_NORMAL_EPS;
                err += e;
            }
            if ( match && err <= bestErr )
            {
                bestErr = err;
                best = f;
            }
        }
        return best;
    }

    // Load time: the face whose normal best matches cod4map's and that passes the spot, or -1.
    int FindFaceNear( const brush_t *def, const float *normal, const float *spot )
    {
        int   best = -1;
        float bestDot = KFRAG_SAME_FACE_DOT;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const plane_t &pl = def->faces[f].plane;
            const float d = Dot3( pl.normal, normal );
            if ( d >= bestDot && fabsf( PlaneOffset( pl, spot ) ) <= KFRAG_ON_PLANE )
            {
                bestDot = d;
                best = f;
            }
        }
        return best;
    }

    // Load time: the other face at the spot closest in angle to face `f` (the crease partner).
    int FindPartner( const brush_t *def, int f, const float *spot )
    {
        int   best = -1;
        float bestDot = KFRAG_PARALLEL_DOT;
        for ( int k = 0; k < def->faceCount; ++k )
        {
            const plane_t &pl = def->faces[k].plane;
            const float d = Dot3( pl.normal, def->faces[f].plane.normal );
            if ( k != f && d >= bestDot && d < 1.0f && fabsf( PlaneOffset( pl, spot ) ) <= KFRAG_ON_PLANE )
            {
                bestDot = d;
                best = k;
            }
        }
        return best;
    }

    // Load time: the face of `def` within acos(minDot) of `normal` (either way round) whose
    // plane passes closest to `spot`, or -1.
    int FindAlignFace( const brush_t *def, const float *normal, const float *spot, float minDot )
    {
        int   best = -1;
        float bestDist = KFRAG_ON_PLANE;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const plane_t &pl = def->faces[f].plane;
            const float dist = fabsf( PlaneOffset( pl, spot ) );
            if ( fabsf( Dot3( pl.normal, normal ) ) >= minDot && dist <= bestDist )
            {
                bestDist = dist;
                best = f;
            }
        }
        return best;
    }

    // Does `def` carry the entry's planes at the spot?  `face` gets the face to act on (-1 for a
    // BSP-cut nudge, which has no face and is found by brush number instead).
    bool HasTarget( const brush_t *def, const fragileMark_t &m, int *face )
    {
        *face = -1;
        if ( !Covers( def, m.spot ) )
            return false;
        if ( !m.target.set )
            return m.action == FRAG_NUDGE;
        *face = FindPlaneFace( def, m.target );
        if ( *face < 0 )
            return false;
        return m.action != FRAG_DELETE || FindPlaneFace( def, m.partner ) >= 0;
    }

    bool HasOther( const brush_t *def, const fragileMark_t &m, int *face )
    {
        *face = m.other.set && Covers( def, m.spot ) ? FindPlaneFace( def, m.other ) : -1;
        return *face >= 0;
    }

    template <typename Fn>
    void ForEachBrush( Fn fn )
    {
        for ( selbrush_t *head : { &active_brushes, &selected_brushes } )
            for ( selbrush_t *i = head->next; i && i != head; i = i->next )
                if ( UsableBrush( i ) )
                    fn( i );
    }

    // Load time: another brush with a face (FindAlignFace) at the spot - an unnamed ALIGN
    // neighbour, or the brush whose face made a nudge's BSP cut.
    selbrush_t *FindNeighbourNear( const fragileMark_t &m, const float *normal, float minDot, int *face )
    {
        selbrush_t *best = nullptr;
        float bestDist = KFRAG_ON_PLANE;
        *face = -1;
        ForEachBrush( [&]( selbrush_t *i ) {
            if ( i == m.inst || !Covers( i->def, m.spot ) )
                return;
            const int f = FindAlignFace( i->def, normal, m.spot, minDot );
            if ( f < 0 )
                return;
            const float dist = fabsf( PlaneOffset( i->def->faces[f].plane, m.spot ) );
            if ( dist <= bestDist )
            {
                bestDist = dist;
                best = i;
                *face = f;
            }
        } );
        return best;
    }

    // Load time: find the entry's brush (by number, else by geometry, since the map may have
    // been edited while it compiled) and capture its planes.
    void Bind( fragileMark_t &m )
    {
        selbrush_t *inst = ResolveBrush( m.ent, m.brush );
        int f = UsableBrush( inst ) && Covers( inst->def, m.spot ) ? FindFaceNear( inst->def, m.normal, m.spot ) : -1;
        if ( f < 0 && m.action != FRAG_NUDGE )
        {
            inst = nullptr;
            ForEachBrush( [&]( selbrush_t *i ) {
                if ( inst || !Covers( i->def, m.spot ) )
                    return;
                const int k = FindFaceNear( i->def, m.normal, m.spot );
                if ( k >= 0 && ( m.action != FRAG_DELETE || FindPartner( i->def, k, m.spot ) >= 0 ) )
                {
                    inst = i;
                    f = k;
                }
            } );
        }
        m.inst = UsableBrush( inst ) ? inst : nullptr;
        if ( m.inst && f < 0 && m.action == FRAG_NUDGE )
        {
            // a BSP-cut nudge goes by number; its normal is the cut's, which another brush's
            // face put there - find that face to show it
            int g = -1;
            if ( selbrush_t *nb = FindNeighbourNear( m, m.normal, KFRAG_SAME_FACE_DOT, &g ) )
            {
                m.neighbour = nb;
                Capture( m.other, nb->def->faces[g].plane );
            }
            return;
        }
        if ( !m.inst || f < 0 )
            return;                         // DELETE/ALIGN stay unbound
        const brush_t *def = m.inst->def;

        if ( m.action == FRAG_DELETE )
        {
            const int p = FindPartner( def, f, m.spot );
            if ( p < 0 )
                return;
            Capture( m.partner, def->faces[p].plane );
        }
        Capture( m.target, def->faces[f].plane );
        if ( m.action != FRAG_ALIGN )
            return;

        selbrush_t *nb = ResolveBrush( m.ent, m.neighbourNum );
        int g = UsableBrush( nb ) && nb != m.inst && Covers( nb->def, m.spot )
              ? FindAlignFace( nb->def, m.target.normal, m.spot, KFRAG_PARALLEL_DOT ) : -1;
        if ( g < 0 )
        {
            nb = FindNeighbourNear( m, m.target.normal, KFRAG_PARALLEL_DOT, &g );
            m.neighbourNum = -1;            // whichever brush this is, its number is unknown
        }
        if ( !nb )
            return;
        m.neighbour = nb;
        Capture( m.other, nb->def->faces[g].plane );
        m.moveNeighbour = Extent( nb->def ) < Extent( def );
    }

    // `"map" ent brush ox oy oz dx dy dz "message"` (cod4map errors.cpp Error).
    bool ParseLine( const char *line, std::string &map, fragileMark_t &m )
    {
        const char *p = strchr( line, '"' );
        const char *q = p ? strchr( p + 1, '"' ) : nullptr;
        if ( !q )
            return false;
        map.assign( p + 1, q );
        float o[3], d[3];
        int used = 0;
        if ( sscanf( q + 1, " %d %d %f %f %f %f %f %f %n", &m.ent, &m.brush, &o[0], &o[1], &o[2],
                     &d[0], &d[1], &d[2], &used ) != 8 )
            return false;
        const char *ms = strchr( q + 1 + used, '"' );
        const char *me = ms ? strrchr( ms + 1, '"' ) : nullptr;
        if ( !me )
            return false;
        m.message.assign( ms + 1, me );
        for ( int k = 0; k < 3; ++k )
        {
            m.spot[k]   = o[k] + 16.0f * d[k];
            m.normal[k] = -d[k];
        }
        return true;
    }

    // The action, keyed on brush.cpp ReportKeptBoundingSide's wording.
    void ParseAction( fragileMark_t &m )
    {
        char  a[128] = "", b[128] = "";
        int   n = -1;
        float deg = 0.0f;
        const char *s = m.message.c_str();

        if ( sscanf( s, "fragile corner: delete face '%127[^']' - it is only %f degrees from face '%127[^']'",
                     a, &deg, b ) == 3 )
        {
            m.action = FRAG_DELETE;
            m.partnerName = b;
        }
        else if ( sscanf( s, "fragile corner: line up face '%127[^']' with brush %d - their faces are only %f",
                          a, &n, &deg ) == 3 )
        {
            m.action = FRAG_ALIGN;
            m.neighbourNum = n;
        }
        else if ( sscanf( s, "fragile corner: face '%127[^']' is only %f degrees off", a, &deg ) == 2 )
            m.action = FRAG_ALIGN;
        else
        {
            m.action = FRAG_NUDGE;
            sscanf( s, "fragile corner: face '%127[^']'", a );
        }
        m.faceName = a;
        m.degrees = deg;
    }

    // Re-establish every entry against the current brushes (see the banner).
    void RefreshLive( rescan_t rescan )
    {
        const int n = (int)s_marks.size();
        std::vector<selbrush_t *> hints( 2 * n );
        std::unique_ptr<bool[]> live( new bool[2 * n] );
        for ( int i = 0; i < n; ++i )
        {
            hints[2 * i]     = s_marks[i].inst;
            hints[2 * i + 1] = s_marks[i].neighbour;
        }
        const uint64_t lists = Sel_BrushesLive( hints.data(), 2 * n, live.get() );

        bool pending = false;
        for ( int i = 0; i < n; ++i )
        {
            fragileMark_t &m = s_marks[i];
            if ( !live[2 * i] )
                m.inst = nullptr;           // freed: not even an exclusion in the walk below
            if ( !live[2 * i + 1] )
                m.neighbour = nullptr;
            m.targetFace = m.otherFace = -1;
            m.found = UsableBrush( m.inst ) && HasTarget( m.inst->def, m, &m.targetFace );
            m.otherFound = UsableBrush( m.neighbour ) && HasOther( m.neighbour->def, m, &m.otherFace );
            pending |= ( !m.found && ( m.target.set || m.action == FRAG_NUDGE ) ) || ( m.other.set && !m.otherFound );
        }
        // Brush nodes only appear or get replaced when the lists change (undo, redo, delete,
        // map load), so a missing brush is looked for again only then.
        if ( !pending || rescan == RESCAN_NO || ( rescan == RESCAN_THROTTLED && lists == s_rescanLists ) )
            return;
        const DWORD now = ::GetTickCount();
        if ( rescan == RESCAN_THROTTLED && now - s_lastRescan < KFRAG_RESCAN_MS )
            return;
        s_lastRescan = now;
        s_rescanLists = lists;

        // by number first: it tells coplanar pieces of one wall apart
        for ( fragileMark_t &m : s_marks )
        {
            if ( !m.found && ( m.target.set || m.action == FRAG_NUDGE ) )
            {
                selbrush_t *inst = ResolveBrush( m.ent, m.brush );
                if ( UsableBrush( inst ) && inst != m.neighbour && Sel_BrushLive( inst )
                  && HasTarget( inst->def, m, &m.targetFace ) )
                {
                    m.inst = inst;
                    m.found = true;
                }
            }
            if ( !m.otherFound && m.other.set && m.neighbourNum >= 0 )
            {
                selbrush_t *nb = ResolveBrush( m.ent, m.neighbourNum );
                if ( UsableBrush( nb ) && nb != m.inst && Sel_BrushLive( nb ) && HasOther( nb->def, m, &m.otherFace ) )
                {
                    m.neighbour = nb;
                    m.otherFound = true;
                }
            }
        }
        ForEachBrush( [&]( selbrush_t *i ) {
            for ( fragileMark_t &m : s_marks )
            {
                int f;
                if ( !m.found && m.target.set && i != m.neighbour && HasTarget( i->def, m, &f ) )
                {
                    m.inst = i;
                    m.targetFace = f;
                    m.found = true;
                }
                if ( !m.otherFound && m.other.set && i != m.inst && HasOther( i->def, m, &f ) )
                {
                    m.neighbour = i;
                    m.otherFace = f;
                    m.otherFound = true;
                }
            }
        } );
    }

    // Open: the brushes still carry the planes cod4map complained about (an ALIGN whose
    // neighbour was never found stays open on its own face, without a Fix).
    bool IsOpen( const fragileMark_t &m )
    {
        return m.found && ( m.action != FRAG_ALIGN || m.otherFound || !m.other.set );
    }

    bool Fixable( const fragileMark_t &m )
    {
        return m.action != FRAG_NUDGE && IsOpen( m ) && m.targetFace >= 0
            && ( m.action != FRAG_ALIGN || m.otherFace >= 0 );
    }

    // Remove Face's steps (kiwi_bevel.cpp KiwiBevel_RemoveFaceRestoreEdge): cover, remove,
    // validate, and put the face back when the brush would not close.
    bool FixDelete( fragileMark_t &m, const char **why )
    {
        brush_t *def = m.inst->def;
        const int f = m.targetFace;
        if ( def->faceCount <= 4 )
        {
            *why = "the brush has only four faces, so removing one would open it";
            return false;
        }

        face_t saved = def->faces[f];
        saved.w = 0;                        // the winding is Brush_RemoveFace's to free
        KiwiCmd_UndoCoverBrush( m.inst );
        Brush_RemoveFace( def, (unsigned int)f );
        KiwiValid_Rebuild( def );

        const char *v = "invalid geometry";
        if ( !KiwiValid_CheckBrush( def, &v ) || !KiwiValid_BrushCloses( def, &v ) )
        {
            Face_Alloc( def, &saved );
            KiwiValid_Rebuild( def );
            *why = v;
            return false;
        }
        return true;
    }

    // Turn the smaller brush about the corner by the few degrees between the two faces, so its
    // face lies on the other's, then give that face the other's exact plane points: cod4map then
    // sees one plane rather than two nearly equal ones.  The turning brush keeps its shape.
    bool FixAlign( fragileMark_t &m, const char **why )
    {
        static char reason[160];
        selbrush_t *mover = m.moveNeighbour ? m.neighbour : m.inst;
        selbrush_t *onto  = m.moveNeighbour ? m.inst : m.neighbour;
        const int   mf    = m.moveNeighbour ? m.otherFace : m.targetFace;
        const int   of    = m.moveNeighbour ? m.targetFace : m.otherFace;
        brush_t       *def   = mover->def;
        const brush_t *other = onto->def;

        const plane_t tp = def->faces[mf].plane;
        const plane_t np = other->faces[of].plane;
        const float sign = Dot3( tp.normal, np.normal ) < 0.0f ? -1.0f : 1.0f;
        const float want[3] = { sign * np.normal[0], sign * np.normal[1], sign * np.normal[2] };
        const float wantDist = sign * np.dist;

        // Rodrigues rotation taking tp.normal onto `want`, about the corner on the face.
        float axis[3], R[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        Cross3( tp.normal, want, axis );
        const float s = sqrtf( Dot3( axis, axis ) );
        const float c = Dot3( tp.normal, want );
        if ( s > 1e-7f )
        {
            const float k[3] = { axis[0] / s, axis[1] / s, axis[2] / s };
            const float K[3][3] = { { 0.0f, -k[2], k[1] }, { k[2], 0.0f, -k[0] }, { -k[1], k[0], 0.0f } };
            for ( int r = 0; r < 3; ++r )
                for ( int col = 0; col < 3; ++col )
                    R[r][col] = ( r == col ? c : 0.0f ) + s * K[r][col] + ( 1.0f - c ) * k[r] * k[col];
        }
        float pivot[3];
        const float off = PlaneOffset( tp, m.spot );
        for ( int a = 0; a < 3; ++a )
            pivot[a] = m.spot[a] - tp.normal[a] * off;
        const float shift = wantDist - Dot3( want, pivot );
        auto apply = [&]( const float *p, float *out ) {
            const float d[3] = { p[0] - pivot[0], p[1] - pivot[1], p[2] - pivot[2] };
            for ( int a = 0; a < 3; ++a )
                out[a] = pivot[a] + R[a][0] * d[0] + R[a][1] * d[1] + R[a][2] * d[2] + want[a] * shift;
        };

        // a big brush turned about one corner swings its far end: leave that to the mapper
        float maxMove = 0.0f;
        for ( int k = 0; k < def->faceCount; ++k )
        {
            const winding_t *w = def->faces[k].w;
            for ( int i = 0; w && i < w->numpoints; ++i )
            {
                float q[3];
                apply( w->p[i], q );
                const float d[3] = { q[0] - w->p[i][0], q[1] - w->p[i][1], q[2] - w->p[i][2] };
                const float move = sqrtf( Dot3( d, d ) );
                if ( move > maxMove )
                    maxMove = move;
            }
        }
        if ( maxMove > KFRAG_MAX_MOVE )
        {
            _snprintf( reason, sizeof( reason ), "turning it %.1f deg would move its far corners up to %.0f"
                       " units - line the faces up by hand", m.degrees, maxMove );
            reason[sizeof( reason ) - 1] = '\0';
            *why = reason;
            return false;
        }

        kiwiBaseBrush_t base;
        if ( !KiwiValid_Snapshot( def, &base ) )
        {
            *why = "the brush could not be snapshotted";
            return false;
        }
        KiwiCmd_UndoCoverBrush( mover );
        for ( int k = 0; k < def->faceCount; ++k )
            for ( int i = 0; i < 3; ++i )
            {
                float q[3];
                apply( def->faces[k].planepts[i], q );
                for ( int a = 0; a < 3; ++a )
                    def->faces[k].planepts[i][a] = q[a];
            }
        // reversed point order flips the plane to face this brush's way (Face_MakePlane)
        for ( int i = 0; i < 3; ++i )
            for ( int a = 0; a < 3; ++a )
                def->faces[mf].planepts[i][a] = other->faces[of].planepts[sign > 0.0f ? i : 2 - i][a];
        KiwiValid_Rebuild( def );

        const char *v = "invalid geometry";
        if ( !KiwiValid_CheckBrush( def, &v ) )
        {
            KiwiValid_Restore( base );
            *why = v;
            return false;
        }
        return true;
    }

    // One undo record: the entry `only`, or every fixable one when it is negative.
    void RunFixes( int only )
    {
        RefreshLive( RESCAN_NOW );
        Select_Deselect( 1 );               // fixes change face indices; drop face picks first
        KiwiCmd_UndoBegin( only < 0 ? "fix fragile corners" : "fix fragile corner" );
        int fixed = 0, failed = 0;
        for ( int i = 0; i < (int)s_marks.size(); ++i )
        {
            fragileMark_t &m = s_marks[i];
            if ( ( only >= 0 && i != only ) || !Fixable( m ) )
                continue;
            // an earlier fix in this loop may have edited this brush (in place: nothing is
            // freed before the cancel below), so check the planes again
            if ( !HasTarget( m.inst->def, m, &m.targetFace )
              || ( m.action == FRAG_ALIGN && !HasOther( m.neighbour->def, m, &m.otherFace ) ) )
            {
                ++failed;
                Sys_Printf( "Fragile corner %d: not fixed - an earlier fix changed its brush.  Build again.\n", i + 1 );
                continue;
            }
            const char *why = "unknown";
            const bool ok = m.action == FRAG_DELETE ? FixDelete( m, &why ) : FixAlign( m, &why );
            if ( ok )
            {
                m.fixedByUs = true;
                ++fixed;
            }
            else
            {
                ++failed;
                Sys_Printf( "Fragile corner %d: not fixed - %s.  The brush is unchanged.\n", i + 1, why );
            }
        }
        if ( fixed )
            KiwiCmd_UndoCommit();
        else
            KiwiCmd_UndoCancel();           // may replace brush nodes: the next refresh re-finds them
        Sel_Clear( KiwiSel() );
        Sel_RebuildFromLegacy();
        g_nUpdateBits = W_ALL;
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
        Sys_Printf( "Fragile corners: %d fixed%s.%s\n", fixed, failed ? ", some could not be (see above)" : "",
                    s_liveEnabled ? "" : "  Build again to check." );
    }

    // One staged line batch; flushes before R_Add3DLine would auto-flush through the
    // depth-tested command.
    struct lineBatch_t
    {
        enum { VERTS = 1024 };
        GfxPointVertex v[VERTS];
        int  n = 0;
        char width;
        bool noDepth;
        unsigned int color;

        lineBatch_t( char w, bool nd, unsigned int c ) : width( w ), noDepth( nd ), color( c ) {}

        void Add( const float *a, const float *b )
        {
            if ( n + 2 > VERTS )
                Flush();
            n = R_Add3DLine( v, (const orientation_t *)world_orient_matrix, a, b, &color, width, n, VERTS );
        }

        void Flush()
        {
            if ( n )
            {
                if ( noDepth )
                    R_AddCmd_Line3DNoDepth( (short)( n / 2 ), width, v );
                else
                    R_AddCmd_Line3D( (short)( n / 2 ), width, v );
            }
            n = 0;
        }
    };

    // 2D views: keep the view-plane axes, park the depth axis in front of everything.
    void Flatten( float *p, int viewType )
    {
        const int h = ( viewType == ED_VIEW_YZ );
        const int v = ( viewType != ED_VIEW_XY ) + 1;
        p[3 - h - v] = KFRAG_XY_DEPTH;
    }

    void AddLine( lineBatch_t &b, const float *a, const float *c, int viewType )
    {
        float p[3] = { a[0], a[1], a[2] };
        float q[3] = { c[0], c[1], c[2] };
        if ( viewType >= 0 )
        {
            Flatten( p, viewType );
            Flatten( q, viewType );
        }
        b.Add( p, q );
    }

    void EmitFaceOutline( lineBatch_t &b, const face_t &face, int viewType )
    {
        const winding_t *w = face.w;
        if ( !w || w->numpoints < 2 )
            return;
        for ( int i = 0, j = w->numpoints - 1; i < w->numpoints; j = i++ )
            AddLine( b, w->p[j], w->p[i], viewType );
    }

    void EmitBrush( lineBatch_t &b, const brush_t *def, int viewType )
    {
        for ( int f = 0; f < def->faceCount; ++f )
            EmitFaceOutline( b, def->faces[f], viewType );
    }

    // Diagonal hatching across the face, so it reads as "this face" rather than an edge.
    void EmitFaceHatch( lineBatch_t &b, const face_t &face, int viewType )
    {
        const winding_t *w = face.w;
        if ( !w || w->numpoints < 3 )
            return;
        const float *n = face.plane.normal;
        const float e[3] = { w->p[1][0] - w->p[0][0], w->p[1][1] - w->p[0][1], w->p[1][2] - w->p[0][2] };
        const float len = sqrtf( Dot3( e, e ) );
        if ( len < 1e-4f )
            return;
        const float u[3] = { e[0] / len, e[1] / len, e[2] / len };
        float v[3];
        Cross3( n, u, v );
        float across[3];                    // 45 degrees to the first edge
        for ( int a = 0; a < 3; ++a )
            across[a] = ( v[a] - u[a] ) * 0.70710678f;

        float tMin = 1e30f, tMax = -1e30f;
        for ( int i = 0; i < w->numpoints; ++i )
        {
            const float t = Dot3( w->p[i], across );
            tMin = t < tMin ? t : tMin;
            tMax = t > tMax ? t : tMax;
        }
        int lines = (int)( ( tMax - tMin ) / 4.0f );
        lines = lines < 4 ? 4 : lines > 48 ? 48 : lines;
        for ( int k = 1; k <= lines; ++k )
        {
            const float t = tMin + ( tMax - tMin ) * k / ( lines + 1 );
            float hit[2][3];
            int hits = 0;
            for ( int i = 0, j = w->numpoints - 1; i < w->numpoints && hits < 2; j = i++ )
            {
                const float tj = Dot3( w->p[j], across ) - t, ti = Dot3( w->p[i], across ) - t;
                if ( ( tj < 0.0f ) == ( ti < 0.0f ) )
                    continue;
                const float f = tj / ( tj - ti );
                for ( int a = 0; a < 3; ++a )
                    hit[hits][a] = w->p[j][a] + ( w->p[i][a] - w->p[j][a] ) * f;
                ++hits;
            }
            if ( hits == 2 )
                AddLine( b, hit[0], hit[1], viewType );
        }
    }

    // A small box: one corner marker (a square in the 2D views).
    void EmitMarker( lineBatch_t &b, const float *s, float h, int viewType )
    {
        float c[8][3];
        for ( int i = 0; i < 8; ++i )
            for ( int a = 0; a < 3; ++a )
                c[i][a] = s[a] + ( ( i >> a ) & 1 ? h : -h );
        static const int E[12][2] = { {0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7} };
        for ( int e = 0; e < 12; ++e )
            AddLine( b, c[E[e][0]], c[E[e][1]], viewType );
    }

    // Marker half-size: about 6 px on screen in either view.
    float MarkerSize( const camera_s *cam, const float *p, float scale )
    {
        if ( !cam )
            return 6.0f / ( scale > 1e-4f ? scale : 1e-4f );
        const float d[3] = { p[0] - cam->origin[0], p[1] - cam->origin[1], p[2] - cam->origin[2] };
        return sqrtf( Dot3( d, d ) ) * 0.01f + 0.05f;
    }

    struct corner_t
    {
        int          face, vert;            // a winding point, for Sel_MakeVertex
        const float *p;
    };

    // The brush's corners within KFRAG_CORNER_RADIUS of the spot - where the planes meet too
    // flat - each once (coincident winding points within 0.1 units, lowest face first:
    // kiwi_boxselect's rule).
    void NearCorners( const brush_t *def, const float *spot, std::vector<corner_t> &out )
    {
        out.clear();
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            for ( int i = 0; w && i < w->numpoints; ++i )
            {
                const float *p = w->p[i];
                const float d[3] = { p[0] - spot[0], p[1] - spot[1], p[2] - spot[2] };
                if ( Dot3( d, d ) > KFRAG_CORNER_RADIUS * KFRAG_CORNER_RADIUS )
                    continue;
                bool dup = false;
                for ( size_t k = 0; k < out.size() && !dup; ++k )
                    dup = fabsf( out[k].p[0] - p[0] ) < 0.1f && fabsf( out[k].p[1] - p[1] ) < 0.1f
                       && fabsf( out[k].p[2] - p[2] ) < 0.1f;
                if ( !dup )
                    out.push_back( { f, i, p } );
            }
        }
    }

    // What an open entry points at: the brush and face the Fix changes (FRAG_ALIGN: the
    // turning brush's; a nudge: its face, or -1 for a BSP cut), and the other brush's face
    // involved (the one to line up with, or the face whose BSP cut meets the corner).
    bool Subject( const fragileMark_t &m, selbrush_t **owner, int *face, const face_t **onto )
    {
        *onto = nullptr;
        if ( !IsOpen( m ) )
            return false;
        *owner = m.inst;
        *face = m.targetFace;
        if ( m.otherFound )
        {
            *onto = &m.neighbour->def->faces[m.otherFace];
            if ( m.action == FRAG_ALIGN && m.moveNeighbour )
            {
                *owner = m.neighbour;
                *face = m.otherFace;
                *onto = &m.inst->def->faces[m.targetFace];
            }
        }
        return true;
    }

    // Every open entry blinks markers on its corner; the one picked with Show also gets its
    // face hatched, the other face outlined and its brush drawn for context.
    void Draw( bool camera, int viewType, float scale )
    {
        const unsigned int color = PackColor( BlinkPhase() ? KFRAG_ORANGE : KFRAG_BLUE );
        const int vt = camera ? -1 : viewType;
        RefreshLive( RESCAN_NO );

        lineBatch_t context( 1, camera, PackColor( KFRAG_DIM ) );
        lineBatch_t outline( 3, camera, color );
        lineBatch_t hatch( 1, camera, color );
        lineBatch_t markers( 2, camera, color );
        const camera_s *cam = camera ? Ed_Camera() : nullptr;
        std::vector<corner_t> corners;
        for ( int i = 0; i < (int)s_marks.size(); ++i )
        {
            const fragileMark_t &m = s_marks[i];
            selbrush_t   *owner = nullptr;
            int           face = -1;
            const face_t *onto = nullptr;
            if ( !Subject( m, &owner, &face, &onto ) )
                continue;                   // fixed, or its brush changed
            NearCorners( owner->def, m.spot, corners );
            if ( corners.empty() )
                EmitMarker( markers, m.spot, MarkerSize( cam, m.spot, scale ), vt );
            for ( const corner_t &c : corners )
                EmitMarker( markers, c.p, MarkerSize( cam, c.p, scale ), vt );
            if ( i != s_current )
                continue;
            EmitBrush( context, owner->def, vt );
            if ( face >= 0 )
            {
                EmitFaceOutline( outline, owner->def->faces[face], vt );
                EmitFaceHatch( hatch, owner->def->faces[face], vt );
            }
            if ( onto )
                EmitFaceOutline( outline, *onto, vt );
        }
        context.Flush();
        hatch.Flush();
        outline.Flush();
        markers.Flush();
    }

    // Zoom both views onto the corner and select what to act on: the face the Fix changes
    // (face mode; Delete removes it by hand too), else the corner points to nudge (point mode),
    // else the brush.  Camera angles as the error list sets them (errorfile.cpp ErrorLog_01:
    // from the look direction, pitch negated).
    void Show( int i )
    {
        RefreshLive( RESCAN_NOW );
        s_current = i;
        fragileMark_t &m = s_marks[i];
        float focus[3] = { m.spot[0], m.spot[1], m.spot[2] };
        float normal[3] = { m.normal[0], m.normal[1], m.normal[2] };
        float radius = 4.0f;

        selbrush_t   *owner = nullptr;
        int           face = -1;
        const face_t *onto = nullptr;
        std::vector<corner_t> corners;
        const bool open = Subject( m, &owner, &face, &onto );
        if ( open )
            NearCorners( owner->def, m.spot, corners );
        if ( !corners.empty() )
        {
            focus[0] = focus[1] = focus[2] = 0.0f;
            for ( const corner_t &c : corners )
                for ( int a = 0; a < 3; ++a )
                    focus[a] += c.p[a] / (float)corners.size();
            for ( const corner_t &c : corners )
            {
                const float d[3] = { c.p[0] - focus[0], c.p[1] - focus[1], c.p[2] - focus[2] };
                const float r = sqrtf( Dot3( d, d ) );
                radius = r > radius ? r : radius;
            }
        }
        if ( face >= 0 )
            for ( int a = 0; a < 3; ++a )
                normal[a] = owner->def->faces[face].plane.normal[a];

        Select_Deselect( 1 );
        if ( open && ( m.action != FRAG_NUDGE || !corners.empty() ) )
        {
            Sel_Clear( KiwiSel() );
            if ( m.action != FRAG_NUDGE )
                Sel_Add( KiwiSel(), Sel_MakeFace( owner, face ) );
            else
                for ( const corner_t &c : corners )
                    Sel_Add( KiwiSel(), Sel_MakeVertex( owner, c.face, c.vert ) );
            Sel_SyncToLegacy();
            KiwiSel_SetModeMask( m.action != FRAG_NUDGE ? SEL_MASK_FACE : SEL_MASK_VERTEX );
        }
        else if ( open )
        {
            Select_Brush( owner, 0, 1, 0 );
            KiwiSel_SetModeMask( SEL_MASK_OBJECT );
        }

        // Close in, a little off the face normal so the faces show some depth.
        float tilt[3] = { 0.0f, 0.0f, 1.0f };
        if ( fabsf( normal[2] ) > 0.9f )
            tilt[0] = 1.0f, tilt[2] = 0.0f;
        const float tn = Dot3( tilt, normal );
        for ( int a = 0; a < 3; ++a )
            tilt[a] -= normal[a] * tn;
        const float tl = sqrtf( Dot3( tilt, tilt ) );
        float out[3];
        for ( int a = 0; a < 3; ++a )
            out[a] = normal[a] + 0.45f * tilt[a] / tl;
        const float ol = sqrtf( Dot3( out, out ) );
        for ( int a = 0; a < 3; ++a )
            out[a] /= ol;
        const float back = radius * 5.0f > KFRAG_SHOW_DISTANCE ? radius * 5.0f : KFRAG_SHOW_DISTANCE;
        const float dir[3] = { -out[0], -out[1], -out[2] };
        camera_s *cam = Ed_Camera();
        for ( int k = 0; k < 3; ++k )
            cam->origin[k] = focus[k] + out[k] * back;
        Radiant_VecToAngles( cam->angles, dir );
        cam->angles[0] = -cam->angles[0];

        xywndState_t *xy = Ed_ActiveXY();
        for ( int k = 0; k < 3; ++k )
            xy->m_vOrigin[k] = focus[k];
        if ( xy->m_nWidth > 0 )
        {
            const float span = radius * 2.0f > KFRAG_SHOW_DISTANCE ? radius * 2.0f : KFRAG_SHOW_DISTANCE;
            const float s = xy->m_nWidth * 0.35f / span;
            xy->m_fScale = s < 0.01f ? 0.01f : s > 32.0f ? 32.0f : s;   // XYWnd_OnMouseWheel's clamp
        }
        g_nUpdateBits = W_ALL;
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
        Sys_Printf( "Fragile corner %d/%d (entity %d brush %d): %s\n", i + 1, (int)s_marks.size(),
                    m.ent, m.brush, m.message.c_str() );
    }

    std::string Describe( const fragileMark_t &m )
    {
        char text[320];
        if ( m.action == FRAG_DELETE )
            _snprintf( text, sizeof( text ), "Delete face '%s' of brush %d (%.1f deg crease with '%s')",
                       m.faceName.c_str(), m.brush, m.degrees, m.partnerName.c_str() );
        else if ( m.action == FRAG_ALIGN )
        {
            char a[32] = "brush ?", b[32] = "its neighbour";
            _snprintf( a, sizeof( a ), "brush %d", m.brush );
            if ( m.neighbourNum >= 0 )
                _snprintf( b, sizeof( b ), "brush %d", m.neighbourNum );
            _snprintf( text, sizeof( text ), "Turn %s %.1f deg to line up with %s",
                       m.moveNeighbour ? b : a, m.degrees, m.moveNeighbour ? a : b );
        }
        else if ( !m.faceName.empty() )
            _snprintf( text, sizeof( text ), "Brush %d: face '%s' meets others at too flat a corner - nudge it",
                       m.brush, m.faceName.c_str() );
        else
            _snprintf( text, sizeof( text ), "Brush %d: another brush's face cuts its corner too flat - nudge it",
                       m.brush );
        text[sizeof( text ) - 1] = '\0';
        return text;
    }

    // ---- live check (user 2026-10-04: "WYSIWYG the editor. If I saw these holes in realtime,
    // I would just go fix them") ----

    HANDLE      s_liveProc = nullptr;
    std::string s_liveSnapshot;         // the map file the running check reads
    std::string s_liveErrlog;           // where it writes its entries
    std::string s_liveTarget;           // <root>\raw\maps\mp\<name>_kiwilive, for its side files
    std::string s_liveMap;              // currentmap the live state belongs to
    std::string s_liveStatus;
    uint64_t    s_geomHash = 0;         // GeometryHash at the last poll
    uint64_t    s_checkedHash = 0;      // the geometry the last check started from
    DWORD       s_geomChangedAt = 0;
    DWORD       s_lastPoll = 0;

    const DWORD KFRAG_LIVE_POLL_MS = 250;   // geometry hash cadence
    const DWORD KFRAG_LIVE_IDLE_MS = 600;   // an edit settles this long before a check starts

    // Order-free sum of every brush's plane hash: selection and undo's node swaps leave it
    // alone, any geometry edit changes it.  0 = no brushes, nothing to check.
    uint64_t GeometryHash()
    {
        uint64_t sum = 0;
        ForEachBrush( [&]( selbrush_t *i ) {
            const brush_t *def = i->def;
            uint64_t h = 14695981039346656037ull ^ (uint64_t)def->faceCount;
            for ( int f = 0; f < def->faceCount; ++f )
            {
                uint32_t bits[4];
                memcpy( bits, def->faces[f].plane.normal, sizeof( float ) * 3 );
                memcpy( bits + 3, &def->faces[f].plane.dist, sizeof( float ) );
                for ( int k = 0; k < 4; ++k )
                    h = ( h ^ bits[k] ) * 1099511628211ull;
            }
            sum += h;
        } );
        return sum;
    }

    // The map load writes lightgrid volume lists (and on some maps a .d3dpoly) beside the
    // target before the check; the live target is ours alone, so they go.
    void LiveRemoveSideFiles()
    {
        for ( const char *ext : { ".grid_auto", ".grid_not", ".d3dpoly" } )
            ::DeleteFileA( ( s_liveTarget + ext ).c_str() );
    }

    void LiveStop()
    {
        if ( !s_liveProc )
            return;
        ::TerminateProcess( s_liveProc, 1 );
        ::WaitForSingleObject( s_liveProc, 500 );   // let it release its files
        ::CloseHandle( s_liveProc );
        s_liveProc = nullptr;
        LiveRemoveSideFiles();
    }

    // Snapshot the map beside the editor and start KIWI-cod4map on it: the build's command
    // (cwd = the root, output under raw\maps\mp so the game filesystem finds prefabs), with
    // -fragileCheck and a target name of its own.
    void LiveStart()
    {
        kiwiLaunchTool_t tool;
        if ( !KiwiLaunch_Cod4mapTool( &tool ) )
        {
            s_liveStatus = "needs fs_basepath and KIWI-cod4map.exe";
            return;
        }
        char dir[MAX_PATH], snapshot[MAX_PATH], errlog[MAX_PATH], cmd[2048];
        _snprintf( dir, sizeof( dir ), "%s\\kiwi_cache", tool.binDir );
        dir[sizeof( dir ) - 1] = '\0';
        ::CreateDirectoryA( dir, nullptr );
        strncat( dir, "\\fragile", sizeof( dir ) - strlen( dir ) - 1 );
        ::CreateDirectoryA( dir, nullptr );
        _snprintf( snapshot, sizeof( snapshot ), "%s\\%s.map", dir, tool.name );
        _snprintf( errlog, sizeof( errlog ), "%s\\%s.errlog", dir, tool.name );
        snapshot[sizeof( snapshot ) - 1] = errlog[sizeof( errlog ) - 1] = '\0';
        ::DeleteFileA( errlog );            // Error() appends
        if ( !Map_WriteSnapshot( snapshot ) )
        {
            s_liveStatus = "could not write the snapshot";
            return;
        }
        _snprintf( cmd, sizeof( cmd ), "\"%s\" -platform pc -fragileCheck -loadFrom \"%s\" \"raw\\maps\\mp\\%s_kiwilive.map\"",
                   tool.exe, snapshot, tool.name );
        cmd[sizeof( cmd ) - 1] = '\0';

        STARTUPINFOA si;
        memset( &si, 0, sizeof( si ) );
        si.cb = sizeof( si );
        PROCESS_INFORMATION pi;
        memset( &pi, 0, sizeof( pi ) );
        if ( !::CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, tool.root, &si, &pi ) )
        {
            s_liveStatus = "could not start KIWI-cod4map";
            return;
        }
        ::CloseHandle( pi.hThread );
        s_liveProc = pi.hProcess;
        s_liveSnapshot = snapshot;
        s_liveErrlog = errlog;
        s_liveTarget = std::string( tool.root ) + "\\raw\\maps\\mp\\" + tool.name + "_kiwilive";
        s_liveStatus = "checking...";
    }

    void LiveFinish()
    {
        DWORD code = 1;
        ::GetExitCodeProcess( s_liveProc, &code );
        ::CloseHandle( s_liveProc );
        s_liveProc = nullptr;
        LiveRemoveSideFiles();
        char text[96];
        if ( code == 0 || code == KIWI_FRAGILE_STOP_EXIT_CODE )
        {
            int skipped = 0;
            const int n = KiwiFragile_Load( s_liveErrlog.c_str(), s_liveSnapshot.c_str(), &skipped );
            if ( n )
                _snprintf( text, sizeof( text ), "%d fragile corner(s)", n );
            else
                _snprintf( text, sizeof( text ), "clean" );
        }
        else if ( code == KIWI_FRAGILE_CHECK_LEAKED_EXIT_CODE )
        {
            KiwiFragile_Clear();
            _snprintf( text, sizeof( text ), "the map leaks - checked once it is sealed" );
        }
        else
            _snprintf( text, sizeof( text ), "check failed (KIWI-cod4map exit %lu) - build for details", code );
        text[sizeof( text ) - 1] = '\0';
        s_liveStatus = text;
    }

    // Once per tick: collect a finished check, and start the next one once the geometry has
    // changed since the last and then held still for KFRAG_LIVE_IDLE_MS.
    void LivePoll()
    {
        if ( s_liveProc && ::WaitForSingleObject( s_liveProc, 0 ) == WAIT_OBJECT_0 )
            LiveFinish();
        if ( _stricmp( currentmap, s_liveMap.c_str() ) != 0 )
        {
            LiveStop();                     // another map: it gets its own first check
            s_liveMap = currentmap;
            s_geomHash = s_checkedHash = 0;
            s_liveStatus.clear();
        }
        const DWORD now = ::GetTickCount();
        if ( !s_liveEnabled || now - s_lastPoll < KFRAG_LIVE_POLL_MS )
            return;
        s_lastPoll = now;
        const uint64_t h = GeometryHash();
        if ( h != s_geomHash )
        {
            s_geomHash = h;
            s_geomChangedAt = now;
        }
        if ( !s_liveProc && s_geomHash != s_checkedHash && now - s_geomChangedAt >= KFRAG_LIVE_IDLE_MS
          && !KiwiLaunch_Busy() )
        {
            s_checkedHash = s_geomHash;
            LiveStart();
        }
    }
}

void KiwiFragile_LiveCancel()
{
    LiveStop();
    s_liveStatus.clear();
    s_checkedHash = s_geomHash;             // the build's BSP checks this geometry; edits re-arm
}

int KiwiFragile_Load( const char *errlogPath, const char *mapPath, int *skipped )
{
    // keep the entry picked with Show across a reload (the live check reloads after edits)
    const bool hadCurrent = s_current >= 0 && s_current < (int)s_marks.size();
    fragileMark_t previous;
    if ( hadCurrent )
        previous = s_marks[s_current];
    KiwiFragile_Clear();
    *skipped = 0;
    FILE *f = errlogPath ? fopen( errlogPath, "rt" ) : nullptr;
    if ( !f )
        return 0;
    char line[4096];
    while ( fgets( line, sizeof( line ), f ) )
    {
        if ( !strstr( line, "\"fragile corner" ) )
            continue;
        std::string map;
        fragileMark_t m;
        if ( !ParseLine( line, map, m ) )
            continue;
        if ( !KiwiLaunch_SamePath( map.c_str(), mapPath ) )
        {
            ++*skipped;
            continue;
        }
        ParseAction( m );
        Bind( m );
        s_marks.push_back( m );
    }
    fclose( f );
    s_mapName = currentmap;
    s_lastPhase = -1;
    s_rescanLists = 0;
    for ( int i = 0; hadCurrent && s_current < 0 && i < (int)s_marks.size(); ++i )
    {
        const float d[3] = { s_marks[i].spot[0] - previous.spot[0], s_marks[i].spot[1] - previous.spot[1],
                             s_marks[i].spot[2] - previous.spot[2] };
        if ( s_marks[i].action == previous.action && Dot3( d, d ) < 1.0f )
            s_current = i;
    }
    KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
    g_nUpdateBits |= W_CAMERA;
    return (int)s_marks.size();
}

void KiwiFragile_Clear()
{
    s_current = -1;
    if ( s_marks.empty() )
        return;
    s_marks.clear();
    KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
    g_nUpdateBits |= W_CAMERA;
}

int KiwiFragile_Count()
{
    return (int)s_marks.size();
}

void KiwiFragile_Tick()
{
    LivePoll();
    if ( s_marks.empty() )
        return;
    if ( !MarksForThisMap() )
    {
        KiwiFragile_Clear();                // another map is open
        return;
    }
    RefreshLive( RESCAN_THROTTLED );        // re-find brushes an undo or reload replaced
    const int phase = BlinkPhase();
    if ( phase != s_lastPhase )
    {
        s_lastPhase = phase;
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );    // the camera renders every tick anyway
    }
}

void KiwiFragile_DrawCamera()
{
    if ( MarksForThisMap() )
        Draw( true, -1, 1.0f );
}

void KiwiFragile_DrawXY( int viewType, float scale )
{
    if ( viewType != ED_VIEW_XY && viewType != ED_VIEW_XZ && viewType != ED_VIEW_YZ )
        return;
    if ( MarksForThisMap() )
        Draw( false, viewType, scale );
}

bool KiwiFragile_DrawBuildRow( bool offerBuildAnyway )
{
    ImGui::Separator();
    if ( ImGui::Checkbox( "Live fragile-corner check", &s_liveEnabled ) )
    {
        if ( s_liveEnabled )
            s_checkedHash = 0;              // check the map as it is now
        else
        {
            LiveStop();
            s_liveStatus.clear();
        }
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "After each edit, KIWI-cod4map checks a snapshot of the map in the background\n"
                           "(the build's own BSP and test) and the corners it finds blink in the views." );
    if ( s_liveEnabled && !s_liveStatus.empty() )
    {
        ImGui::SameLine();
        ImGui::TextDisabled( "%s", s_liveStatus.c_str() );
    }

    if ( !MarksForThisMap() )
    {
        // the errlog was not loaded (open map != compiled map): keep the way forward
        if ( !offerBuildAnyway )
            return false;
        ImGui::TextColored( ImVec4( KFRAG_ORANGE[0], KFRAG_ORANGE[1], KFRAG_ORANGE[2], 1.0f ),
                            "Build stopped at fragile corners (see the log)" );
        ImGui::SameLine();
        return ImGui::SmallButton( "Build anyway##fragile" );
    }
    RefreshLive( RESCAN_NO );

    int open = 0, fixable = 0;
    for ( const fragileMark_t &m : s_marks )
    {
        open    += IsOpen( m );
        fixable += Fixable( m );
    }

    bool buildAnyway = false;
    if ( open )
    {
        const float *c = BlinkPhase() ? KFRAG_ORANGE : KFRAG_BLUE;
        ImGui::TextColored( ImVec4( c[0], c[1], c[2], 1.0f ), "Fragile corners: %d%s", open,
                            offerBuildAnyway ? " - build stopped" : "" );
    }
    else
        ImGui::TextColored( ImVec4( 0.45f, 0.85f, 0.45f, 1.0f ), "Fragile corners: all fixed%s",
                            s_liveEnabled ? "" : " - build again to check" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Corners too flat for retail cod4map: it turns them into void strips in\n"
                           "nearby walls. KIWI-cod4map compiles them correctly, so fixing is optional.\n"
                           "Their corners blink in the views; Show zooms in on one and selects what\n"
                           "to change, Fix changes it (undoable)." );
    ImGui::SameLine();
    ImGui::BeginDisabled( fixable == 0 );
    if ( ImGui::SmallButton( "Fix all##fragile" ) )
        RunFixes( -1 );
    ImGui::EndDisabled();
    ImGui::SameLine();
    if ( ImGui::SmallButton( "Clear##fragile" ) )
    {
        KiwiFragile_Clear();
        return false;
    }
    if ( offerBuildAnyway )
    {
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Build anyway##fragile" ) )
            buildAnyway = true;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Build once without stopping at fragile corners." );
    }

    const int rows = (int)s_marks.size() < 6 ? (int)s_marks.size() : 6;
    ImGui::BeginChild( "##fragile_list", ImVec2( 0.0f, rows * ImGui::GetFrameHeightWithSpacing() + 4.0f ),
                       ImGuiChildFlags_None );
    for ( int i = 0; i < (int)s_marks.size(); ++i )
    {
        const fragileMark_t &m = s_marks[i];
        ImGui::PushID( i );
        if ( ImGui::SmallButton( "Show" ) )
            Show( i );
        ImGui::SameLine();
        ImGui::BeginDisabled( !Fixable( m ) );
        if ( ImGui::SmallButton( "Fix" ) )
            RunFixes( i );
        ImGui::EndDisabled();
        const char *noFix = m.action == FRAG_NUDGE
            ? "No automatic fix: these planes meet along nearly the same edge.\n"
              "Show selects the corner points: move them (or the brush) slightly, then build again."
            : m.action == FRAG_ALIGN && m.target.set && !m.other.set
            ? "No automatic fix: no brush to line up with was found at the corner.\n"
              "Turn this brush onto its neighbour by hand, then build again."
            : nullptr;
        if ( noFix && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "%s", noFix );
        ImGui::SameLine();

        const std::string text = Describe( m );
        if ( IsOpen( m ) && i == s_current )
        {
            const float *c = BlinkPhase() ? KFRAG_ORANGE : KFRAG_BLUE;
            ImGui::TextColored( ImVec4( c[0], c[1], c[2], 1.0f ), "%d. %s", i + 1, text.c_str() );
        }
        else if ( IsOpen( m ) )
            ImGui::Text( "%d. %s", i + 1, text.c_str() );
        else if ( !m.target.set && m.action != FRAG_NUDGE )
            ImGui::TextDisabled( "%d. %s - not found in the open map", i + 1, text.c_str() );
        else if ( m.fixedByUs )
            ImGui::TextDisabled( "%d. fixed: %s", i + 1, text.c_str() );
        else
            ImGui::TextDisabled( "%d. changed: %s - build again to check", i + 1, text.c_str() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", m.message.c_str() );
        ImGui::PopID();
    }
    ImGui::EndChild();
    return buildAnyway;
}

// ── Clean Up Precision ───────────────────────────────────────────────────────────
// KIWI (2026-10-05, precision gauntlet; user: "it's easy to make geometry that fails the
// compiler"): repair what the old 0.5 plane-point rounding and per-frame rotation residuals
// left in existing maps (new edits no longer make it).  cod4map squares a normal onto an
// axis only within 1e-7 and merges planes within 1e-7 / 0.001, and a layered (l_sm_*)
// material coalesces only onto an exactly aligned surface (kisak_trash had 5 such faces and a
// chimney cap patch 0.004 out of flat).
//  * a brush face within KFRAG_AXIAL_TILT of an axis becomes exactly axial (its three
//    points take their mean coordinate on that axis);
//  * a patch whose control points all lie within KFRAG_FLAT_DEV of one plane is flattened
//    onto it - onto the exact axial plane when that plane is within 1e-3 of an axis.
// Scope: the selection, else the whole map.  One undo record.

namespace
{
    const double KFRAG_AXIAL_TILT = 1e-4;     // rad (0.006 deg): never meant as a slope
    const double KFRAG_FLAT_DEV   = 0.02;     // units: a patch this flat was meant flat
    const double KFRAG_FLAT_AXIAL = 1e-3;     // a flat patch this near an axis goes axial

    int DominantAxis( const double n[3] )
    {
        int a = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabs( n[k] ) > fabs( n[a] ) )
                a = k;
        return a;
    }

    // Square one face onto its axis; false = nothing to do.  `apply` false = dry run.
    bool SquareFace( face_t *f, bool apply )
    {
        double p[3][3];
        for ( int i = 0; i < 3; ++i )
            for ( int k = 0; k < 3; ++k )
                p[i][k] = f->planepts[i][k];
        const double u[3] = { p[0][0] - p[1][0], p[0][1] - p[1][1], p[0][2] - p[1][2] };
        const double v[3] = { p[2][0] - p[1][0], p[2][1] - p[1][1], p[2][2] - p[1][2] };
        double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const double len = sqrt( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
        if ( !( len > 0.0 ) )
            return false;
        for ( int k = 0; k < 3; ++k )
            n[k] /= len;
        const int a = DominantAxis( n );
        double tilt = 0.0;
        for ( int k = 0; k < 3; ++k )
            if ( k != a )
                tilt += n[k] * n[k];
        tilt = sqrt( tilt );
        if ( !( tilt > 0.0 ) || tilt > KFRAG_AXIAL_TILT )
            return false;
        const float m = (float)( ( p[0][a] + p[1][a] + p[2][a] ) / 3.0 );
        if ( f->planepts[0][a] == m && f->planepts[1][a] == m && f->planepts[2][a] == m )
            return false;
        if ( apply )
            for ( int i = 0; i < 3; ++i )
                f->planepts[i][a] = m;
        return true;
    }

    // Flatten a near-flat patch; false = not near-flat, or already exact.  *dev: how far
    // the worst control point was off the plane.
    bool FlattenPatch( patchMesh_t *pm, bool apply, double *dev )
    {
        *dev = 0.0;
        if ( !pm || pm->width < 2 || pm->height < 2 || pm->width > 16 || pm->height > 16 )
            return false;
        // Newell normal around the grid's boundary ring
        std::vector<const float *> ring;
        for ( int c = 0; c < pm->width; ++c )       ring.push_back( pm->ctrl[c][0].xyz );
        for ( int r = 1; r < pm->height; ++r )      ring.push_back( pm->ctrl[pm->width - 1][r].xyz );
        for ( int c = pm->width - 2; c >= 0; --c )  ring.push_back( pm->ctrl[c][pm->height - 1].xyz );
        for ( int r = pm->height - 2; r >= 1; --r ) ring.push_back( pm->ctrl[0][r].xyz );
        double n[3] = { 0.0, 0.0, 0.0 };
        for ( size_t i = 0; i < ring.size(); ++i )
        {
            const float *a = ring[i];
            const float *b = ring[( i + 1 ) % ring.size()];
            n[0] += ( (double)a[1] - b[1] ) * ( (double)a[2] + b[2] );
            n[1] += ( (double)a[2] - b[2] ) * ( (double)a[0] + b[0] );
            n[2] += ( (double)a[0] - b[0] ) * ( (double)a[1] + b[1] );
        }
        const double len = sqrt( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
        if ( !( len > 0.0 ) )
            return false;                           // a degenerate ring (a wrapped curve)
        for ( int k = 0; k < 3; ++k )
            n[k] /= len;
        double cen[3] = { 0.0, 0.0, 0.0 };
        const int count = pm->width * pm->height;
        for ( int c = 0; c < pm->width; ++c )
            for ( int r = 0; r < pm->height; ++r )
                for ( int k = 0; k < 3; ++k )
                    cen[k] += pm->ctrl[c][r].xyz[k] / (double)count;
        // The axial plane when the patch is that near an axis - exact for cod4map.
        const int a = DominantAxis( n );
        double tilt = 0.0;
        for ( int k = 0; k < 3; ++k )
            if ( k != a )
                tilt += n[k] * n[k];
        const bool axial = sqrt( tilt ) < KFRAG_FLAT_AXIAL;
        if ( axial )
            for ( int k = 0; k < 3; ++k )
                n[k] = ( k == a ) ? ( n[a] > 0.0 ? 1.0 : -1.0 ) : 0.0;
        for ( int c = 0; c < pm->width; ++c )
            for ( int r = 0; r < pm->height; ++r )
            {
                const float *p = pm->ctrl[c][r].xyz;
                const double d = ( p[0] - cen[0] ) * n[0] + ( p[1] - cen[1] ) * n[1] + ( p[2] - cen[2] ) * n[2];
                if ( fabs( d ) > *dev )
                    *dev = fabs( d );
            }
        if ( *dev > KFRAG_FLAT_DEV || *dev < 1e-6 )
            return false;                           // shaped on purpose, or already flat
        if ( !apply )
            return true;
        for ( int c = 0; c < pm->width; ++c )
            for ( int r = 0; r < pm->height; ++r )
            {
                float *p = pm->ctrl[c][r].xyz;
                const double d = ( p[0] - cen[0] ) * n[0] + ( p[1] - cen[1] ) * n[1] + ( p[2] - cen[2] ) * n[2];
                for ( int k = 0; k < 3; ++k )
                    p[k] = (float)( p[k] - d * n[k] );
                if ( axial )
                    p[a] = (float)cen[a];           // exactly on the axial plane
            }
        return true;
    }
}

void KiwiFragile_CleanPrecision()
{
    std::vector<selbrush_t *> nodes;
    const bool selection = selected_brushes.next && selected_brushes.next != &selected_brushes;
    for ( selbrush_t *head : { &selected_brushes, &active_brushes } )
    {
        if ( head == &active_brushes && selection )
            break;
        for ( selbrush_t *i = head->next; i && i != head; i = i->next )
            if ( i->def )
                nodes.push_back( i );
    }
    KiwiCmd_UndoBegin( "clean up precision" );
    int faces = 0, patches = 0;
    double worst = 0.0;
    std::vector<const brush_t *> done;
    for ( selbrush_t *node : nodes )
    {
        brush_t *def = node->def;
        bool seen = false;
        for ( const brush_t *d : done )
            seen = seen || d == def;
        if ( seen )
            continue;
        done.push_back( def );
        const entity_s *owner = def->owner;
        if ( owner && owner->eclass && owner->eclass->fixedsize )
            continue;                               // model / light proxies
        if ( def->patch )
        {
            double dev = 0.0;
            if ( !FlattenPatch( def->patch, false, &dev ) )
                continue;
            KiwiCmd_UndoCoverBrush( node );
            FlattenPatch( def->patch, true, &dev );
            Patch_Rebuild( def->patch, 1 );
            ++patches;
            if ( dev > worst )
                worst = dev;
            continue;
        }
        if ( !def->faces )
            continue;
        int n = 0;
        for ( int f = 0; f < def->faceCount; ++f )
            n += SquareFace( &def->faces[f], false ) ? 1 : 0;
        if ( !n )
            continue;
        KiwiCmd_UndoCoverBrush( node );
        for ( int f = 0; f < def->faceCount; ++f )
            SquareFace( &def->faces[f], true );
        KiwiValid_Rebuild( def );
        faces += n;
    }
    if ( faces || patches )
        KiwiCmd_UndoCommit();
    else
        KiwiCmd_UndoCancel();
    g_nUpdateBits = W_ALL;
    KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
    if ( patches )
        Sys_Printf( "Clean Up Precision (%s): %d face(s) squared onto an axis, %d patch(es) flattened "
                    "(the worst was %.4g units off flat).\n", selection ? "selection" : "whole map",
                    faces, patches, worst );
    else
        Sys_Printf( "Clean Up Precision (%s): %d face(s) squared onto an axis, no patch needed "
                    "flattening.\n", selection ? "selection" : "whole map", faces );
}

void KiwiFragile_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiCleanPrecision", 0, 0, KIWI_CMD_CLEAN_PRECISION );
}

bool KiwiFragile_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != KIWI_CMD_CLEAN_PRECISION )
        return false;
    KiwiFragile_CleanPrecision();
    return true;
}
