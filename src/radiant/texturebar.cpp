#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Docked texture-alignment bar. It reads and writes the selected brush definition's
// current-layer texdef; spin controls reuse the selection texture-edit operations.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <stdio.h>
#include <stdlib.h>
// (CSpinButtonCtrl / UDN_DELTAPOS / NMUPDOWN come from <afxcmn.h> in the PCH.)

// ── selection state (select.cpp) ──────────────────────────────────────────────
#define SEL_FACE_COUNT() (g_SelectedFaces.GetSize())

// ── the quick texture ops (select.cpp, already ported — the SAME path the Surface
//    Inspector spinners use; the texture bar must NOT re-port these) ────────────
extern void Brush_ShiftTexture ( float a1, float a2 );   // 0x491F20
extern void Brush_ScaleTexture ( int   a1, int   a2 );   // 0x492650
extern void Brush_RotateTexture( int   a1 );             // 0x4929F0

// ── texdef target resolution + name (materialdef.cpp / surfacedlg.cpp) ─────────
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *m );             // 0x431640
namespace LayerMat { int GetCurrentLayer( MaterialDef *def ); }             // 0x431B30

extern int          g_nUpdateBits;                    // 0x25D5A74 (engine_stubs.cpp)

// The radiant verbose Assert stub (engine_stubs.cpp): type 0 = log-to-stderr + CONTINUE.
extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ══════════════════════════════════════════════════════════════════════════════
//  texdef CORE — the picked face's MaterialDef (else the current-texture template).
//
//  Faithful to GetSurfaceAttributes' two branches: a selected face → its
//  brush->def->faces[index].mtldef[current_edit_layer]; otherwise the editor's
//  current-texture-window template random_texture_stuff[2100*layer].  Returns the
//  current-layer texdef_sub_t the bar reads/writes (NULL only if the def path is broken).
// ══════════════════════════════════════════════════════════════════════════════
// A MaterialDef is usable by the accessors (GetCurrentLayer / Materialdef_GetName) only when
// exactly one of lyrMtl/radMtl is set (the binary's MtlDef_IsValid invariant; an invalid def
// trips an Assert and then NULL-derefs radMtl->name).  Before the editor's current-texture
// template is initialised (e.g. a refresh that lands before the first map/material load), the
// template MaterialDef is zeroed — so guard it.  The binary's bar only refreshes AFTER init,
// where the template is always valid; this guard makes the early/no-texture path safe.
static inline bool TexBar_MtlDefUsable( const MaterialDef *m )
{
    return m && ( ( m->lyrMtl != nullptr ) ^ ( m->radMtl != nullptr ) );
}

// The picked face's MaterialDef (else the current-texture template).  NULL if no editable
// def is available (no selection + uninitialised template).
static MaterialDef *TexBar_TargetMaterialDef()
{
    MaterialDef *md;
    if ( SEL_FACE_COUNT() > 0 )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( 0 );
        selbrush_t *b = selFace.brush;
        if ( !b || !b->def )
            return nullptr;
        // TextureBar.cpp:160/161 (both type-0): the picked face must still be live + the
        // instance/def versions in sync.  The binary asserts these INLINE in BOTH
        // GetSurfaceAttributes (160/161) and ApplyFields (188/189, identical conditions);
        // this shared helper reports 160/161 for both — accepted file/line divergence.
        // (The texDef-null Assert 169 is a never-fire address-null check subsumed by the
        // upstream-null early-return.)
        iassert( selFace.face == &selFace.brush->faces[selFace.index] );    // TextureBar.cpp:160
        iassert( selFace.brush->version == selFace.brush->def->version );   // TextureBar.cpp:161
        md = &b->def->faces[selFace.index].mtldef[g_qeglobals.current_edit_layer];
    }
    else
    {
        md = &g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].mtl;
    }
    return TexBar_MtlDefUsable( md ) ? md : nullptr;
}

// The current-layer texdef_sub_t the bar reads/writes.  NULL when no usable MaterialDef.
static texdef_sub_t *TexBar_TargetTexdef()
{
    MaterialDef *md = TexBar_TargetMaterialDef();
    if ( !md )
        return nullptr;
    int layer = LayerMat::GetCurrentLayer( md );   // safe: md is MtlDef_IsValid here
    return &md->mat_texDef + layer;
}

// ══════════════════════════════════════════════════════════════════════════════
//  CTextureBar — the hand-built docked bar (CVehicleDlg / CSurfaceDlg plumbing pattern).
// ══════════════════════════════════════════════════════════════════════════════

