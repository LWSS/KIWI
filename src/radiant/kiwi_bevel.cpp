#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Bevel frame/appender, inset clone, and remove-face commands. Every plane is
// expressed as three plane points and rebuilt through KiwiValid_Rebuild.

#include "stdafx.h"
#include "qe3.h"
#include "winding.h"

#include "kiwi_bevel.h"
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_material.h"              // R5/R6 material inheritance
#include "kiwi_numeric.h"
#include "kiwi_patchfillet.h"           // bevel command implementation
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // shared vector helpers

#include <imgui/imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int          Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118
extern int          g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)

extern face_t      *Face_Alloc( brush_t *b, face_t *f );                      // brush.cpp:304  0x471500
extern size_t Brush_RemoveFace( brush_t *b, unsigned int faceIndex );   // brush.cpp:345  0x471640
extern brush_t     *Brush_Clone( brush_t *def );                              // brush.cpp:729  0x475D20
extern void         Brush_Free_R( brush_t *def );                             // brush.cpp:706  0x475AF0
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );         // brush.cpp:669  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                        // brush.cpp:927  0x4765A0
extern void         Entity_LinkBrush( brush_t *b, entity_s *world_ent );      // entity.cpp:445 0x484FC0
extern void         Select_Deselect( int a1 );                                // select.cpp:1444 0x48E800 (int, NOT char — mangling)
extern void         Select_Delete();                                          // select.cpp:1520 0x48E9A0

extern void         Radiant_ExecCommand( unsigned int cmdId );                // mainfrm.cpp:4054

// texturevecs.cpp: texdef -> 2x4 world texture matrix (0x45A1C0) and its inverse (0x459CC0).
extern void Face_MoveTexture( const float *surfDef, const float *normal, float *outVecs,
                              const float *uvBase, float rotate, float crossterm );
extern void texturevecs_02( float *outSize, float *texMatPtr, float st1_phantom,
                            const float *planeNormalPtr, float planeDist,
                            float *outShift, float *outRotate, float *outCrossterm );

// Keep at file scope; a block-scope declaration inside the anonymous namespace
// receives incompatible MSVC linkage.
extern bool         Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                             int commandId );                 // mainfrm.cpp:1340

namespace
{
    const float KBV_EPS          = 1.0e-4f;
    const float KBV_MATCH_TOL    = 0.1f;      // the ported FindPoint / SetupVertexSelection tolerance

    const float KBV_COL_OK[3]    = { 0.55f, 0.85f, 1.00f };
    const float KBV_COL_BAD[3]   = { 1.00f, 0.30f, 0.25f };

    inline bool PointNear( const float *a, const float *b, float tol )
    {
        float d[3];
        Sub3( a, b, d );
        return fabsf( d[0] ) <= tol && fabsf( d[1] ) <= tol && fabsf( d[2] ) <= tol;
    }

    // Mirrors kiwi_transform.cpp's ray/plane test.
    bool RayPlane( const ray_t &ray, const float *pt, const float *n, float *out )
    {
        const float den = Dot3( ray.dir, n );
        if ( fabsf( den ) < 1.0e-5f )
            return false;
        float rel[3];
        Sub3( pt, ray.origin, rel );
        const float t = Dot3( rel, n ) / den;
        if ( !( t > 0.0f ) || t > 1.0e6f )
            return false;
        Mad3( ray.origin, ray.dir, t, out );
        return true;
    }

    bool WindingCentre( const winding_t *w, float *out )
    {
        if ( !w || w->numpoints < 1 || w->numpoints > MAX_POINTS_ON_WINDING )
            return false;
        out[0] = out[1] = out[2] = 0.0f;
        for ( int i = 0; i < w->numpoints; ++i )
            Add3( out, w->p[i], out );
        const float inv = 1.0f / (float)w->numpoints;
        out[0] *= inv; out[1] *= inv; out[2] *= inv;
        return true;
    }

    // Static storage is required because the numeric layer retains the label pointer.
    const kiwiNumField_t KBEV_FIELDS[1] = { { "distance", KNUM_LENGTH, false } };

