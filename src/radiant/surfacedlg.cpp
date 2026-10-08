#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Surface inspector for face and patch texture alignment, sample size, and fitting.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include "kiwi_ported.h"   // KIWI: shared engine/editor externs
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// ── selection state (select.cpp) ──────────────────────────────────────────────
#define SEL_FACE_COUNT() (g_SelectedFaces.GetSize())

// ── texture edit core (select.cpp / materialdef.cpp / brush.cpp) ───────────────
extern void         Brush_SetTexture( MaterialDef *material, char planarProject );   // 0x48F170
extern void         Brush_SetTextureMapping( texdef_sub_t *texdef );        // 0x48F4F0 (select.cpp) — apply the current-layer texdef to the selection (Undo-bracketed)
extern void         Brush_SetSampleSize( int size );                        // 0x48F800 (select.cpp) — apply sample size to the selection
extern void         TexMatToFakeTexCoords( MaterialDef *def, texdef_sub_t *texDef ); // 0x472DF0 (materialdef.cpp)
extern void         Patch_SetTextureInfo( texdef_sub_t *texDef );           // 0x447760 (pmesh.cpp) — relative patch texdef transform
namespace LayerMat { int GetCurrentLayer( MaterialDef *def ); }             // 0x431B30
extern void         Material_SetMode( int iMode );                          // 0x45B910 (texwnd.cpp)

// g_patch_texdef (0x23F15F8, patch_texdef_t = 44 B) — the current-texture-window TEMPLATE
// the dialog edits when nothing (or only patches) is selected, PLUS the shared sample-size the
// readout shows / the apply reads back.  {MaterialDef def; int unk3; float sample_size}.
struct patch_texdef_t
{
    MaterialDef mtlDef;       // 0x00 (36 B)  (name used by the assert strings)
    int         unk3;         // 0x24
    float       sample_size;  // 0x28
};
static_assert( sizeof( patch_texdef_t ) == (sizeof(void *) == 8 ? 56 : 44), "patch_texdef_t" );
patch_texdef_t g_patch_texdef;      // 0x23F15F8

// Fit the texture to the selected face(s) (Brush_FitTexture 0x4939E0, select.cpp) —
// scale/shift the texdef so the texture spans the face `x`×`y` times.  The "Fit"
// button passes (1,1,0): one tiling across the face, computed bounds, no selection-wide.
extern void         Brush_FitTexture( float x, float y, int acrossSelection );   // 0x4939E0

// ── patch-texturing core (pmesh.cpp / select.cpp) — the Surface Inspector patch row ──
extern void         Select_SetTexture( float *out );                        // 0x456D70 (layer sample size)
extern void         Patch_NaturalizeSelected( bool unk, bool cap, float x, float y ); // 0x447FD0
extern void         Patch_Lightmap_Texturing();                             // 0x448110
extern void         Patch_SetTexturing( float sx, float sy, int mode );     // 0x446B60

// KIWI: the multi-layer texmod transaction (SetTexMods 0x458270 snapshot into face mtldef[3] +
// a template scratch, SurfaceDlg_Wnd02 0x4583F0 commit back, the pending-edits flag) is gone
// with the MFC inspector; the ImGui inspector applies each edit live.

// ── surface-inspector globals (surfDlgGlob.hwnd 0x23F1624) ──
//  surfDlgGlob.hwnd = the inspector HWND when open, 0 when closed (map.cpp reads it to know
//  whether to refresh the inspector after a load).  g_bNewFace (0x23F15D0) gates the
//  "edit the picked face" vs "edit the current-texture window" path (the binary derives
//  it from prefs m_bFace; this build keeps it on so a selected face is what you edit).
SurfDlgGlob_t surfDlgGlob = { 0 };
char g_bNewFace  = 1;