// MFC shell — the control ids + the child-HWND statics.  The texdef cores above and the
// TextureBar_Gather / _Apply / _Spin actions below stay COMMON (they are what a future
// ImGui texture-bar panel drives), as do g_tbRotateAmt and g_texBarHeight.

// CTextureBar member ints (the binary lays them out at +0x248..; here they are real fields,
// but the read/write helpers below transcribe the exact field→member mapping from the disasm).
//   m_nHShift  (0x248)   m_nHScale (0x24C)   m_nRotate (0x250)
//   m_nVShift  (0x254)   m_nVScale (0x258)   m_nRotateAmt (0x25C, init 45)

static int g_tbRotateAmt = 45;        // CTextureBar member @0x25C (the Rotate spin step)


// ── format / read helpers (the binary's DDX_Text int<->edit; we keep them %d, the bar
//    shows integer texels/degrees exactly like the IDB member ints) ─────────────

// One texture-bar control snapshot: the current-material readout plus the five member ints
// the bar exchanges with the picked face's texdef (the binary's CTextureBar members
// @0x248..0x25C described above).
struct textureBarState_t
{
    char texName[132];    // IDC_TB_TEXNAME (out only — the resolved material name)
    int  hShift;          // m_nHShift @0x248
    int  vShift;          // m_nVShift @0x254
    int  hScale;          // m_nHScale @0x24C
    int  vScale;          // m_nVScale @0x258
    int  rotate;          // m_nRotate @0x250
};

// UI-independent core reads behind the texture bar's field refresh.
// ─────────────────────────────────────────────────────────────────────────────
// 0x459610  CTextureBar::GetSurfaceAttributes — READ the picked face's texdef into the
// member ints and DISPLAY them (UpdateData(FALSE)).  Faithful field mapping (disasm):
//   shift[0] → m_nHShift   shift[1] → m_nVShift
//   size[0]  → m_nHScale   size[1]  → m_nVScale   rotate → m_nRotate
// Each is int(float) (truncating __ftol2).  The bar shows raw texel scale/shift + degrees
// (NOT the Surface Inspector's size/width "stretch" — the texture bar is the raw-texel view).
// THE REFRESH ENTRY POINT (called whenever the current texture / selection changes).
// Returns false when there is no editable texdef (the display is then left as-is).
// ─────────────────────────────────────────────────────────────────────────────
bool TextureBar_Gather( textureBarState_t &out )
{
    texdef_sub_t *td = TexBar_TargetTexdef();
    if ( !td )
        return false;

    // Texture name (the bar's current-material readout) — the brief's "shows the current
    // material name".  TexBar_TargetMaterialDef only returns a MtlDef_IsValid def, so
    // Materialdef_GetName is safe (no NULL radMtl->name deref).
    MaterialDef *md = TexBar_TargetMaterialDef();
    const char *name = md ? (const char *)Materialdef_GetName( md ) : "";
    memset( out.texName, 0, sizeof( out.texName ) );
    strncpy( out.texName, name ? name : "", sizeof( out.texName ) - 1 );

    out.hShift = (int)td->shift[0];   // m_nHShift @0x248
    out.vShift = (int)td->shift[1];   // m_nVShift @0x254
    out.hScale = (int)td->size[0];    // m_nHScale @0x24C
    out.vScale = (int)td->size[1];    // m_nVScale @0x258
    out.rotate = (int)td->rotate;     // m_nRotate @0x250
    return true;
}


// UI-independent action behind the texture bar's live field commit.
// ─────────────────────────────────────────────────────────────────────────────
// 0x459750  TextureBar_02 (via the 0x459600 thunk) — UpdateData(TRUE) then WRITE the
// member ints back onto the picked face's texdef (live edit-as-you-type), g_nUpdateBits|=1.
// No Brush_SetTexture propagation — the binary writes the picked face's def directly and
// relies on the view invalidation to re-read (the spin nudges below DO propagate, via the
// already-ported Brush_*Texture).  Faithful field mapping (disasm v5[2]/v5[3]/v5[0]/v5[1]/v5[4]):
//   m_nHShift → shift[0]   m_nVShift → shift[1]
//   m_nHScale → size[0]    m_nVScale → size[1]   m_nRotate → rotate
// Each member is read as int and stored as (float).  No-op when nothing is selected
// (result == GetSelectedFaces size == 0).
// ─────────────────────────────────────────────────────────────────────────────
void TextureBar_Apply( const textureBarState_t &st )
{
    if ( SEL_FACE_COUNT() <= 0 )
        return;                       // the binary's `if (GetSelectedFaces.size)` guard

    texdef_sub_t *td = TexBar_TargetTexdef();
    if ( !td )
        return;

    td->shift[0] = (float)st.hShift;   // m_nHShift → shift[0]
    td->shift[1] = (float)st.vShift;   // m_nVShift → shift[1]
    td->size[0]  = (float)st.hScale;   // m_nHScale → size[0]
    td->size[1]  = (float)st.vScale;   // m_nVScale → size[1]
    td->rotate   = (float)st.rotate;   // m_nRotate → rotate

    g_nUpdateBits |= 1;
}


