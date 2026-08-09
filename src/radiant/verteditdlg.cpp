#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Modeless patch vertex color/alpha editor. Apply writes enabled channels to
// selected control points and rebuilds each changed patch under Undo.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <cstring>

extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ── externs (per-TU, matching pmesh.cpp / undo.cpp) ───────────────────────────
extern undo_s   *g_lastundo;                                   // 0x23F162C (undo.cpp)
extern void      Undo_AddBrush( entity_brush_s *pBrushInst );  // 0x45E680 (undo.cpp)
extern void      Undo_AddEntity( int a1 );                     // 0x45E8B0 (undo.cpp)
extern void      Patch_Rebuild( patchMesh_t *p, char doBounds );// 0x438D80 (pmesh.cpp)
extern int       Sys_Printf( const char *fmt, ... );           // 0x499E90
extern int       g_nUpdateBits;                                // 0x25D5A74 (engine_stubs.cpp)
// selected_brushes sentinel + g_qeglobals are declared in qe3.h.

// The Undo bracket the binary opens on the FIRST recoloured control point of a patch
// (0x461210 inner: g_lastundo==0 ? "no last undo" : the Undo_AddBrushList idiom).  Sets the
// patch's xx22b "already-bracketed" flag so subsequent points on the same patch don't
// re-bracket.  Returns true once the patch is bracketed (so the apply marks it changed).
static void VED_BracketPatch( patchMesh_t *patch )
{
    if ( patch->xx22b )
        return;
    patch->xx22b = true;

    if ( !g_lastundo )
    {
        Sys_Printf( "Undo_AddBrush: no last undo.\n" );
        return;
    }
    if ( g_lastundo->entitylist.next != &g_lastundo->entitylist )
        Sys_Printf( "Undo_AddBrush: WARNING adding brushes after entity.\n" );

    entity_brush_s *pSymbiot = patch->pSymbiot;          // the symbiont brush DEF node
    entity_s_def   *owner    = (entity_s_def *)pSymbiot->owner;
    if ( *(int *)&owner->eclass->fixedsize )             // owner->eclass->fixedsize (eclass@0x60, fixedsize@0x08)
        Undo_AddEntity( (int)(intptr_t)owner );
    Undo_AddBrush( pSymbiot );
}

// One [Apply] pass snapshot: the four R/G/B/A slider values and the two enable check
// boxes the paint consults.
struct vertEditState_t
{
    byte r;               // IDC_VED_R_SLIDER   (binary this+128)
    byte g;               // IDC_VED_G_SLIDER   (binary this+124)
    byte b;               // IDC_VED_B_SLIDER   (binary this+120)
    byte a;               // IDC_VED_A_SLIDER   (binary this+116)
    bool doColour;        // IDC_VED_CHK_COLOR  (binary CButton @this+600)
    bool doAlpha;         // IDC_VED_CHK_ALPHA  (binary CButton @this+516)
};

// ═════════════════════════════════════════════════════════════════════════════
//  UI-independent action behind CVertEditDlg's [Apply] button.
//  THE CORE — 0x461210  CVertEditDlg::Apply (VertEditDlg_01)
//  Paint (R,G,B) and/or A onto the selected patch control points.
//    doColour → write vert_color.{r,g,b};  doAlpha → write vert_color.a.
// ═════════════════════════════════════════════════════════════════════════════
void VertEditDlg_Apply( const vertEditState_t &st )
{
    for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
    {
        patch_t *pInst = sb->patch;
        if ( !pInst )
            continue;

        // 0x46123C (type 0 → log+continue).  b = the binary's local.
        {
            selbrush_t *b = sb;
            iassert( b->patch->def == b->def->patch );   // VertEditDlg.cpp:199
        }

        patchMesh_t *patch = sb->def->patch;             // the DEF grid (== pInst->def)
        bool changed = false;

        for ( int col = 0; col < patch->width; ++col )
            for ( int row = 0; row < patch->height; ++row )
            {
                drawVert_t *cp = &patch->ctrl[col][row];
                if ( cp->turned_edge & 2 )               // 0x4C: turned edge → skip
                    continue;

                // Only picked (move-point) control points (0x461272: scan d_move_points).
                bool picked = false;
                for ( int m = 0; m < g_qeglobals.d_num_move_points; ++m )
                    if ( cp == g_qeglobals.d_move_points[m] ) { picked = true; break; }
                if ( !picked )
                    continue;

                if ( st.doColour )                       // SendMessageA(this+600, BM_GETCHECK)
                {
                    VED_BracketPatch( patch );
                    changed = true;
                    cp->vert_color.b = st.b;             // this+120
                    cp->vert_color.g = st.g;             // this+124
                    cp->vert_color.r = st.r;             // this+128
                }
                if ( st.doAlpha )                        // SendMessageA(this+516, BM_GETCHECK)
                {
                    VED_BracketPatch( patch );
                    changed = true;
                    cp->vert_color.a = st.a;             // this+116
                }
            }

        if ( changed )                                   // 0x4611C8: rebuild touched patch
            Patch_Rebuild( patch, 1 );
    }

    g_nUpdateBits = -1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CVertEditDlg, the hand-built modeless popup (CVehicleDlg / CModelDlg
//  pattern).  vertEditState_t + VertEditDlg_Apply above stay COMMON (the ImGui panel
//  imgui_panel_vertedit.cpp mirrors the struct and calls the action).
// ══════════════════════════════════════════════════════════════════════════════