// ══════════════════════════════════════════════════════════════════════════════
//  texdef CORE — read / apply a face's texture alignment (headless-safe).
//
//  texdef_sub_t (qedefs.h, 28 bytes / 7 floats): size[2] scale-as-texels, shift[2]
//  texel shift, rotate (deg), unk3, sample_size.  The .map face line writes the
//  current layer's block as `<material> size0 size1 shift0 shift1 rotate unk3`
//  (Brush_Write 0x474e90); the camera reads size/shift/rotate through Face_MoveTexture.
// ══════════════════════════════════════════════════════════════════════════════

// SurfaceInspector_GetMaterialDef (0x457950) essence — WHICH MaterialDef the inspector
// edits for the current selection (no dialog-field reads; those are committed separately):
//   * a face is selected (g_bNewFace) → that face's mtldef[current_edit_layer]
//     (a pointer INTO the face def, so edits land on the face; Brush_SetTexture then
//      propagates it to every selected face),
//   * else → the editor's current-texture-window template (random_texture_stuff[layer]).
//  Returns NULL only when nothing is selected at all (the apply path then no-ops).
MaterialDef *Surf_TargetMaterialDef()
{
    if ( g_bNewFace && SEL_FACE_COUNT() > 0 )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( 0 );
        selbrush_t *b = selFace.brush;
        if ( b && b->def )
        {
            // SurfaceDlg.cpp:420/421 (type-0): the picked face must still be live and the
            // instance/def versions in sync before reading the DEF face's mtldef.
            iassert( selFace.face == &selFace.brush->faces[selFace.index] );    // SurfaceDlg.cpp:420
            iassert( selFace.brush->version == selFace.brush->def->version );   // SurfaceDlg.cpp:421
            return &b->def->faces[selFace.index].mtldef[g_qeglobals.current_edit_layer];
        }
    }
    if ( selected_brushes.next != &selected_brushes )
        return &g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].mtl;
    // Nothing selected — fall back to the current-texture template so the inspector still
    // shows something coherent; the apply path checks SEL state before mutating anything.
    return &g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].mtl;
}
// SurfaceDlg_GetMaterialSize (0x456f50) helper — the editor texture's pixel dimensions
// (autoTexScale), falling back to 512×512 when the material has no resolved image.
void Surf_MaterialWH( MaterialDef *md, int *w, int *h )
{
    qtexture_s *lm = md ? MaterialDef_GetLayeredMaterial( md ) : nullptr;
    if ( lm ) { *w = lm->width;  *h = lm->height; }
    else      { *w = 512;        *h = 512; }
}

// ── Surface-inspector sample-size readouts ────────────────────────────────────
//   Feed the inspector's sample-size field + patch name.  face_t has 36-byte mtldef[]
//   slots, so the per-layer step is `&face->mtldef[layer]` (matching Surf_ReadTexdef).
extern int   QE_SingleBrush_Check();                            // qe3.cpp (QE_SingleBrush 0x48C8B0, silent)
extern LayerMaterialDef *Patch_GetTextureName();                // pmesh.cpp 0x448670 (below)

// SurfaceDlg_GetMaterialSize (0x456F50) — the face's proportional texel scale
// (size[0]/width) when the scale is isotropic (size[1]*width == height*size[0]), else 0.
float SurfaceDlg_GetMaterialSize( face_t *f )
{
    iassert( f );   // SurfaceDlg.cpp:166
    MaterialDef *md = &f->mtldef[ LayerMat::GetCurrentLayer( &f->mtldef[0] ) ];
    qtexture_s *lm = MaterialDef_GetLayeredMaterial( &f->mtldef[0] );
    int width  = lm ? lm->width  : 512;
    int height = lm ? lm->height : 512;
    float *size = md->mat_texDef.size;
    if ( size[1] * (float)width != (double)height * size[0] )
        return 0.0f;
    iassert( width );   // SurfaceDlg.cpp:173
    return (float)( size[0] / (float)width );
}

