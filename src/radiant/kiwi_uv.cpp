#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_uv.cpp — RADIANT_UX_DESIGN §26 (Textures/UV) Phase 6 v1.
// See kiwi_uv.h for the scope ruling, the per-core inventory (what each core
// operates on, its units and its undo behaviour), the undo ruling that decides
// which command is live and which is deferred, and the cancel semantics.
//
// NEW code over the ported texture cores.  Nothing here re-implements a texdef
// mutation: every write goes through Brush_ShiftTexture / Brush_ScaleTexture /
// Brush_RotateTexture / Texture_SetTexture.  This file owns the input mapping,
// the constraint state, the HUD and the undo bracket for the one core that does
// not bring its own.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_uv.h"
#include "kiwi_command.h"
#include "kiwi_hints.h"     // ROUND Z, ITEM 5 — the shared bottom band
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

// ── ported entry points (each verified against its definition) ──────────────
extern int   Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern int   g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)

extern void  Brush_ShiftTexture ( float a1, float a2 );                 // select.cpp:3085  0x491F20
extern void  Brush_ScaleTexture ( int   a1, int   a2 );                 // select.cpp:3258  0x492650
extern void  Brush_RotateTexture( int   a1 );                           // select.cpp:3340  0x4929F0

extern char  Texture_SetTexture( const int *a1, MaterialDef *a2 );      // texwnd.cpp:2156  0x45BE50
extern char  Radiant_PatchGetTexdef( patchMesh_t *patch,
                                     texdef_sub_t *texdef );            // brush.cpp:2231   0x44B620
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );    // materialdef.cpp:159  0x431640
namespace LayerMat       { int  GetCurrentLayer( MaterialDef *mtlDef ); }  // materialdef.cpp:252  0x431B30
namespace SurfaceInspector { void UpdateSurfaceDialog(); }                 // surfacedlg.cpp:545   0x458590
extern void  UpdatePatchInspector();                                    // patchdialog.cpp:500  0x436DB0
// KIWI-UX (ROUND AZ, ITEM 1): the round-AK patch re-naturalize, applied to the
// pick-texture funnel as well as the browser one.  See PickTexture for why.  At
// FILE scope on purpose (round AI's MSVC namespace-mangling link error) — the
// same reason texwnd.cpp:1129 declares it here rather than in its caller.
extern void  Patch_KiwiReNaturalizeSelected();                          // pmesh.cpp:1641 (ROUND AK)

extern float grid_sizes[];                                              // engine_stubs.cpp:771  0x6DDE5C

extern bool  ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );  // imgui_shell.cpp:290
extern bool  Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1340
extern void  Radiant_ExecCommand( unsigned int cmdId );                 // mainfrm.cpp:4083

namespace
{
    const int   KUV_GRID_COUNT = 11;    // grid_sizes[11] (engine_stubs.cpp:771)
    const float KUV_EPS        = 1.0e-3f;

    enum uvcon_t { UVC_FREE = 0, UVC_S, UVC_T };

    // ── shakeout E: the numeric FIELD tables (kiwi_command.h NumericFields) ──
    // STATIC storage — the numeric layer copies the structs but never the labels.
    // The KIND matters here more than anywhere else: without it the bubble would
    // print a texture-unit shift and a degree count as "12 in".  It changes only
    // FORMATTING — every one of these commands still receives the same value it
    // did before, and still undoes the units conversion with NumRaw (kiwi_uv.cpp's
    // own note, unchanged).
    const kiwiNumField_t KUV_FIELDS_SHIFT [1] = { { "shift", KNUM_FACTOR, false } };
    const kiwiNumField_t KUV_FIELDS_ROTATE[1] = { { "angle", KNUM_ANGLE,  false } };
    const kiwiNumField_t KUV_FIELDS_SCALE [1] = { { "steps", KNUM_COUNT,  false } };

    // ── the camera cursor, LATCHED for the instant Pick Texture command ──────
    // ImGuiShell_CameraPaintCursor is a "cursor is over the camera image RIGHT
    // NOW" query (imgui_shell.cpp:202 returns false the moment it leaves), and
    // Pick Texture is invoked from a menu button or the palette — i.e. with the
    // cursor over ImGui, never over the 3D view.  So the last position the cursor
    // HAD over the camera is sampled once per frame from the overlay draw (the one
    // place in this file guaranteed to run every frame) and that is what the pick
    // ray is built from.  KiwiCmd_LastCursor cannot serve here: it is only valid
    // while a modal command is active (kiwi_command.h).
    bool s_camCursorValid = false;
    int  s_camCursorX = 0, s_camCursorY = 0;

    void SampleCameraCursor()
    {
        int x, y;
        if ( ImGuiShell_CameraPaintCursor( &x, &y, 0, 0 ) )
        {
            s_camCursorValid = true;
            s_camCursorX = x;
            s_camCursorY = y;
        }
    }

    bool CameraCursor( int *x, int *y )
    {
        if ( ImGuiShell_CameraPaintCursor( x, y, 0, 0 ) )
            return true;
        if ( !s_camCursorValid )
            return false;
        *x = s_camCursorX;
        *y = s_camCursorY;
        return true;
    }

    // ── selection reads (exactly what the cores themselves test) ────────────
    bool AnyWholeBrushSelected()
    {
        return selected_brushes.next != &selected_brushes;
    }

    int SelectedFaceCount()
    {
        return g_SelectedFaces.GetSize();
    }