    // Shared cursor, numeric, and HUD state without transform constraints.
    class KiwiBevelBase : public KiwiEditorCommand
    {
    public:
        // Never snap or pick the geometry the gesture is reshaping (kiwi_pick.h).
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        // Subclasses own the live scalar, so only typed values appear in the bubble;
        // their HUD reports the current distance outside numeric entry.
        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KBEV_FIELDS; return 1; }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

    protected:
        virtual void Recompute() = 0;

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        void OpenUndo( const char *literalOp )
        {
            if ( m_undoOpen )
                return;
            KiwiCmd_UndoBegin( literalOp );      // string LITERAL — stored by pointer
            m_undoOpen = true;
        }

        // Drag and numeric distances remain raw world-space floats; there is no
        // grid quantisation in this command.

        bool          m_hasNum   = false;
        float         m_numWorld = 0.0f;
        bool          m_undoOpen = false;
        bool          m_invalid  = false;
        const char   *m_why      = 0;
        snap_result_t m_snap;
        char          m_hud[192] = { 0 };
    };

    // Inset face clone; see kiwi_bevel.h for the half-space limitation.
    class KiwiInsetCommand : public KiwiBevelBase
    {
    public:
        const char *Name() const override { return "Inset Face (clone)"; }
        bool CanExecute() override { return KiwiBevel_CanInset(); }

        struct unit_t
        {
            selbrush_t        *node;         // the SOURCE instance (never mutated)
            brush_t           *srcDef;
            int                faceIndex;    // the target face, same index on the clone
            brush_t           *clone;        // UNLINKED until commit
            std::vector<int>   adj;          // clone face indices to push inward
            std::vector<float> adjNormal;    // 3 floats per adj entry, baseline outward
            std::vector<float> basePts;      // clone planepts baseline, faceCount*9
            float              centre[3];    // target face winding centre (baseline)
            float              normal[3];    // target face outward normal (baseline)
        };

        bool Begin() override
        {
            FreeClones();            // release retained clones before Reset clears the vector
            Reset();
            if ( !Gather() )
            {
                FreeClones();
                return false;
            }
            LatchStart();
            Apply();
            UpdateHud();
            Sys_Printf( "Inset: drag toward the face centre, or type a depth "
                        "(%i face(s)).  The ORIGINAL brush is not modified.\n",
                        (int)m_units.size() );
            return true;
        }

        void Commit() override
        {
            if ( m_invalid || m_dist < KBEV_MIN_DIST )
            {
                Sys_Printf( "Inset: cancelled — %s.\n",
                            m_why ? m_why : ( m_dist < KBEV_MIN_DIST ? "depth too small"
                                                                     : "invalid geometry" ) );
                FreeClones();
                Reset();
                g_nUpdateBits = -1;
                return;
            }
            Land();
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            FreeClones();
            Reset();
            g_nUpdateBits = -1;
        }

        void DrawWorld() override
        {
            // Unlinked clones need this wireframe preview; the batch is shared.
            const float *col = m_invalid ? KBV_COL_BAD : KBV_COL_OK;
            KiwiLines_Color( col[0], col[1], col[2] );
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                brush_t *c = m_units[i].clone;
                if ( !c || !c->faces )
                    continue;
                for ( int f = 0; f < c->faceCount; ++f )
                {
                    winding_t *w = c->faces[f].w;
                    if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                        continue;
                    for ( int p = 0; p < w->numpoints; ++p )
                        if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                            return;
                }
            }
        }

    private:
        void Reset()
        {
            m_units.clear();
            m_dist       = 0.0f;
            m_startRad   = 0.0f;
            m_haveStart  = false;
            m_hasNum     = false;
            m_invalid    = false;
            m_why        = 0;
            m_undoOpen   = false;
            m_hud[0]     = '\0';
        }

        void FreeClones()
        {
            // An unlinked, zero-ref clone satisfies Brush_Free_R's precondition.
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                if ( m_units[i].clone )
                    Brush_Free_R( m_units[i].clone );
                m_units[i].clone = 0;
            }
        }

        bool AllLive() const
        {
            for ( size_t i = 0; i < m_units.size(); ++i )
                if ( !Sel_BrushLive( m_units[i].node ) )
                    return false;
            return true;
        }

        bool Gather()
        {
            const selection_t &sel = KiwiSel();
            for ( size_t i = 0; i < sel.items.size(); ++i )
            {
                const sel_item_t &it = sel.items[i];
                if ( it.kind != SEL_FACE || !Sel_BrushLive( it.brush ) || it.brush->patch )
                    continue;
                brush_t *def = it.brush->def;
                if ( !def || !def->faces || it.faceIndex < 0 || it.faceIndex >= def->faceCount )
                    continue;
                winding_t *tw = def->faces[it.faceIndex].w;
                if ( !tw || tw->numpoints < 3 || tw->numpoints > MAX_POINTS_ON_WINDING )
                    continue;

                bool dup = false;
                for ( size_t k = 0; k < m_units.size() && !dup; ++k )
                    dup = ( m_units[k].srcDef == def && m_units[k].faceIndex == it.faceIndex );
                if ( dup )
                    continue;
                if ( (int)m_units.size() >= KBEV_MAX_FACES )
                {
                    Sys_Printf( "Inset: more than %i faces selected — the rest are ignored.\n",
                                KBEV_MAX_FACES );
                    break;
                }

                unit_t u;
                u.node      = it.brush;
                u.srcDef    = def;
                u.faceIndex = it.faceIndex;
                u.clone     = 0;
                WindingCentre( tw, u.centre );
                Copy3( def->faces[it.faceIndex].plane.normal, u.normal );

                // Edge adjacency requires two points within the 0.1-unit tolerance.
                for ( int f = 0; f < def->faceCount; ++f )
                {
                    if ( f == it.faceIndex )
                        continue;
                    winding_t *w = def->faces[f].w;
                    if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
                        continue;
                    int shared = 0;
                    for ( int a = 0; a < w->numpoints && shared < 2; ++a )
                        for ( int b = 0; b < tw->numpoints; ++b )
                            if ( PointNear( w->p[a], tw->p[b], KBV_MATCH_TOL ) )
                            {
                                ++shared;
                                break;
                            }
                    if ( shared < 2 )
                        continue;
                    u.adj.push_back( f );
                    for ( int k = 0; k < 3; ++k )
                        u.adjNormal.push_back( def->faces[f].plane.normal[k] );
                }
                if ( u.adj.empty() )
                    continue;

                // Brush_Clone preserves face order, so gathered indices remain valid.
                u.clone = Brush_Clone( def );
                if ( !u.clone || !u.clone->faces || u.clone->faceCount != def->faceCount )
                {
                    if ( u.clone )
                        Brush_Free_R( u.clone );
                    continue;
                }
                u.basePts.resize( (size_t)u.clone->faceCount * 9 );
                for ( int f = 0; f < u.clone->faceCount; ++f )
                    memcpy( &u.basePts[(size_t)f * 9], &u.clone->faces[f].planepts[0][0],
                            sizeof( float ) * 9 );

                m_units.push_back( u );
            }

            if ( m_units.empty() )
            {
                Sys_Printf( "Inset: no insettable face is selected "
                            "(select faces with mode 3).\n" );
                return false;
            }
            return true;
        }

        void LatchStart()
        {
            m_haveStart = false;
            ray_t ray;
            float q[3];
            const unit_t &u = m_units[0];
            if ( !CursorRay( &ray ) || !RayPlane( ray, u.centre, u.normal, q ) )
                return;
            float rel[3];
            Sub3( q, u.centre, rel );
            m_startRad  = Len3( rel );
            m_haveStart = true;
        }

        void Recompute() override
        {
            if ( !AllLive() )
            {
                Sys_Printf( "Inset: selection changed under the gesture — cancelled.\n" );
                KiwiCmd_Cancel();
                return;
            }
            if ( m_units.empty() )
                return;
            if ( !m_haveStart )
                LatchStart();

            float d = m_dist;
            ray_t ray;
            float q[3];
            const unit_t &u = m_units[0];
            if ( m_haveStart && CursorRay( &ray ) && RayPlane( ray, u.centre, u.normal, q ) )
            {
                float rel[3];
                Sub3( q, u.centre, rel );
                d = m_startRad - Len3( rel );      // toward the centre = more inset
            }

            // Numeric input overrides the raw, unquantised drag distance.
            if ( m_hasNum )
                d = m_numWorld;

            if ( d < 0.0f )
                d = 0.0f;
            m_dist = d;
            Apply();
            UpdateHud();
        }

        // Translate adjacent planes from clone baseline; never accumulate or
        // write the source brush.
        void Apply()
        {
            m_invalid = false;
            m_why     = 0;

            // Gate the shared depth once; every legal unit still receives §19 validation.
            const bool tooShallow = ( m_dist < KBEV_MIN_DIST );
            if ( tooShallow )
            {
                m_invalid = true;
                m_why     = "depth too small";
            }

            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                unit_t &u = m_units[i];
                if ( !u.clone || !u.clone->faces )
                    continue;

                for ( size_t k = 0; k < u.adj.size(); ++k )
                {
                    const int f = u.adj[k];
                    if ( f < 0 || f >= u.clone->faceCount )
                        continue;
                    const float *n = &u.adjNormal[k * 3];
                    for ( int p = 0; p < 3; ++p )
                        Mad3( &u.basePts[(size_t)f * 9 + (size_t)p * 3], n, -m_dist,
                              u.clone->faces[f].planepts[p] );
                }

                KiwiValid_Rebuild( u.clone );
                const char *why = 0;
                if ( !tooShallow && !KiwiValid_CheckBrush( u.clone, &why ) )
                {
                    m_invalid = true;
                    m_why     = why;
                }
            }
            g_nUpdateBits |= 1;
        }

        // Preserve the kiwi_extrude.h creation order; deselect before landing.
        void Land()
        {
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "inset face" );
            m_undoOpen = true;

            int landed = 0;
            for ( size_t i = 0; i < m_units.size(); ++i )
            {
                unit_t &u = m_units[i];
                if ( !u.clone )
                    continue;
                if ( !Sel_BrushLive( u.node ) || !u.srcDef->owner )
                {
                    Brush_Free_R( u.clone );
                    u.clone = 0;
                    continue;
                }
                Entity_LinkBrush( u.clone, (entity_s *)u.srcDef->owner );
                selbrush_t *inst = Brush_AddToList( u.clone, u.node->owner );
                Brush_AddToList2( inst );        // → selected_brushes
                u.clone = 0;                     // ownership handed to the map
                ++landed;
            }
            // Rebuild typed selection from the newly selected legacy clones.
            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();

            Sys_Printf( "Inset: %i brush(es) created (the originals are unchanged).\n", landed );
            g_nUpdateBits = -1;
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_dist );
            if ( m_invalid )
                _snprintf( m_hud, sizeof( m_hud ), "inset (clone)  %i face(s)  %s  INVALID (%s)",
                           (int)m_units.size(), b, m_why ? m_why : "rejected" );
            else
                _snprintf( m_hud, sizeof( m_hud ), "inset (clone)  %i face(s)  %s",
                           (int)m_units.size(), b );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        std::vector<unit_t> m_units;
        float               m_dist      = 0.0f;
        float               m_startRad  = 0.0f;
        bool                m_haveStart = false;
    };

    KiwiInsetCommand s_inset;
}