// SurfaceDlg_GetMaterialSize_Patch (0x457010) — patch or brush texel scale.  For a patch
// symbiont, reads the cached sample (bDirty gates it); for a brush, the common scale of
// all its faces (0 if they disagree).
float SurfaceDlg_GetMaterialSize_Patch( brush_t *b )
{
    iassert( b );   // SurfaceDlg.cpp:183
    patchMesh_t *patch = b->patch;
    if ( patch )
    {
        if ( !patch->bDirty )
            return 0.0f;
        return *(float *)&patch->size_of_struct_0x504C;
    }
    float sz = SurfaceDlg_GetMaterialSize( &b->faces[0] );
    if ( sz == 0.0f )
        return 0.0f;
    for ( unsigned int i = 1; i < (unsigned int)b->faceCount; ++i )
        if ( SurfaceDlg_GetMaterialSize( &b->faces[i] ) != sz )
            return 0.0f;
    return sz;
}

// GetBrushSampleSize (0x4570C0) — the common patch/brush sample size across the whole
// selection (0 unless every selected brush shares it).
float GetBrushSampleSize()
{
    if ( selected_brushes.next == &selected_brushes )
        return 0.0f;
    float sz = SurfaceDlg_GetMaterialSize_Patch( selected_brushes.next->def );
    if ( sz == 0.0f )
        return 0.0f;
    for ( selbrush_t *n = selected_brushes.next->next; n != &selected_brushes; n = n->next )
        if ( SurfaceDlg_GetMaterialSize_Patch( n->def ) != sz )
            return 0.0f;
    return sz;
}

// GetBrushfaceSampleSize (0x457170) — the common sample size across all SELECTED FACES
// (patch faces via _Patch, brush faces via GetMaterialSize); 0 unless all agree.
float GetBrushfaceSampleSize()
{
    int count = SEL_FACE_COUNT();
    if ( count <= 0 )
        return 0.0f;
    selbrush_t *b0 = g_SelectedFaces.GetAt( 0 ).brush;
    float sz = b0->patch ? SurfaceDlg_GetMaterialSize_Patch( b0->def )
                         : SurfaceDlg_GetMaterialSize( &b0->def->faces[g_SelectedFaces.GetAt( 0 ).index] );
    if ( sz == 0.0f )
        return 0.0f;
    for ( int i = 1; i < count; ++i )
    {
        selbrush_t *b = g_SelectedFaces.GetAt( i ).brush;
        float s = b->patch ? SurfaceDlg_GetMaterialSize_Patch( b->def )
                           : SurfaceDlg_GetMaterialSize( &b->def->faces[g_SelectedFaces.GetAt( i ).index] );
        if ( s != sz )
            return 0.0f;
    }
    return sz;
}


// ══════════════════════════════════════════════════════════════════════════════
//  CSurfaceDlg — the Surface Inspector, a modeless CDialog on IDD_SURFACE_INSPECTOR (116).
//  Created by DoSurface (0x4585d0); the RC template + control ids are in res/radiant.rc.
//  The data flow:
//    * SurfaceInspector_GetMaterialDef (0x457950) — READ every edit control into the target
//      texdef (stretch*width/height → texels for non-patch), update the "Repeats in" readouts,
//      SetMaterial + TexMatToFakeTexCoords, set the dirty flag; returns the MaterialDef.
//    * GetTexMods (0x458960) — on Done/Apply + on each spin: if the "texdef edited" flag is up,
//      GetMaterialDef then Brush_SetTextureMapping (propagate to the selection); if the
//      "sample edited" flag is up, Brush_SetSampleSize; then Select_SetTexture_2 refresh.
//    * Select_SetTexture_2 (0x4572d0) — WRITE the target texdef back into the controls.
// ══════════════════════════════════════════════════════════════════════════════

