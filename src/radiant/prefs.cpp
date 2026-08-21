// Radiant preferences, registry persistence, and settings dialog.
#include "stdafx.h"
#include "prefs.h"
#include "radiant_registry.h"   // Radiant_Profile* (was AfxGetApp()->*Profile* before U-SHIM removal)
#include <stdlib.h>   // atof

// ── the one editor-wide preference instance ──────────────────────────────────
// prefData_t's ctor sets the binary defaults at static-init, so g_PrefsDlg is a
// valid, defaults-loaded object before any view/drag/brush code runs (headless
// included) — which is what lets every reader drop its NULL guard.
static prefData_t s_radiantPrefs;
prefData_t *g_PrefsDlg = &s_radiantPrefs;

prefData_t::prefData_t()
{
    Prefs_SetDefaults( this );
}

// CPrefsDlg::CPrefsDlg (0x44d900) — the constructor's per-field default writes.
void Prefs_SetDefaults( prefData_t *p )
{
    p->m_nMouse_unsure        = 1;            // raw; m_nMouseButtons derived below
    p->m_nMouseButtons        = 3;            // (1!=0)+2
    p->m_nView                = 0;
    p->m_bTextureLock         = 1;
    p->m_bRotateLock          = 1;            // (LoadPrefs default; ctor leaves 0, Load wins)
    p->m_bLightmapLock        = 0;
    p->m_bLoadLast            = 1;
    p->m_bRunBefore           = 0;
    p->camera_mode            = 1;
    p->camera_masked          = 1;
    p->m_bFace                = 1;
    p->m_bRightClick          = 1;
    p->m_bAutoSave            = 1;
    p->m_bNewApplyHandling    = 0;
    p->m_bLoadLastMap         = 0;
    p->m_bTextureWindowSearch = 0;
    p->m_bCleanTinyBrushes    = 0;
    p->m_fTinySize            = 0.5f;
    p->m_nAutoSave            = 5;
    p->m_bSnapShots           = 0;
    p->loose_changes          = 0;
    p->m_nStatusSize          = 10;
    p->m_nMoveSpeed           = 350;
    p->m_nAngleSpeed          = 150;
    p->m_bCamXYUpdate         = 0;
    p->m_bALTEdge             = 1;
    p->m_bTextureBar          = 0;
    p->m_bSnapTToGrid         = 0;
    p->linking_keeps_selection= 0;
    p->m_bXZVis               = 0;
    p->m_bYZVis               = 0;
    p->m_bZVis                = 1;
    p->m_bSizePaint           = 1;
    p->m_dropHeight           = 28;           // ctor default (LoadPrefs key "DropHeight" 28)
    p->m_bForceZeroDropHeight = 0;            // ctor default (no registry key; OnDropSelected gate)
    p->m_bNoClamp             = 0;
    p->m_bDropModel           = 0;
    p->m_bOrientModel         = 0;
    p->m_nRotation            = 45;
    p->farplane               = 8192;         // ctor 0x2000
    p->tolerant_weld          = 24;
    p->vehicle_arrow_time     = 1000;
    p->vehicle_arrow_size     = 128;
    p->splay                  = 128;
    p->m_bChaseMouse          = 1;
    p->m_nEntityShowState     = 65552;        // ctor 65552 (LoadPrefs maps 0→65552)
    p->m_nTextureWindowScale  = 50;
    p->m_bTextureScrollbar    = 1;
    p->m_bSwitchClip          = 1;
    p->m_bSelectWholeEntities = 1;
    p->thick_selection_lines  = 1;
    p->m_bColoredEnts         = 0;
    p->m_bTolerantWeld        = 0;
    p->m_bVertSnapModel       = 0;
    p->m_bVertSnapBrush       = 0;
    p->m_bVertSnapPrefab      = 0;
    p->m_bSelectableModels    = 0;
    p->m_bSelectCurves        = 1;
    p->texture_brush_2d       = 0;
    p->texture_mesh_2d        = 0;
    p->fast_2d_view_dragging  = 1;
    p->detatch_windows        = 0;
    p->transparent_background = 0;
    p->m_nUndoLevels          = 10;
    p->patch_wireframe        = 0;
    p->g_bPatchWeld           = 1;
    p->patch_drill_down       = 1;
    p->entities_off           = 0;
    p->sky_brush_off          = 0;
    p->draw_toggle            = 0;
    p->scale_base             = 100;
    p->scale_range            = 30;
    p->camera_fov             = 65.0f;
    p->camera_use_wheel       = 1;            // ctor dword_25D6064 (LoadPrefs "CameraUseWheel" 1)
    p->model_origin_size      = 4.0f;
    p->prefab_origin_size     = 16.0f;
    p->enable_light_preview   = 1;            // ctor dword_25D6068 = 1
    p->preview_sun_aswell     = 0;            // ctor dword_25D606C = 0
    p->m_strLastProject       = "";
    p->m_strLastMap           = "";
    p->which_game             = "";
    p->ScriptGroupKey         = "script_group";
    p->ScriptGroupTokenKey    = "script_group_tokens";
    p->ScriptColorTeamKey     = "script_color_allies";
    p->ScriptColorKey         = "red";
    p->ScriptSubKey_key       = "script_objective_active";
    p->ScriptSubValue_key     = "";
    p->m_strUserIniPath       = "";
    p->m_strUserFilterPath    = "";
}