bool KiwiBevel_CanBevel()
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( sel.items[i].kind == SEL_EDGE && Sel_BrushLive( sel.items[i].brush ) && !sel.items[i].brush->patch )
            return true;
    return false;
}

bool KiwiBevel_CanInset()
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( sel.items[i].kind == SEL_FACE && Sel_BrushLive( sel.items[i].brush ) && !sel.items[i].brush->patch )
            return true;
    return false;
}

// Remove one face to restore the edge cut by that half-space.
namespace
{
    // Accept exactly one face plus optional patch objects from a fillet.
    struct removeSel_t
    {
        selbrush_t *node      = 0;
        int         faceIndex = -1;
        int         faces     = 0;
        int         patches   = 0;
        int         others    = 0;      // anything other than a face or patch
    };

    removeSel_t SurveyRemoveSelection()
    {
        removeSel_t r;
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( !Sel_BrushLive( it.brush ) )
                continue;
            if ( it.kind == SEL_FACE && !it.brush->patch )
            {
                ++r.faces;
                r.node      = it.brush;
                r.faceIndex = it.faceIndex;
                continue;
            }
            if ( it.kind == SEL_OBJECT && it.brush->patch )
            {
                ++r.patches;
                continue;
            }
            ++r.others;
        }
        // Include legacy-only object selections so this handler cannot swallow
        // a Delete intended for another solid.
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( !b->patch )
                ++r.others;
        return r;
    }
}