// One Surface Inspector control snapshot: the seven edit-field values the dialog exchanges
// with the texdef core, the two "Repeats in" readouts the read pass computes, and the three
// dialog state members (m_bPatchMode / m_texdefDirty / m_sampleDirty) the commit + spin
// paths consult.
struct surfaceDlgState_t
{
    char  currTex[132];    // IDC_SURFACE_INSP_CURR_TEX (in; the resolved name comes back out)
    float horzShift;       // IDC_SURFACE_INSP_HORZ_SHIFT_IN   (texels)
    float vertShift;       // IDC_SURFACE_INSP_VERT_SHIFT_IN
    float horzStretch;     // IDC_SURFACE_INSP_HORZ_STRETCH    ("stretch" = size/width; raw for a patch)
    float vertStretch;     // IDC_SURFACE_INSP_VERT_STRETCH
    float rotate;          // IDC_SURFACE_INSP_ROTATE          (degrees)
    float sampleSize;      // IDC_SURFACE_INSP_SAMPLE_SIZE     (shared g_patch_texdef.sample_size)
    float repeatsX;        // IDC_SURFACE_INSP_TXT_REPEATS_X   (raw texel size[0])
    float repeatsY;        // IDC_SURFACE_INSP_TXT_REPEATS_Y   (raw texel size[1])
    int   bPatchMode;      // m_bPatchMode
    char  texdefDirty;     // m_texdefDirty
    char  sampleDirty;     // m_sampleDirty
};

// MFC shell — the four control<->state shuttles (CWnd / CSurfaceDlg typed).  The panel
// builds its own surfaceDlgState_t instead, so nothing outside this block needs them.

// UI-independent action behind the inspector's read-fields-into-texdef pass.
// SurfaceInspector_GetMaterialDef (0x457950) — READ the dialog's edit controls into the
// current-layer texdef of the target MaterialDef (the picked face's mtldef[layer], or the
// current-texture template / g_patch_texdef for a patch/no-selection), then TexMatToFakeTexCoords.
// Returns the MaterialDef (NULL only when nothing editable).  For a NON-patch selection the
// "stretch" fields are texel-scale = size/width, so they are multiplied by width/height back to
// texels; for a patch they are stored raw.  Sample size always lands in g_patch_texdef.sample_size.
MaterialDef *SurfaceDlg_StateToTexdef( surfaceDlgState_t &st )
{
    // WHICH MaterialDef + patch-mode (also sets m_bPatchMode for the patch/no-face-selection path).
    bool onlyPatches = ( SEL_FACE_COUNT() <= 0 && selected_brushes.next != &selected_brushes );
    if ( onlyPatches )
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( !b->patch ) { onlyPatches = false; break; }

    MaterialDef *md;
    if ( onlyPatches )
    {
        // patch-only selection → the shared g_patch_texdef template.
        md = &g_patch_texdef.mtlDef;
        st.bPatchMode = 1;
    }
    else
    {
        md = Surf_TargetMaterialDef();
        st.bPatchMode = 0;
    }
    if ( !md )
        return nullptr;

    int width = 512, height = 512;
    Surf_MaterialWH( md, &width, &height );
    if ( width  <= 0 ) width  = 512;
    if ( height <= 0 ) height = 512;

    // Name → SetMaterial (empty/blank → "$default", then reflect the resolved name back).
    if ( (byte)st.currTex[0] > 32 )
    {
        SetMaterial( st.currTex, (patchMesh_material *)md );
    }
    else
    {
        SetMaterial( "$default", (patchMesh_material *)md );
        LayerMaterialDef *n = Materialdef_GetName( md );
        memset( st.currTex, 0, sizeof( st.currTex ) );
        strncpy( st.currTex, n ? (const char *)n : "", sizeof( st.currTex ) - 1 );
    }

    texdef_sub_t *td = &md->mat_texDef + LayerMat::GetCurrentLayer( md );
    td->shift[0] = st.horzShift;
    td->shift[1] = st.vertShift;

    float sx = st.horzStretch;
    td->size[0] = st.bPatchMode ? sx : sx * (float)width;
    st.repeatsX = td->size[0];   // "Repeats in" X readout

    float sy = st.vertStretch;
    td->size[1] = st.bPatchMode ? sy : sy * (float)height;
    st.repeatsY = td->size[1];   // "Repeats in" Y readout

    td->rotate = st.rotate;
    g_patch_texdef.sample_size = st.sampleSize;

    TexMatToFakeTexCoords( md, td );
    return md;
}