// CPrefsDlg::LoadPrefs (0x44e330). Section "Prefs" except RunBefore ("Internals").
void Prefs_LoadPrefs( prefData_t *p )
{
    p->m_nMouse_unsure        = Radiant_ProfileGetInt( "Prefs", "MouseButtons", 1 );
    p->m_nMouseButtons        = ( p->m_nMouse_unsure != 0 ) + 2;        // 2 or 3
    p->m_nView                = Radiant_ProfileGetInt( "Prefs", "QE4StyleWindows", 0 );
    p->m_bTextureLock         = Radiant_ProfileGetInt( "Prefs", "TextureLock", 1 );
    p->m_bRotateLock          = Radiant_ProfileGetInt( "Prefs", "RotateLock", 1 );
    p->m_bLightmapLock        = Radiant_ProfileGetInt( "Prefs", "LightmapLock", 0 );
    p->m_strLastProject       = Radiant_ProfileGetString( "Prefs", "LastProject", "" );
    p->m_strLastMap           = Radiant_ProfileGetString( "Prefs", "LastMap", "" );
    p->m_bLoadLast            = Radiant_ProfileGetInt( "Prefs", "LoadLast", 1 );
    p->m_bRunBefore           = Radiant_ProfileGetInt( "Internals", "RunBefore", 0 );
    p->camera_mode            = Radiant_ProfileGetInt( "Prefs", "CameraMode", 1 );
    p->camera_masked          = Radiant_ProfileGetInt( "Prefs", "CameraMasked", 1 );
    p->m_bFace                = Radiant_ProfileGetInt( "Prefs", "NewFaceGrab", 1 );
    p->m_bRightClick          = Radiant_ProfileGetInt( "Prefs", "NewRightClick", 1 );
    p->m_bAutoSave            = Radiant_ProfileGetInt( "Prefs", "Autosave", 1 );
    p->m_bNewApplyHandling    = Radiant_ProfileGetInt( "Prefs", "ApplyDismissesSurface", 0 );
    p->m_bLoadLastMap         = Radiant_ProfileGetInt( "Prefs", "LoadLastMap", 0 );
    p->m_bTextureWindowSearch = Radiant_ProfileGetInt( "Prefs", "NewTextureWindowStuff", 0 );
    p->m_bCleanTinyBrushes    = Radiant_ProfileGetInt( "Prefs", "CleanTinyBrushes", 0 );
    p->m_fTinySize            = (float)atof( Radiant_ProfileGetString( "Prefs", "CleanTinyBrusheSize", "0.5" ).c_str() );
    p->m_nAutoSave            = Radiant_ProfileGetInt( "Prefs", "AutosaveMinutes", 5 );
    p->m_bSnapShots           = Radiant_ProfileGetInt( "Prefs", "Snapshots", 0 );
    p->loose_changes          = Radiant_ProfileGetInt( "Prefs", "DefaultSaveNo", 0 );
    p->m_nStatusSize          = Radiant_ProfileGetInt( "Prefs", "StatusPointSize", 10 );
    p->m_nMoveSpeed           = Radiant_ProfileGetInt( "Prefs", "MoveSpeed", 350 );
    p->m_nAngleSpeed          = Radiant_ProfileGetInt( "Prefs", "AngleSpeed", 150 );
    p->m_bCamXYUpdate         = Radiant_ProfileGetInt( "Prefs", "CamXYUpdate", 0 );
    p->m_bALTEdge             = Radiant_ProfileGetInt( "Prefs", "ALTEdgeDrag", 1 );
    p->m_bTextureBar          = Radiant_ProfileGetInt( "Prefs", "UseTextureBar", 0 );
    p->which_game             = Radiant_ProfileGetString( "Prefs", "WhichGame", "" );
    p->m_bSnapTToGrid         = Radiant_ProfileGetInt( "Prefs", "SnapT", 0 );
    p->linking_keeps_selection= Radiant_ProfileGetInt( "Prefs", "LinkSelect", 0 );
    p->m_bXZVis               = Radiant_ProfileGetInt( "Prefs", "XZVIS", 0 );
    p->m_bYZVis               = Radiant_ProfileGetInt( "Prefs", "YZVIS", 0 );
    p->m_bZVis                = Radiant_ProfileGetInt( "Prefs", "ZVIS", 1 );
    p->m_bSizePaint           = Radiant_ProfileGetInt( "Prefs", "SizePainting", 1 );
    p->m_dropHeight           = Radiant_ProfileGetInt( "Prefs", "DropHeight", 28 );
    p->m_bNoClamp             = Radiant_ProfileGetInt( "Prefs", "NoClamp", 0 );
    p->m_bDropModel           = Radiant_ProfileGetInt( "Prefs", "DropModel", 0 );
    p->m_bOrientModel         = Radiant_ProfileGetInt( "Prefs", "OrientModel", 0 );
    p->ScriptGroupKey         = Radiant_ProfileGetString( "Prefs", "ScriptGroupKey", "script_group" );
    p->ScriptGroupTokenKey    = Radiant_ProfileGetString( "Prefs", "ScriptGroupTokenKey", "script_group_tokens" );
    p->ScriptColorTeamKey     = Radiant_ProfileGetString( "Prefs", "ScriptColorTeamKey", "script_color_allies" );
    p->ScriptColorKey         = Radiant_ProfileGetString( "Prefs", "ScriptColorKey", "red" );
    p->ScriptSubKey_key       = Radiant_ProfileGetString( "Prefs", "ScriptSubKey_key", "script_objective_active" );
    p->ScriptSubValue_key     = Radiant_ProfileGetString( "Prefs", "ScriptSubValue_key", "" );
    p->m_strUserIniPath       = Radiant_ProfileGetString( "Prefs", "UserINIPath", "" );
    p->m_strUserFilterPath    = Radiant_ProfileGetString( "Prefs", "UserFiltersPath", "" );
    p->m_nRotation            = Radiant_ProfileGetInt( "Prefs", "Rotation", 45 );
    p->farplane               = Radiant_ProfileGetInt( "Prefs", "Farplane", 8192 );
    p->tolerant_weld          = Radiant_ProfileGetInt( "Prefs", "TolerantWeldThreshold", 24 );
    p->vehicle_arrow_time     = Radiant_ProfileGetInt( "Prefs", "VehArrowTime", 1000 );
    p->vehicle_arrow_size     = Radiant_ProfileGetInt( "Prefs", "VehArrowSize", 128 );
    p->splay                  = Radiant_ProfileGetInt( "Prefs", "SplayDistance", 128 );
    p->m_bChaseMouse          = Radiant_ProfileGetInt( "Prefs", "ChaseMouse", 1 );
    p->m_nEntityShowState     = Radiant_ProfileGetInt( "Prefs", "EntityShow", 0 );
    if ( !p->m_nEntityShowState )
        p->m_nEntityShowState = 65552;
    p->m_nTextureWindowScale  = Radiant_ProfileGetInt( "Prefs", "TextureScale", 50 );
    p->m_bTextureScrollbar    = Radiant_ProfileGetInt( "Prefs", "TextureScrollbar", 1 );
    p->m_bSwitchClip          = Radiant_ProfileGetInt( "Prefs", "SwitchClipKey", 1 );
    p->m_bSelectWholeEntities = Radiant_ProfileGetInt( "Prefs", "SelectWholeEntitiesKey", 1 );
    p->thick_selection_lines  = Radiant_ProfileGetInt( "Prefs", "ThickLines", 1 );
    p->m_bColoredEnts         = Radiant_ProfileGetInt( "Prefs", "ColoredEnts", 0 );
    p->m_bTolerantWeld        = Radiant_ProfileGetInt( "Prefs", "TolerantWeld", 0 );
    p->m_bVertSnapModel       = Radiant_ProfileGetInt( "Prefs", "VertSnapModel", 0 );
    p->m_bVertSnapBrush       = Radiant_ProfileGetInt( "Prefs", "VertSnapBrush", 0 );
    p->m_bVertSnapPrefab      = Radiant_ProfileGetInt( "Prefs", "VertSnapPrefab", 0 );
    p->m_bSelectableModels    = Radiant_ProfileGetInt( "Prefs", "ModelSelection", 0 );
    p->m_bSelectCurves        = Radiant_ProfileGetInt( "Prefs", "SelectCurves", 1 );
    p->texture_brush_2d       = Radiant_ProfileGetInt( "Prefs", "2dTextured", 0 );
    p->texture_mesh_2d        = Radiant_ProfileGetInt( "Prefs", "2dMeshTextured", 0 );
    p->fast_2d_view_dragging  = Radiant_ProfileGetInt( "Prefs", "Fast2dDragging", 1 );
    p->detatch_windows        = Radiant_ProfileGetInt( "Prefs", "FloatingWindows", 0 );
    p->transparent_background = Radiant_ProfileGetInt( "Prefs", "TransparentBackground", 0 );
    p->m_nUndoLevels          = Radiant_ProfileGetInt( "Prefs", "UndoLevels", 10 );
    p->patch_wireframe        = Radiant_ProfileGetInt( "Prefs", "PatchWireframe", 0 );
    p->g_bPatchWeld           = Radiant_ProfileGetInt( "Prefs", "PatchWeld", 1 ) != 0;
    p->patch_drill_down       = Radiant_ProfileGetInt( "Prefs", "PatchDrillDown", 1 ) != 0;
    p->entities_off           = Radiant_ProfileGetInt( "Prefs", "EntitiesOff", 0 );
    p->sky_brush_off          = Radiant_ProfileGetInt( "Prefs", "SkyBrushOff", 0 );
    p->draw_toggle            = Radiant_ProfileGetInt( "Prefs", "DrawToggle", 0 );
    p->scale_base             = Radiant_ProfileGetInt( "Prefs", "ScaleBase", 100 );
    p->scale_range            = Radiant_ProfileGetInt( "Prefs", "ScaleRange", 30 );
    p->camera_fov             = (float)(unsigned int)Radiant_ProfileGetInt( "Prefs", "Fov", 65 );
    p->camera_use_wheel       = Radiant_ProfileGetInt( "Prefs", "CameraUseWheel", 1 );
    p->model_origin_size      = (float)(unsigned int)Radiant_ProfileGetInt( "Prefs", "ModelOrgSize", 4 );
    p->prefab_origin_size     = (float)(unsigned int)Radiant_ProfileGetInt( "Prefs", "PrefabOrgSize", 16 );
    p->enable_light_preview   = Radiant_ProfileGetInt( "Prefs", "LightPreviewEnable", 1 );
    p->preview_sun_aswell     = Radiant_ProfileGetInt( "Prefs", "SunLightPreviewEnable", 0 );
    // The binary re-reads VertSnap* under the Snap* keys (the later read wins).
    p->m_bVertSnapModel       = Radiant_ProfileGetInt( "Prefs", "SnapModel", 0 );
    p->m_bVertSnapBrush       = Radiant_ProfileGetInt( "Prefs", "SnapBrush", 0 );
    p->m_bVertSnapPrefab      = Radiant_ProfileGetInt( "Prefs", "SnapPrefab", 0 );
    // NOTE: the binary's "if (!RunBefore) SavePrefs()" first-run cascade is omitted
    // here to keep load side-effect-free (the GUI Save-on-OK / toggle handlers create
    // the keys on first change). Behaviour at defaults is identical.
}