// The five spin control ids TextureBar_Spin dispatches on. These lived in the MFC
// bar's control-id enum (deleted with the CTextureBar shell in U-RIP); TextureBar_Spin
// is common code called by both the old bar and any future ImGui spin, so the ids it
// switches on live out here. Values verbatim from that enum (IDC_TB_TEXNAME = 1810, the
// bar's controls counting up in cell order).
enum
{
    IDC_TB_SHIFT_H_SPIN = 1812,
    IDC_TB_SHIFT_V_SPIN = 1814,
    IDC_TB_SCALE_H_SPIN = 1816,
    IDC_TB_SCALE_V_SPIN = 1818,
    IDC_TB_ROTATE_SPIN  = 1820,
};

// UI-independent action behind the texture bar's spin-arrow nudges.
// ── the five spin handlers (sub_459470/4594C0/459510/459550/459590) ───────────
// HShift/VShift spins: ±gridsize via Brush_ShiftTexture; HScale/VScale spins: ±gridsize via
// Brush_ScaleTexture; Rotate spin: ±m_nRotateAmt via Brush_RotateTexture.  Each then refreshes
// the member ints via GetSurfaceAttributes (the binary's tail — issued by the caller).
// Returns false for a spin id the bar does not own (no step, no refresh).
bool TextureBar_Spin( int idFrom, bool negate )
{
    // disasm 0x45947e: `mov eax,[d_savedinfo.d_gridsize]; cdq;xor;sub` (INTEGER abs) then `fild`
    // (INT->float) — the binary stores d_savedinfo.d_gridsize @0x2BC as an INT (the texel snap is
    // integer); the port typed it float (qe3.h:640) so (int)8.0f == fild(int 8) for every grid the
    // texel shift uses.  (Only texturebar + mainfrm read this float field, both (int)-cast it.)
    int grid = (int)g_qeglobals.d_savedinfo.d_gridsize;
    if ( grid < 0 ) grid = -grid;          // abs32(d_gridsize)
    int step = negate ? -grid : grid;

    switch ( idFrom )
    {
        case IDC_TB_SHIFT_H_SPIN:  Brush_ShiftTexture( (float)step, 0.0f ); break;   // sub_459470
        case IDC_TB_SHIFT_V_SPIN:  Brush_ShiftTexture( 0.0f, (float)step ); break;   // sub_4594C0
        case IDC_TB_SCALE_H_SPIN:  Brush_ScaleTexture( step, 0 );           break;   // sub_459510
        case IDC_TB_SCALE_V_SPIN:  Brush_ScaleTexture( 0, step );           break;   // sub_459550
        case IDC_TB_ROTATE_SPIN:                                                     // sub_459590
        {
            int rot = g_tbRotateAmt;       // member @0x25C (UpdateData(TRUE) reads it first)
            if ( rot < 0 ) rot = -rot;
            Brush_RotateTexture( negate ? -rot : rot );
            break;
        }
        default:
            return false;
    }
    return true;
}

// `iDelta>=0` (down arrow) = negative step (disasm `if (*(int*)(a2+16) >= 0) v4 = -v4`).

// ══════════════════════════════════════════════════════════════════════════════
//  CMainFrame integration — the bar is created across the top strip of the frame.
//  CMainFrame::UpdateTextureBar (0x429150) refreshes it; the QE4 layout insets below it.
// ══════════════════════════════════════════════════════════════════════════════
int g_texBarHeight = 0;                            // top inset consumed by the bar (0 = none)
                                                   // (COMMON: mainfrm.h:376 externs it for the
                                                   //  QE4 layout inset in BOTH shells)

// Create the embedded texture bar across the top of the frame.  Returns its height (so the
// caller can inset the QE4 layout).  Hand-built (radiant.rc has no template) — a child window
// of the frame holding the name static + the five edit/spin cells.