// UI-independent core reads behind the inspector's field refresh.
// Select_SetTexture_2 (0x4572d0) — resolve the target texdef for the current selection and
// snapshot it for the dialog controls.  Returns false when there is no editable MaterialDef.
// Shift is in texels; "stretch" = size/width (size/height); rotate in degrees; sample-size the
// shared g_patch_texdef.sample_size.  Also fills the "Repeats in" readouts (raw size).
bool SurfaceDlg_Gather( surfaceDlgState_t &out )
{
    // Target + patch-mode.
    bool onlyPatches = ( SEL_FACE_COUNT() <= 0 && selected_brushes.next != &selected_brushes );
    if ( onlyPatches )
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( !b->patch ) { onlyPatches = false; break; }

    MaterialDef *md;
    if ( onlyPatches )
    {
        // patch-only selection → the shared g_patch_texdef template.  Populate its material:
        // single selected patch → Patch_GetTextureName; else copy the current-texture template.
        MaterialDef *tmpl = &g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].mtl;
        if ( QE_SingleBrush_Check() == 2 )   // KIWI: silent; the panel re-gathers on every change
            SetMaterial( (const char *)Patch_GetTextureName(), (patchMesh_material *)&g_patch_texdef );
        else
        {
            g_patch_texdef.mtlDef.lyrMtl = tmpl->lyrMtl;
            g_patch_texdef.mtlDef.radMtl = tmpl->radMtl;
        }
        md = &g_patch_texdef.mtlDef;
        out.bPatchMode = 1;
        g_patch_texdef.sample_size = GetBrushSampleSize();
    }
    else
    {
        md = Surf_TargetMaterialDef();
        out.bPatchMode = 0;
        g_patch_texdef.sample_size = ( g_bNewFace && SEL_FACE_COUNT() > 0 )
                                   ? GetBrushfaceSampleSize()
                                   : GetBrushSampleSize();
    }
    if ( !md || ( ( md->lyrMtl != nullptr ) + ( md->radMtl != nullptr ) ) != 1 )
        return false;   // no editable MaterialDef (uninitialized template) — leave fields as-is

    int width = 512, height = 512;
    Surf_MaterialWH( md, &width, &height );
    if ( width  <= 0 ) width  = 512;
    if ( height <= 0 ) height = 512;

    texdef_sub_t *td = &md->mat_texDef + LayerMat::GetCurrentLayer( md );
    if ( !out.bPatchMode )
        TexMatToFakeTexCoords( md, td );

    // Name: single selected patch → Patch_GetTextureName; else the MaterialDef name.
    const char *name;
    if ( out.bPatchMode && QE_SingleBrush_Check() == 2 )
        name = (const char *)Patch_GetTextureName();
    else
        name = (const char *)Materialdef_GetName( md );
    memset( out.currTex, 0, sizeof( out.currTex ) );
    strncpy( out.currTex, name ? name : "", sizeof( out.currTex ) - 1 );

    out.horzShift   = td->shift[0];
    out.vertShift   = td->shift[1];

    out.horzStretch = out.bPatchMode ? td->size[0] : td->size[0] / (float)width;
    out.repeatsX    = td->size[0];
    out.vertStretch = out.bPatchMode ? td->size[1] : td->size[1] / (float)height;
    out.repeatsY    = td->size[1];
    out.rotate      = td->rotate;
    out.sampleSize  = g_patch_texdef.sample_size;
    return true;
}

// Select_SetTexture_2 (0x4572d0) — WRITE the target texdef back into the dialog controls.
// Public (texwnd.cpp / map.cpp / Material_SetMode call it as Surf_RefreshFields).  Shift is
// shown in texels; "stretch" = size/width (size/height); rotate in degrees; sample-size the
// shared g_patch_texdef.sample_size ("" when 0).  Also fills the "Repeats in" readouts (raw size).
void Surf_RefreshFields()
{
    // NO-MFC: no-op; imgui_panel_surface.cpp re-gathers through SurfaceDlg_Gather after its
    // own actions and whenever Surf_UpdateInspector flags a change.
    return;
}