bool KiwiBevel_CanRemoveFace()
{
    const removeSel_t r = SurveyRemoveSelection();
    return r.faces == 1 && r.others == 0 && Sel_BrushLive( r.node );
}

bool KiwiBevel_RemoveFaceRestoreEdge()
{
    const removeSel_t r = SurveyRemoveSelection();
    if ( r.faces != 1 || r.others != 0 || !Sel_BrushLive( r.node ) )
        return false;                       // not ours — the caller falls through

    brush_t *def = r.node->def;
    if ( !def || !def->faces || r.faceIndex < 0 || r.faceIndex >= def->faceCount )
        return false;

    // Four half-spaces is the closed-solid floor; refuse before mutation.
    if ( def->faceCount <= 4 )
    {
        Sys_Printf( "Remove Face: this brush has only %i faces — removing one "
                    "would leave an open solid.  Nothing was changed.\n",
                    def->faceCount );
        return true;                        // consumed: DO NOT fall through to delete
    }

    // Save the whole face, not only planepts: Brush_RemoveFace shifts complete
    // face_t records. Face_Alloc restores plane and material together, although
    // the restored face moves to the tail.
    face_t saved = def->faces[r.faceIndex];
    saved.w = 0;                            // the winding is Brush_RemoveFace's to free

    // The removed face and any paired fillet patches share one undo record.
    KiwiCmd_UndoBegin( "remove face" );     // string LITERAL — stored by pointer
    KiwiCmd_UndoCoverBrush( r.node );               // the face's brush is NOT on the legacy list

    Brush_RemoveFace( def, (unsigned int)r.faceIndex );
    KiwiValid_Rebuild( def );

    // Whole-record shifts keep each survivor's material on its plane; only its
    // derived winding expands. The closure test is separate because ordinary
    // winding/bounds validation can accept an unbounded half-space intersection.
    const char *why = "invalid geometry";
    if ( !KiwiValid_CheckBrush( def, &why ) || !KiwiValid_BrushCloses( def, &why ) )
    {
        // Put it back, then close the record as a CANCEL so the timeline is clean.
        Face_Alloc( def, &saved );
        KiwiValid_Rebuild( def );
        KiwiCmd_UndoCancel();

        // Tail restoration changes face indices; rebuild rather than retain stale
        // typed face/edge indices.
        Sel_Clear( KiwiSel() );
        Sel_RebuildFromLegacy();

        Sys_Printf( "Remove Face: refused — %s.  The brush is unchanged "
                    "(the selection was dropped).\n", why );
        g_nUpdateBits = -1;
        return true;                        // consumed, deliberately: see kiwi_bevel.h
    }

    // SurveyRemoveSelection proved that every legacy-selected object is a patch.
    int patches = 0;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        ++patches;
    if ( patches )
        Select_Delete();

    Sel_Clear( KiwiSel() );
    Sel_RebuildFromLegacy();
    KiwiCmd_UndoCommit();

    Sys_Printf( "Remove Face: face removed and the edge restored (%i face(s) left)%s.\n",
                def->faceCount,
                patches ? ", and the paired patch(es) deleted" : "" );
    g_nUpdateBits = -1;
    return true;
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiBevel_RegisterCommands()
{
    // Register for dispatch without claiming new direct key bindings.
    Radiant_RegisterCommand( "KiwiBevelEdge", 0, 0, KIWI_CMD_BEVEL_EDGE );
    Radiant_RegisterCommand( "KiwiInsetFace", 0, 0, KIWI_CMD_INSET_FACE );
    // The Delete funnel owns VK_DELETE, so this row remains unbound.
    Radiant_RegisterCommand( "KiwiRemoveFaceRestoreEdge", 0, 0, KIWI_CMD_REMOVE_FACE );
}

KiwiEditorCommand *KiwiBevel_CommandForId( int commandId )
{
    // Bevel starts in the shared fillet command's chamfer mode.
    if ( commandId == KIWI_CMD_BEVEL_EDGE )
        return KiwiPatchFillet_CommandForId( KIWI_CMD_FILLET_EDGE );
    if ( commandId == KIWI_CMD_INSET_FACE )
        return &s_inset;
    return 0;
}

// ─── Modeling buttons ────────────────────────────────────────────────────────
void KiwiBevel_MenuItems()
{
    const bool canBevel = KiwiBevel_CanBevel();
    const bool canInset = KiwiBevel_CanInset();

    ImGui::BeginDisabled( !canBevel );
    if ( ImGui::Button( "Bevel / Fillet Edge" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_BEVEL_EDGE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( canBevel
            ? "Chamfers the selected brush edges (the default).\n"
              "D toggles a quake3 bezier patch fillet into the\n"
              "notch; A drags the chamfer angle with the mouse,\n"
              "Tab types it.\n"
              "DEL on one selected face removes it and restores\n"
              "the original edge.  (Also bare B.)"
            : "Select one or more brush EDGES first (mode 2)." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canInset );
    if ( ImGui::Button( "Inset Face (clone)" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_INSET_FACE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( canInset
            ? "Clones the brush and pulls the target face's\n"
              "neighbours inward.  The original is untouched.\n"
              "(A classic brush cannot do a mesh-style inset.)"
            : "Select one or more brush FACES first (mode 3)." );
}

// Shared chamfer frame and face appender; contracts are in kiwi_bevel.h.
bool KiwiBevel_MakeFrame( kiwiBevelEdge_t *e )
{
    if ( !e )
        return false;
    brush_t *def = e->def;
    if ( !def || !def->faces )
        return false;

    // A chamfer requires exactly two adjacent faces at a manifold edge.
    e->adj[0] = -1;
    e->adj[1] = -1;
    int nAdj  = 0;
    for ( int f = 0; f < def->faceCount; ++f )
    {
        winding_t *w = def->faces[f].w;
        if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
            continue;
        bool has0 = false, has1 = false;
        for ( int i = 0; i < w->numpoints; ++i )
        {
            if ( PointNear( w->p[i], e->e0, KBV_MATCH_TOL ) ) has0 = true;
            if ( PointNear( w->p[i], e->e1, KBV_MATCH_TOL ) ) has1 = true;
        }
        if ( !has0 || !has1 )
            continue;
        if ( nAdj < 2 )
            e->adj[nAdj] = f;
        ++nAdj;
    }
    if ( nAdj != 2 )
        return false;

    // The outward bisector collapses for opposed faces, which are not a corner.
    Add3( def->faces[e->adj[0]].plane.normal, def->faces[e->adj[1]].plane.normal, e->n );
    if ( !Norm3( e->n ) )
        return false;

    // Re-orthogonalise the edge direction against n to absorb float error.
    float dir[3];
    Sub3( e->e1, e->e0, dir );
    const float edgeLen = Len3( dir );
    if ( !( edgeLen > KBV_EPS ) )
        return false;
    Mad3( dir, e->n, -Dot3( dir, e->n ), e->u );
    if ( !Norm3( e->u ) )
        return false;
    Cross3( e->n, e->u, e->v );
    if ( !Norm3( e->v ) )
        return false;

    for ( int k = 0; k < 3; ++k )
        e->mid[k] = ( e->e0[k] + e->e1[k] ) * 0.5f;

    e->spread = edgeLen;
    if ( e->spread < KBEV_MIN_SPREAD ) e->spread = KBEV_MIN_SPREAD;
    if ( e->spread > KBEV_MAX_SPREAD ) e->spread = KBEV_MAX_SPREAD;

    e->bias = 0.0f;              // symmetric bisector until the caller applies bias

    // R5 prefers an inheritable adjacent/dominant face, falling back to adj[0]
    // so an all-tool-material brush retains its material.
    {
        const int pick = KiwiMtl_PickSourceFace( def, e->n, e->adj[0], e->adj[1] );
        e->srcFace = ( pick >= 0 ) ? pick : e->adj[0];
    }
    return true;
}

// Patch arc and notch must use the same exported bias calculations.
float KiwiBevel_BiasLimit( const kiwiBevelEdge_t &e )
{
    if ( !e.def || !e.def->faces )
        return 0.0f;
    if ( e.adj[0] < 0 || e.adj[0] >= e.def->faceCount )
        return 0.0f;
    // k is the half-angle cosine; clamp float drift before acosf to avoid NaN.
    float k = Dot3( e.n, e.def->faces[e.adj[0]].plane.normal );
    if ( k < 0.0f ) k = -k;
    if ( k > 1.0f ) k = 1.0f;
    return acosf( k );
}

void KiwiBevel_BiasedNormal( const kiwiBevelEdge_t &e, float out[3] )
{
    if ( !out )
        return;
    if ( fabsf( e.bias ) < 1.0e-6f )
    {
        Copy3( e.n, out );
        return;
    }
    // Apply the API's +v convention: n' = n·cos + v·sin.
    const float c = cosf( e.bias ), s = sinf( e.bias );
    for ( int k = 0; k < 3; ++k )
        out[k] = e.n[k] * c + e.v[k] * s;
    if ( !Norm3( out ) )
        Copy3( e.n, out );
}

// KIWI (2026-09-22, user: "the bevels get a skewed look. I want a new behavior where they
// dont stretch the texture when beveling and instead just use more texture").
//
// A texdef is NOT a mapping in the face's plane: Face_MoveTexture projects it from the
// face normal's DOMINANT WORLD AXIS (Ed_Normal_Calc, the Quake TextureAxisFromPlane).  A
// chamfer face that copies its neighbour's texdef verbatim is therefore projected from an
// axis it leans 30-60 degrees away from, and the texture is drawn out by 1/cos along the
// slope - on a pitched roof the chamfer's dominant axis can even differ from the roof's.
//
// Instead the source face's mapping is UNFOLDED onto the chamfer, as if the texture were a
// sheet bent over the new edge: each channel's world texture matrix is reduced to its
// in-plane gradients, those are turned about the line the two planes share by the angle
// between the normals, and the offset is chosen so both faces agree along that line.
// Texel density is the source face's, the pattern runs on across the edge, and the chamfer
// simply shows more of it.  texturevecs_02 (the binary's own inverse of Face_MoveTexture)
// turns the result back into size / shift / rotate / crossterm for the chamfer's axis.
static void KiwiBevel_UnfoldTexdefs( face_t *nf, const face_t *src, const float *nB, float dB )
{
    const float *nF = src->plane.normal;
    const float  dF = src->plane.dist;
    const float  lf = Dot3( nF, nF );
    if ( !( lf > 0.81f && lf < 1.21f ) )
        return;                                 // no usable source plane: keep the copy

    float axis[3];
    Cross3( nF, nB, axis );
    const float sinA = Len3( axis );
    const float cosA = Dot3( nF, nB );
    if ( sinA < 1.0e-4f )
        return;                                 // parallel planes: the copy is already exact
    for ( int k = 0; k < 3; ++k )
        axis[k] /= sinA;

    // A point on both planes (n.p = dist on each).
    float p0[3];
    {
        const float inv = 1.0f / ( sinA * sinA );
        const float a   = ( dF - dB * cosA ) * inv;
        const float b   = ( dB - dF * cosA ) * inv;
        for ( int k = 0; k < 3; ++k )
            p0[k] = nF[k] * a + nB[k] * b;
    }

    for ( int ch = 0; ch < 3; ++ch )            // the three projected channels (Face_TexLock_Save)
    {
        MaterialDef *md = &nf->mtldef[ch];
        if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
            continue;                           // MtlDef_IsValid's rule, without its assert
        texdef_sub_t *td = &md->mat_texDef;     // copied from `src` by Face_Alloc

        float m[8];
        Face_MoveTexture( td->size, nF, m, td->shift, td->rotate, td->crossterm );

        float out[8];
        bool  ok = true;
        for ( int row = 0; row < 2 && ok; ++row )
        {
            const float *a  = &m[row * 4];
            const float  an = Dot3( a, nF );
            float g[3], cr[3], r[3];
            for ( int k = 0; k < 3; ++k )
                g[k] = a[k] - nF[k] * an;       // the mapping's gradient IN the source plane
            Cross3( axis, g, cr );
            const float ag = Dot3( axis, g );
            for ( int k = 0; k < 3; ++k )       // Rodrigues: nF -> nB about the shared line
                r[k] = g[k] * cosA + cr[k] * sinA + axis[k] * ag * ( 1.0f - cosA );
            if ( !( Dot3( r, r ) > 1.0e-12f ) )
                ok = false;
            const float at0 = Dot3( a, p0 ) + a[3];     // the source's value on the shared line
            out[row * 4 + 0] = r[0];
            out[row * 4 + 1] = r[1];
            out[row * 4 + 2] = r[2];
            out[row * 4 + 3] = at0 - Dot3( r, p0 );
        }
        if ( !ok )
            continue;

        texdef_sub_t res = *td;                 // sample_size rides along untouched
        texturevecs_02( res.size, out, 0.0f, nB, dB, res.shift, &res.rotate, &res.crossterm );

        bool finite = true;
        const float chk[6] = { res.size[0], res.size[1], res.shift[0], res.shift[1],
                               res.rotate, res.crossterm };
        for ( int k = 0; k < 6; ++k )
            if ( !( chk[k] == chk[k] ) || fabsf( chk[k] ) > 1.0e9f )
                finite = false;
        if ( finite && res.size[0] != 0.0f && res.size[1] != 0.0f )
            *td = res;
    }
}

int KiwiBevel_AppendFace( brush_t *def, const kiwiBevelEdge_t &e, float dist )
{
    if ( !def || !def->faces || e.srcFace < 0 || e.srcFace >= def->faceCount )
        return -1;

    // Face_Alloc copies the source face, preserving material, contents, and flags.
    face_t *nf = Face_Alloc( def, &def->faces[e.srcFace] );
    if ( !nf )
        return -1;

    // R6 realizes name-only material handles when the new face is created.
    KiwiMtl_RealizeFace( nf );

    // Repair missing/zero-scaled copied layers; sound sources are unchanged.
    KiwiMtl_EnsureFaceLayers( nf );      // kiwi_material.h:245

    // Recompute v from the biased normal so the three-point winding still yields
    // +n(bias); see kiwi_bevel.h for the cross-product proof.
    float nb[3], vb[3];
    KiwiBevel_BiasedNormal( e, nb );
    Cross3( nb, e.u, vb );
    if ( !Norm3( vb ) )
        Copy3( e.v, vb );

    float c[3];
    Mad3( e.mid, nb, -dist, c );
    Mad3( c, e.u, e.spread, nf->planepts[0] );
    Copy3( c,               nf->planepts[1] );
    Mad3( c, vb, e.spread, nf->planepts[2] );

    // Face_Alloc may have moved the face array: index the source again.
    KiwiBevel_UnfoldTexdefs( nf, &def->faces[e.srcFace], nb, Dot3( nb, c ) );
    return def->faceCount - 1;
}