    // The classic grid step the RMB+Alt texture drag quantises to
    // (camwnd.cpp:2686 uses grid_sizes[g_qeglobals.d_gridsize] directly).
    float ClassicGridStep()
    {
        int gi = g_qeglobals.d_gridsize;
        if ( gi < 0 )                gi = 0;
        if ( gi >= KUV_GRID_COUNT )  gi = KUV_GRID_COUNT - 1;
        const float g = grid_sizes[gi];
        return ( g >= 1.0f ) ? g : 1.0f;
    }

    float QuantizeTo( float v, float q )
    {
        if ( !( q > 0.0f ) )
            return v;
        return q * floorf( v / q + 0.5f );
    }

    float ClampF( float v, float lo, float hi )
    {
        return ( v < lo ) ? lo : ( v > hi ) ? hi : v;
    }

    int ClampI( int v, int lo, int hi )
    {
        return ( v < lo ) ? lo : ( v > hi ) ? hi : v;
    }

    // KIWI-UX (CLEANUP, UndoCoverBrush): the local copy is gone — this was one
    // of five verbatim bodies.  It is KiwiCmd_UndoCoverBrush (kiwi_command.h)
    // now, beside the bracket whose blind spot it exists to fill.

    // Every brush named by g_SelectedFaces — the set KiwiCmd_UndoBegin's
    // Undo_AddBrushList(&selected_brushes) provably misses (kiwi_selection.h
    // DESIGN NOTE 2).  Undo_AddBrush self-dedupes (undo.cpp:485), so a brush with
    // several selected faces costs one clone.
    void UndoCoverSelectedFaces()
    {
        const int n = SelectedFaceCount();
        for ( int i = 0; i < n; ++i )
            KiwiCmd_UndoCoverBrush( g_SelectedFaces.GetAt( i ).brush );
    }

    // ── KIWI-UX (ROUND BD) ──────────────────────────────────────────────────
    // The per-face texdef accessor used to live HERE, in the anonymous namespace.
    // Round BD's UV editor window needs the same slot and must not carry a second
    // spelling of a two-level layer walk, so it moved to FILE SCOPE as
    // `KiwiUv_FaceTexdef` (below the namespace, declared in kiwi_uv.h).  Nothing
    // inside this namespace called it — its one user, KiwiUv_DrawReadout, is at
    // file scope already — so the move is a rename and a scope change, nothing else.
    // The header-declared definition is deliberately NOT in an anonymous namespace:
    // that is the MSVC mangling LNK2019 the round-AI note on kiwi_uv.cpp:52 records.
    // ── KIWI-UX end ─────────────────────────────────────────────────────────