// SurfaceInspector::UpdateSurfaceDialog (0x458590) — the global "selection/texture changed"
// refresh, called from Brush_AddToList2 / Brush_RemoveFromList / drag apply etc.
// The binary also ran SetTexMods first and pushed to the texture bar afterwards.
// KIWI: SetTexMods is gone (see the texmod note at the top).
namespace SurfaceInspector
{
    void UpdateSurfaceDialog()
    {
        if ( surfDlgGlob.hwnd )
            Surf_RefreshFields();
        // NO-MFC: texture-bar push skipped — CTextureBar was an MFC CWnd (KIWI: deleted),
        // so there is nothing to push.
    }
}

// UI-independent action behind the inspector's Done/Apply commit.
// GetTexMods (0x458960) — commit any pending dialog edits to the selection.  Called on
// Done/Apply and after each spin.  m_texdefDirty (edit changed) → read controls into the texdef
// (GetMaterialDef) then Brush_SetTextureMapping (Undo-bracketed propagate to the selection);
// m_sampleDirty (sample-size changed) → Brush_SetSampleSize; then refresh.
void SurfaceDlg_Apply( surfaceDlgState_t &st )
{
    if ( !st.texdefDirty && !st.sampleDirty )
        return;
    MaterialDef *md = SurfaceDlg_StateToTexdef( st );
    int layer = md ? LayerMat::GetCurrentLayer( md ) : 0;
    if ( st.texdefDirty )
    {
        if ( md )
            Brush_SetTextureMapping( &md->mat_texDef + layer );
        st.texdefDirty = 0;
    }
    if ( st.sampleDirty )
    {
        if ( g_patch_texdef.sample_size != 0.0f )
        {
            Brush_SetSampleSize( (int)g_patch_texdef.sample_size );
            st.sampleDirty = 0;
            Surf_RefreshFields();
            return;
        }
        st.sampleDirty = 0;
    }
    Surf_RefreshFields();
}