// CPrefsDlg::SavePrefs (0x44f280) — clamp the few validated fields, then write all.
void Prefs_SavePrefs( prefData_t *p )
{
    Radiant_ProfileSetInt( "Prefs", "MouseButtons", p->m_nMouse_unsure );
    p->m_nMouseButtons = ( p->m_nMouse_unsure != 0 ) + 2;
    Radiant_ProfileSetInt( "Prefs", "QE4StyleWindows", p->m_nView );
    Radiant_ProfileSetInt( "Prefs", "TextureLock", p->m_bTextureLock );
    Radiant_ProfileSetInt( "Prefs", "RotateLock", p->m_bRotateLock );
    Radiant_ProfileSetInt( "Prefs", "LightmapLock", p->m_bLightmapLock );
    Radiant_ProfileSetInt( "Prefs", "LoadLast", p->m_bLoadLast );
    Radiant_ProfileSetString( "Prefs", "LastProject", p->m_strLastProject.c_str() );
    Radiant_ProfileSetString( "Prefs", "LastMap", p->m_strLastMap.c_str() );
    Radiant_ProfileSetInt( "Internals", "RunBefore", p->m_bRunBefore );
    Radiant_ProfileSetInt( "Prefs", "CameraMode", p->camera_mode );
    Radiant_ProfileSetInt( "Prefs", "CameraMasked", p->camera_masked );
    Radiant_ProfileSetInt( "Prefs", "NewFaceGrab", p->m_bFace );
    Radiant_ProfileSetInt( "Prefs", "NewRightClick", p->m_bRightClick );
    Radiant_ProfileSetInt( "Prefs", "Autosave", p->m_bAutoSave );
    Radiant_ProfileSetInt( "Prefs", "LoadLastMap", p->m_bLoadLastMap );
    Radiant_ProfileSetInt( "Prefs", "NewTextureWindowStuff", p->m_bTextureWindowSearch );
    Radiant_ProfileSetInt( "Prefs", "AutosaveMinutes", p->m_nAutoSave );
    Radiant_ProfileSetInt( "Prefs", "Snapshots", p->m_bSnapShots );
    Radiant_ProfileSetInt( "Prefs", "DefaultSaveNo", p->loose_changes );
    Radiant_ProfileSetInt( "Prefs", "StatusPointSize", p->m_nStatusSize );
    Radiant_ProfileSetInt( "Prefs", "CamXYUpdate", p->m_bCamXYUpdate );
    Radiant_ProfileSetInt( "Prefs", "MoveSpeed", p->m_nMoveSpeed );
    Radiant_ProfileSetInt( "Prefs", "AngleSpeed", p->m_nAngleSpeed );
    Radiant_ProfileSetInt( "Prefs", "ALTEdgeDrag", p->m_bALTEdge );
    Radiant_ProfileSetInt( "Prefs", "UseTextureBar", p->m_bTextureBar );
    Radiant_ProfileSetString( "Prefs", "WhichGame", p->which_game.c_str() );
    Radiant_ProfileSetInt( "Prefs", "SnapT", p->m_bSnapTToGrid );
    Radiant_ProfileSetInt( "Prefs", "LinkSelect", p->linking_keeps_selection );
    Radiant_ProfileSetInt( "Prefs", "XZVIS", p->m_bXZVis );
    Radiant_ProfileSetInt( "Prefs", "YZVIS", p->m_bYZVis );
    Radiant_ProfileSetInt( "Prefs", "ZVIS", p->m_bZVis );
    Radiant_ProfileSetInt( "Prefs", "SizePainting", p->m_bSizePaint );
    Radiant_ProfileSetInt( "Prefs", "DropHeight", p->m_dropHeight );
    Radiant_ProfileSetInt( "Prefs", "NoClamp", p->m_bNoClamp );
    Radiant_ProfileSetInt( "Prefs", "DropModel", p->m_bDropModel );
    Radiant_ProfileSetInt( "Prefs", "OrientModel", p->m_bOrientModel );
    Radiant_ProfileSetString( "Prefs", "ScriptGroupKey", p->ScriptGroupKey.c_str() );
    Radiant_ProfileSetString( "Prefs", "ScriptGroupTokenKey", p->ScriptGroupTokenKey.c_str() );
    Radiant_ProfileSetString( "Prefs", "ScriptColorTeamKey", p->ScriptColorTeamKey.c_str() );
    Radiant_ProfileSetString( "Prefs", "ScriptColorKey", p->ScriptColorKey.c_str() );
    Radiant_ProfileSetString( "Prefs", "ScriptSubKey_key", p->ScriptSubKey_key.c_str() );
    Radiant_ProfileSetString( "Prefs", "ScriptSubValue_key", p->ScriptSubValue_key.c_str() );
    Radiant_ProfileSetString( "Prefs", "UserINIPath", p->m_strUserIniPath.c_str() );
    Radiant_ProfileSetString( "Prefs", "UserFiltersPath", p->m_strUserFilterPath.c_str() );
    Radiant_ProfileSetInt( "Prefs", "Rotation", p->m_nRotation );
    Radiant_ProfileSetInt( "Prefs", "Farplane", p->farplane );
    Radiant_ProfileSetInt( "Prefs", "TolerantWeldThreshold", p->tolerant_weld );
    Radiant_ProfileSetInt( "Prefs", "VehArrowTime", p->vehicle_arrow_time );
    Radiant_ProfileSetInt( "Prefs", "VehArrowSize", p->vehicle_arrow_size );
    Radiant_ProfileSetInt( "Prefs", "SplayDistance", p->splay );
    Radiant_ProfileSetInt( "Prefs", "ModelSelection", p->m_bSelectableModels );
    // IDA SavePrefs 0x44f280 does NOT write "SelectCurves" here (LoadPrefs reads it, but the
    // binary never persists it — a latent quirk); matched by omitting the write.  The binary
    // instead re-writes "ModelSelection" a SECOND time later (after VertSnapPrefab — restored
    // below), so this was a swapped/invented write.
    Radiant_ProfileSetInt( "Prefs", "ChaseMouse", p->m_bChaseMouse );
    Radiant_ProfileSetInt( "Prefs", "EntityShow", p->m_nEntityShowState );
    Radiant_ProfileSetInt( "Prefs", "TextureScale", p->m_nTextureWindowScale );
    Radiant_ProfileSetInt( "Prefs", "TextureScrollbar", p->m_bTextureScrollbar );
    Radiant_ProfileSetInt( "Prefs", "SwitchClipKey", p->m_bSwitchClip );
    Radiant_ProfileSetInt( "Prefs", "SelectWholeEntitiesKey", p->m_bSelectWholeEntities );
    Radiant_ProfileSetInt( "Prefs", "ThickLines", p->thick_selection_lines );
    Radiant_ProfileSetInt( "Prefs", "ColoredEnts", p->m_bColoredEnts );
    Radiant_ProfileSetInt( "Prefs", "TolerantWeld", p->m_bTolerantWeld );
    Radiant_ProfileSetInt( "Prefs", "VertSnapModel", p->m_bVertSnapModel );
    Radiant_ProfileSetInt( "Prefs", "VertSnapBrush", p->m_bVertSnapBrush );
    Radiant_ProfileSetInt( "Prefs", "VertSnapPrefab", p->m_bVertSnapPrefab );
    Radiant_ProfileSetInt( "Prefs", "ModelSelection", p->m_bSelectableModels );  // IDA v78: binary writes ModelSelection a 2nd time here
    Radiant_ProfileSetInt( "Prefs", "2dTextured", p->texture_brush_2d );
    Radiant_ProfileSetInt( "Prefs", "2dMeshTextured", p->texture_mesh_2d );
    Radiant_ProfileSetInt( "Prefs", "Fast2dDragging", p->fast_2d_view_dragging );
    Radiant_ProfileSetInt( "Prefs", "FloatingWindows", p->detatch_windows );
    Radiant_ProfileSetInt( "Prefs", "TransparentBackground", p->transparent_background );
    Radiant_ProfileSetInt( "Prefs", "UndoLevels", p->m_nUndoLevels );
    Radiant_ProfileSetInt( "Prefs", "PatchWireframe", p->patch_wireframe );
    Radiant_ProfileSetInt( "Prefs", "PatchWeld", p->g_bPatchWeld );
    Radiant_ProfileSetInt( "Prefs", "PatchDrillDown", p->patch_drill_down );
    Radiant_ProfileSetInt( "Prefs", "DrawToggle", p->draw_toggle );
    Radiant_ProfileSetInt( "Prefs", "EntitiesOff", p->entities_off );
    Radiant_ProfileSetInt( "Prefs", "SkyBrushOff", p->sky_brush_off );
    Radiant_ProfileSetInt( "Prefs", "CameraUseWheel", p->camera_use_wheel );
    Radiant_ProfileSetInt( "Prefs", "ModelOrgSize", (int)p->model_origin_size );
    Radiant_ProfileSetInt( "Prefs", "PrefabOrgSize", (int)p->prefab_origin_size );
    Radiant_ProfileSetInt( "Prefs", "LightPreviewEnable", p->enable_light_preview );
    Radiant_ProfileSetInt( "Prefs", "SunLightPreviewEnable", p->preview_sun_aswell );
    Radiant_ProfileSetInt( "Prefs", "SnapModel", p->m_bVertSnapModel );
    Radiant_ProfileSetInt( "Prefs", "SnapBrush", p->m_bVertSnapBrush );
    Radiant_ProfileSetInt( "Prefs", "SnapPrefab", p->m_bVertSnapPrefab );
    if ( p->scale_base <= 1 )
        p->scale_base = 100;
    Radiant_ProfileSetInt( "Prefs", "ScaleBase", p->scale_base );
    if ( p->scale_base - p->scale_range <= 0 )
        p->scale_range = p->scale_base - 1;
    Radiant_ProfileSetInt( "Prefs", "ScaleRange", p->scale_range );
    if ( p->camera_fov < 2.0f )          p->camera_fov = 2.0f;
    else if ( p->camera_fov > 160.0f )   p->camera_fov = 160.0f;
    Radiant_ProfileSetInt( "Prefs", "Fov", (int)p->camera_fov );
}

