#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Difference uses KiwiSplit_DefByPlaneCarve over the ported splitter; union
// delegates to the classic CSG-merge handler.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_boolean.h"
#include "kiwi_csg.h"                 // KiwiCsg_BrushUsable
#include "kiwi_boxselect.h"         // KiwiBox_CollectBrushes
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_split.h"
#include "kiwi_vec.h"     // Dot3/Sub3/...

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

// Ported entry points.
extern camera_s   *Ed_Camera();                                               // camwnd.cpp:161
extern int         Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern int         g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)
extern selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner );          // brush.cpp:669  (0x475980)
extern void        Brush_AddToList2( selbrush_t *b );                         // brush.cpp:927  (0x4765a0)
extern void        Brush_Free( selbrush_t *b );                               // brush.cpp:1002  (0x475ba0)
extern void        Select_Deselect( int bAlsoFreeFaces );                     // select.cpp:1444 (0x48E800)
extern void        Select_Brush( selbrush_t *brush, char some_overwrite,
                                 char bStatus, char center_grid_on_selection ); // select.cpp:884
extern void        Radiant_ExecCommand( unsigned int cmdId );                 // mainfrm.cpp:4054
// Difference tools are outside selected_brushes, so cover them explicitly.
// Entities must precede their brushes in the undo record.
extern void        Undo_AddBrush( entity_brush_s *pBrushInst );                // undo.cpp:494  (0x45e680)
extern void        Undo_AddEntity( int a1 );                                   // undo.cpp:601  (0x45e8a0)

