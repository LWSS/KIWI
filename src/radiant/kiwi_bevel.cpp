#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_bevel.cpp — RADIANT_UX_DESIGN §25 bevel/chamfer + inset.  See kiwi_bevel.h
// for the plane derivation, the winding-order proof, the Face_Alloc-vs-
// Ed_BrushSetFaceCount ruling and the honest statement of what "inset" means on a
// plane-defined brush.
//
// NEW code over the ported cores.  Every plane is written as THREE PLANEPTS and
// every rebuild goes through KiwiValid_Rebuild (Brush_BuildWindings, bFull 0) —
// this file never computes a plane equation itself.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "winding.h"

#include "kiwi_bevel.h"
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_material.h"              // ROUND T — the inheritance rules (R5 / R6)
#include "kiwi_numeric.h"
#include "kiwi_patchfillet.h"           // ROUND Q — the Modeling block's fillet button
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <imgui/imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int          Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118
extern int          g_nUpdateBits;                                            // 0x25D5A74 (mainfrm.cpp)

extern face_t      *Face_Alloc( brush_t *b, face_t *f );                      // brush.cpp:304  0x471500
extern unsigned int Brush_RemoveFace( brush_t *b, unsigned int faceIndex );   // brush.cpp:345  0x471640
extern brush_t     *Brush_Clone( brush_t *def );                              // brush.cpp:729  0x475D20
extern void         Brush_Free_R( brush_t *def );                             // brush.cpp:706  0x475AF0
extern selbrush_t  *Brush_AddToList( brush_t *def, entity_s *owner );         // brush.cpp:669  0x475980
extern void         Brush_AddToList2( selbrush_t *b );                        // brush.cpp:927  0x4765A0
extern void         Entity_LinkBrush( brush_t *b, entity_s *world_ent );      // entity.cpp:445 0x484FC0
extern void         Select_Deselect( int a1 );                                // select.cpp:1444 0x48E800 (int, NOT char — mangling)
extern void         Select_Delete();                                          // select.cpp:1520 0x48E9A0

extern void         Radiant_ExecCommand( unsigned int cmdId );                // mainfrm.cpp:4054

// KIWI-UX (CLEANUP, B-28): FILE SCOPE, not block scope.  Round AI shipped a link
// error from a block-scope extern that MSVC mangled with its enclosing namespace;
// kiwi_uv.cpp carries the full account.  This is the declaration that used to sit
// inside KiwiBevel_RegisterCommands.
extern bool         Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                             int commandId );                 // mainfrm.cpp:1340

namespace
{
    const float KBV_EPS          = 1.0e-4f;
    const float KBV_MATCH_TOL    = 0.1f;      // the ported FindPoint / SetupVertexSelection tolerance

    const float KBV_COL_OK[3]    = { 0.55f, 0.85f, 1.00f };
    const float KBV_COL_BAD[3]   = { 1.00f, 0.30f, 0.25f };

    // ── local predicate over the shared vec helpers (kiwi_vec.h) ────────────
    inline bool PointNear( const float *a, const float *b, float tol )
    {
        float d[3];
        Sub3( a, b, d );
        return fabsf( d[0] ) <= tol && fabsf( d[1] ) <= tol && fabsf( d[2] ) <= tol;
    }

    // ray ∩ plane(point, normal) — kiwi_transform.cpp's RayPlane, verbatim shape.
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

    // (ROUND T: RayAxis / WindingOf / EdgeEnds lived here and were the BEVEL
    //  command's alone.  That command merged into kiwi_patchfillet.cpp this
    //  round and took them with it; leaving three unreferenced statics behind
    //  would be the start of the dead-helper drift this tree keeps paying for.)

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

    // KIWI-UX (CLEANUP, UndoCoverBrush): the local copy is gone — this was one
    // of five verbatim bodies.  It is KiwiCmd_UndoCoverBrush (kiwi_command.h)
    // now, beside the bracket whose blind spot it exists to fill.

    // ── shakeout E: the numeric FIELD table (kiwi_command.h NumericFields) ──
    // STATIC storage — the numeric layer copies the struct but never the label.
    const kiwiNumField_t KBEV_FIELDS[1] = { { "distance", KNUM_LENGTH, false } };