void Prefs_Init( bool loadFromRegistry )
{
    // s_radiantPrefs is already defaults-constructed at static init; re-assert so a
    // re-init (or a future non-static instance) is well-defined.
    Prefs_SetDefaults( g_PrefsDlg );
    if ( loadFromRegistry )
        Prefs_LoadPrefs( g_PrefsDlg );
}

// One preferences-dialog control snapshot: every DDX-backed member of CPrefsDlg below
// (field name = the member minus m_).  The dialog exchanges it with prefData_t through
// PrefsDlg_Gather (load) and Prefs_ApplyFromDialogState (commit), so the field↔pref
// mapping is UI-free; the control ids / registry keys are annotated on the CPrefsDlg
// members + DoDataExchange.
struct prefsDlgState_t
{
    // Radio group indices (DDX_Radio): 0-based position of the checked button.
    int  rMouse;
    int  rView;
    // Checkboxes (BOOL).
    BOOL bLoadLast, bFace, bRightClick, bAutoSave, bLoadLastMap, bTexSubset;
    BOOL bSnapshots, bLoseChanges, bCamXYUpdate, bUseWheel, bAltAlwaysMove;
    BOOL bSnapTGrid, bLinkKeepSel, bPaintSizing, bDontClamp;
    BOOL bTexToolbar;
    BOOL bChaseMouse, bTexScrollbar, bThickLines, bColoredEnts, bTexBrush2d;
    BOOL bTexMesh2d, bFast2dDrag, bDetachWin, bTransBg;
    // Edit fields (ints / floats).
    int   nAutoSaveMin, nStatusSize, nRotation, nFarplane, nUndoLevels;
    int   nTolerantWeld, nSplay, nDropHeight, nScaleBase, nScaleRange;
    int   nVehArrowTime, nVehArrowSize;
    float fFov, fModelOrg, fPrefabOrg;
    std::string sUserIni, sUserFilters;   // was MFC CString before U-SHIM removal
};

