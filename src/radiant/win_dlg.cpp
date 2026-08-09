#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Find-brush, go-to-position, and arbitrary-rotation utility dialogs.
// Selection indices resolve entity instances to their definition brush lists.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include "xywnd.h"     // xywndState_t / Ed_ActiveXY (U-GLOBALS)
#include "radiant_registry.h"   // Radiant_Profile* (was AfxGetApp()->*Profile* before U-SHIM removal)
#include <cstdio>
#include <cstdlib>

// ── globals / cores reused from other TUs ─────────────────────────────────────
extern entity_s   entityInsts;                                  // 0x23F1748 (map.cpp) — instance list sentinel
extern entity_s  *world_entity;                                 // 0x25D5B30 (map.cpp)
extern int        Sys_Printf( const char *fmt, ... );           // 0x499E90
extern void       Assert( const char *file, int line, int type, const char *fmt, ... ); // 0x49CEA0
extern void       Select_Brush( selbrush_t *b, char overwrite, char status, char center ); // select.cpp (0x48DCC0)
extern void       Select_Deselect( int bDeselectFaces );        // select.cpp (0x48E800)
extern void       Select_GetMid( float *mid );                  // select.cpp (0x48FC70)
extern void       Select_RotateAxis( int axis, float deg, float (*rot_around)[4][3] ); // select.cpp (0x48FF40)
extern void       Select_ApplyMatrix_SelectedBrushes( int bSnap, float *mat, float deg, char bSwap ); // select.cpp (0x48FD10)
extern int        g_nUpdateBits;                                // 0x25D5A74 (engine_stubs.cpp)
extern camera_s   *Ed_Camera();                                 // camwnd.cpp — THE editor camera (never NULL)

extern void       Undo_ClearRedo();                             // undo.cpp (0x45DF20)
extern void       Undo_GeneralStart( const char *opName );      // undo.cpp (0x45E3F0)
extern void       Undo_AddBrushList( selbrush_t *list );        // undo.cpp (0x45E7C0)
extern void       Undo_EndBrushList( selbrush_t *list );        // undo.cpp (0x45E870)
extern void       Undo_End();                                   // undo.cpp (0x45EA20)
extern int        UpdateSelection( int wParam, eclass_t *cls ); // win_ent.cpp (0x497180)

// selected_brushes sentinel is declared in qe3.h.

// ══════════════════════════════════════════════════════════════════════════════
//  0x495550  Select_ByEntityNumber(brushIdx, entIdx) — select the entIdx-th entity
//  instance's brushIdx-th DEF brush, by number.  Walks entityInsts to the entIdx-th
//  entity, then that entity's DEF brush list to the brushIdx-th brush_t, then finds the
//  matching INSTANCE (selbrush_t whose .def == that brush_t) and Select_Brush()es it;
//  finally recenters the XY view + camera on the brush's bbox centre.  Status messages
//  go to the status bar exactly as the binary (Sys_Printf + the d_hwndStatus pane text).
// ══════════════════════════════════════════════════════════════════════════════
void Select_ByEntityNumber( int brushIdx, int entIdx )
{
    iassert(entityInsts.next == world_entity);

    // Walk to the entIdx-th entity instance.
    entity_s *ent = entityInsts.next;
    if ( ent == &entityInsts )
    {
        Sys_Printf( "Couldn't select brush by number: no such entity.\n" );
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0, (LPARAM)"No such entity." );
        return;
    }
    while ( entIdx )
    {
        ent = ent->next;
        --entIdx;
        if ( ent == &entityInsts )
        {
            Sys_Printf( "Couldn't select brush by number: no such entity.\n" );
            SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0, (LPARAM)"No such entity." );
            return;
        }
    }

    // Walk that entity's DEF brush list to the brushIdx-th brush_t def.
    entity_s_def *def = (entity_s_def *)ent->def;
    brush_t      *bdef = (brush_t *)def->brushes.prev;            // IDA brushes.oprev (+0x0C)
    void         *defSentinel = (void *)&def->def; // IDA &def->def (+0x08)
    if ( (void *)bdef == defSentinel )
    {
        Sys_Printf( "Couldn't select brush by number: no such brush in entity.\n" );
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0, (LPARAM)"No such brush." );
        return;
    }
    while ( brushIdx )
    {
        bdef = bdef->onext;
        --brushIdx;
        if ( (void *)bdef == defSentinel )
        {
            Sys_Printf( "Couldn't select brush by number: no such brush in entity.\n" );
            SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0, (LPARAM)"No such brush." );
            return;
        }
    }

    // Find the INSTANCE whose def == bdef (walk the entity's instance brush list).
    // selectedBrushInst/selectedEntityInst = the binary's locals.
    entity_s   *selectedEntityInst = ent;
    selbrush_t *selectedBrushInst  = ent->brushes.ownerNext;
    selbrush_t *&inst = selectedBrushInst;
    for ( ; ; inst = inst->ownerNext )
    {
        iassert( selectedBrushInst != &(selectedEntityInst->brushes) );   // win_dlg.cpp:113
        if ( inst->def == bdef )
            break;
    }

    Select_Brush( inst, 0, 1, 0 );
    g_nUpdateBits = -1;

    // Recenter XY view + camera on the brush's bbox centre.
    float cx = ( bdef->mins[0] + bdef->maxs[0] ) * 0.5f;
    float cy = ( bdef->mins[1] + bdef->maxs[1] ) * 0.5f;
    float cz = ( bdef->mins[2] + bdef->maxs[2] ) * 0.5f;
    // U-GLOBALS: Ed_ActiveXY() / Ed_Camera() are never NULL, so the shell guards fall away.
    {
        xywndState_t *xy = Ed_ActiveXY();
        xy->m_vOrigin[0] = cx;
        xy->m_vOrigin[1] = cy;
        xy->m_vOrigin[2] = cz;

        camera_s *cam = Ed_Camera();
        cam->origin[0] = cx;
        cam->origin[1] = cy;
        cam->origin[2] = cz;
    }
    SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0, (LPARAM)"Selected." );
}

