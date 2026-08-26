#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Viewport UV commands wrap the ported texture cores; input, HUD, and the
// caller-owned shift undo bracket live here. See kiwi_uv.h for their invariants.

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_uv.h"
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_hints.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

// Ported entry points.
extern int   Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern int   g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)

extern void  Brush_ShiftTexture ( float a1, float a2 );                 // select.cpp:3084  0x491F20
extern void  Brush_ScaleTexture ( int   a1, int   a2 );                 // select.cpp:3257  0x492650
extern void  Brush_RotateTexture( int   a1 );                           // select.cpp:3339  0x4929F0

extern char  Texture_SetTexture( const int *a1, MaterialDef *a2 );      // texwnd.cpp:2169  0x45BE50
extern char  Radiant_PatchGetTexdef( patchMesh_t *patch,
                                     texdef_sub_t *texdef );            // brush.cpp:2231   0x44B620
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );    // materialdef.cpp:159  0x431640
namespace LayerMat       { int  GetCurrentLayer( MaterialDef *mtlDef ); }  // materialdef.cpp:252  0x431B30
namespace SurfaceInspector { void UpdateSurfaceDialog(); }                 // surfacedlg.cpp:545   0x458590
extern void  UpdatePatchInspector();                                    // patchdialog.cpp:500  0x436DB0
// File scope avoids MSVC namespace-mangling mismatch; pick uses the browser's
// patch normalization after applying a material.
extern void  Patch_KiwiReNaturalizeSelected();                          // pmesh.cpp:1641

extern float grid_sizes[];                                              // engine_stubs.cpp:771  0x6DDE5C

extern bool  ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );  // imgui_shell.cpp:297
extern bool  Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
extern void  Radiant_ExecCommand( unsigned int cmdId );                 // mainfrm.cpp:4054

namespace
{
    const int   KUV_GRID_COUNT = 11;    // grid_sizes[11] (engine_stubs.cpp:771)
    const float KUV_EPS        = 1.0e-3f;

    enum uvcon_t { UVC_FREE = 0, UVC_S, UVC_T };

    // Static because the numeric layer retains label pointers; kinds prevent
    // texture units, degrees, and counts from being formatted as lengths.
    const kiwiNumField_t KUV_FIELDS_SHIFT [1] = { { "shift", KNUM_FACTOR, false } };
    const kiwiNumField_t KUV_FIELDS_ROTATE[1] = { { "angle", KNUM_ANGLE,  false } };
    const kiwiNumField_t KUV_FIELDS_SCALE [1] = { { "steps", KNUM_COUNT,  false } };

    // Menu/palette invocation moves the cursor off-camera, so instant pick uses
    // the last camera position; KiwiCmd_LastCursor is modal-only.
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

    // Selection reads used by the texture cores.
    bool AnyWholeBrushSelected()
    {
        return selected_brushes.next != &selected_brushes;
    }

    int SelectedFaceCount()
    {
        return g_SelectedFaces.GetSize();
    }

    // The classic grid step the RMB+Alt texture drag quantises to
    // (camwnd.cpp:2750 uses grid_sizes[g_qeglobals.d_gridsize] directly).
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

    // KiwiCmd_UndoBegin misses face-only brushes; Undo_AddBrush deduplicates them.
    void UndoCoverSelectedFaces()
    {
        const int n = SelectedFaceCount();
        for ( int i = 0; i < n; ++i )
            KiwiCmd_UndoCoverBrush( g_SelectedFaces.GetAt( i ).brush );
    }