// Translucent fill entry points; R_AddCmdSetMaterialColor is in r_rendercmds.h.
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    // mainfrm.cpp:5331 dispatches this id to Cmd_OnSelectionCsgmerge, keeping one
    // merge implementation and undo path.
    const int KBOOL_ID_MERGE = 32927;

    // Difference tools are red; union operands are green to remain distinct from
    // KIWI's blue selection tint. Alpha 0.22 stays visible over lit surfaces.
    const float KBOOL_DIFF_RGBA [4] = { 1.00f, 0.16f, 0.16f, 0.22f };   // the tool, carving
    const float KBOOL_UNION_RGBA[4] = { 0.25f, 1.00f, 0.45f, 0.22f };   // both, combining
    const float KBOOL_DIFF_LINE [3] = { 1.00f, 0.35f, 0.30f };
    const float KBOOL_UNION_LINE[3] = { 0.40f, 1.00f, 0.55f };
    // Shared modelling-layer yellow: this is what the next click will take.
    const float KBOOL_HOT_LINE  [3] = { 1.00f, 0.90f, 0.30f };

    // Fill costs one draw per face. Cap common multi-cut gestures at 32 filled
    // operands, spending newest-first; older operands fall back to the reported line budget.
    const int KBOOL_MAX_FILL = 32;

    // Preserve room for the framework's later snap marker and hover colour run.
    const int KBOOL_LINE_HEADROOM = 96;


    // The shared CSG gate rejects patches and fixed-size entities; boolean also
    // requires four planes because fewer cannot bound a volume.
    inline bool Usable( const selbrush_t *b )
    {
        return KiwiCsg_BrushUsable( b ) && b->def->faceCount >= 4;
    }

    // Brush_BuildWindings keeps these bounds current; reject misses before N splits.
    bool BoundsOverlap( const brush_t *a, const brush_t *b )
    {
        for ( int k = 0; k < 3; ++k )
            if ( a->mins[k] > b->maxs[k] || a->maxs[k] < b->mins[k] )
                return false;
        return true;
    }

    void FreeDefs( std::vector<brush_t *> *defs )
    {
        for ( size_t i = 0; i < defs->size(); ++i )
            KiwiSplit_FreeUnlandedDef( ( *defs )[i] );
        defs->clear();
    }

    // N-plane subtract; see kiwi_boolean.h.
    enum carveResult_t
    {
        KBOOL_CARVED = 0,   // outPieces holds the surviving parts, none landed yet
        KBOOL_MISS,         // the two solids do not intersect: nothing to do, no error
        KBOOL_CONSUMED,     // the input lies wholly INSIDE the tool: zero pieces survive
        KBOOL_REFUSED       // §19 said no
    };

    // `target` is borrowed and never mutated; all outputs and remainders are fresh,
    // unlanded clones. Splitter material rules therefore texture new cut faces from
    // the target descendant, never from the disposable tool.
    //
    // A front half may be landed, so §19 rejects or drops invalid fronts. The back
    // half is only a running intersection and is discarded; a collapsed back means
    // a miss, while a refused back with real extent may be carried and reported.
    // KBOOL_CONSUMED is only a per-piece result; CarveTarget decides whether the
    // user's original target disappeared and must instead be refused.
    int s_carveSliverDrops = 0;

    // Per-commit diagnostics: skipped cuts, first miss reason, and unlanded backs
    // carried past §19. The command is synchronous and resets these before carving.
    // Emit at most eight per-cut messages while retaining the full refusal count.
    enum { KBOOL_REFUSE_REPORT_MAX = 8 };
    int  s_carveRefusals   = 0;
    char s_carveMissWhy[192] = { 0 };
    int  s_carveCarried    = 0;

    void KiwiBool_ResetCarveReport()
    {
        s_carveSliverDrops = 0;
        s_carveRefusals    = 0;
        s_carveCarried     = 0;              // reset per commit
        s_carveMissWhy[0]  = '\0';
    }

    carveResult_t CarveByOneTool( brush_t *target, const brush_t *tool,
                                  std::vector<brush_t *> *outPieces, const char **why )
    {
        outPieces->clear();
        *why = "unknown";

        if ( !BoundsOverlap( target, tool ) )
        {
            *why = "their bounding boxes do not overlap";   // cheap miss
            return KBOOL_MISS;
        }

        brush_t *remainder = target;
        bool     ownRemainder = false;      // true once `remainder` is ours to free

        for ( int f = 0; f < tool->faceCount; ++f )
        {
            const face_t &tf = tool->faces[f];

            brush_t        *front = 0, *back = 0;
            kiwiSplitHalf_t fs = KSPLIT_HALF_NONE, bs = KSPLIT_HALF_NONE;
            // The back is an unlanded running intersection. The splitter may carry
            // a §19-refused back only when every extent meets KSPLIT_CARRY_EXTENT;
            // front halves that could become brushes remain fully gated.
            bool backRefused = false;
            if ( !KiwiSplit_DefByPlaneCarve( remainder, tf.planepts[0], tf.planepts[1],
                                             tf.planepts[2], &front, &back,
                                             &fs, &bs, &backRefused, why ) )
            {
                // The splitter's false contract allocates nothing.
                FreeDefs( outPieces );
                if ( ownRemainder )
                    KiwiSplit_FreeUnlandedDef( remainder );
                return KBOOL_REFUSED;
            }

            if ( bs != KSPLIT_HALF_OK )
            {
                // NONE puts the remainder wholly outside this tool plane; SLIVER
                // collapses the running intersection below §19. Both mean no
                // removable shared volume, so leave the target whole.
                if ( front )
                    KiwiSplit_FreeUnlandedDef( front );
                FreeDefs( outPieces );
                if ( ownRemainder )
                    KiwiSplit_FreeUnlandedDef( remainder );
                // Distinguish a true miss from a near-tangent collapse. Copy first
                // because `*why` may alias the static destination used below.
                char gateWhy[96];
                _snprintf( gateWhy, sizeof( gateWhy ), "%s", *why ? *why : "invalid geometry" );
                gateWhy[sizeof( gateWhy ) - 1] = '\0';
                static char s_missWhy[160];
                _snprintf( s_missWhy, sizeof( s_missWhy ),
                           ( bs == KSPLIT_HALF_NONE )
                             ? "no shared volume — tool plane %i leaves the target "
                               "entirely outside it"
                             : "the shared volume collapsed below the validity gate "
                               "at tool plane %i (a near-tangent cut: %s)",
                           f, gateWhy );
                s_missWhy[sizeof( s_missWhy ) - 1] = '\0';
                *why = s_missWhy;
                return KBOOL_MISS;
            }

            // Carried intermediates never land, but report available gate detail because the
            // resulting hole may be inexact around this plane.
            if ( backRefused )
            {
                ++s_carveCarried;
                if ( s_carveCarried <= KBOOL_REFUSE_REPORT_MAX )
                    Sys_Printf( "Boolean: tool plane %i left the running intersection "
                                "outside the validity gate (%s) — carried on, because "
                                "that piece is an intermediate and is never landed.\n",
                                f, ( *why && **why ) ? *why : "invalid geometry" );
            }

            if ( fs == KSPLIT_HALF_NONE )
            {
                // Everything is INSIDE this half-space: this plane carves nothing.
                // `back` is a re-clone of the same volume, so drop it and carry the
                // remainder we already have to the next plane.
                KiwiSplit_FreeUnlandedDef( back );
                continue;
            }

            if ( fs == KSPLIT_HALF_SLIVER )
            {
                // The outside slice is below the §19 gate: it was freed by the
                // splitter and it is NOT a piece.  The remainder still advances —
                // the volume forfeited is smaller than the gate that rejected it.
                ++s_carveSliverDrops;
            }
            else
            {
                outPieces->push_back( front );      // outside this plane = it survives
            }

            if ( ownRemainder )
                KiwiSplit_FreeUnlandedDef( remainder );
            remainder    = back;                    // still possibly inside the tool
            ownRemainder = true;
        }

        // The final remainder is target ∩ tool; discard it without landing it.
        if ( ownRemainder )
            KiwiSplit_FreeUnlandedDef( remainder );

        if ( outPieces->empty() )
        {
            // The input lies wholly inside this tool; the cascade owns the verdict.
            return KBOOL_CONSUMED;
        }
        return KBOOL_CARVED;
    }

    // Cascade each tool over every surviving piece to compute
    // `target - (t0 ∪ t1 ∪ …)` without constructing a non-convex tool union.
    // `!owned` means cur contains only the borrowed target; once any cut changes it,
    // every entry is an owned unlanded clone. Misses and refused individual cuts
    // preserve their input piece and allow the rest of the cascade to continue.
    carveResult_t CarveTarget( brush_t *target, brush_t *const *tools, int toolCount,
                               std::vector<brush_t *> *outPieces, const char **why )
    {
        outPieces->clear();
        *why = "unknown";
        if ( !target || !tools || toolCount <= 0 )
            return KBOOL_MISS;

        std::vector<brush_t *> cur;
        bool                   owned = false;      // see the OWNERSHIP note above
        cur.push_back( target );

        for ( int t = 0; t < toolCount; ++t )
        {
            const brush_t *tool = tools[t];
            if ( !tool )
                continue;

            std::vector<brush_t *> next;
            bool                   changed = false;

            for ( size_t p = 0; p < cur.size(); ++p )
            {
                std::vector<brush_t *> made;
                const carveResult_t r = CarveByOneTool( cur[p], tool, &made, why );

                if ( r == KBOOL_REFUSED )
                {
                    // A refusal frees all attempted outputs and leaves the input
                    // piece unchanged, so skip only this cut. Nothing invalid lands;
                    // report the possibly incomplete hole, with a bounded line count.
                    ++s_carveRefusals;
                    if ( s_carveRefusals <= KBOOL_REFUSE_REPORT_MAX )
                        Sys_Printf( "Boolean: tool %i could not cut piece %i — %s.  "
                                    "That cut was skipped; the rest of the "
                                    "difference still ran.\n",
                                    t, (int)p, ( *why && **why ) ? *why : "invalid geometry" );
                    else if ( s_carveRefusals == KBOOL_REFUSE_REPORT_MAX + 1 )
                        Sys_Printf( "Boolean: (further per-cut refusals suppressed "
                                    "for this operation)\n" );
                    next.push_back( cur[p] );      // survives whole; ownership unchanged
                    continue;
                }
                if ( r == KBOOL_MISS )
                {
                    // Latch the first miss observed across this commit.
                    if ( !s_carveMissWhy[0] && *why && **why )
                    {
                        _snprintf( s_carveMissWhy, sizeof( s_carveMissWhy ),
                                   "tool %i vs piece %i: %s", t, (int)p, *why );
                        s_carveMissWhy[sizeof( s_carveMissWhy ) - 1] = '\0';
                    }
                    next.push_back( cur[p] );      // survives whole; ownership unchanged
                    continue;
                }

                // CARVED or CONSUMED replaces this piece with `made`. The first
                // input is the borrowed map target; later inputs are owned clones.
                changed = true;
                if ( owned )
                    KiwiSplit_FreeUnlandedDef( cur[p] );
                for ( size_t k = 0; k < made.size(); ++k )
                    next.push_back( made[k] );
            }

            cur.swap( next );
            if ( changed )
                owned = true;
        }

        if ( !owned )
        {
            // Every tool missed; cur is still the borrowed target. Return the
            // commit-wide first reason so the caller can distinguish miss classes.
            if ( s_carveMissWhy[0] )
                *why = s_carveMissWhy;
            return KBOOL_MISS;
        }
        if ( cur.empty() )
        {
            // Refuse an empty result rather than silently deleting the target.
            *why = "it is entirely inside the tool(s) (a difference would remove it "
                   "completely) — delete it directly if that is what you want";
            return KBOOL_REFUSED;
        }
        outPieces->swap( cur );
        return KBOOL_CARVED;
    }

    // Convex face windings are triangle fans. Bracket per-vertex colour with the
    // selected-face fill's neutral/white material state (camwnd.cpp 0x408106).
    const int KBOOL_FILL_MAX_PTS = 64;      // fixed preview scratch; larger legal faces are skipped

    void FillBrush( const brush_t *def, const float rgba[4] )
    {
        if ( !def || !def->faces )
            return;
        // Ed_Camera never returns NULL (camwnd.cpp:159).
        const camera_s *cam = Ed_Camera();

        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        static float    s_xyzw  [KBOOL_FILL_MAX_PTS][4];
        static float    s_normal[KBOOL_FILL_MAX_PTS][3];
        static float    s_st    [KBOOL_FILL_MAX_PTS][2];
        static float    s_color [KBOOL_FILL_MAX_PTS];
        static uint16_t s_idx   [( KBOOL_FILL_MAX_PTS - 2 ) * 3];

        R_AddCmdSetMaterialColor( s_neutral );
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 3 || w->numpoints > KBOOL_FILL_MAX_PTS )
                continue;
            const int n = w->numpoints;
            for ( int i = 0; i < n; ++i )
            {
                s_xyzw[i][0] = w->p[i][0];
                s_xyzw[i][1] = w->p[i][1];
                s_xyzw[i][2] = w->p[i][2];
                s_xyzw[i][3] = 1.0f;
                Copy3( def->faces[f].plane.normal, s_normal[i] );
                s_st[i][0] = 0.0f;
                s_st[i][1] = 0.0f;
                s_color[i] = packedAsFloat;
            }
            const int tris = n - 2;
            for ( int t = 0; t < tris; ++t )
            {
                s_idx[t * 3 + 0] = 0;
                s_idx[t * 3 + 1] = (uint16_t)( t + 1 );
                s_idx[t * 3 + 2] = (uint16_t)( t + 2 );
            }
            // Orient each outward-wound fan to the eye or the preview becomes a half-shell.
            KiwiTris_OrientToEye( &s_xyzw[0][0], 4, s_idx, tris * 3, cam->origin );
            R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                    (short)( tris * 3 ), s_idx, (short)n,
                                    s_xyzw, s_normal, s_color, s_st );
        }
        R_AddCmdSetMaterialColor( s_white );
    }

    // Add face boundaries to the framework's shared line batch. Callers reserve
    // exact cost first; a false return remains the no-wrap safety net.
    bool OutlineBrush( const brush_t *def )
    {
        if ( !def || !def->faces )
            return true;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;
            for ( int p = 0; p < w->numpoints; ++p )
                if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                    return false;
        }
        return true;
    }

    // Mirror OutlineBrush's walk exactly; winding shapes make estimates unreliable.
    int OutlineCost( const brush_t *def )
    {
        if ( !def || !def->faces )
            return 0;
        int n = 0;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const winding_t *w = def->faces[f].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                continue;
            n += w->numpoints;
        }
        return n;
    }

    // Q: selected solids are targets; clicked or marquee-selected solids are tools.
    class KiwiBooleanCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Boolean"; }
        bool CanExecute() override { return KiwiBool_CanBoolean(); }

        // Clicks keep adding or replacing tools after the preview goes live;
        // RMB/Enter commits and Esc walks the tool stack back.
        bool WantsClicks() const override { return true; }

        // Shift+drag adds tools in either stage.
        bool WantsMarquee() const override { return true; }

        // This command has no numeric fields.
        int NumericFields( const kiwiNumField_t **out ) const override
        { (void)out; return 0; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        // The tool must never be picked out of the geometry the verb is about to
        // carve — that is what makes "click ANOTHER solid" unambiguous.
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            // Advertise the additive grammar and live-stage Esc behavior explicitly.
            static const kiwiPrompt_t s_pick[] = {
                { "LMB",       "Pick the other solid" },
                { "Shift+LMB", "Add another solid" },
                { "Shift+Drag","Box-add solids" },
            };
            static const kiwiPrompt_t s_live[] = {
                { "Q",         "Difference / Union" },
                { "Shift+LMB", "Add another solid" },
                { "Shift+Drag","Box-add solids" },
                { "Esc",       "Drop the last solid" },
            };
            if ( m_stage == KBOOL_PICK_TOOL )
            {
                *out = s_pick;
                return (int)( sizeof( s_pick ) / sizeof( s_pick[0] ) );
            }
            *out = s_live;
            return (int)( sizeof( s_live ) / sizeof( s_live[0] ) );
        }

        bool Begin() override
        {
            Reset();

            // Patches can enter selected_brushes in face mode but have no volume;
            // report each skipped class rather than silently dropping the selection.
            int skippedPatches = 0;
            for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            {
                if ( Usable( b ) )
                    m_targets.push_back( b );
                else if ( b->patch )
                    ++skippedPatches;
            }
            if ( skippedPatches > 0 )
                Sys_Printf( "Boolean: %i curve/patch(es) in the selection were "
                            "skipped — a patch is a render surface with no volume, "
                            "so it can neither be carved nor carve.\n",
                            skippedPatches );
            if ( m_targets.empty() )
            {
                Sys_Printf( "Boolean: select at least one solid first (patches and "
                            "fixed-size entities cannot be booleaned).\n" );
                return false;
            }

            m_stage = KBOOL_PICK_TOOL;
            UpdateHud();
            Sys_Printf( "Boolean: click the OTHER solid to use as the tool — "
                        "Shift+click or Shift+drag a box to add more tools "
                        "(Esc cancels).\n" );
            return true;
        }

        // Recast the framework pick in object mode so component selection modes
        // cannot hide solids. Keep hover live in both stages, admit existing tools
        // for Shift+click removal, and always exclude targets.
        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick; (void)snap;

            selbrush_t *was = m_hover;
            m_hover = 0;
            m_hoverIsPatch = false;             // recomputed from this ray

            int   x, y;
            ray_t ray;
            if ( KiwiCmd_LastCursor( &x, &y ) && Pick_RayFromImagePos( x, y, &ray ) )
            {
                const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT, PICKF_EXCLUDE_SELECTED );
                if ( hit.valid && hit.item.brush && Sel_BrushLive( hit.item.brush )
                  && Usable( hit.item.brush ) && !IsTarget( hit.item.brush ) )
                    m_hover = hit.item.brush;
                // Preserve a patch-specific refusal instead of conflating it with empty space.
                else if ( hit.valid && hit.item.brush && Sel_BrushLive( hit.item.brush )
                       && hit.item.brush->patch )
                    m_hoverIsPatch = true;
            }
            if ( m_hover != was )
                g_nUpdateBits |= 1;
            UpdateHud();
        }

        // Plain click replaces the tool set; Shift+click toggles one member. The
        // first tool selects difference, but later picks preserve an explicit Q toggle.
        // Click never commits; RMB or Enter does.
        bool Click() override
        {
            if ( !m_hover )
            {
                if ( m_hoverIsPatch )           // name the specific refusal
                    Sys_Printf( "Boolean: that is a curve/patch — a patch is a render "
                                "surface with no volume and cannot cut.  Use the "
                                "caulk brush inside it as the tool.\n" );
                else
                    Sys_Printf( "Boolean: that is not a usable solid — click another "
                                "one, or press Esc.\n" );
                return true;
            }

            const bool add = KiwiCmd_LastShift();
            if ( add )
            {
                for ( size_t i = 0; i < m_tools.size(); ++i )
                {
                    if ( m_tools[i] != m_hover )
                        continue;
                    m_tools.erase( m_tools.begin() + i );
                    if ( m_tools.empty() )
                        m_stage = KBOOL_PICK_TOOL;
                    UpdateHud();
                    Sys_Printf( "Boolean: solid removed — %i tool(s).\n",
                                (int)m_tools.size() );
                    g_nUpdateBits = -1;
                    return true;
                }
                m_tools.push_back( m_hover );
            }
            else
            {
                m_tools.clear();
                m_tools.push_back( m_hover );
            }

            if ( m_stage == KBOOL_PICK_TOOL )
            {
                m_op    = KBOOL_DIFFERENCE;
                m_stage = KBOOL_LIVE;
                Sys_Printf( "Boolean: DIFFERENCE — Shift+click adds more tools, Q "
                            "switches to union, RMB / Enter applies, Esc drops the "
                            "last tool.\n" );
            }
            else
            {
                Sys_Printf( "Boolean: %i tool(s).\n", (int)m_tools.size() );
            }
            UpdateHud();
            g_nUpdateBits = -1;
            return true;
        }

        // The Shift-only marquee adds usable non-targets, suppresses duplicates,
        // and selects difference only when it first enters the live stage.
        void Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift ) override
        {
            (void)shift;                        // Shift-only rung: it is always true

            // Bound a whole-map marquee: every tool costs an N-plane carve per target.
            const int   KBOOL_MARQUEE_MAX = 256;
            selbrush_t *found[KBOOL_MARQUEE_MAX];
            const int n = KiwiBox_CollectBrushes( x0, y0, x1, y1, crossing,
                                                  found, KBOOL_MARQUEE_MAX );

            int added = 0;
            for ( int i = 0; i < n; ++i )
            {
                selbrush_t *b = found[i];
                if ( !Sel_BrushLive( b ) || !Usable( b ) || IsTarget( b ) || IsTool( b ) )
                    continue;
                m_tools.push_back( b );
                ++added;
            }

            if ( !added )
            {
                Sys_Printf( "Boolean: nothing usable in that box — %i tool(s).\n",
                            (int)m_tools.size() );
                return;
            }

            if ( m_stage == KBOOL_PICK_TOOL )
            {
                m_op    = KBOOL_DIFFERENCE;
                m_stage = KBOOL_LIVE;
            }
            UpdateHud();
            Sys_Printf( "Boolean: %i solid(s) added — %i tool(s).\n",
                        added, (int)m_tools.size() );
            g_nUpdateBits = -1;
        }

        // Q toggles live operation; Esc pops tools before cancellation; Enter needs a tool.
        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            if ( vk == 0x51 && m_stage == KBOOL_LIVE )       // 'Q'
            {
                m_op = ( m_op == KBOOL_DIFFERENCE ) ? KBOOL_UNION : KBOOL_DIFFERENCE;
                UpdateHud();
                Sys_Printf( "Boolean: %s.\n", ( m_op == KBOOL_UNION )
                            ? "UNION — the solids become one (must stay convex)"
                            : "DIFFERENCE — the tools carve the selection" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x1B && m_stage == KBOOL_LIVE )       // VK_ESCAPE
            {
                // Pop the most recent tool; after the last, one more Esc cancels.
                if ( m_tools.size() > 1 )
                {
                    m_tools.pop_back();
                    UpdateHud();
                    Sys_Printf( "Boolean: last solid dropped — %i tool(s).\n",
                                (int)m_tools.size() );
                    g_nUpdateBits = -1;
                    return true;
                }
                m_stage = KBOOL_PICK_TOOL;
                m_tools.clear();
                m_hover = 0;
                // Defensive no-op while WantsClicks prevents this command from parking.
                KiwiCmd_Resume();
                UpdateHud();
                Sys_Printf( "Boolean: tool dropped — click another solid (Esc again "
                            "cancels).\n" );
                g_nUpdateBits = -1;
                return true;
            }
            if ( vk == 0x0D && m_stage == KBOOL_PICK_TOOL )  // VK_RETURN
            {
                Sys_Printf( "Boolean: click the other solid first.\n" );
                return true;
            }
            return false;
        }

        // Difference fills tools red and only outlines already selected targets;
        // union fills both roles green. Fill and line budgets are whole-frame.
        // Spend newest-first, reserve the final hover outline, and report degradation
        // once so the most recent operand and next click target remain visible.
        void DrawWorld() override
        {
            const bool hoverOn = ( m_hover && Sel_BrushLive( m_hover )
                                && !IsTool( m_hover ) && m_hover->def );
            const int  reserve = hoverOn ? OutlineCost( m_hover->def ) : 0;

            int fillDropped = 0;
            int lineDropped = 0;

            if ( m_stage == KBOOL_LIVE && !m_tools.empty() )
            {
                int         filled = 0;
                const bool  uni  = ( m_op == KBOOL_UNION );
                const float *fill = uni ? KBOOL_UNION_RGBA : KBOOL_DIFF_RGBA;
                const float *line = uni ? KBOOL_UNION_LINE : KBOOL_DIFF_LINE;

                // Reverse walk spends preview budget on the newest operands first.
                for ( size_t i = m_tools.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_tools[i] ) )
                        continue;
                    if ( filled >= KBOOL_MAX_FILL ) { ++fillDropped; continue; }
                    FillBrush( m_tools[i]->def, fill );
                    ++filled;
                }
                if ( uni )
                {
                    for ( size_t i = m_targets.size(); i-- > 0; )
                    {
                        if ( !Sel_BrushLive( m_targets[i] ) )
                            continue;
                        if ( filled >= KBOOL_MAX_FILL ) { ++fillDropped; continue; }
                        FillBrush( m_targets[i]->def, fill );
                        ++filled;
                    }
                }

                KiwiLines_Color( line[0], line[1], line[2] );
                for ( size_t i = m_tools.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_tools[i] ) || !m_tools[i]->def )
                        continue;
                    // Stop before the reserved hover budget so drops remain countable.
                    if ( KiwiLines_Remaining() - reserve < OutlineCost( m_tools[i]->def ) )
                        { ++lineDropped; continue; }
                    OutlineBrush( m_tools[i]->def );
                }
                for ( size_t i = m_targets.size(); i-- > 0; )
                {
                    if ( !Sel_BrushLive( m_targets[i] ) || !m_targets[i]->def )
                        continue;
                    if ( KiwiLines_Remaining() - reserve < OutlineCost( m_targets[i]->def ) )
                        { ++lineDropped; continue; }
                    OutlineBrush( m_targets[i]->def );
                }
            }

            if ( hoverOn )
            {
                KiwiLines_Color( KBOOL_HOT_LINE[0], KBOOL_HOT_LINE[1], KBOOL_HOT_LINE[2] );
                OutlineBrush( m_hover->def );
            }

            NoteDegraded( fillDropped, lineDropped );
        }

        // Request the measured current outline cost; the framework clamps it to
        // KCMD_LINE_BUDGET_MAX while retaining the normal minimum budget.
        int LineBudget() const override
        {
            int n = KBOOL_LINE_HEADROOM;
            if ( m_stage == KBOOL_LIVE )
            {
                for ( size_t i = 0; i < m_tools.size(); ++i )
                    if ( Sel_BrushLive( m_tools[i] ) )
                        n += OutlineCost( m_tools[i]->def );
                for ( size_t i = 0; i < m_targets.size(); ++i )
                    if ( Sel_BrushLive( m_targets[i] ) )
                        n += OutlineCost( m_targets[i]->def );
            }
            if ( m_hover && Sel_BrushLive( m_hover ) )
                n += OutlineCost( m_hover->def );
            return n;
        }

        void Commit() override
        {
            if ( m_stage != KBOOL_LIVE || m_tools.empty() )
            {
                Sys_Printf( "Boolean: no tool solid was picked — nothing was done.\n" );
                Reset();
                return;
            }
            if ( m_op == KBOOL_UNION )
                DoUnion();
            else
                DoDifference();
            Reset();
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Preview is draw-only; no geometry or undo bracket exists before Commit.
            Reset();
            g_nUpdateBits |= 1;
        }

    private:
        void Reset()
        {
            m_targets.clear();
            m_tools.clear();
            m_stage  = KBOOL_PICK_TOOL;
            m_op     = KBOOL_DIFFERENCE;
            m_hover  = 0;
            m_hud[0] = '\0';
            m_notedDegrade = false;     // one report per gesture
        }

        bool IsTarget( const selbrush_t *b ) const
        {
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( m_targets[i] == b )
                    return true;
            return false;
        }

        // Tool sets are small enough that the predictable linear scan is appropriate.
        bool IsTool( const selbrush_t *b ) const
        {
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( m_tools[i] == b )
                    return true;
            return false;
        }

        // Report preview truncation once per gesture; this runs every frame.
        void NoteDegraded( int fillDropped, int lineDropped )
        {
            if ( ( !fillDropped && !lineDropped ) || m_notedDegrade )
                return;
            m_notedDegrade = true;
            Sys_Printf( "Boolean: %i operand(s) are outlined but not tinted and %i "
                        "are not drawn at all — the preview budget is full.  The "
                        "NEWEST operands are the ones you can see; every operand in "
                        "the set is still applied on commit.\n",
                        fillDropped, lineDropped );
        }

        // Build every target carve before landing any replacement.
        void DoDifference()
        {
            struct carve_t
            {
                selbrush_t            *node;
                std::vector<brush_t *> pieces;
            };
            std::vector<carve_t> carves;
            int missed  = 0;
            int refused = 0;

            // Resolve live tool defs once so the synchronous carve uses a fixed set;
            // tools that disappeared during the gesture are ignored.
            std::vector<selbrush_t *> tools;
            std::vector<brush_t *>    toolDefs;
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( Sel_BrushLive( m_tools[i] ) && m_tools[i]->def )
                {
                    tools.push_back( m_tools[i] );
                    toolDefs.push_back( m_tools[i]->def );
                }
            if ( toolDefs.empty() )
            {
                Sys_Printf( "Boolean: no tool solid is still there — nothing was "
                            "done.\n" );
                return;
            }

            // Scope all diagnostics to this commit.
            KiwiBool_ResetCarveReport();

            for ( size_t i = 0; i < m_targets.size(); ++i )
            {
                selbrush_t *node = m_targets[i];
                if ( !Sel_BrushLive( node ) || IsTool( node ) )
                    continue;

                // Build in place so the piece vector has one owner; refusals leave it empty.
                carves.push_back( carve_t() );
                carves.back().node = node;

                const char *why = "unknown";
                const carveResult_t r = CarveTarget( node->def, &toolDefs[0],
                                                     (int)toolDefs.size(),
                                                     &carves.back().pieces, &why );
                if ( r == KBOOL_CARVED )
                    continue;

                carves.pop_back();
                if ( r == KBOOL_MISS )
                {
                    ++missed;
                    // Name non-overlap versus a near-tangent validity-gate miss.
                    Sys_Printf( "Boolean: one brush left untouched — %s.\n",
                                ( why && *why ) ? why : "the tool(s) did not reach it" );
                }
                else
                {
                    ++refused;
                    Sys_Printf( "Boolean: one brush left untouched — %s.\n",
                                why ? why : "invalid geometry" );
                }
            }

            if ( carves.empty() )
            {
                // Do not create an empty undo record when nothing changed.
                Sys_Printf( "Boolean: nothing was carved (%i brush(es) did not meet "
                            "the tool(s), %i refused).\n", missed, refused );
                return;
            }

            // Report validity-policy diagnostics for a commit that produced carves.
            if ( s_carveSliverDrops > 0 )
                Sys_Printf( "Boolean: %i fragment(s) came out below the validity gate "
                            "(zero-area / collapsed) and were dropped rather than "
                            "refusing the whole carve.\n", s_carveSliverDrops );
            if ( s_carveRefusals > 0 )
                Sys_Printf( "Boolean: %i individual cut(s) were refused and skipped — "
                            "the hole may be incomplete where those planes fell.\n",
                            s_carveRefusals );
            if ( s_carveCarried > 0 )
                Sys_Printf( "Boolean: %i running intersection(s) were outside the "
                            "validity gate and were carried on anyway (they are "
                            "intermediates and are never landed).\n", s_carveCarried );

            // Clear typed selection before freeing nodes; keep the legacy list for undo cloning.
            Sel_Clear( KiwiSel() );

            KiwiCmd_UndoBegin( "boolean difference" );

            // Picked tools are consumed but excluded from selected_brushes, so the
            // bracket head did not clone them. Cover each explicitly, entity first;
            // the target guard avoids duplicate restoration if invariants are breached.
            for ( size_t t = 0; t < tools.size(); ++t )
            {
                if ( IsTarget( tools[t] ) )
                    continue;
                entity_s *owner = tools[t]->def->owner;
                if ( owner && owner->eclass && owner->eclass->fixedsize )
                    Undo_AddEntity( (int)(intptr_t)owner );
                Undo_AddBrush( (entity_brush_s *)tools[t]->def );
            }

            int landed = 0;
            for ( size_t c = 0; c < carves.size(); ++c )
            {
                carve_t &cv = carves[c];
                // Land replacements before freeing their source so its owner never goes empty.
                for ( size_t p = 0; p < cv.pieces.size(); ++p )
                {
                    selbrush_t *inst = Brush_AddToList( cv.pieces[p], cv.node->owner );
                    if ( inst->next || inst->prev )
                        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                    Brush_AddToList2( inst );
                    ++landed;
                }
                Brush_Free( cv.node );
            }

            // Free tools only after every replacement has landed; tools may share an
            // owner with targets. Brush_Free removes only the instance, unlike
            // Select_Delete's additional empty-owner behavior.
            for ( size_t t = 0; t < tools.size(); ++t )
                Brush_Free( tools[t] );
            m_tools.clear();

            Sys_Printf( "Boolean: difference — %i brush(es) carved into %i, and %i "
                        "tool(s) were consumed (%i missed, %i refused).\n",
                        (int)carves.size(), landed, (int)tools.size(),
                        missed, refused );
        }

        // Select live targets and tools, then delegate to CSG_Merge (csg.cpp:572).
        // That handler owns geometry, undo, reporting, entity checks, and the rule
        // that a classic brush union must have a convex hull.
        void DoUnion()
        {
            int live = 0;
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) && !IsTool( m_targets[i] ) )
                    ++live;
            if ( live < 1 )
            {
                Sys_Printf( "Boolean: nothing left to union with.\n" );
                return;
            }

            int tools = 0;
            Sel_Clear( KiwiSel() );
            Select_Deselect( 1 );
            for ( size_t i = 0; i < m_targets.size(); ++i )
                if ( Sel_BrushLive( m_targets[i] ) && !IsTool( m_targets[i] ) )
                    Select_Brush( m_targets[i], 0, 0, 0 );
            for ( size_t i = 0; i < m_tools.size(); ++i )
                if ( Sel_BrushLive( m_tools[i] ) )
                {
                    Select_Brush( m_tools[i], 0, 0, 0 );
                    ++tools;
                }

            Sys_Printf( "Boolean: union of %i brush(es) + %i tool(s)...\n",
                        live, tools );
            Radiant_ExecCommand( (unsigned int)KBOOL_ID_MERGE );
        }

        void UpdateHud()
        {
            if ( m_stage == KBOOL_PICK_TOOL )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "boolean  %i solid(s) selected  %s", (int)m_targets.size(),
                           m_hover ? "click this solid to use as the tool"
                                   : "click the other solid" );
            }
            else
            {
                // Counts make successful additive picks visible even without motion.
                _snprintf( m_hud, sizeof( m_hud ),
                           "boolean  %s  %i target(s) + %i tool(s)  (Q switches)",
                           ( m_op == KBOOL_UNION ) ? "UNION" : "DIFFERENCE",
                           (int)m_targets.size(), (int)m_tools.size() );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        enum stage_t { KBOOL_PICK_TOOL = 0, KBOOL_LIVE };
        enum op_t    { KBOOL_DIFFERENCE = 0, KBOOL_UNION };

        std::vector<selbrush_t *> m_targets;
        // Pick order controls Esc and fragment topology, though not the difference volume.
        std::vector<selbrush_t *> m_tools;
        selbrush_t *m_hover = 0;
        // Latch a patch-specific refusal for Click.
        bool        m_hoverIsPatch = false;
        stage_t     m_stage = KBOOL_PICK_TOOL;
        op_t        m_op    = KBOOL_DIFFERENCE;
        // One preview-degradation report per gesture.
        bool        m_notedDegrade = false;
        char        m_hud[160] = { 0 };
    };

    KiwiBooleanCommand s_boolean;
}

// Palette predicate.
bool KiwiBool_CanBoolean()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( Usable( b ) )
            return true;
    return false;
}

// Registration and lookup.
void KiwiBool_RegisterCommands()
{
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    Radiant_RegisterCommand( "KiwiBoolean", 0, 0, KIWI_CMD_BOOLEAN );
}

KiwiEditorCommand *KiwiBool_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_BOOLEAN )
        return &s_boolean;
    return 0;
}