// UI-independent load pass behind the dialog's OnInitDialog: prefData_t → control state.
void PrefsDlg_Gather( const prefData_t *p, prefsDlgState_t &out )
{
    out.rMouse         = ( p->m_nMouseButtons == 3 ) ? 1 : 0;
    out.rView          = p->m_nView;
    out.bLoadLast      = p->m_bLoadLast != 0;
    out.bFace          = p->m_bFace != 0;
    out.bRightClick    = p->m_bRightClick != 0;
    out.bAutoSave      = p->m_bAutoSave != 0;
    out.nAutoSaveMin   = p->m_nAutoSave;
    out.bLoadLastMap   = p->m_bLoadLastMap != 0;
    out.bTexSubset     = p->m_bTextureWindowSearch != 0;
    out.bSnapshots     = p->m_bSnapShots != 0;
    out.bLoseChanges   = p->loose_changes != 0;
    out.nStatusSize    = p->m_nStatusSize;
    out.bCamXYUpdate   = p->m_bCamXYUpdate != 0;
    out.bUseWheel      = p->camera_use_wheel != 0;
    out.bAltAlwaysMove = p->m_bALTEdge != 0;
    out.bTexToolbar    = p->m_bTextureBar != 0;
    out.bSnapTGrid     = p->m_bSnapTToGrid != 0;
    out.bLinkKeepSel   = p->linking_keeps_selection != 0;
    out.bPaintSizing   = p->m_bSizePaint != 0;
    out.bDontClamp     = p->m_bNoClamp != 0;
    out.sUserIni       = p->m_strUserIniPath;
    out.sUserFilters   = p->m_strUserFilterPath;
    out.nRotation      = p->m_nRotation;
    out.nFarplane      = p->farplane;
    out.nTolerantWeld  = p->tolerant_weld;
    out.nVehArrowTime  = p->vehicle_arrow_time;
    out.nVehArrowSize  = p->vehicle_arrow_size;
    out.nSplay         = p->splay;
    out.nDropHeight    = p->m_dropHeight;
    out.bChaseMouse    = p->m_bChaseMouse != 0;
    out.bTexScrollbar  = p->m_bTextureScrollbar != 0;
    out.bThickLines    = p->thick_selection_lines != 0;
    out.bColoredEnts   = p->m_bColoredEnts != 0;
    out.bTexBrush2d    = p->texture_brush_2d != 0;
    out.bTexMesh2d     = p->texture_mesh_2d != 0;
    out.bFast2dDrag    = p->fast_2d_view_dragging != 0;
    out.bDetachWin     = p->detatch_windows != 0;
    out.bTransBg       = p->transparent_background != 0;
    out.nUndoLevels    = p->m_nUndoLevels;
    out.nScaleBase     = p->scale_base;
    out.nScaleRange    = p->scale_range;
    out.fFov           = p->camera_fov;
    out.fModelOrg      = p->model_origin_size;
    out.fPrefabOrg     = p->prefab_origin_size;
}