    // Shared modal base: S/T constraint, numeric state, and HUD.
    class KiwiUvBase : public KiwiEditorCommand
    {
    public:
        // Texture commands drag no geometry, so no pick exclusions are needed.
        unsigned PickFlags() const override { return PICKF_NONE; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        bool CanExecute() override { return KiwiUv_CanEdit(); }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            // Menu-started commands may lack a camera seed; first camera movement
            // becomes the origin so the drag begins at zero.
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

        // These values are not lengths; reverse the numeric layer's display-to-world conversion.
        float NumRaw() const { return Units_ToDisplay( m_numWorld ); }

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

        const char *ScopeText() const
        {
            const int f = SelectedFaceCount();
            const bool b = AnyWholeBrushSelected();
            if ( f && b ) return "faces+brushes";
            if ( f )      return "faces";
            return "brushes";
        }

        // Off-camera starts remain numeric-only until a camera position is available.
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

    // Shift is live because Brush_ShiftTexture opens no undo bracket (select.cpp:3058).
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
            m_undoOpen = false;
            g_nUpdateBits = -1;
        }

        void Cancel() override
        {
            // The inverse is provisional; framework rollback provides the exact restore.
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
                // Match classic RMB+Alt: 1 px = 1 texture unit, right/down positive.
                s = (float)( x - m_startX );
                t = (float)( y - m_startY );
            }
            if ( m_con == UVC_S ) t = 0.0f;
            if ( m_con == UVC_T ) s = 0.0f;

            // The brush pass truncates deltas to integers, so use whole texture
            // units: classic grid while snapped, one unit while snapping is inactive.
            const float q = SnapActive() ? ClassicGridStep() : 1.0f;
            s = QuantizeTo( s, q );
            t = QuantizeTo( t, q );

            if ( m_hasNum )
            {
                // Round for the whole-unit brush path; the HUD shows the landed value.
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
                UndoCoverSelectedFaces();               // must precede the first mutation
                m_covered = true;
            }

            Brush_ShiftTexture( ds, dt );
            m_appliedS += ds;
            m_appliedT += dt;
            g_nUpdateBits = -1;
        }

        void UpdateHud()
        {
            char step[32];
            SetHud( "%s  %s  S %+.0f  T %+.0f  step %s",
                    ScopeText(), TargetText(), (double)m_s, (double)m_t,
                    KiwiFmt_Num( step, sizeof( step ),
                                 SnapActive() ? ClassicGridStep() : 1.0f, 6 ) );
        }

        float m_s = 0.0f, m_t = 0.0f;
        float m_appliedS = 0.0f, m_appliedT = 0.0f;
        bool  m_undoOpen = false;
        bool  m_covered  = false;
    };

    // Rotate is deferred: its core owns undo, so invoke it exactly once at commit.
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
        // The core accepts whole degrees; accumulate fractional drag and round once.
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

    // Scale is deferred because its core owns undo; arguments are additive texdef
    // size steps, matching the classic texture-bar spins (select.cpp:3237-3249).
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

    // Instant pick mirrors classic MMB (drag.cpp:694-737) but deliberately omits
    // its unrelated new-brush height update. It still retextures the selection.
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

        // Texture under the cursor is always a face-level pick, independent of mode.
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

        // Match drag.cpp:718-722: recover the patch's planar texdef first.
        // 0x44B620 has no success path, so its return value is intentionally ignored.
        if ( inst->patch && def->patch )
            Radiant_PatchGetTexdef( def->patch, &md->mat_texDef + LayerMat::GetCurrentLayer( md ) );

        const int *patchDef = inst->patch ? (const int *)inst->patch->def : 0;
        Texture_SetTexture( patchDef, md );

        // The patch apply path swaps materials without rebuilding control-point UVs;
        // match the browser funnel by re-naturalizing selected patches afterward.
        Patch_KiwiReNaturalizeSelected();

        SurfaceInspector::UpdateSurfaceDialog();
        UpdatePatchInspector();

        const char *name = (const char *)Materialdef_GetName( md );
        Sys_Printf( "Pick Texture: %s\n", name ? name : "(unnamed)" );
        g_nUpdateBits = -1;
        return true;
    }
}