// UI-independent action behind the inspector's spin-arrow nudges.
// UpdateSpinners (0x457cf0) — a spin arrow stepped the texdef.  The non-patch path (the common
// case) steps the current-layer texdef of SurfaceInspector_GetMaterialDef's target: stretch
// size[0]/size[1] ±8, shift[0]/shift[1] ±1, rotate ±45 (0..360 wrap), sample-size ±4 (min 4).
// The patch path builds a relative texdef_sub_t and Patch_SetTextureInfo's it.  Then refresh +
// Brush_SetTexture + clear the edit-dirty flag.
void SurfaceDlg_Spin( surfaceDlgState_t &st, int idFrom, bool up )
{
    if ( st.bPatchMode )
    {
        // patch path (Patch_SetTextureInfo relative transform).
        SurfaceDlg_StateToTexdef( st );   // commit current fields
        texdef_sub_t *pt = &g_patch_texdef.mtlDef.mat_texDef + LayerMat::GetCurrentLayer( &g_patch_texdef.mtlDef );
        texdef_sub_t rel; memset( &rel, 0, sizeof( rel ) );
        switch ( idFrom )
        {
            case IDC_SURFACE_INSP_ROTATE_SPIN:        rel.rotate  = up ?  pt->rotate  : -pt->rotate;  break;
            case IDC_SURFACE_INSP_HORZ_STRETCH_SPIN:  rel.size[0] = up ? 1.0f - pt->size[0] : pt->size[0] + 1.0f; break;
            case IDC_SURFACE_INSP_VERT_STRETCH_SPIN:  rel.size[1] = up ? 1.0f - pt->size[1] : pt->size[1] + 1.0f; break;
            case IDC_SURFACE_INSP_HORZ_SHIFT_SPIN:    rel.shift[0] = up ?  pt->shift[0] : -pt->shift[0]; break;
            case IDC_SURFACE_INSP_VERT_SHIFT_SPIN:    rel.shift[1] = up ?  pt->shift[1] : -pt->shift[1]; break;
            case IDC_SURFACE_INSP_SAMPLE_SIZE_SPIN:
            {
                if ( g_patch_texdef.sample_size == 0.0f )
                    break;
                float s = g_patch_texdef.sample_size + ( up ? 4.0f : -4.0f );
                if ( s < 4.0f ) s = 4.0f;
                g_patch_texdef.sample_size = s;
                Brush_SetSampleSize( (int)g_patch_texdef.sample_size );
                Surf_RefreshFields();
                st.sampleDirty = 0;
                return;
            }
        }
        MaterialDef *md = &g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].mtl;
        Patch_SetTextureInfo( &rel );
        Surf_RefreshFields();
        Brush_SetTexture( md, 0 );
        st.texdefDirty = 0;
        return;
    }

    // brush / face path — read the current control values into the target texdef FIRST,
    // then step that texdef by the delta.
    MaterialDef *md = SurfaceDlg_StateToTexdef( st );
    if ( !md )
        return;
    texdef_sub_t *td = &md->mat_texDef + LayerMat::GetCurrentLayer( md );
    switch ( idFrom )
    {
        case IDC_SURFACE_INSP_ROTATE_SPIN:
            td->rotate += up ? 45.0f : -45.0f;
            if ( td->rotate <    0.0f ) td->rotate += 360.0f;
            if ( td->rotate >= 360.0f ) td->rotate -= 360.0f;
            break;
        case IDC_SURFACE_INSP_HORZ_STRETCH_SPIN: td->size[0]  += up ?  8.0f : -8.0f; break;
        case IDC_SURFACE_INSP_VERT_STRETCH_SPIN: td->size[1]  += up ?  8.0f : -8.0f; break;
        case IDC_SURFACE_INSP_HORZ_SHIFT_SPIN:   td->shift[0] += up ?  1.0f : -1.0f; break;
        case IDC_SURFACE_INSP_VERT_SHIFT_SPIN:   td->shift[1] += up ?  1.0f : -1.0f; break;
        case IDC_SURFACE_INSP_SAMPLE_SIZE_SPIN:
        {
            if ( g_patch_texdef.sample_size == 0.0f )
                break;
            float s = g_patch_texdef.sample_size + ( up ? 4.0f : -4.0f );
            if ( s < 4.0f ) s = 4.0f;
            g_patch_texdef.sample_size = s;
            Brush_SetSampleSize( (int)g_patch_texdef.sample_size );
            Surf_RefreshFields();
            st.sampleDirty = 0;
            return;
        }
        default:
            break;
    }
    Surf_RefreshFields();
    Brush_SetTexture( md, 0 );
    st.texdefDirty = 0;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — the CSurfaceDlg handler bodies.  Each is a thin wrapper over the
//  Surface*_Apply / SurfaceDlg_* core above it (which imgui_panel_surface.cpp calls), so
//  the fences below interleave with those cores rather than wrapping one block.
// ══════════════════════════════════════════════════════════════════════════════

// UI-independent action behind CSurfaceDlg's "Fit" button.
// "Fit" (SurfaceInspector::OnFit 0x458DE0): fit the texture to the selected face(s) —
// Brush_FitTexture(1,1,0) computes the texdef so the texture tiles once across the face,
// then invalidate every view (camera re-projects via Face_MoveTexture) and re-read the fields.
void SurfaceFit_Apply()
{
    Brush_FitTexture( 1.0f, 1.0f, 0 );
    g_nUpdateBits = -1;
    Surf_RefreshFields();
}


// UI-independent action behind CSurfaceDlg's "Natural" button.
// "Natural" (SurfaceInspector::OnNaturalize 0x458C60): patch 1:1 natural projection at the
// layer's default sample size (Select_SetTexture).  No-op when no patch is selected
// (Patch_NaturalizeSelected scans for the first selected patch).
void SurfaceNaturalize_Apply()
{
    float x[2];
    Select_SetTexture( x );
    Patch_NaturalizeSelected( 0, 0, x[0], x[1] );
    g_nUpdateBits = -1;
}