// UI-independent action behind the dialog's OK: control state → prefData_t, then persist.
void Prefs_ApplyFromDialogState( prefData_t *p, const prefsDlgState_t &st )
{
    p->m_nMouse_unsure    = st.rMouse;                // raw registry value (0/1)
    p->m_nMouseButtons    = st.rMouse ? 3 : 2;
    p->m_nView            = st.rView;
    p->m_bLoadLast        = st.bLoadLast ? 1 : 0;
    p->m_bFace            = st.bFace ? 1 : 0;
    p->m_bRightClick      = st.bRightClick ? 1 : 0;
    p->m_bAutoSave        = st.bAutoSave ? 1 : 0;
    p->m_nAutoSave        = st.nAutoSaveMin;
    p->m_bLoadLastMap     = st.bLoadLastMap ? 1 : 0;
    p->m_bTextureWindowSearch = st.bTexSubset ? 1 : 0;
    p->m_bSnapShots       = st.bSnapshots ? 1 : 0;
    p->loose_changes      = st.bLoseChanges ? 1 : 0;
    p->m_nStatusSize      = st.nStatusSize;
    p->m_bCamXYUpdate     = st.bCamXYUpdate ? 1 : 0;
    p->camera_use_wheel   = st.bUseWheel ? 1 : 0;
    p->m_bALTEdge         = st.bAltAlwaysMove ? 1 : 0;
    p->m_bTextureBar      = st.bTexToolbar ? 1 : 0;
    p->m_bSnapTToGrid     = st.bSnapTGrid ? 1 : 0;
    p->linking_keeps_selection = st.bLinkKeepSel ? 1 : 0;
    p->m_bSizePaint       = st.bPaintSizing ? 1 : 0;
    p->m_bNoClamp         = st.bDontClamp ? 1 : 0;
    p->m_strUserIniPath   = st.sUserIni;
    p->m_strUserFilterPath= st.sUserFilters;
    p->m_nRotation        = st.nRotation;
    p->farplane           = st.nFarplane;
    p->tolerant_weld      = st.nTolerantWeld;
    p->vehicle_arrow_time = st.nVehArrowTime;
    p->vehicle_arrow_size = st.nVehArrowSize;
    p->splay              = st.nSplay;
    p->m_dropHeight       = st.nDropHeight;
    p->m_bChaseMouse      = st.bChaseMouse ? 1 : 0;
    p->m_bTextureScrollbar= st.bTexScrollbar ? 1 : 0;
    p->thick_selection_lines = st.bThickLines ? 1 : 0;
    p->m_bColoredEnts     = st.bColoredEnts ? 1 : 0;
    p->texture_brush_2d   = st.bTexBrush2d ? 1 : 0;
    p->texture_mesh_2d    = st.bTexMesh2d ? 1 : 0;
    p->fast_2d_view_dragging = st.bFast2dDrag ? 1 : 0;
    p->detatch_windows    = st.bDetachWin ? 1 : 0;
    p->transparent_background = st.bTransBg ? 1 : 0;
    p->m_nUndoLevels      = st.nUndoLevels;
    p->scale_base         = st.nScaleBase;
    p->scale_range        = st.nScaleRange;
    p->camera_fov         = st.fFov;
    p->model_origin_size  = st.fModelOrg;
    p->prefab_origin_size = st.fPrefabOrg;
    Prefs_SavePrefs( p );
}