// MaterialDef::mat_texDef is at +0x08 and texdef_sub_t is 28 bytes; this sub-layer
// walk intentionally enters following mtldef[4] entries, matching select.cpp:3086.
texdef_sub_t *KiwiUv_FaceTexdef( brush_t *def, int faceIndex, MaterialDef **outMtl )
{
    if ( !def || !def->faces || faceIndex < 0 || faceIndex >= def->faceCount )
        return 0;
    int layer = g_qeglobals.current_edit_layer;
    if ( layer < 0 || layer > 3 )
        return 0;
    MaterialDef *md = &def->faces[faceIndex].mtldef[layer];
    // Both callees assert exactly one material pointer; validate before entering them.
    if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
        return 0;
    if ( outMtl )
        *outMtl = md;
    return &md->mat_texDef + LayerMat::GetCurrentLayer( md );
}
// Core availability.
bool KiwiUv_CanEdit()
{
    // Matches the early-out in all three cores (select.cpp:3062/3233/3317).
    return SelectedFaceCount() > 0 || AnyWholeBrushSelected();
}

bool KiwiUv_CanPick()
{
    int x, y;
    return CameraCursor( &x, &y );
}

// Registration and lookup.
void KiwiUv_RegisterCommands()
{
    // Unbound: panel/palette access remains available, plus classic MMB pick.
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

// Read-only texdef overlay; Surface Inspector remains the numeric editor.
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
    texdef_sub_t *td = KiwiUv_FaceTexdef( act.brush->def, act.faceIndex, &md );
    if ( !td || !md )
        return;

    const char *name = (const char *)Materialdef_GetName( md );

    char line[320];
    char shiftS[48], shiftT[48], sizeS[48], sizeT[48], rotate[48];
    _snprintf( line, sizeof( line ),
               "%s   shift %s, %s   size %s, %s   rot %s   layer %d",
               name ? name : "(unnamed)",
               KiwiFmt_Num( shiftS, sizeof( shiftS ), td->shift[0], 6 ),
               KiwiFmt_Num( shiftT, sizeof( shiftT ), td->shift[1], 6 ),
               KiwiFmt_Num( sizeS, sizeof( sizeS ), td->size[0], 6 ),
               KiwiFmt_Num( sizeT, sizeof( sizeT ), td->size[1], 6 ),
               KiwiFmt_Num( rotate, sizeof( rotate ), td->rotate, 6 ),
               g_qeglobals.current_edit_layer );
    line[sizeof( line ) - 1] = '\0';

    const ImVec2 sz = ImGui::CalcTextSize( line );
    const ImVec2 pad( 8.0f, 4.0f );
    const float  boxW = sz.x + pad.x * 2.0f;
    const float  boxH = sz.y + pad.y * 2.0f;

    // Stack in the shared bottom band to avoid the numeric HUD and command chips;
    // the fallback preserves placement for non-overlay callers.
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

// "Textures" panel block.
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

    // Mouse access is needed because End is bound only by the modern profile.
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

// End a live face push before external material writes: its saved MaterialDef is
// otherwise restored over the write. Commit moved swappable gestures, cancel idle
// ones, preserve commit-cleared selection, then optionally restart with a fresh baseline.
namespace
{
    // Cleared on each apply so unpaired callers cannot leave a stale restart request.
    bool s_reenterFacePush = false;
}

bool KiwiUv_EndGestureBeforeApply( const char *what )
{
    s_reenterFacePush = false;
    KiwiEditorCommand *live = KiwiCmd_Active();
    if ( !live )
        return true;

    // PreemptIdle cancellation is record-free, but the restart latch assumes the
    // command was an auto-entered face push even though other commands can opt in.
    if ( live->PreemptIdle() )
    {
        KiwiCmd_Cancel();
        s_reenterFacePush = true;
        return true;
    }

    // Only opt-in commands may be preempted; otherwise a texture click could commit
    // an unrelated live modeling command.
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
    // Mirror the box-select auto-enter gate; there is no click here, so additive
    // modifier clauses do not apply.
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
