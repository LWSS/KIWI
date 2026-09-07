#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Modeless patch vertex color/alpha editor. Apply writes enabled channels to
// selected control points and rebuilds each changed patch under Undo.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include "radiant_ui_actions.h"
#include <cstring>

extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ── externs (per-TU, matching pmesh.cpp / undo.cpp) ───────────────────────────
extern undo_s   *g_lastundo;                                   // 0x23F162C (undo.cpp)
extern void      Undo_ClearRedo();
extern void      Undo_GeneralStart( const char *operation );
extern void      Undo_AddBrushList( selbrush_t *list );
extern void      Undo_EndBrushList( selbrush_t *list );
extern void      Undo_End();
extern void      Patch_Rebuild( patchMesh_t *p, char doBounds );// 0x438D80 (pmesh.cpp)
extern int       Sys_Printf( const char *fmt, ... );           // 0x499E90
extern int       g_nUpdateBits;                                // 0x25D5A74 (engine_stubs.cpp)
// selected_brushes sentinel + g_qeglobals are declared in qe3.h.

// ═════════════════════════════════════════════════════════════════════════════
//  UI-independent action behind CVertEditDlg's [Apply] button.
//  THE CORE — 0x461210  CVertEditDlg::Apply (VertEditDlg_01)
//  Paint (R,G,B) and/or A onto the selected patch control points.
//    doColour → write vert_color.{r,g,b};  doAlpha → write vert_color.a.
// ═════════════════════════════════════════════════════════════════════════════
void VertEditDlg_Apply( const vertEditState_t &st )
{
    if ( !st.doColour && !st.doAlpha )
        return;
    // Do not nest a dialog edit inside an unfinished transform/paint record.
    if ( g_lastundo && !g_lastundo->done )
    {
        Sys_Printf( "Finish the current edit before applying vertex color.\n" );
        return;
    }
    bool undoStarted = false;

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

                const bool colourChanged = st.doColour &&
                    ( cp->vert_color.r != st.r || cp->vert_color.g != st.g || cp->vert_color.b != st.b );
                const bool alphaChanged = st.doAlpha && cp->vert_color.a != st.a;
                if ( !colourChanged && !alphaChanged )
                    continue;

                if ( !undoStarted )
                {
                    Undo_ClearRedo();
                    Undo_GeneralStart( "vertex color" );
                    // Snapshot the same list that EndBrushList stamps, including
                    // selected brushes without picked points, so undo retains them.
                    Undo_AddBrushList( &selected_brushes );
                    undoStarted = true;
                }
                changed = true;

                if ( st.doColour )                       // SendMessageA(this+600, BM_GETCHECK)
                {
                    cp->vert_color.b = st.b;             // this+120
                    cp->vert_color.g = st.g;             // this+124
                    cp->vert_color.r = st.r;             // this+128
                }
                if ( st.doAlpha )                        // SendMessageA(this+516, BM_GETCHECK)
                {
                    cp->vert_color.a = st.a;             // this+116
                }
            }

        if ( changed )                                   // 0x4611C8: rebuild touched patch
            Patch_Rebuild( patch, 1 );
    }

    if ( undoStarted )
    {
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
        g_nUpdateBits = -1;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CVertEditDlg, the hand-built modeless popup (CVehicleDlg / CModelDlg
//  pattern).  vertEditState_t + VertEditDlg_Apply above stay COMMON (the ImGui panel
//  imgui_panel_vertedit.cpp uses the shared state and calls the action).
// ══════════════════════════════════════════════════════════════════════════════