// ─────────────────────────────────────────────────────────────────────────────
//  CPrefsDlg — the GUI editor for prefData_t (Edit→Preferences).
//
//  FULL FAITHFUL PORT (2026-07-03): uses the real dialog TEMPLATE 127
//  (IDD_COD4RADIANT_PREFERENCES, extracted from IW3xRadiant.exe into radiant.rc, font
//  normalized) and binds EVERY control the binary's CPrefsDlg::DoDataExchange (0x44de40)
//  binds, mapping each control id → its prefData_t field (field↔registry-key map from
//  CPrefsDlg::LoadPrefs 0x44e330 / SavePrefs 0x44f280).  A few controls drive parked
//  features but are still BOUND (persist their field) per the operator directive.
//  The 4-checkbox IDD_RADIANT_PREFS_MINI stand-in is retired.
// ─────────────────────────────────────────────────────────────────────────────
// U-GUARD: the class + its message map + Prefs_ShowDialog are MFC; everything above
// (prefData_t, Prefs_Load/SavePrefs, prefsDlgState_t, PrefsDlg_Gather,
// Prefs_ApplyFromDialogState) stays COMMON — imgui_panel_prefs.cpp calls the last two, and
// the AfxGetApp()/CString profile usage is exactly what kisak_mfc_shim.h covers, so those
// lines are deliberately NOT fenced.