// UI-independent action behind CSurfaceDlg's "CAP" button.
// "CAP" (SurfaceInspector::OnCap 0x458B40): patch cap texturing — scale the layer sample
// size by the tex-repeat X/Y fields, then Patch_NaturalizeSelected(unk=1).  Bails (matching
// the binary) if either tex-repeat reads 0.
void SurfaceCap_Apply( float xSize, float ySize )
{
    float v4[2];
    Select_SetTexture( v4 );
    if ( xSize != 0.0f )
    {
        xSize *= v4[0];
        if ( ySize != 0.0f )
        {
            ySize *= v4[1];
            Patch_NaturalizeSelected( 1, 0, xSize, ySize );
            g_nUpdateBits = -1;
        }
    }
}


// UI-independent action behind CSurfaceDlg's "Lmap" button.
// "Lmap" (SurfaceInspector::OnLightmap 0x458CA0): re-align the lightmap layer's texCoords
// across the selected patches (Patch_Lightmap_Texturing).
void SurfaceLightmap_Apply()
{
    Patch_Lightmap_Texturing();
    g_nUpdateBits = -1;
}


// UI-independent action behind CSurfaceDlg's "Set..." button.
// "Set..." (SurfaceInspector::OnSet 0x458CB0): read the tex-repeat X/Y + the terrain-distance
// (2D/3D/Curve) mode radio and texture the selected patches (Patch_SetTexturing).  Caches the
// chosen mode + repeats into g_qeglobals for the next open (OnInitDialog restores them).
void SurfaceSet_Apply( float v5, float v6, int checked )
{
    if ( checked )
        g_qeglobals.surfInsp_nIDButton = checked;
    g_qeglobals.surfInsp_tex_repeatx = (int)v5;
    g_qeglobals.surfInsp_tex_repeaty = (int)v6;

    switch ( checked )
    {
        case IDC_SURFACE_INSP_PATCH_2D:    Patch_SetTexturing( v5, v6, 0 ); break;
        case IDC_SURFACE_INSP_PATCH_3D:    Patch_SetTexturing( v5, v6, 1 ); break;
        case IDC_SURFACE_INSP_PATCH_CURVE: Patch_SetTexturing( v5, v6, 2 ); break;
        default: Sys_Printf( "Select a texturing mode for SET\n" ); break;
    }
    g_nUpdateBits = -1;
}


// The selection→inspector refresh hook (UpdateSurfaceDialog 0x458590) — refresh the edit
// fields (Select_SetTexture_2) whenever the inspector is open and the selection / map / a live
// texdef edit changes.  Called from CMainFrame::UpdateWindows (the invalidation broadcast) and
// after a map load (map.cpp).
bool g_surfInspStale;   // KIWI: consumed by ImGuiPanel_Surface_Draw

void Surf_UpdateInspector()
{
    // KIWI: flag the ImGui panel; it re-gathers on its next frame that has no typed,
    // unapplied edit (a stale field set used to spin/apply onto the NEW selection)
    g_surfInspStale = true;
}

// KIWI: silent twin of Surf_TargetMaterialDef's face asserts — false while the picked
// face's brush is mid-rebuild, so a polling gather waits a frame instead of asserting
bool SurfaceDlg_PickSettled()
{
    if ( !g_bNewFace || SEL_FACE_COUNT() <= 0 )
        return true;
    selface_t  &selFace = g_SelectedFaces.GetAt( 0 );
    selbrush_t *b = selFace.brush;
    if ( !b || !b->def )
        return true;    // Surf_TargetMaterialDef falls back to the template
    return (unsigned)selFace.index < (unsigned)b->faceCount
        && selFace.face == &b->faces[selFace.index]
        && b->version == b->def->version;
}