    // ═════════════════════════════════════════════════════════════════════════
    //  Shared modal scaffolding: cursor, numeric, HUD, undo latch.
    //  (Deliberately its own small base rather than a reach into
    //  kiwi_transform.cpp's KiwiXformBase, which is file-static there and carries
    //  constraint state these two commands have no use for.)
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiBevelBase : public KiwiEditorCommand
    {
    public:
        // Never snap or pick the geometry the gesture is reshaping (kiwi_pick.h).
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        // KIWI-UX (shakeout E): ONE named LENGTH field.  Both commands' scalar is
        // a distance, so the label is all the field table has to add — and it is
        // what makes the HUD's "[distance]" tag and the bubble's units read right.
        // No NumericFieldValue override: the live scalar lives in the SUBCLASSES
        // (there is no shared member to report), so the bubble shows the typed
        // value while typing and stays quiet otherwise, with the verbose HUD line
        // carrying the live number exactly as it did before.
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

        // KIWI-UX (CLEANUP, SnapActive / B-24): KiwiBevelBase::SnapActive had NO
        // caller — the bevel command that used it was folded into
        // kiwi_patchfillet.cpp in round T (kiwi_bevel.h) and the one surviving
        // subclass, KiwiInsetCommand, never asked.  Deleted rather than re-pointed
        // at KiwiSnap_Active; `m_snap` stays, because MouseMove writes it and a
        // future subclass would want it.

        void OpenUndo( const char *literalOp )
        {
            if ( m_undoOpen )
                return;
            KiwiCmd_UndoBegin( literalOp );      // string LITERAL — stored by pointer
            m_undoOpen = true;
        }

        // ── KIWI-UX (ROUND Q): THE BEVEL SCALAR IS NEVER QUANTISED ───────────
        // USER DIRECTIVE, verbatim: "The current Bevel tool need to be way finer
        // […] we need to make the chamfer's(bevel) much finer in detail, smooth
        // by default with 0 snapping".
        //
        // WHAT IT WAS.  The drag already produced a RAW closest-point scalar in
        // full float precision (Recompute's RayAxis solve — see below); the
        // coarseness was one line downstream of it, a §6 "quantise the scalar,
        // never the cursor point" grid round:
        //
        //     const float g = KiwiUnits_GridSpacingWorld();
        //     if ( g > 0.0f ) d = floorf( d / g + 0.5f ) * g;
        //
        // and it fired on EVERY frame whose snap query came back SNAP_GRID —
        // which is the fallback arm, i.e. essentially always (kiwi_snap.h).  At
        // the editor's default spacing that made the chamfer step in whole grid
        // cells and skip every depth between them: a "bevel" with three usable
        // values.  CTRL (which SUPPRESSES snapping, kiwi_snap.h arm 0) was the
        // only way to get a smooth one, i.e. exactly backwards from the ask.
        //
        // WHAT IT IS NOW.  Nothing.  The scalar the drag computes is the scalar
        // the chamfer uses — raw, unrounded, full float, no grid involvement at
        // any spacing — so CTRL now changes nothing about this gesture because
        // there is no longer anything for it to suppress.  Numeric entry is
        // unaffected and still exact (m_hasNum wins outright, above).
        //
        // A GEOMETRY snap is deliberately still honoured: it does not QUANTISE
        // anything, it names an exact target ("chamfer up to that vertex"), which
        // is the opposite of the defect and is the one thing a modeller cannot
        // get by dragging carefully.  It is also rare by construction — it needs
        // the cursor within the arm's pixel radius of unselected geometry.

        bool          m_hasNum   = false;
        float         m_numWorld = 0.0f;
        bool          m_undoOpen = false;
        bool          m_invalid  = false;
        const char   *m_why      = 0;
        snap_result_t m_snap;
        char          m_hud[192] = { 0 };
    };

    // KIWI-UX (CLEANUP, B-31): KiwiBevelCommand used to live here; round T folded
    // it into kiwi_patchfillet.cpp's command.  kiwi_bevel.h states the merge and
    // what it means for anyone looking for the old command.

