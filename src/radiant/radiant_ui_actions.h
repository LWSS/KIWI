#pragma once
// radiant_ui_actions.h — UI-rework shared declarations for the UI-independent
// action/read functions extracted from the MFC dialogs (Phase 1). Both shells
// call these: the MFC handlers (same-TU, mostly without this header) and the
// ImGui panels (imgui_panel_*.cpp, via this header). Definitions live in the
// dialog .cpp named in each comment; signatures must match exactly.
//
// Shared action snapshots belong here so panels and cores use one definition.

// verteditdlg.cpp: shared snapshot for the vertex-color Apply action.
struct vertEditState_t
{
    unsigned char r, g, b, a;
    bool doColour;
    bool doAlpha;
};
void VertEditDlg_Apply( const vertEditState_t &state );

// win_dlg.cpp
void FindBrush_Apply( int brushIdx, int entIdx );
void GoTo_Apply( const char *text );
void ArbRotate_Apply( float xDeg, float yDeg, float zDeg );

// ── mainfrm.cpp ───────────────────────────────────────────────────────────────
void CurveThicken_Apply( int amount, bool seam );        // Patch_Thicken owns the undo bracket
void SelectScale_Apply( float x, float y, float z );     // brackets + validations inside
void Radiant_ExecCommand( unsigned int cmdId );          // any menu/accel command id

// ── findtexture.cpp ───────────────────────────────────────────────────────────
void FindTexture_Apply( const char *find, const char *replace,
                        bool bSelectedOnly, bool bForce, bool bRecursePrefabs, bool bLive );
const char *FindTexture_GetCurrentMaterialName();

// ── dynentitydlg.cpp ──────────────────────────────────────────────────────────
void DynEntSetKey_Apply( const char *value, const char *key );
void DynEntClearKey_Apply( const char *key );
void DynEntSetType_Apply( const char *type );
const char *DynEntHelp_Gather();

// ── vehicledlg.cpp ────────────────────────────────────────────────────────────
void VehSetKey_Apply( const char *value, const char *key );
void VehSetAccuracy_Apply( const char *text );
void VehClearKey_Apply( const char *key );
void VehSetCrashType_Apply( int crashType );
void VehSetToggle_Apply( const char *value, const char *key );
void VehScriptGroup_Apply( const char *key );

// ── layersdlg.cpp ─────────────────────────────────────────────────────────────
bool LayerNew_Apply( const char *name, const char *parentLayer );
bool Layers_CanDeleteLayer( const char *layerName );
bool LayerDelete_Apply( const char *layerName );
bool LayerRename_Apply( const char *oldFull, const char *newLeaf );

// ── select.cpp ────────────────────────────────────────────────────────────────
void KeyValueSelect_Apply( const char *key, const char *value, bool keySubstr, bool valueSubstr );

// ── win_ent.cpp ───────────────────────────────────────────────────────────────
void EntSetKey_Apply( const char *key, const char *value );
void EntDeleteKey_Apply( const char *key );
void SpawnFlags_Apply( int flags );          // NOTE: no !edit_entity guard (faithful) — guard at call site
// imgui_shell.cpp — panels call this between Begin()/End() so a floating panel auto-closes
// when clicked off (and its show-bool resyncs, so one hotkey press reopens it).
void ImGuiShell_CloseOnFocusLoss( bool *p_open );

void EntAngle_Apply( int idx );              // 0..9: the angle-button switch.
                                             // NOTE: cases 8/9 (Up/Dn) deref edit_entity
                                             // unguarded (faithful) — guard at call site