    // ═════════════════════════════════════════════════════════════════════════
    //  Shared modal base: S/T constraint, numeric state, HUD.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiUvBase : public KiwiEditorCommand
    {
    public:
        // A texture command drags no geometry, so it has nothing to exclude from
        // the pick — the framework's default flags are right.
        unsigned PickFlags() const override { return PICKF_NONE; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        bool CanExecute() override { return KiwiUv_CanEdit(); }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            // LATE LATCH: these commands are reachable from a menu button, so Begin
            // often runs with the cursor over ImGui and KiwiCmd_Start's seed
            // (kiwi_command.cpp:439, ImGuiShell_CameraPaintCursor) fails.  The first
            // move over the camera image becomes the gesture's origin instead, which
            // makes the drag start from zero exactly where the user picked it up.
            if ( !m_haveStart )
                m_haveStart = CursorPixels( &m_startX, &m_startY );
            Recompute();
            g_nUpdateBits |= 1;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)mods;
            uvcon_t want = UVC_FREE;
            if      ( vk == 0x58 ) want = UVC_S;      // X → S
            else if ( vk == 0x59 ) want = UVC_T;      // Y → T
            else                   return false;

            // Second press of the same key releases (kiwi_transform.h's v1 rule).
            m_con = ( m_con == want ) ? UVC_FREE : want;
            Recompute();
            return true;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

    protected:
        virtual void Recompute() = 0;

        bool CursorPixels( int *x, int *y ) const
        {
            return KiwiCmd_LastCursor( x, y );
        }

        // Texture units, degrees and size steps are NOT lengths: undo the numeric
        // layer's inches→world conversion to recover exactly what was typed
        // (kiwi_transform.h says the same about R and S).
        float NumRaw() const { return Units_ToDisplay( m_numWorld ); }

        // KIWI-UX (CLEANUP, SnapActive): one spelling, KiwiSnap_Active
        // (kiwi_snap.h) — this member is the accessor over THIS command's result.
        bool SnapActive() const { return KiwiSnap_Active( m_snap ); }

        void SetHud( const char *fmt, ... )
        {
            va_list ap;
            va_start( ap, fmt );
            _vsnprintf( m_hud, sizeof( m_hud ), fmt, ap );
            va_end( ap );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        const char *TargetText() const
        {
            return ( m_con == UVC_S ) ? "S only" : ( m_con == UVC_T ) ? "T only" : "S+T";
        }

        // What the selection the cores will walk actually is, for the HUD.
        const char *ScopeText() const
        {
            const int f = SelectedFaceCount();
            const bool b = AnyWholeBrushSelected();
            if ( f && b ) return "faces+brushes";
            if ( f )      return "faces";
            return "brushes";
        }

        // Latch the gesture's start pixel.  A gesture started with the cursor OFF
        // the camera image is still legal — it simply has no drag mapping until the
        // cursor arrives, and numeric entry alone drives it (m_haveStart false).
        void LatchStart()
        {
            m_con      = UVC_FREE;
            m_hasNum   = false;
            m_numWorld = 0.0f;
            m_hud[0]   = '\0';
            m_snap     = snap_result_t();
            m_haveStart = CursorPixels( &m_startX, &m_startY );
        }

        uvcon_t       m_con      = UVC_FREE;
        bool          m_hasNum   = false;
        float         m_numWorld = 0.0f;
        int           m_startX   = 0;
        int           m_startY   = 0;
        bool          m_haveStart = false;
        snap_result_t m_snap;
        char          m_hud[192] = { 0 };
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  Texture Shift — the one LIVE command (Brush_ShiftTexture brackets nothing,
    //  select.cpp:3058, so this file owns exactly one bracket per gesture).
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiTexShiftCommand : public KiwiUvBase
    {
    public:
        const char *Name() const override { return "Texture Shift"; }
        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KUV_FIELDS_SHIFT; return 1; }

        bool Begin() override
        {
            m_s = m_t = 0.0f;
            m_appliedS = m_appliedT = 0.0f;
            m_undoOpen = false;
            m_covered  = false;
            LatchStart();

            if ( !KiwiUv_CanEdit() )
            {
                Sys_Printf( "Texture Shift: select brushes or faces first.\n" );
                return false;
            }
            UpdateHud();
            return true;
        }

        void Commit() override
        {
            // Everything the gesture asked for is already applied (the numeric
            // path applied its exact correction the moment it was typed).
            m_undoOpen = false;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Inverse of the applied total; the framework's KiwiCmd_UndoCancel then
            // rolls the bracket back, which is the EXACT restore (kiwi_uv.h "CANCEL
            // SEMANTICS").  Both legs run, same as kiwi_transform's R.
            Apply( -m_appliedS, -m_appliedT );
            m_undoOpen = false;
            g_nUpdateBits = -1;
        }

    protected:
        void Recompute() override
        {
            float s = 0.0f, t = 0.0f;
            int x, y;
            if ( m_haveStart && CursorPixels( &x, &y ) )
            {
                // 1 px = 1 texture unit; right/down positive — the same sign the
                // classic RMB+Alt drag accumulates with (camwnd.cpp:2676-2686).
                s = (float)( x - m_startX );
                t = (float)( y - m_startY );
            }
            if ( m_con == UVC_S ) t = 0.0f;
            if ( m_con == UVC_T ) s = 0.0f;

            // Quantise to the CLASSIC grid step, or to 1 unit while CTRL suppresses
            // snapping.  Either way the result is a whole number of texture units,
            // which is what the core's brush pass can actually see: it truncates
            // with `(float)(int)a1` (select.cpp:3078).
            const float q = SnapActive() ? ClassicGridStep() : 1.0f;
            s = QuantizeTo( s, q );
            t = QuantizeTo( t, q );

            if ( m_hasNum )
            {
                // Rounded to whole units for the same truncation reason; the HUD
                // shows the value that will actually land.
                const float v = floorf( ClampF( NumRaw(), -KUV_MAX_SHIFT, KUV_MAX_SHIFT ) + 0.5f );
                if ( m_con == UVC_T ) { s = 0.0f; t = v; }
                else                  { s = v;    t = 0.0f; }
            }
            else
            {
                s = ClampF( s, -KUV_MAX_SHIFT, KUV_MAX_SHIFT );
                t = ClampF( t, -KUV_MAX_SHIFT, KUV_MAX_SHIFT );
            }

            m_s = s;
            m_t = t;
            Apply( m_s - m_appliedS, m_t - m_appliedT );
            UpdateHud();
        }

    private:
        void Apply( float ds, float dt )
        {
            if ( fabsf( ds ) < KUV_EPS && fabsf( dt ) < KUV_EPS )
                return;
            if ( !KiwiUv_CanEdit() )
                return;

            if ( !m_undoOpen )
            {
                KiwiCmd_UndoBegin( "shift texture" );   // string LITERAL — stored by pointer
                m_undoOpen = true;
            }
            if ( !m_covered )
            {
                UndoCoverSelectedFaces();               // BEFORE the first mutation
                m_covered = true;
            }

            Brush_ShiftTexture( ds, dt );
            m_appliedS += ds;
            m_appliedT += dt;
            g_nUpdateBits = -1;
        }

        void UpdateHud()
        {
            SetHud( "%s  %s  S %+.0f  T %+.0f  step %g",
                    ScopeText(), TargetText(), (double)m_s, (double)m_t,
                    (double)( SnapActive() ? ClassicGridStep() : 1.0f ) );
        }

        float m_s = 0.0f, m_t = 0.0f;
        float m_appliedS = 0.0f, m_appliedT = 0.0f;
        bool  m_undoOpen = false;
        bool  m_covered  = false;
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  Texture Rotate — DEFERRED.  Brush_RotateTexture opens its OWN bracket
    //  (select.cpp:3320-3322 -> Undo_ClearRedo / Undo_GeneralStart / Undo_AddBrushList), so the gesture mutates nothing until Commit, which
    //  makes exactly one call and inherits exactly one undo record.  See the
    //  "UNDO RULING" note in kiwi_uv.h for why live driving is not an option.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiTexRotateCommand : public KiwiUvBase
    {
    public:
        const char *Name() const override { return "Texture Rotate"; }
        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KUV_FIELDS_ROTATE; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_deg;
            return true;
        }

        bool Begin() override
        {
            m_deg = 0.0f;
            LatchStart();
            if ( !KiwiUv_CanEdit() )
            {
                Sys_Printf( "Texture Rotate: select brushes or faces first.\n" );
                return false;
            }
            UpdateHud();
            return true;
        }

        bool KeyDown( int vk, unsigned mods ) override
        {
            (void)vk; (void)mods;
            return false;         // rotation has no S/T axis to constrain
        }

        void Commit() override
        {
            const int deg = Degrees();
            if ( deg && KiwiUv_CanEdit() )
                Brush_RotateTexture( deg );     // core opens/closes "rotate texture"
            m_deg = 0.0f;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // Nothing was applied — exact by construction.
            m_deg = 0.0f;
            g_nUpdateBits |= 1;
        }

    protected:
        void Recompute() override
        {
            float deg = m_deg;
            int x, y;
            if ( m_haveStart && CursorPixels( &x, &y ) )
                deg = (float)( x - m_startX ) * KUV_DEG_PER_PIXEL;

            if ( m_hasNum )
                deg = NumRaw();                                   // exact degrees
            else if ( SnapActive() )
                deg = floorf( deg / KUV_ANGLE_STEP + 0.5f ) * KUV_ANGLE_STEP;

            m_deg = ClampF( deg, -100000.0f, 100000.0f );
            UpdateHud();
        }

    private:
        // Brush_RotateTexture takes an INT and the texdef itself is rounded to a
        // whole degree ((int)(v + 2^-30) % 360, select.cpp:3334), so the fractional
        // part of the drag is accumulated in m_deg and rounded once, here.
        int Degrees() const
        {
            int d = (int)floorf( m_deg + 0.5f );
            d %= 360;
            return d;
        }

        void UpdateHud()
        {
            SetHud( "%s  %+d deg (whole degrees; applied on commit)",
                    ScopeText(), Degrees() );
        }

        float m_deg = 0.0f;
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  Texture Scale — DEFERRED, same reason as Rotate (Brush_ScaleTexture opens
    //  its own "scale texture" bracket, select.cpp:3237-3239).
    //  NOTE THE SEMANTICS: the core ADDS its arguments to texdef size[0]/size[1]
    //  (select.cpp:3249) — it is a size STEP, not a multiplicative factor.  That is
    //  the same operation the classic texture bar's scale spins perform.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiTexScaleCommand : public KiwiUvBase
    {
    public:
        const char *Name() const override { return "Texture Scale"; }
        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KUV_FIELDS_SCALE; return 1; }

        bool Begin() override
        {
            m_sSteps = m_tSteps = 0;
            LatchStart();
            if ( !KiwiUv_CanEdit() )
            {
                Sys_Printf( "Texture Scale: select brushes or faces first.\n" );
                return false;
            }
            UpdateHud();
            return true;
        }

        void Commit() override
        {
            if ( ( m_sSteps || m_tSteps ) && KiwiUv_CanEdit() )
                Brush_ScaleTexture( m_sSteps, m_tSteps );   // core owns "scale texture"
            m_sSteps = m_tSteps = 0;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            m_sSteps = m_tSteps = 0;
            g_nUpdateBits |= 1;
        }

    protected:
        void Recompute() override
        {
            float s = 0.0f, t = 0.0f;
            int x, y;
            if ( m_haveStart && CursorPixels( &x, &y ) )
            {
                s = (float)( x - m_startX ) / KUV_PX_PER_SIZE_STEP;
                t = (float)( y - m_startY ) / KUV_PX_PER_SIZE_STEP;
            }
            if ( m_con == UVC_S ) t = 0.0f;
            if ( m_con == UVC_T ) s = 0.0f;

            int si = (int)floorf( s + 0.5f );
            int ti = (int)floorf( t + 0.5f );

            if ( m_hasNum )
            {
                const int v = (int)floorf( NumRaw() + 0.5f );
                if ( m_con == UVC_T ) { si = 0; ti = v; }
                else                  { si = v; ti = 0; }
            }

            m_sSteps = ClampI( si, -KUV_MAX_SIZE_STEP, KUV_MAX_SIZE_STEP );
            m_tSteps = ClampI( ti, -KUV_MAX_SIZE_STEP, KUV_MAX_SIZE_STEP );
            UpdateHud();
        }

    private:
        void UpdateHud()
        {
            SetHud( "%s  %s  size %+d, %+d (added to texdef size; applied on commit)",
                    ScopeText(), TargetText(), m_sSteps, m_tSteps );
        }

        int m_sSteps = 0, m_tSteps = 0;
    };

    KiwiTexShiftCommand  s_shift;
    KiwiTexRotateCommand s_rotate;
    KiwiTexScaleCommand  s_scale;

    // ═════════════════════════════════════════════════════════════════════════
    //  Pick Texture (instant) — the modern route to the classic middle-button
    //  texture pick (drag.cpp:694-737, Drag_Begin's BRANCH 1).
    //
    //  DELIBERATE OMISSION: the classic branch also overwrites the new-brush
    //  vertical template (g_qeglobals.d_new_brush_bottom_* / _top_* from the hit
    //  brush's bounds, drag.cpp:702-708).  That is a side effect of the classic
    //  MMB gesture, not of "pick this texture", and silently changing the
    //  next-brush size from a texture pick is not behaviour worth reproducing in a
    //  new command.  Everything else — the patch texdef extraction, the
    //  Texture_SetTexture call, the two inspector refreshes — is the same sequence
    //  in the same order.
    //
    //  SIDE EFFECT THAT IS KEPT (and is the classic behaviour): Texture_SetTexture
    //  ends in Brush_SetTexture( a2, 1 ) (texwnd.cpp:2009), which APPLIES the
    //  picked material to the CURRENT SELECTION under its own undo bracket
    //  ("set face textures" / "set brush textures", select.cpp:1787).  Picking with
    //  something selected therefore also retextures it.  The menu tooltip says so.
    // ═════════════════════════════════════════════════════════════════════════
    bool PickTexture()
    {
        int cx, cy;
        if ( !CameraCursor( &cx, &cy ) )
        {
            Sys_Printf( "Pick Texture: move the cursor over the 3D view first.\n" );
            return true;
        }

        ray_t ray;
        if ( !Pick_RayFromImagePos( cx, cy, &ray ) )
        {
            Sys_Printf( "Pick Texture: the 3D view has no usable viewport yet.\n" );
            return true;
        }

        // Always a FACE pick, whatever the current mode mask says — "the texture
        // under the cursor" is a face-level notion (the §25 double-click makes the
        // mirror-image argument for OBJECT, kiwi_selext.cpp:434).
        const pick_result_t hit = Pick( ray, SEL_MASK_FACE );
        if ( !hit.valid || !hit.item.brush || !Sel_BrushLive( hit.item.brush ) )
        {
            Sys_Printf( "Did not select a texture\n" );      // the classic message
            return true;
        }

        selbrush_t *inst = hit.item.brush;
        brush_t    *def  = inst->def;
        const int   fi   = hit.item.faceIndex;
        if ( !def || !def->faces || fi < 0 || fi >= def->faceCount )
        {
            Sys_Printf( "Did not select a texture\n" );
            return true;
        }

        const int layer = g_qeglobals.current_edit_layer;
        if ( layer < 0 || layer > 3 )
            return true;

        face_t      *face = &def->faces[fi];
        MaterialDef *md   = &face->mtldef[layer];
        if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
        {
            Sys_Printf( "Pick Texture: that face has no material on layer %d.\n", layer );
            return true;
        }

        // Patch pick: recover a planar texdef from the control grid into the face's
        // own texdef slot first, exactly as drag.cpp:718-722 does.  The return value
        // is discarded there too — 0x44B620 has no success path (brush.cpp:2080).
        if ( inst->patch && def->patch )
            Radiant_PatchGetTexdef( def->patch, &md->mat_texDef + LayerMat::GetCurrentLayer( md ) );

        const int *patchDef = inst->patch ? (const int *)inst->patch->def : 0;
        Texture_SetTexture( patchDef, md );

        // ── KIWI-UX (ROUND AZ, ITEM 1): THE ROUND-AK FENCE, ON THIS FUNNEL TOO ──
        // `Texture_SetTexture` ends in `Brush_SetTexture` (texwnd.cpp:2123), which
        // reaches a patch through `sub_476ED0` with a5 == 1 — that branch swaps the
        // two material POINTERS and returns (brush.cpp:2852-2866) without re-laying
        // `ctrl[][].texCoord`, so the patch comes out STRETCHED by
        // newWidth/oldWidth.  Round AK diagnosed that and hung the re-naturalize on
        // the texture-browser funnel (texwnd.cpp:1187); this funnel applies the same
        // material to the same selection through the same call and never got it, so
        // a patch retextured by MMB pick stretched where one retextured by a
        // thumbnail click did not.
        //
        // ROUND AZ MAKES THAT VISIBLE rather than merely inconsistent: item 1 is
        // about a selection holding patches AND faces at once, so a mixed selection
        // picked onto would have textured the faces correctly and stretched the
        // patches in the same gesture.  The sweep walks the SAME `selected_brushes`
        // list the apply walked and skips every non-patch node (pmesh.cpp:1641-1650),
        // so the face half is untouched.
        //
        // FILE SCOPE, not block scope: round AI shipped a link error from a
        // block-scope extern that MSVC mangled with its enclosing namespace, which
        // is why texwnd.cpp:1129 declares this at file scope too.  See below.
        Patch_KiwiReNaturalizeSelected();

        SurfaceInspector::UpdateSurfaceDialog();
        UpdatePatchInspector();

        const char *name = (const char *)Materialdef_GetName( md );
        Sys_Printf( "Pick Texture: %s\n", name ? name : "(unnamed)" );
        g_nUpdateBits = -1;
        return true;
    }
}

// ─── KIWI-UX (ROUND BD): the per-face texdef accessor, at FILE SCOPE ─────────
// MaterialDef (qe3.h:109, 36 bytes): lyrMtl@0x00, radMtl@0x04, mat_texDef@0x08.
// texdef_sub_t (qedefs.h:178, 28 bytes): size[2]@0x00, shift[2]@0x08,
// rotate@0x10, crossterm@0x14, sample_size@0x18.
// face_t.mtldef[4] lives at face_t+0x24 (qe3.h:152, stride 36).
// The slot is `(&md->mat_texDef)[LayerMat::GetCurrentLayer(md)]` — the SAME
// 28-byte-stride sub-layer walk every core does (select.cpp:3086, and the
// "shift[7*N] == (&mat_texDef)[N].shift[0]" note there); it deliberately walks
// INTO the following MaterialDefs of the mtldef[4] block.
texdef_sub_t *KiwiUv_FaceTexdef( brush_t *def, int faceIndex, MaterialDef **outMtl )
{
    if ( !def || !def->faces || faceIndex < 0 || faceIndex >= def->faceCount )
        return 0;
    int layer = g_qeglobals.current_edit_layer;
    if ( layer < 0 || layer > 3 )
        return 0;
    MaterialDef *md = &def->faces[faceIndex].mtldef[layer];
    // MtlDef_IsValid (materialdef.cpp:53) is an L0 assert inside both
    // GetCurrentLayer and Materialdef_GetName: EXACTLY ONE of the two pointers.
    // The readout runs every frame over whatever is selected, so it checks the
    // invariant instead of tripping it.
    if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
        return 0;
    if ( outMtl )
        *outMtl = md;
    return &md->mat_texDef + LayerMat::GetCurrentLayer( md );
}
// ── KIWI-UX end ─────────────────────────────────────────────────────────────

// ─── §3 canExecute predicates ────────────────────────────────────────────────
bool KiwiUv_CanEdit()
{
    // Verbatim the guard all three cores open with (select.cpp:3062 / 3233 / 3317):
    // empty selected_brushes AND an empty g_SelectedFaces means the core returns
    // without doing anything, so the palette row is greyed instead.
    return SelectedFaceCount() > 0 || AnyWholeBrushSelected();
}

bool KiwiUv_CanPick()
{
    int x, y;
    return CameraCursor( &x, &y );
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiUv_RegisterCommands()
{
    // Unbound: these are the CLASSIC-profile bindings, and the modern profile
    // deliberately claims NO new key this phase.  KEY CANDIDATES, logged rather
    // than taken — each checked against BOTH g_radiantCommandsDefault
    // (mainfrm.cpp:981-1169) AND the modern profile (kiwi_keymap.cpp:62-99):
    //   Texture Shift   — Alt+T  (0x54 mods 2.  T mods 0/1/5 are taken:
    //                     ViewTextures / ToggleTexMoveLock / ThickenPatch)
    //   Texture Rotate  — Alt+R  (0x52 mods 2.  R mods 0/1/4 are taken classically
    //                     and the modern profile also uses mods 0/1/3)
    //   Texture Scale   — Alt+E  (0x45 mods 2.  E mods 0/1/4/5/6 are all taken)
    //   Pick Texture    — Alt+P  (0x50 mods 2.  P mods 0/5 are taken.)  Note the
    //                     CLASSIC route already exists and still works: the
    //                     middle-button pick over the 3D view (drag.cpp:695).
    // Alt+letter is safe here even though the classic camera texture drag is
    // RMB+Alt / Ctrl+RMB+Alt (camwnd.cpp:2656/2695) — those are mouse gestures
    // tested with GetAsyncKeyState, not rows in the hotkey table.
    Radiant_RegisterCommand( "KiwiTextureShift",  0, 0, KIWI_CMD_TEX_SHIFT );
    Radiant_RegisterCommand( "KiwiTextureRotate", 0, 0, KIWI_CMD_TEX_ROTATE );
    Radiant_RegisterCommand( "KiwiTextureScale",  0, 0, KIWI_CMD_TEX_SCALE );
    Radiant_RegisterCommand( "KiwiPickTexture",   0, 0, KIWI_CMD_PICK_TEXTURE );
}

KiwiEditorCommand *KiwiUv_CommandForId( int commandId )
{
    switch ( commandId )
    {
    case KIWI_CMD_TEX_SHIFT:  return &s_shift;
    case KIWI_CMD_TEX_ROTATE: return &s_rotate;
    case KIWI_CMD_TEX_SCALE:  return &s_scale;
    default:                  return 0;
    }
}

bool KiwiUv_DispatchInstant( unsigned int commandId )
{
    if ( commandId == (unsigned int)KIWI_CMD_PICK_TEXTURE )
        return PickTexture();
    return false;
}

// ─── the §26 read-only texdef readout ────────────────────────────────────────
// Drawn from KiwiVP_DrawCameraOverlay's block, next to the §13 numeric HUD.
// Read-only by ruling: the Surface Inspector stays THE numeric editor (kiwi_uv.h).
void KiwiUv_DrawReadout( float imgMinX, float imgMinY, float imgW, float imgH )
{
    (void)imgW;

    // The overlay is this file's once-per-frame tick — see SampleCameraCursor.
    SampleCameraCursor();

    const selection_t &sel = KiwiSel();
    const sel_item_t  &act = sel.active;
    if ( act.kind != SEL_FACE || !act.brush || !Sel_BrushLive( act.brush ) )
        return;

    MaterialDef  *md = 0;
    texdef_sub_t *td = KiwiUv_FaceTexdef( act.brush->def, act.faceIndex, &md );   // KIWI-UX (ROUND BD)
    if ( !td || !md )
        return;

    const char *name = (const char *)Materialdef_GetName( md );

    char line[320];
    _snprintf( line, sizeof( line ),
               "%s   shift %g, %g   size %g, %g   rot %g   layer %d",
               name ? name : "(unnamed)",
               (double)td->shift[0], (double)td->shift[1],
               (double)td->size[0],  (double)td->size[1],
               (double)td->rotate,
               g_qeglobals.current_edit_layer );
    line[sizeof( line ) - 1] = '\0';

    const ImVec2 sz = ImGui::CalcTextSize( line );
    const ImVec2 pad( 8.0f, 4.0f );
    const float  boxW = sz.x + pad.x * 2.0f;
    const float  boxH = sz.y + pad.y * 2.0f;

    // Bottom-LEFT: the §11 mode chips own top-left, so this is the free corner.
    // ── KIWI-UX (ROUND Z, ITEM 5): …WHICH IT WAS NOT ────────────────────────
    // "Bottom-centre" and "bottom-left" are a HORIZONTAL distinction and this line
    // is wide enough to reach the middle of the image, so it was landing on both
    // the numeric HUD and the command chip strip — all three at the same y.  The
    // vertical anchor now comes from the shared band (kiwi_hints.h), which stacks
    // whatever is bottom-anchored this frame.  The fallback keeps the old geometry
    // for any caller reached outside KiwiVP_DrawCameraOverlay.
    float x = imgMinX + 10.0f;
    float y = KiwiHud_BandTake( boxH, imgMinY + imgH - boxH - 12.0f );
    if ( y < imgMinY ) y = imgMinY;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled( ImVec2( x, y ), ImVec2( x + boxW, y + boxH ),
                       IM_COL32( 16, 16, 20, 210 ), 4.0f );
    dl->AddRect( ImVec2( x, y ), ImVec2( x + boxW, y + boxH ),
                 IM_COL32( 150, 130, 90, 190 ), 4.0f, 0, 1.0f );
    dl->AddText( ImVec2( x + pad.x, y + pad.y ), IM_COL32( 220, 210, 180, 255 ), line );
}

// ─── the "Textures" block in the shell's panel window ────────────────────────
void KiwiUv_MenuItems()
{
    const bool canEdit = KiwiUv_CanEdit();

    ImGui::SeparatorText( "Textures (UV v1)" );

    ImGui::BeginDisabled( !canEdit );
    if ( ImGui::Button( "Texture Shift" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_TEX_SHIFT );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( canEdit
            ? "Drag to shift the texture: 1 px = 1 unit, quantised to the\n"
              "classic grid (CTRL for 1-unit steps).  X = S only, Y = T only.\n"
              "Type a value for an exact whole-unit shift.  Live; Esc restores."
            : "Select brushes or faces first." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canEdit );
    if ( ImGui::Button( "Texture Rotate" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_TEX_ROTATE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( canEdit
            ? "Drag horizontally: 0.5 deg/px, snapped to 5 deg.\n"
              "The texdef stores WHOLE degrees, so the value is rounded.\n"
              "Applied once on commit (the core owns its own undo record)."
            : "Select brushes or faces first." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canEdit );
    if ( ImGui::Button( "Texture Scale" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_TEX_SCALE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( canEdit
            ? "Drag: one texdef SIZE step per 8 px (right = S, down = T).\n"
              "This ADDS to the texdef size, exactly like the classic\n"
              "texture bar's scale spins.  Applied once on commit."
            : "Select brushes or faces first." );

    if ( ImGui::Button( "Pick Texture" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_PICK_TEXTURE );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Makes the material under the 3D cursor current.\n"
                           "Uses the last cursor position over the 3D view.\n"
                           "As in the classic middle-button pick, this ALSO\n"
                           "applies the picked material to the current selection." );

    // ── KIWI-UX (ROUND BH, ITEM 1): the mouse route to Caulk Selection ──────
    // The key is End, and only in the MODERN profile (kiwi_keymap.cpp moves
    // View->Center to Shift+End there and leaves the classic profile alone), so a
    // user on classic needs a surface that is not the palette.  This block is it.
    // No kiwi_caulk.h include: the predicate IS KiwiUv_CanEdit (kiwi_caulk.h's
    // canExecute note says why they are the same function) and the verb goes
    // through the ordinary command route like every other button here.
    ImGui::SameLine();
    ImGui::BeginDisabled( !canEdit );
    if ( ImGui::Button( "Caulk Selection" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_CAULK_FACES );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( canEdit
            ? "End - set every selected face (and every face of a selected\n"
              "brush, and selected patches) to the caulk material, then FIT\n"
              "it one repeat per face.  Caulk tells the BSP compiler the face\n"
              "can be optimised away.  Two undo steps: the fit, then the apply."
            : "Select brushes or faces first." );
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BH, ITEM 3) — A TEXTURE APPLY IS AN OPERATION, NOT A PENDING EDIT.
// ═════════════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"Make it so texture applications dont require a right-click/
// enter to confirm, they are just an operation that goes through when you click the new
// texture (same with UVs).  Leave them undoable."*
//
// ── THE APPLY WAS NEVER GATED.  IT WAS BEING SILENTLY REVERTED. ────────────────────
// The click path itself is direct and always was: ImGuiShell_ViewportInput ->
// VP_Down( RTT_TEXTURE ) -> TexWnd_OnLButtonDown -> TexWnd_ApplyMaterialAtIndex ->
// Texture_SetTexture -> Brush_SetTexture (imgui_shell.cpp:363-366, texwnd.cpp:1337-1345).
// No modal command sees that press — KiwiCmd_MouseButton is only offered CAMERA input
// (kiwi_viewport.cpp:577) — and Brush_SetTexture opens and CLOSES its own undo record
// ("set face textures" / "set brush textures", select.cpp:1814-1815/:1879).  So the
// material really is written the instant the thumbnail is clicked.
//
// WHAT TAKES IT BACK is the gesture the face selection auto-entered.  In Face mode a
// click on a face starts KIWI_CMD_MOVE and PAUSES it (kiwi_boxselect.cpp:884-891), and
// KiwiMoveCommand::BeginFaces SNAPSHOTS THE WHOLE MaterialDef BLOCK of every face it
// latches:
//     memcpy( u.baseMtl, &def->faces[it.faceIndex].mtldef[0], sizeof( u.baseMtl ) );
//         kiwi_transform.cpp:1663   (baseMtl is byte[ sizeof(MaterialDef) * 4 ], :1013)
// and TWO paths write that snapshot back over the live face:
//     RestoreAll()  kiwi_transform.cpp:1567-1574   — run by Cancel() (:1389) and by the
//                                                    invalid-commit arm (:1280)
//     ApplyFaces()  kiwi_transform.cpp:2638        — run EVERY drag frame, before the push
// Both predate this round and both are correct for what they were written for (texture
// lock rewrites the texdef during a push, so the baseline has to carry it).  What they
// assume is round K's proof that "Cancel here is RestoreAll over an UNTOUCHED brush"
// (kiwi_transform.cpp:889-897) — true of the GEOMETRY, and no longer true of the
// MATERIAL once a browser click has landed while the gesture was parked.
//
// So the user's sequence was: click a face (gesture parks, snapshot taken) -> click a
// texture (applied, one undo record) -> click the NEXT face, or press Esc, or start any
// other verb -> the parked gesture CANCELS -> RestoreAll puts the old material back.  The
// only exits that do NOT run RestoreAll are Commit's normal arm (:1319-1385) — which is
// RMB / Enter.  That is exactly "texture applications require a right-click/enter to
// confirm", and it applies to UV EDITOR writes for the same reason: a texdef lives inside
// the MaterialDef that baseMtl covers.
//
// ── THE FIX: END THE GESTURE FIRST, THEN APPLY.  (Round Z's SwapVerb shape.) ───────
// kiwi_command.h's CanSwapTo note already settled what "take a live gesture over" means:
// GestureMoved() true -> COMMIT (its record closes on its own edit, exactly as Enter
// would have); false -> CANCEL (provably record-free — no bracket was opened).  This is
// that fork, asked by an APPLY instead of by a chord, and it leaves the modal framework's
// own confirm untouched: nothing here runs unless something outside the viewport is about
// to write a material, and a real gesture still confirms with RMB / Enter as before.
//
// THE ONE EXTRA CLAUSE is the after-confirm deselect.  A committed FACE push clears the
// selection (round K, kiwi_transform.cpp:1341/:1379-1383) — which is precisely why
// KiwiMoveCommand::CanSwapTo refuses a MOVED face gesture (:943-944) — and an apply into
// an empty selection is a no-op.  So the selection is captured first and re-seeded when
// the commit emptied it, item by item, skipping anything the commit invalidated.
//
// ORDER GUARANTEE: this must run BEFORE the apply, never after.  Cancel() restores the
// baseline; running it afterwards would undo the very material that was just applied.
//
// ── AND THE FACE GESTURE COMES BACK, WITH A FRESH BASELINE ────────────────────────
// Ending the gesture would otherwise cost the user the push/pull gizmo on every
// texture click — click a face (gizmo up), click a thumbnail, gizmo gone, click the
// face again.  So the browser funnel pairs this with KiwiUv_RestoreGestureAfterApply
// below, which re-runs kiwi_boxselect.cpp's own two calls (KiwiCmd_Start(
// KIWI_CMD_MOVE ) + KiwiCmd_Pause, :889-890) under the same gate.  That is not merely
// cosmetic: the restart takes a NEW BeginFaces snapshot, so the material that was just
// applied IS the baseline from that moment on and the next Cancel keeps it.
namespace
{
    // Set by EndGestureBeforeApply when it ended an auto-entered FACE push, consumed
    // by RestoreGestureAfterApply.  Cleared on entry so a caller that does not pair
    // the two (the UV editor) can never leave a stale latch for a later texture click.
    bool s_reenterFacePush = false;
}

bool KiwiUv_EndGestureBeforeApply( const char *what )
{
    s_reenterFacePush = false;
    KiwiEditorCommand *live = KiwiCmd_Active();
    if ( !live )
        return true;                       // nothing running — the common case

    // PreemptIdle == "an auto-entered face gesture that has applied nothing" — the
    // reported state, and the one the entity browser already cancels on the same
    // argument (kiwi_entbrowser.cpp:654-666).  Cancel is record-free and Cancel never
    // clears a selection, so nothing else is needed.
    if ( live->PreemptIdle() )
    {
        KiwiCmd_Cancel();
        s_reenterFacePush = true;          // PreemptIdle IS "auto-entered face push"
        return true;
    }

    // Anything else must have OPTED IN to being taken over.  CanSwapTo defaults to
    // false (kiwi_command.h:1016) and GestureMoved defaults to TRUE (:1022), so without
    // this gate a live extrude / bevel / loft would be COMMITTED by a texture click.
    if ( !live->CanSwapTo( KIWI_CMD_PICK_TEXTURE ) )
    {
        Sys_Printf( "%s: finish or cancel \"%s\" first (Enter confirms, Esc cancels).\n",
                    what ? what : "Texture", live->Name() );
        return false;
    }

    selection_t saved = KiwiSel();         // items + active, by value
    if ( live->GestureMoved() ) KiwiCmd_Commit();
    else                        KiwiCmd_Cancel();

    if ( KiwiSel().items.empty() && !saved.items.empty() )
    {
        selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < saved.items.size(); ++i )
            if ( Sel_ItemValid( saved.items[i] ) && Sel_BrushLive( saved.items[i].brush ) )
                Sel_Add( sel, saved.items[i] );
        if ( Sel_ItemValid( saved.active ) && Sel_BrushLive( saved.active.brush ) )
            sel.active = saved.active;
        Sel_SyncToLegacy();
    }
    return true;
}

void KiwiUv_RestoreGestureAfterApply()
{
    if ( !s_reenterFacePush )
        return;
    s_reenterFacePush = false;
    // The SAME gate kiwi_boxselect.cpp:884-891 applies before auto-entering, minus the
    // shift/ctrl clauses (there is no click here to be additive): Face mode ONLY, a live
    // face is the active item, and nothing else is running.  Mode 5 (EVERYTHING) does not
    // auto-enter there and must not acquire the behaviour here either.
    if ( KiwiCmd_Active() )
        return;
    if ( KiwiSel_GetModeMask() != SEL_MASK_FACE )
        return;
    const sel_item_t &act = KiwiSel().active;
    if ( act.kind != SEL_FACE || !Sel_ItemValid( act ) || !Sel_BrushLive( act.brush ) )
        return;
    if ( KiwiCmd_Start( KIWI_CMD_MOVE ) )
        KiwiCmd_Pause();
}
// ── KIWI-UX end ─────────────────────────────────────────────────────────────────────