    // ═════════════════════════════════════════════════════════════════════════
    //  §25 INSET FACE — the CLONE compound (kiwi_bevel.h says why).
    // ═════════════════════════════════════════════════════════════════════════
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
            FreeClones();            // belt and braces: Reset() drops the vector
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
            // The clone is UNLINKED, so nothing else draws it — this wireframe IS
            // the preview.  Budgeted: the batch is shared with the snap marker.
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
            // Unlinked, refCount 0, onext/oprev NULL (Brush_Clone memsets the def)
            // — exactly Brush_Free_R's precondition.
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

                // Adjacent = shares an EDGE with the target winding, i.e. two or
                // more of its points coincide with the target's (0.1 tolerance).
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

                // Brush_Clone preserves face ORDER (it copies index for index), so
                // every index gathered above addresses the same face on the clone.
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

            // KIWI-UX (ROUND Q): the grid quantisation is gone here too.  Inset is
            // the bevel's sibling in this file, shares its base class and its
            // numeric field, and a user who has just been told the chamfer is
            // smooth would not expect the inset next to it to still tick.  The
            // reasoning is identical and is written out once, on the base class.
            if ( m_hasNum )
                d = m_numWorld;

            if ( d < 0.0f )
                d = 0.0f;
            m_dist = d;
            Apply();
            UpdateHud();
        }

        // Every adjacent face's plane TRANSLATED inward along its own outward
        // normal by d, from the clone's baseline planepts (rule 1: never
        // accumulate).  Nothing else on the clone moves, and the source brush is
        // never written at all.
        void Apply()
        {
            m_invalid = false;
            m_why     = 0;

            // ── KIWI-UX (CLEANUP, B-40): THE DEPTH GATE IS PRE-LOOP ──────────
            // It was inside the per-unit loop, re-testing a value that cannot
            // change inside it and short-circuiting KiwiValid_CheckBrush on EVERY
            // unit rather than on none.  Hoisted to one explicit early branch,
            // mirroring kiwi_patchfillet.cpp's shape.  TWO deliberate consequences:
            //   * an empty m_units at depth zero is now INVALID, where before the
            //     flag stayed false because the loop body never ran;
            //   * when the depth IS legal, KiwiValid_CheckBrush runs for units
            //     after the first — it used not to, because the first unit's
            //     `m_invalid = true` was set by this same test.  That is the
            //     intended reading: §19 gates every unit, not just unit 0.
            // The planes below are still written at every depth, so the live
            // preview is unchanged; only the verdict is.
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

        // The kiwi_extrude.h creation sequence, in its order.
        // KIWI-UX (CLEANUP, B-20): deselect before landing — the rule and its
        // reasons are stated once, at kiwi_patchfillet.cpp's LandPatches.
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
            // KIWI-UX (CLEANUP, B-17): the typed selection is resynced, exactly as
            // every other landing path in this layer does (kiwi_bevel.cpp:814-815 in
            // the OTHER verb in this same file, kiwi_loft.cpp:1902-1903 / :2246-2247,
            // kiwi_patchfillet.cpp:1582-1583, kiwi_matchface.cpp:540-541).  Without it
            // the typed side kept the pre-gesture SEL_FACE items — faces of the source
            // brushes — while selected_brushes held the new clones.
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

// ─── §3 canExecute ───────────────────────────────────────────────────────────
bool KiwiBevel_CanBevel()
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
        // KIWI-UX (CLEANUP, B-5): liveness first, same shape as
        // KiwiPatchFillet_CanFillet — kiwi_bevel.h:267 already promises "live".
        if ( sel.items[i].kind == SEL_EDGE && Sel_BrushLive( sel.items[i].brush ) && !sel.items[i].brush->patch )
            return true;
    return false;
}