// ══════════════════════════════════════════════════════════════════════════════
//  0x495700  GetSelectionIndex(&entOut, &brushOut) — report the entity# + brush# of the
//  first selected brush (the inverse of Select_ByEntityNumber, used to preload the Find
//  brush dialog).  Counts the selected instance's owner position in entityInsts (entOut)
//  and its def's position in the owner's DEF brush list (brushOut).  No selection → 0/0.
// ══════════════════════════════════════════════════════════════════════════════
void GetSelectionIndex( int *entOut, int *brushOut )
{
    selbrush_t *sb = selected_brushes.next;
    *entOut   = 0;
    *brushOut = 0;
    if ( sb == &selected_brushes )
        return;

    iassert(entityInsts.next == world_entity);

    // entity index = position of sb->owner in the instance list.
    for ( entity_s *iterEntityInst = entityInsts.next; ; iterEntityInst = iterEntityInst->next )
    {
        iassert(iterEntityInst != &entityInsts);
        if ( iterEntityInst == sb->owner )
            break;
        ++*entOut;
    }

    // brush index = position of sb->def in the owner's DEF brush list.
    entity_s_def *def = (entity_s_def *)sb->owner->def;
    void         *defSentinel = (void *)&def->def;
    for ( brush_t *j = (brush_t *)def->brushes.prev; ; j = j->onext )
    {
        if ( (void *)j == defSentinel )
            Assert( "C:\\trees\\cod3-pc\\cod3-modtools\\cod3src\\Radiant\\win_dlg.cpp",
                    175, 0, "%s", "iterBrushDef != &(selectedBrushInst->owner->def->brushes)" );
        if ( j == sb->def )
            break;
        ++*brushOut;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — shared hand-built-popup helpers (the CMapInfo / CVehicleDlg pattern).
//  AfxGetInstanceHandle / AfxGetMainWnd / CWnd::CreateEx / AfxRegisterWndClass are all
//  MFC; only the MFC dialog classes below use them, and so does the control-id enum.
// ══════════════════════════════════════════════════════════════════════════════

// UI-independent action behind CFindBrushDlg's Find button.
// FindBrushDlgProc OK (wParam==1): atol the brush# + ent# edits → Select_ByEntityNumber.
void FindBrush_Apply( int brushIdx, int entIdx )
{
    Select_ByEntityNumber( brushIdx, entIdx );
    g_nUpdateBits |= 1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CFindBrushDlg — Misc→Find brush (FindBrushDlgProc @0x4957E0).
//  The ImGui shell drives FindBrush_Apply above directly.
// ══════════════════════════════════════════════════════════════════════════════

// UI-independent action behind CGoToDlg's Go button.
// GoToDlgProc OK (a3==1): if the text is non-empty, zero camera origin then try the four
// coordinate formats in order (last 3-field match wins); Select_Deselect + PositionView.
void GoTo_Apply( const char *text )
{
    // U-GUARD-2 / NO-MFC: the `g_pParentWnd` half is a PORT guard ("the frame is up"), not the
    // binary's — GoToDlgProc never reads it.  There is no CMainFrame in this shell, and the
    // only things inside the block are Ed_Camera() (never NULL) + Select_Deselect, so the
    // text[0] test alone is the faithful gate.
    if ( text[0] )
    {
        // U-GLOBALS: Ed_Camera() is never NULL — the `if ( cam )` guard is gone.  The outer
        // g_pParentWnd guard STAYS: the PositionView() tail is still a CXYWnd method (U-CMD).
        {
            camera_s *cam = Ed_Camera();
            cam->origin[0] = 0.0f;
            cam->origin[1] = 0.0f;
            cam->origin[2] = 0.0f;

            float x, yv, z;
            if ( sscanf( text, "%f, %f, %f", &x, &yv, &z ) == 3 )
            { cam->origin[0] = x; cam->origin[1] = yv; cam->origin[2] = z; }
            if ( sscanf( text, "(%f, %f, %f)", &x, &yv, &z ) == 3 )
            { cam->origin[0] = x; cam->origin[1] = yv; cam->origin[2] = z; }
            if ( sscanf( text, "%f %f %f", &x, &yv, &z ) == 3 )
            { cam->origin[0] = x; cam->origin[1] = yv; cam->origin[2] = z; }
            if ( sscanf( text, "(%f %f %f)", &x, &yv, &z ) == 3 )
            { cam->origin[0] = x; cam->origin[1] = yv; cam->origin[2] = z; }
        }
        Select_Deselect( 1 );
        // NO-MFC: XY recenter skipped — CXYWnd::PositionView is still an MFC method (U-CMD
        // owns lifting it to a free fn); the camera origin write above still happens.
    }
    g_nUpdateBits |= 5;     // W_CAMERA | W_XY_OVERLAY
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CGoToDlg — Misc→Go to position (GoToDlgProc @0x495980).
// ══════════════════════════════════════════════════════════════════════════════

// Apply one typed angle about `axis` to the selection (the Brush→Rotate pattern: pivot →
// matrix → apply).  Mirrors sub_450D50's per-axis block (`0.0 != atof(text)` gate).
static void ArbRot_ApplyAxis( int axis, float deg )
{
    if ( deg == 0.0f )
        return;
    float rot_around[4][3];
    Select_GetMid( rot_around[0] );
    Select_RotateAxis( axis, deg, (float (*)[4][3])rot_around );
    Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], deg, 0 );
    g_nUpdateBits = -1;
}

// UI-independent action behind CArbRotateDlg's OK button.
// CRotateDlg OnOK (sub_450D50): UpdateData(TRUE) then atof X/Y/Z → per-axis rotate.  The
// undo bracket (CMainFrame::OnSelectionArbitraryrotation 0x425300) is applied here.
void ArbRotate_Apply( float xDeg, float yDeg, float zDeg )
{
    if ( selected_brushes.next == &selected_brushes )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "arbitrary rotation" );
    Undo_AddBrushList( &selected_brushes );

    ArbRot_ApplyAxis( 0, xDeg );
    ArbRot_ApplyAxis( 1, yDeg );
    ArbRot_ApplyAxis( 2, zDeg );

    UpdateSelection( -1, 0 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();

    g_nUpdateBits = -1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CArbRotateDlg + CAboutDlg.
//  CArbRotateDlg — Selection→Rotate→Arbitrary (CRotateDlg @0x450B10, OnOK = sub_450D50).
//  The CMainFrame handler (0x425300) undo-brackets around DoModal; since this popup is
//  modeless, the bracket lives in the OK handler instead (same captured state).
// ══════════════════════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════════════════════
//  DoColor (0x499350) — the editor's colour picker, wired to every Colors-menu item
//  and the K-accelerator (Select Entity Color).  Seeds a CColorDialog from
//  g_qeglobals.d_savedinfo.colors[n] (float[0,1] RGB → COLORREF), pops it modal, and on
//  OK converts the chosen COLORREF back to float[0,1] into colors[n] (with alpha = 1).
//
//  The binary is textbook MFC — CColorDialog IS a ChooseColorA wrapper, so this is a
//  1:1 port of the common-dialog approach (not a hand-built template).  What is custom:
//  the editor keeps its OWN 16-entry custom-colour array (dword_240A1E0), separate from
//  MFC's built-in CColorDialog::GetSavedCustomColors(), persisted to the registry
//  "Custom Colors" section (values "0".."15").  The dialog's m_cc.lpCustColors is pointed
//  at the editor's array (the CMyColorDialog ctor's `a2[33] = dword_240A1E0`), so edits to
//  the 16 custom swatches round-trip through the editor's own persisted array.
//
//  §11: the float→byte seed is `(int)(colors[n][c] * 255.0)` — _ftol2 truncation, matched
//  by C++ (int) cast.  The byte→float write-back multiplies by 1/255.
// ══════════════════════════════════════════════════════════════════════════════

// The editor's 16 custom colours (IDB dword_240A1E0 = live, dword_240A1A0 = last-persisted
// baseline).  s_customColorsLoaded (IDB byte_739B0C, initial 1) forces a one-shot registry
// load on the first DoColor call.  0xFFFFFF is the "unset" sentinel (IDB unk_FFFFFF).
static COLORREF s_customColorsLive[16];      // dword_240A1E0
static COLORREF s_customColorsSaved[16];     // dword_240A1A0
static bool     s_customColorsLoaded = false;// !byte_739B0C (binary: byte_739B0C==1 → not loaded)

// sub_498F90 — first-use loader: read the 16 custom colours from the registry into both the
// live and the baseline arrays, then clear the "needs load" flag.
static void CustomColors_LoadFromRegistry()
{
    for ( int i = 0; i < 16; ++i )
    {
        // U-GUARD rule 4: was `CString name; name.Format( "%d", i );` — CString::Format is
        // MFC-only (the shim has no Format), and this fn is COMMON (the registry swatch
        // store outlives the MFC picker), so the one line becomes a plain sprintf.  Same
        // bytes reach GetProfileInt/WriteProfile* in both shells.
        char name[16];
        sprintf( name, "%d", i );
        COLORREF c = (COLORREF)Radiant_ProfileGetInt( "Custom Colors", name, (int)0xFFFFFF );
        s_customColorsLive[i]  = c;
        s_customColorsSaved[i] = c;
    }
    s_customColorsLoaded = true;               // byte_739B0C = 0
}

// sub_499070 — after DoModal: write back any custom colours the user changed (live vs
// baseline diff) to the registry, updating the baseline.  0xFFFFFF entries are DELETED
// (WriteProfileString(...,NULL)) rather than written, matching the binary's unk_FFFFFF
// branch.  Runs regardless of OK/Cancel (the binary calls it before checking the result).
static void CustomColors_SaveToRegistry()
{
    for ( int i = 0; i < 16; ++i )
    {
        if ( s_customColorsLive[i] != s_customColorsSaved[i] )
        {
            // U-GUARD rule 4: CString::Format → sprintf (see CustomColors_LoadFromRegistry).
            char name[16];
            sprintf( name, "%d", i );
            if ( s_customColorsLive[i] == (COLORREF)0xFFFFFF )
                Radiant_ProfileSetString( "Custom Colors", name, NULL );
            else
                Radiant_ProfileSetInt( "Custom Colors", name, (int)s_customColorsLive[i] );
            s_customColorsSaved[i] = s_customColorsLive[i];
        }
    }
}

int DoColor( int index )
{
    // Seed the dialog from colors[index] (float[0,1] → COLORREF bytes).  (int) casts
    // truncate exactly like the binary's _ftol2.
    int r = (int)( g_qeglobals.d_savedinfo.colors[index][0] * 255.0 );
    int g = (int)( g_qeglobals.d_savedinfo.colors[index][1] * 255.0 );
    int b = (int)( 255.0 * g_qeglobals.d_savedinfo.colors[index][2] );
    COLORREF seed = ( (unsigned __int8)r ) | ( (unsigned __int8)g << 8 ) | ( (unsigned __int8)b << 16 );

    // First use: pull the editor's 16 custom swatches from the registry (CMyColorDialog ctor
    // gate on byte_739B0C).
    if ( !s_customColorsLoaded )
        CustomColors_LoadFromRegistry();

    // NO-MFC: CColorDialog is MFC-only, so DoColor behaves exactly like a Cancel here —
    // colors[index] untouched, returns 0; the swatch save still runs (the binary runs it
    // regardless of the dialog result).  An ImGui colour picker is a follow-up unit.
    (void)seed;
    CustomColors_SaveToRegistry();
    return 0;
}