bool KiwiBevel_CanInset()
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
        // KIWI-UX (CLEANUP, B-5): liveness first, as above.
        if ( sel.items[i].kind == SEL_FACE && Sel_BrushLive( sel.items[i].brush ) && !sel.items[i].brush->patch )
            return true;
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND T — REMOVE FACE (RESTORE EDGE).  See kiwi_bevel.h for the rule and why
//  it is the GENERAL verb rather than a "was this made by a bevel" guess.
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
    // The single selected FACE, plus a census of everything else in the typed
    // selection.  The funnel's rung is deliberately narrow: exactly one face, and
    // nothing else except PATCH objects (which is what a FILLET's other half is).
    struct removeSel_t
    {
        selbrush_t *node      = 0;
        int         faceIndex = -1;
        int         faces     = 0;
        int         patches   = 0;
        int         others    = 0;      // anything that is not one of those two
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
        // The LEGACY list is asked too: an OBJECT selection lives there as well,
        // and a brush that reached it by some route the typed selection did not
        // see must still count as "something else is selected" or this verb would
        // silently swallow a delete the user meant for it.
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

    // §19 V1 up front, with its own words: four half-spaces is the floor, so a
    // five-faced brush cannot give one back.  Refusing HERE rather than after the
    // rebuild means nothing was touched at all.
    if ( def->faceCount <= 4 )
    {
        Sys_Printf( "Remove Face: this brush has only %i faces — removing one "
                    "would leave an open solid.  Nothing was changed.\n",
                    def->faceCount );
        return true;                        // consumed: DO NOT fall through to delete
    }

    // THE BASELINE, and it is ONE face_t, not a planepts table.
    //
    // TRAP, worth stating because the obvious repair is wrong: restoring a
    // brush by writing a saved planepts ARRAY back over def->faces[] only works
    // while the face ORDER is unchanged.  Brush_RemoveFace shifts every later
    // face DOWN one slot — planepts and material together, it is a memmove of
    // whole face_t records — so index f after the removal is a different face
    // than index f before it, and replaying a planepts table onto it would put
    // each material on somebody else's plane.  Nothing would look broken; every
    // surface would just be textured wrong.
    //
    // So the restore is the exact inverse of the removal instead: keep a COPY of
    // the one face that goes, and Face_Alloc it back.  Face_Alloc appends a copy
    // of a template (brush.cpp:291), and a face_t carries its own planepts, all
    // four material channels, contents and toolflags — so the brush comes back
    // whole, with only the face ORDER differing (which is not part of a brush's
    // shape: a brush IS the intersection of its half-spaces, unordered).
    face_t saved = def->faces[r.faceIndex];
    saved.w = 0;                            // the winding is Brush_RemoveFace's to free

    // ONE record for the face removal AND any paired patches (a fillet is a
    // chamfer plus a patch; selecting both and pressing DEL undoes as one thing).
    KiwiCmd_UndoBegin( "remove face" );     // string LITERAL — stored by pointer
    KiwiCmd_UndoCoverBrush( r.node );               // the face's brush is NOT on the legacy list

    Brush_RemoveFace( def, (unsigned int)r.faceIndex );
    KiwiValid_Rebuild( def );

    // EVERY SURVIVING FACE KEEPS ITS MATERIAL AND ITS TEXDEF, and it costs nothing
    // to keep: Brush_RemoveFace memmoves whole 232-byte face_t records down one
    // slot, so each surviving plane carries its own planepts, all four MaterialDef
    // layers, its contents and its toolflags across unchanged.  Nor is there any
    // texture lock to run — lock exists to re-project a texdef when its own PLANE
    // moves, and no surviving plane moves here.  The neighbours only get LARGER
    // windings (they re-extend to the edge the removed face had cut off), and a
    // texdef is anchored to the plane, not to the winding.
    //
    // ── KIWI-UX (ROUND AA, ITEM 4): …AND V8, "DOES IT STILL CLOSE" ───────────
    // USER REPORT, verbatim: "You still can't delete a chamfer that was made on a
    // brush."  V1..V7 are all read off what Brush_BuildWindings produced, and
    // Brush_BuildWindings has no world box — def->[mins,maxs] is the box of the
    // triple-plane intersection POINTS (brush.cpp:1459-1463), and an open cell
    // still has vertices, so V7 passes on a solid that runs away to infinity.
    // V3 catches most opens by accident (the side faces of a lidless box keep only
    // two intersection points each, so their windings come back NULL), but "most"
    // is not a decision procedure, and this verb's entire job is to remove a
    // bounding plane.  KiwiValid_BrushCloses asks the question directly; see
    // kiwi_validity.h's V8 section for the probe box.
    const char *why = "invalid geometry";
    if ( !KiwiValid_CheckBrush( def, &why ) || !KiwiValid_BrushCloses( def, &why ) )
    {
        // Put it back, then close the record as a CANCEL so the timeline is clean.
        Face_Alloc( def, &saved );
        KiwiValid_Rebuild( def );
        KiwiCmd_UndoCancel();

        // The face ORDER changed (the restored face is at the tail now), so every
        // (faceIndex, edgeIndex) in the typed selection is stale — the SAME hazard
        // kiwi_selection.h's rebuild exists for.  Drop it rather than leave a
        // selection that names the wrong surfaces.
        Sel_Clear( KiwiSel() );
        Sel_RebuildFromLegacy();

        Sys_Printf( "Remove Face: refused — %s.  The brush is unchanged "
                    "(the selection was dropped).\n", why );
        g_nUpdateBits = -1;
        return true;                        // consumed, deliberately: see kiwi_bevel.h
    }

    // The paired patches, in the SAME record.  Select_Delete works on the legacy
    // list, and SurveyRemoveSelection has already proved the only things on it are
    // patches — so this cannot reach a solid the user did not mean to lose.
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
    // Unbound: the CLASSIC-profile bindings, and the modern profile deliberately
    // claims NO new key this phase.  KEY CANDIDATES, logged rather than taken:
    //   Bevel Edge  — Ctrl+B (classic 0x42 is free with mods 4; B alone is
    //                 "BrushMakeCone" in the default table)
    //   Inset Face  — Ctrl+I (0x49 mods 4 is free; I alone is unbound)
    Radiant_RegisterCommand( "KiwiBevelEdge", 0, 0, KIWI_CMD_BEVEL_EDGE );
    Radiant_RegisterCommand( "KiwiInsetFace", 0, 0, KIWI_CMD_INSET_FACE );
    // ROUND T: the DEL restore.  Unbound as a ROW — it is reached from the
    // Delete-key funnel (kiwi_command.cpp) and from the palette by name, and a
    // binding row on 0x2E would be a second, competing answer to that key.
    Radiant_RegisterCommand( "KiwiRemoveFaceRestoreEdge", 0, 0, KIWI_CMD_REMOVE_FACE );
}

KiwiEditorCommand *KiwiBevel_CommandForId( int commandId )
{
    // KIWI-UX (CLEANUP, B-31): BEVEL EDGE is answered by the fillet command —
    // the round-T merge, stated in kiwi_bevel.h.
    if ( commandId == KIWI_CMD_BEVEL_EDGE )
        return KiwiPatchFillet_CommandForId( KIWI_CMD_FILLET_EDGE );
    if ( commandId == KIWI_CMD_INSET_FACE )
        return &s_inset;
    return 0;
}

// ─── the "Modeling" block's two buttons ──────────────────────────────────────
void KiwiBevel_MenuItems()
{
    const bool canBevel = KiwiBevel_CanBevel();
    const bool canInset = KiwiBevel_CanInset();

    // KIWI-UX (CLEANUP, B-31): ONE button for ONE tool — the round-T merge,
    // stated in kiwi_bevel.h.
    ImGui::BeginDisabled( !canBevel );
    if ( ImGui::Button( "Bevel / Fillet Edge" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_BEVEL_EDGE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( canBevel
            ? "Chamfers the selected brush edges (the default).\n"
              "D toggles a quake3 bezier patch fillet into the\n"
              "notch; Tab reaches the angle-bias field.\n"
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

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND Q — THE CHAMFER FRAME AND THE CHAMFER FACE, EXPORTED
//
//  Both bodies are lifted UNCHANGED out of KiwiBevelCommand (they were its
//  private MakeFrame / AddFace).  The command now calls these, so there is one
//  derivation and one appender in the editor and kiwi_patchfillet.cpp shares
//  them rather than carrying a second copy.  See kiwi_bevel.h for the contract
//  and the top of that header for the plane derivation and the winding proof.
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiBevel_MakeFrame( kiwiBevelEdge_t *e )
{
    if ( !e )
        return false;
    brush_t *def = e->def;
    if ( !def || !def->faces )
        return false;

    // Requires EXACTLY TWO adjacent faces: a brush edge with any other count is
    // not a manifold corner and has no well-defined chamfer.
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

    // n = normalise(n1 + n2).  Collapses only when the two faces are exactly
    // opposed, which is not a corner.
    Add3( def->faces[e->adj[0]].plane.normal, def->faces[e->adj[1]].plane.normal, e->n );
    if ( !Norm3( e->n ) )
        return false;

    // u = the edge direction, re-orthogonalised against n (it is already
    // perpendicular in exact arithmetic — see kiwi_bevel.h).
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

    e->bias = 0.0f;              // ROUND T: the symmetric bisector until told otherwise

    // ── KIWI-UX (ROUND T): THE CHAMFER INHERITS A **VISIBLE** MATERIAL ──────
    // WAS: e->srcFace = e->adj[0] — an arbitrary one of the two faces at the
    // corner.  With a caulk neighbour (which is what a Cut used to leave behind
    // everywhere) the chamfer came out caulk, and the ROUND Q patch fillet then
    // copied ITS material from the chamfer — so the fillet patch was created
    // carrying a tool material and drew nothing at all.  That is the whole of the
    // "fillets are being created invisible" report.
    //
    // NOW: kiwi_material.h R5 — prefer whichever adjacent face is INHERITABLE,
    // else the brush's dominant inheritable face, else adj[0] exactly as before
    // (an all-caulk brush still chamfers into caulk, which is right).
    {
        const int pick = KiwiMtl_PickSourceFace( def, e->n, e->adj[0], e->adj[1] );
        e->srcFace = ( pick >= 0 ) ? pick : e->adj[0];
    }
    return true;
}

// ROUND T — the bias limit and the biased normal.  Both are two lines and both
// are exported, because the patch fillet has to agree with the appended face
// exactly or the arc and the notch stop meeting.
float KiwiBevel_BiasLimit( const kiwiBevelEdge_t &e )
{
    if ( !e.def || !e.def->faces )
        return 0.0f;
    if ( e.adj[0] < 0 || e.adj[0] >= e.def->faceCount )
        return 0.0f;
    // k = n·n1 = cos(half-angle between the bisector and either face normal), so
    // the half-angle itself is acosf(k).  Clamped defensively: a k outside [-1,1]
    // by a float hair would make acosf return a NaN and poison every later frame.
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
    // Rodrigues about u, with u·n == 0 by construction (MakeFrame
    // re-orthogonalises), so the (1-cos) term vanishes and this is the whole
    // rotation: n' = n·cos + v·sin.
    const float c = cosf( e.bias ), s = sinf( e.bias );
    for ( int k = 0; k < 3; ++k )
        out[k] = e.n[k] * c + e.v[k] * s;
    if ( !Norm3( out ) )
        Copy3( e.n, out );
}

int KiwiBevel_AppendFace( brush_t *def, const kiwiBevelEdge_t &e, float dist )
{
    if ( !def || !def->faces || e.srcFace < 0 || e.srcFace >= def->faceCount )
        return -1;

    // Face_Alloc is the ported grow-by-one; it COPIES the source face, which is
    // what gives the chamfer its material, contents and toolflags for free (§26).
    face_t *nf = Face_Alloc( def, &def->faces[e.srcFace] );
    if ( !nf )
        return -1;

    // ROUND T: kiwi_material.h R6 — the copied MaterialDef carries POINTERS, and
    // one of them can be a degenerate (name-only, zero-layer) handle from a
    // headless or prefab-load SetMaterial.  Realize it here, once, at the moment
    // the face is created, so nothing downstream has to remember to.
    KiwiMtl_RealizeFace( nf );

    // ...and the copy inherits the source's HOLES as well as its handles: a source
    // face whose lightmap or smoothing channel is missing or zero-scaled gives the
    // chamfer the same one, which is invisible in Shift+L and unlit at compile
    // (kiwi_material.h "the three channels").  No-op on a sound source.
    KiwiMtl_EnsureFaceLayers( nf );      // kiwi_material.h:245

    // n(bias), then c = mid - n(bias)*d, then the three points whose
    // cross(p0-p1, p2-p1) is +n(bias) (the proof is in kiwi_bevel.h).  The v used
    // for planept[2] must be the BIASED frame's, not the baseline one, or the
    // winding order stops matching the normal the moment a bias is applied.
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
    return def->faceCount - 1;
}
