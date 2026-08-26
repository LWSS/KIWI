#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// KIWI command metadata, registration, modal lifecycle, and input arbitration.
// g_radiantCommands remains the binding source of truth; feature modules own their
// handlers and this file only routes to them.
#include "stdafx.h"
#include "qe3.h"

#include "kiwi_command.h"
#include "radiant_frame.h"
#include "kiwi_addmenu.h"                // §16b the Shift+A add menu
#include "kiwi_bevel.h"
#include "kiwi_boolean.h"
#include "kiwi_camera.h"                 // KiwiCam_FlySwallowKey (the fly-key funnel rung)
#include "kiwi_construct.h"
#include "kiwi_csg.h"
#include "kiwi_dupe.h"
#include "kiwi_extrude.h"
#include "kiwi_fillet.h"
#include "kiwi_patchfillet.h"
#include "kiwi_focus.h"
#include "kiwi_lines.h"
#include "kiwi_loft.h"
#include "kiwi_offset.h"
#include "kiwi_visibility.h"
#include "kiwi_conclip.h"
#include "kiwi_conselect.h"
#include "kiwi_join.h"
#include "kiwi_autobool.h"
#include "kiwi_matchface.h"
#include "kiwi_trim.h"
#include "kiwi_split.h"
#include "kiwi_selext.h"
#include "kiwi_numeric.h"
#include "kiwi_palette.h"
#include "kiwi_patchverts.h"
#include "kiwi_pick.h"
#include "kiwi_primitive.h"              // §16b Box / Cylinder / Sphere / Cone
#include "kiwi_region.h"
#include "kiwi_selconv.h"
#include "kiwi_selection.h"
#include "kiwi_snap.h"
#include "kiwi_transform.h"
#include "kiwi_units.h"
#include "kiwi_ux.h"                     // KiwiUX_ModernInput (the paste/clone hook's gate)
#include "kiwi_undo.h"
#include "kiwi_uv.h"
#include "kiwi_entbrowser.h"
#include "kiwi_modelbrowser.h"          // KIWI: the Models window + deferred drop
#include "kiwi_skybox.h"
#include "kiwi_uveditor.h"
#include "kiwi_import.h"
#include "kiwi_launch.h"
#include "kiwi_caulk.h"
#include "kiwi_sun.h"                   // the sun helper — Place Sun + the Esc deselect
#include "kiwi_light.h"                 // selected-light helper window registration
#include "kiwi_section.h"
#include "kiwi_outliner.h"
#include "kiwi_windows.h"
#include "kiwi_plastbridge.h"

#include <string.h>

// Ported entry points. Keep address anchors synchronized with their definitions.
extern int  Sys_Printf( const char *fmt, ... );                  // win_qe3.cpp:118 (undo.cpp:57 declares it the same way)
extern void Undo_ClearRedo();                                    // undo.cpp 0x45e2b0
extern void Undo_AddBrush( entity_brush_s *pBrushInst );         // undo.cpp:494  (0x45E680)
extern void Undo_AddEntity( int a1 );                            // undo.cpp:601  (0x45E8B0)
extern void Undo_GeneralStart( const char *operation );          // undo.cpp 0x45e3f0
extern void Undo_AddBrushList( selbrush_t *sb );                 // undo.cpp 0x45e7c0
extern void Undo_EndBrushList( selbrush_t *brushlist );          // undo.cpp 0x45e870
extern void Undo_End();                                          // undo.cpp 0x45ea20
extern void Undo_Undo();                                         // undo.cpp 0x45ea90
extern int  g_nUpdateBits;                                       // 0x25D5A74 (mainfrm.cpp)
extern bool ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );   // imgui_shell.cpp

extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // Exactly one active command and one undo bracket are owned here.
    KiwiEditorCommand *g_activeCommand = nullptr;
    snap_result_t      s_lastSnap;
    bool               s_undoOpen = false;

    // HOT receives MouseMove; PAUSED preserves the preview until bare LMB resumes.
    bool s_hot = true;

    // Commit drains a requested handoff only after undo close and numeric reset.
    int  s_deferId    = 0;
    bool s_deferPause = false;

    // Clear first because KiwiCmd_Start may re-enter the command layer.
    void DrainDeferred()
    {
        const int  id     = s_deferId;
        const bool paused = s_deferPause;
        s_deferId = 0;                        // cleared FIRST: Start may re-enter
        if ( !id || g_activeCommand )
            return;
        if ( KiwiCmd_Start( id ) && paused )
            KiwiCmd_Pause();
    }

    // Deselect only after EndBrushList captures the selection's AFTER state.
    bool s_deferDeselect = false;

    void DrainDeferredDeselect()
    {
        if ( !s_deferDeselect )
            return;
        s_deferDeselect = false;
        Sel_Clear( KiwiSel() );
        Sel_SyncToLegacy();
        g_nUpdateBits = -1;
    }

    // Cursor and Shift are latched together for screen mappings and click hooks.
    int  s_cursorX    = 0;
    int  s_cursorY    = 0;
    bool s_cursorHave = false;
    bool s_lastShift  = false;

    // Grid bounds follow the engine world limit; typed values and ladder steps share them.
    const float KGRID_MIN_INCHES = 0.125f;
    const float KGRID_MAX_INCHES = 65536.0f;

    void SetModeMask( sel_mask_t m )
    {
        KiwiSel_SetModeMask( m );
        g_nUpdateBits |= 1;
    }

    // Nice-number ladder. Off-ladder values step to the nearest rung on the requested side.
    const float KCMD_GRID_LADDER[] =
    {
        0.125f, 0.25f, 0.5f,
        1.0f, 2.0f, 4.0f, 5.0f, 8.0f, 10.0f, 16.0f, 20.0f, 25.0f, 32.0f, 50.0f,
        64.0f, 100.0f, 128.0f, 200.0f, 256.0f, 500.0f, 512.0f, 1000.0f, 1024.0f,
        2000.0f, 2048.0f, 4096.0f, 5000.0f, 8192.0f, 10000.0f, 16384.0f,
        32768.0f, 65536.0f,
    };
    const int KCMD_GRID_LADDER_COUNT =
        (int)( sizeof( KCMD_GRID_LADDER ) / sizeof( KCMD_GRID_LADDER[0] ) );

    // Relative epsilon keeps rung comparisons proportional across the full range.
    void StepGrid( bool doubleIt )
    {
        const float cur = KiwiUnits_GridSpacingInches();
        const float eps = cur * 1.0e-3f;

        float s = cur;
        if ( doubleIt )
        {
            s = KCMD_GRID_LADDER[KCMD_GRID_LADDER_COUNT - 1];
            for ( int i = 0; i < KCMD_GRID_LADDER_COUNT; ++i )
                if ( KCMD_GRID_LADDER[i] > cur + eps ) { s = KCMD_GRID_LADDER[i]; break; }
        }
        else
        {
            s = KCMD_GRID_LADDER[0];
            for ( int i = KCMD_GRID_LADDER_COUNT - 1; i >= 0; --i )
                if ( KCMD_GRID_LADDER[i] < cur - eps ) { s = KCMD_GRID_LADDER[i]; break; }
        }

        if ( s < KGRID_MIN_INCHES ) s = KGRID_MIN_INCHES;
        if ( s > KGRID_MAX_INCHES ) s = KGRID_MAX_INCHES;
        KiwiUnits_SetGridSpacingInches( s );
        g_nUpdateBits = -1;
        Sys_Printf( "grid spacing %g in\n", (double)KiwiUnits_GridSpacingInches() );
    }

    // Only verbs about an unmoved auto-entered selection may cancel and replace it.
    // Creation and selection-conversion ids are explicit; arbitrary hotkeys stay swallowed.
    bool PreemptVerb( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_SPLIT_FACE:
        case KIWI_CMD_MATCH_FACE:
        case KIWI_CMD_EXTRUDE_FACE:
        case KIWI_CMD_JOIN:
        case KIWI_CMD_CUT:
        case KIWI_CMD_BOOLEAN:
        case KIWI_CMD_LOFT:
        case KIWI_CMD_SELCONV_POINT:
        case KIWI_CMD_SELCONV_EDGE:
        case KIWI_CMD_SELCONV_FACE:
        case KIWI_CMD_SELCONV_OBJECT:
        case KIWI_CMD_DRAW_LINE:
        case KIWI_CMD_DRAW_POLYLINE:
        case KIWI_CMD_DRAW_RECT:
        case KIWI_CMD_DRAW_RECT_CENTER:
        case KIWI_CMD_DRAW_CIRCLE:
        case KIWI_CMD_DRAW_CIRCLE_2PT:
        case KIWI_CMD_DRAW_ARC:
        case KIWI_CMD_DRAW_POLYGON:
        case KIWI_CMD_DRAW_SPLINE:
        case KIWI_CMD_PRIM_BOX:
        case KIWI_CMD_PRIM_BOX_CENTER:
        case KIWI_CMD_PRIM_CYLINDER:
        case KIWI_CMD_PRIM_SPHERE:
        case KIWI_CMD_PRIM_CONE:
        case KIWI_CMD_ADD_MENU:
            return true;
        default:
            return false;
        }
    }

    // Subset that may arm a selected-face construction plane for this start only.
    // Keep separate from PreemptVerb: cancellation and plane arming are different gates.
    bool CreationVerb( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_DRAW_LINE:
        case KIWI_CMD_DRAW_POLYLINE:
        case KIWI_CMD_DRAW_RECT:
        case KIWI_CMD_DRAW_RECT_CENTER:
        case KIWI_CMD_DRAW_CIRCLE:
        case KIWI_CMD_DRAW_CIRCLE_2PT:
        case KIWI_CMD_DRAW_ARC:
        case KIWI_CMD_DRAW_POLYGON:
        case KIWI_CMD_DRAW_SPLINE:
        case KIWI_CMD_PRIM_BOX:
        case KIWI_CMD_PRIM_BOX_CENTER:
        case KIWI_CMD_PRIM_CYLINDER:
        case KIWI_CMD_PRIM_SPHERE:
        case KIWI_CMD_PRIM_CONE:
        case KIWI_CMD_ADD_MENU:
            return true;
        default:
            return false;
        }
    }

    // Live tool swaps are restricted to G/R/S over the surviving selection.
    bool SwapVerb( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_MOVE:
        case KIWI_CMD_ROTATE:
        case KIWI_CMD_SCALE:
            return true;
        default:
            return false;
        }
    }

    // Copy and Delete Selection both require at least one selected brush.
    bool CanClipCut()
    {
        return selected_brushes.next != &selected_brushes;
    }

    // First exact binding match, mirroring Radiant_TryHotkey table order.
    int LookupBinding( unsigned int vk, unsigned int mods )
    {
        RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTableMutable( &table );
        if ( !table || count <= 0 )
            return 0;
        for ( int i = 0; i < count; ++i )
            if ( table[i].vk == vk && table[i].mods == mods )
                return table[i].commandId;
        return 0;
    }

    bool Can_HaveSelection()
    {
        return !KiwiSel().items.empty();
    }

    // Palette side table only: no bindings or handlers. selKindMask is declarative;
    // canExecute controls greying, and NULL means always available.
    const struct { int id; kiwiCommandInfo_t info; } KCMD_META[] =
    {
        { KIWI_CMD_SELMODE_POINT,  { "Select Mode: Point",        "Selection", SEL_MASK_VERTEX,     nullptr } },
        { KIWI_CMD_SELMODE_EDGE,   { "Select Mode: Edge",         "Selection", SEL_MASK_EDGE,       nullptr } },
        { KIWI_CMD_SELMODE_FACE,   { "Select Mode: Face",         "Selection", SEL_MASK_FACE,       nullptr } },
        { KIWI_CMD_SELMODE_OBJECT, { "Select Mode: Object",       "Selection", SEL_MASK_OBJECT,     nullptr } },
        { KIWI_CMD_SELMODE_ALL,    { "Select Mode: Everything",   "Selection", SEL_MASK_EVERYTHING, nullptr } },
        { KIWI_CMD_PALETTE,        { "Command Palette",           "KIWI",      0,                   nullptr } },
        { KIWI_CMD_GRID_HALVE,     { "Grid Spacing: Halve",       "Grid",      0,                   nullptr } },
        { KIWI_CMD_GRID_DOUBLE,    { "Grid Spacing: Double",      "Grid",      0,                   nullptr } },
        { KIWI_CMD_SNAP_TOGGLE,    { "Snap Markers: Toggle",      "Grid",      0,                   nullptr } },
        { KIWI_CMD_SELFTEST,       { "UX: Modal Self-Test",       "KIWI",      0,                   nullptr } },
        { KIWI_CMD_PLASTICITY_PUSH,{ "Send Selection to Plasticity", "Export", SEL_MASK_OBJECT,     KiwiPlastBridge_CanPush } },

        { KIWI_CMD_MOVE,   { "Move (G)",   "Transform", SEL_MASK_EVERYTHING, KiwiXform_CanMove   } },
        { KIWI_CMD_ROTATE, { "Rotate (R)", "Transform", SEL_MASK_OBJECT,     KiwiXform_CanRotate } },
        { KIWI_CMD_SCALE,  { "Scale (S)",  "Transform", SEL_MASK_OBJECT,     KiwiXform_CanScale  } },

        { KIWI_CMD_DRAW_LINE,       { "Construct: Line (chained curve)", "Construct", 0, 0                 } },
        { KIWI_CMD_DRAW_POLYLINE,   { "Construct: Polyline (= Line)",    "Construct", 0, 0                 } },
        { KIWI_CMD_DRAW_RECT,       { "Construct: Rectangle",        "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_CIRCLE,     { "Construct: Circle",           "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_ARC,        { "Construct: Arc",              "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_RECT_CENTER,{ "Construct: Rectangle (center)","Construct",0, 0                     } },
        { KIWI_CMD_DRAW_CIRCLE_2PT, { "Construct: Circle (2-point)", "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_POLYGON,    { "Construct: Polygon (n-gon)",  "Construct", 0, 0                     } },
        { KIWI_CMD_DRAW_SPLINE,     { "Construct: Spline",           "Construct", 0, 0                     } },
        { KIWI_CMD_PRIM_BOX,        { "Box (corner)",                "Solids",    0, nullptr               } },
        { KIWI_CMD_PRIM_BOX_CENTER, { "Box (centre)",                "Solids",    0, nullptr               } },
        { KIWI_CMD_PRIM_CYLINDER,   { "Cylinder",                    "Solids",    0, nullptr               } },
        { KIWI_CMD_PRIM_SPHERE,     { "Sphere",                      "Solids",    0, nullptr               } },
        { KIWI_CMD_PRIM_CONE,       { "Cone",                        "Solids",    0, nullptr               } },
        { KIWI_CMD_ADD_MENU,        { "Add Menu (create)",           "Construct", 0, nullptr               } },
        { KIWI_CMD_VIEW_SHOW_GRID,  { "Show Grid",                   "View",      0, nullptr               } },
        { KIWI_CMD_VIEW_SHOW_AXES,  { "Show Axes",                   "View",      0, nullptr               } },
        { KIWI_CMD_VIEW_ORTHO,      { "Toggle Orthographic Camera",  "View",      0, nullptr               } },
        { KIWI_CMD_EXTRUDE_REGION,  { "Extrude Region",              "Construct", 0, KiwiExtrude_CanExecute } },
        { KIWI_CMD_CUT,             { "Cut (along a line)",          "Modeling", SEL_MASK_OBJECT, KiwiSplit_CanCut } },
        { KIWI_CMD_BOOLEAN,         { "Boolean (difference / union)","Modeling", SEL_MASK_OBJECT, KiwiBool_CanBoolean } },
        { KIWI_CMD_LOFT,            { "Loft (bridge two faces)",     "Modeling", SEL_MASK_FACE,   KiwiLoft_CanExecute } },
        { KIWI_CMD_MATCH_FACE,      { "Match Face",                  "Modeling", SEL_MASK_FACE,   KiwiMatch_CanMatch } },
        { KIWI_CMD_SPLIT_FACE,      { "Split Brush at Face (Ctrl+R)", "Modeling", SEL_MASK_FACE,  KiwiSplit_CanSplitFace } },
        { KIWI_CMD_EXTRUDE_FACE,    { "Extrude Face (new body)",     "Modeling", SEL_MASK_FACE,   KiwiExtrudeFace_CanExecute } },
        { KIWI_CMD_JOIN,            { "Join (faces / lines)",        "Modeling", 0,               KiwiJoin_CanJoin } },
        { KIWI_CMD_AUTO_BOOL,       { "Auto Bool (consolidate brushes)", "Modeling", SEL_MASK_OBJECT, KiwiAutoBool_CanExecute } },
        { KIWI_CMD_TRIM,            { "Trim (lines)",                "Construct", 0,               KiwiTrim_CanTrim } },
        { KIWI_CMD_OFFSET_CURVE,    { "Offset Curve",                "Construct", 0,               KiwiOffset_CanOffset } },
        { KIWI_CMD_FILLET_CURVE,    { "Fillet Corners",              "Construct", 0,               KiwiFillet_CanFillet } },
        { KIWI_CMD_FILLET_EDGE,     { "Bevel / Fillet Edge (D toggles)", "Modeling", SEL_MASK_EDGE, KiwiPatchFillet_CanFillet } },
        { KIWI_CMD_CPLANE_XY,       { "Construction Plane: XY",      "Construct", 0, nullptr               } },
        { KIWI_CMD_CPLANE_XZ,       { "Construction Plane: XZ",      "Construct", 0, nullptr               } },
        { KIWI_CMD_CPLANE_YZ,       { "Construction Plane: YZ",      "Construct", 0, nullptr               } },
        { KIWI_CMD_CPLANE_FACE,     { "Construction Plane: From Face","Construct",0, nullptr               } },
        { KIWI_CMD_CPLANE_VIEW,     { "Construction Plane: From View","Construct",0, nullptr               } },
        { KIWI_CMD_CONSTRUCT_CLEAR, { "Construction: Clear All",     "Construct", 0, KiwiCon_HasObjects    } },
        { KIWI_CMD_CONSTRUCT_UNDO,  { "Undo (unified timeline)",     "Construct", 0, nullptr               } },

        { KIWI_CMD_BEVEL_EDGE,      { "Bevel Edge (chamfer)",        "Modeling", SEL_MASK_EDGE,   KiwiBevel_CanBevel } },
        { KIWI_CMD_INSET_FACE,      { "Inset Face (clone)",          "Modeling", SEL_MASK_FACE,   KiwiBevel_CanInset } },
        { KIWI_CMD_ARRAY_LINEAR,    { "Array (Linear)",              "Duplicate", SEL_MASK_OBJECT, KiwiDupe_CanArray } },
        { KIWI_CMD_ARRAY_RADIAL,    { "Array (Radial)",              "Duplicate", SEL_MASK_OBJECT, KiwiDupe_CanArray } },
        { KIWI_CMD_DUPLICATE,       { "Duplicate (and move)",        "Duplicate", SEL_MASK_OBJECT, KiwiDupe_CanDuplicate } },

        { KIWI_CMD_SELECT_COPLANAR, { "Select Coplanar Faces",       "Selection", SEL_MASK_FACE,   KiwiSelExt_CanCoplanar } },
        { KIWI_CMD_SELECT_TOUCHING, { "Select Touching",             "Selection", 0,               KiwiSelExt_CanTouching } },
        { KIWI_CMD_SELECT_MATERIAL, { "Select Same Material",        "Selection", 0,               KiwiSelExt_CanMaterial } },
        { KIWI_CMD_SELECT_CONNECTED,{ "Select Connected (touching)", "Selection", 0,               KiwiSelExt_CanConnected } },

        { KIWI_CMD_SELCONV_POINT,   { "Convert Selection to Points",  "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },
        { KIWI_CMD_SELCONV_EDGE,    { "Convert Selection to Edges",   "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },
        { KIWI_CMD_SELCONV_FACE,    { "Convert Selection to Faces",   "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },
        { KIWI_CMD_SELCONV_OBJECT,  { "Convert Selection to Objects", "Selection", SEL_MASK_EVERYTHING, KiwiSelConv_CanConvert } },

        { KIWI_CMD_CONSTRUCT_JOIN,  { "Join Lines",                   "Construct", 0, KiwiConSel_CanJoin } },
        { KIWI_CMD_CONSTRUCT_DELETE,{ "Construction: Delete Selected","Construct", 0, KiwiConSel_CanDelete } },
        { KIWI_CMD_CONSTRUCT_HIDE,  { "Hide Selected Lines (H)",      "Construct", 0, KiwiConSel_CanHide } },
        { KIWI_CMD_CONSTRUCT_UNHIDE,{ "Unhide All (construction)",    "Construct", 0, KiwiCon_HasHidden  } },

        { KIWI_CMD_GROUP_CREATE,    { "Group Selection",              "Selection", SEL_MASK_OBJECT, KiwiOutliner_CanGroup   } },
        { KIWI_CMD_GROUP_UNGROUP,   { "Ungroup Selection",            "Selection", SEL_MASK_OBJECT, KiwiOutliner_CanUngroup } },

        { KIWI_CMD_MATINFO,         { "Material info (under cursor)", "Textures",  0, 0 } },

        { KIWI_CMD_MODELINFO,       { "Model info (next frame's models)", "Textures", 0, 0 } },

        { KIWI_CMD_INSTBATCH,       { "Instance batching (toggle)", "View", 0, 0 } },

        { KIWI_CMD_IMPORT_BROWSE,   { "Import Textures...",           "Textures",  0, 0 } },

        { KIWI_CMD_BUILD_RUN,       { "Build...",                     "Build",     0, 0 } },   // KIWI: game launch removed from the panel

        { KIWI_CMD_CAULK_FACES,     { "Caulk Selection",              "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiCaulk_CanExecute } },

        { KIWI_CMD_TEX_SHIFT,       { "Texture Shift",               "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiUv_CanEdit } },
        { KIWI_CMD_TEX_ROTATE,      { "Texture Rotate",              "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiUv_CanEdit } },
        { KIWI_CMD_TEX_SCALE,       { "Texture Scale",               "Textures",  SEL_MASK_FACE | SEL_MASK_OBJECT, KiwiUv_CanEdit } },
        { KIWI_CMD_PICK_TEXTURE,    { "Pick Texture",                "Textures",  0,                               KiwiUv_CanPick } },

        { 33002, { "Deselect All",              "Selection", 0, nullptr           } },
        { 33003, { "Delete Selection",          "Edit",      0, Can_HaveSelection } },
        { 33001, { "Clone Selection",           "Edit",      0, Can_HaveSelection } },
        { 33101, { "Invert Selection",          "Selection", 0, nullptr           } },
        { 32923, { "Hide Selected",             "View",      0, KiwiVis_CanHide   } },
        { 32934, { "Isolate (hide unselected)", "View",      0, KiwiVis_CanHide   } },
        { 32924, { "Show Hidden (unhide all)",  "View",      0, KiwiVis_HasHidden } },
        { 33246, { "Show Last Hidden",          "View",      0, KiwiVis_HasHidden } },
        { KIWI_CMD_HIDE_INVERT,     { "Invert Hidden",       "View",      0, KiwiVis_CanInvert } },
        { KIWI_CMD_FOCUS_SELECTION, { "Focus On Selection",  "View",      0, KiwiFocus_CanFocus } },
        { KIWI_CMD_VIEW_FACE,       { "View Face Head-on",   "View",      SEL_MASK_FACE, KiwiFocus_CanViewFace } },
        { KIWI_CMD_SECTION_TOGGLE,  { "Section Analysis",    "View",      0, nullptr } },
        { KIWI_CMD_PLACE_SUN,       { "Place Sun",           "Lighting",  0, KiwiSun_CanPlace } },
        { KIWI_CMD_REPEAT_LAST,     { "Repeat Last Command", "Edit",      0, KiwiCmd_HasRepeatable } },
        { KIWI_CMD_CLIP_CUT,        { "Cut to Clipboard",    "Edit",      SEL_MASK_OBJECT, CanClipCut } },
        { KIWI_CMD_REMOVE_FACE,     { "Remove Face (restore edge)", "Modeling",
                                      SEL_MASK_FACE, KiwiBevel_CanRemoveFace } },
        { 32982, { "CSG: Hollow",               "Modeling",  0, KiwiCsg_CanHollow    } },
        { 32927, { "CSG: Merge",                "Modeling",  0, KiwiCsg_CanMerge     } },
        { 33220, { "CSG: Auto Caulk",           "Modeling",  0, KiwiCsg_CanAutoCaulk } },
        { 32956, { "Mirror X (Flip)",           "Duplicate", 0, KiwiDupe_CanMirror   } },
        { 32957, { "Mirror Y (Flip)",           "Duplicate", 0, KiwiDupe_CanMirror   } },
        { 32958, { "Mirror Z (Flip)",           "Duplicate", 0, KiwiDupe_CanMirror   } },
        { 33041, { "Surface Inspector",         "Textures",  0, nullptr           } },
        { 33092, { "Patch Inspector",           "Patch",     0, nullptr           } },
        { 33104, { "View Filters",              "View",      0, nullptr           } },
        { 32784, { "Preferences",               "Editor",    0, nullptr           } },
        { 32786, { "Map Info",                  "Editor",    0, nullptr           } },
        { 32793, { "Toggle Snap To Grid",       "Grid",      0, nullptr           } },
        { 33083, { "Grid Size: Next (classic)", "Grid",      0, nullptr           } },
        { 33084, { "Grid Size: Prev (classic)", "Grid",      0, nullptr           } },
        { 33183, { "Drop To Floor",             "Edit",      0, Can_HaveSelection } },
        { 32810, { "Mouse Rotate Mode",         "Transform", 0, nullptr           } },
        { 32783, { "Toggle Clipper",            "Modeling",  0, nullptr           } },
        { 33005, { "Drag Vertices",             "Transform", 0, nullptr           } },
        { 33006, { "Drag Edges",                "Transform", 0, nullptr           } },
    };

    // Built-in modal lifecycle probe. It intentionally mutates nothing and opens no undo.
    class KiwiSelfTestCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "UX: Modal Self-Test"; }

        bool Begin() override
        {
            m_haveStart = false;
            m_haveNum   = false;
            m_numWorld  = 0.0f;

            ray_t ray;
            if ( Pick_RayFromCursor( &ray ) )
            {
                snap_result_t s;
                int cx = 0, cy = 0;
                ImGuiShell_CameraPaintCursor( &cx, &cy, nullptr, nullptr );
                if ( KiwiSnap_Query( ray, cx, cy, &s ) && s.valid )
                {
                    m_start[0] = s.position[0];
                    m_start[1] = s.position[1];
                    m_start[2] = s.position[2];
                    m_current  = s;
                    m_haveStart = true;
                }
            }
            Sys_Printf( "Modal self-test: begin (Esc cancels, Enter/LMB commits).\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_current = snap;
            if ( !m_haveStart && snap.valid )
            {
                m_start[0] = snap.position[0];
                m_start[1] = snap.position[1];
                m_start[2] = snap.position[2];
                m_haveStart = true;
            }
            g_nUpdateBits |= 1;              // repaint the camera so the marker tracks
        }

        void NumericChanged( bool has, float world ) override
        {
            m_haveNum  = has;
            m_numWorld = world;
        }

        void Commit() override
        {
            char bx[32], by[32], bz[32];
            if ( m_current.valid )
            {
                KiwiUnits_Format( bx, sizeof( bx ), m_current.position[0] );
                KiwiUnits_Format( by, sizeof( by ), m_current.position[1] );
                KiwiUnits_Format( bz, sizeof( bz ), m_current.position[2] );
                Sys_Printf( "Modal self-test: commit at %s, %s, %s (snap = %s)\n",
                            bx, by, bz, KiwiSnap_TypeName( m_current.type ) );
            }
            else
            {
                Sys_Printf( "Modal self-test: commit with no snap point.\n" );
            }
            if ( m_haveNum )
            {
                char bv[32];
                KiwiUnits_Format( bv, sizeof( bv ), m_numWorld );
                Sys_Printf( "Modal self-test: typed value %s (= %g world units)\n",
                            bv, (double)m_numWorld );
            }
            g_nUpdateBits |= 1;
        }

        void Cancel() override
        {
            Sys_Printf( "Modal self-test: cancelled.\n" );
            m_haveStart = false;
            m_haveNum   = false;
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( !m_haveStart || !m_current.valid )
                return;
            KiwiLines_Color( 1.00f, 0.80f, 0.25f );
            KiwiLines_Add( m_start, m_current.position );
        }

    private:
        float         m_start[3] = { 0.0f, 0.0f, 0.0f };
        bool          m_haveStart = false;
        bool          m_haveNum   = false;
        float         m_numWorld  = 0.0f;
        snap_result_t m_current;
    };

    KiwiSelfTestCommand s_selfTest;

    // Factory chain preserves feature ownership; the built-in self-test wins its id first.
    KiwiEditorCommand *CommandForId( int id )
    {
        if ( id == KIWI_CMD_SELFTEST )
            return &s_selfTest;
        if ( KiwiEditorCommand *c = KiwiXform_CommandForId( id ) )      // §13/§20/§21/§22
            return c;
        if ( KiwiEditorCommand *c = KiwiCon_CommandForId( id ) )        // §7  drawing tools
            return c;
        if ( KiwiEditorCommand *c = KiwiPrim_CommandForId( id ) )       // §16b solid primitives
            return c;
        if ( KiwiEditorCommand *c = KiwiExtrude_CommandForId( id ) )    // §23 extrude
            return c;
        if ( KiwiEditorCommand *c = KiwiBevel_CommandForId( id ) )      // §25 bevel / inset
            return c;
        if ( KiwiEditorCommand *c = KiwiDupe_CommandForId( id ) )       // §25 arrays
            return c;
        if ( KiwiEditorCommand *c = KiwiSplit_CommandForId( id ) )      // C cut, Ctrl+R split
            return c;
        if ( KiwiEditorCommand *c = KiwiMatch_CommandForId( id ) )      // Z match face
            return c;
        if ( KiwiEditorCommand *c = KiwiTrim_CommandForId( id ) )       // T trim lines
            return c;
        if ( KiwiEditorCommand *c = KiwiOffset_CommandForId( id ) )     // O offset curve
            return c;
        if ( KiwiEditorCommand *c = KiwiFillet_CommandForId( id ) )     // B fillet corners
            return c;
        if ( KiwiEditorCommand *c = KiwiPatchFillet_CommandForId( id ) ) // B on brush edges
            return c;
        if ( KiwiEditorCommand *c = KiwiBool_CommandForId( id ) )       // Q boolean
            return c;
        if ( KiwiEditorCommand *c = KiwiLoft_CommandForId( id ) )       // L loft
            return c;
        return KiwiUv_CommandForId( id );                               // §26 texture shift/rotate/scale
    }

    // Same Shift=1, Alt=2, Ctrl=4, Win=8 mask used by Radiant_TryHotkey.
    unsigned int CurrentMods()
    {
        unsigned int mods = 0;
        if ( ::GetKeyState( VK_MENU )    < 0 ) mods |= 2;
        if ( ::GetKeyState( VK_CONTROL ) < 0 ) mods |= 4;
        if ( ::GetKeyState( VK_SHIFT )   < 0 ) mods |= 1;
        if ( ::GetKeyState( VK_LWIN )    < 0 ) mods |= 8;
        return mods;
    }
}

// Exported grid controls share StepGrid's ladder, clamp, and console report.
void KiwiCmd_StepGrid( bool doubleIt )
{
    StepGrid( doubleIt );
}

// Typed spacing accepts any positive value and deliberately bypasses the ladder.
bool KiwiCmd_SetGridSpacing( float inches )
{
    if ( !( inches > 0.0f ) )
        return false;
    if ( inches < KGRID_MIN_INCHES ) inches = KGRID_MIN_INCHES;
    if ( inches > KGRID_MAX_INCHES ) inches = KGRID_MAX_INCHES;
    KiwiUnits_SetGridSpacingInches( inches );
    g_nUpdateBits = -1;
    Sys_Printf( "grid spacing %g in\n", (double)KiwiUnits_GridSpacingInches() );
    return true;
}

// Metadata lookup is linear over the small static side table.
const kiwiCommandInfo_t *KiwiCmd_Info( int commandId )
{
    for ( const auto &m : KCMD_META )
        if ( m.id == commandId )
            return &m.info;
    return nullptr;
}

bool KiwiCmd_CanExecute( int commandId )
{
    const kiwiCommandInfo_t *info = KiwiCmd_Info( commandId );
    if ( !info || !info->canExecute )
        return true;
    return info->canExecute();
}

// Register the complete command set without changing row order. The self-test row,
// feature registrars, classic-id additions, and aliases are all intentional.
void KiwiCmd_RegisterCommands()
{
    KiwiPlastBridge_RegisterCommands(); // Ctrl+Shift+P selected reference export
    Radiant_RegisterCommand( "KiwiSelectModePoint",  0, 0, KIWI_CMD_SELMODE_POINT );
    Radiant_RegisterCommand( "KiwiSelectModeEdge",   0, 0, KIWI_CMD_SELMODE_EDGE );
    Radiant_RegisterCommand( "KiwiSelectModeFace",   0, 0, KIWI_CMD_SELMODE_FACE );
    Radiant_RegisterCommand( "KiwiSelectModeObject", 0, 0, KIWI_CMD_SELMODE_OBJECT );
    Radiant_RegisterCommand( "KiwiSelectModeAll",    0, 0, KIWI_CMD_SELMODE_ALL );
    Radiant_RegisterCommand( "KiwiCommandPalette",   0, 0, KIWI_CMD_PALETTE );
    Radiant_RegisterCommand( "KiwiGridHalve",        0, 0, KIWI_CMD_GRID_HALVE );
    Radiant_RegisterCommand( "KiwiGridDouble",       0, 0, KIWI_CMD_GRID_DOUBLE );
    Radiant_RegisterCommand( "KiwiSnapMarkers",      0, 0, KIWI_CMD_SNAP_TOGGLE );
    Radiant_RegisterCommand( "KiwiModalSelfTest",    0, 0, KIWI_CMD_SELFTEST );
    KiwiXform_RegisterCommands();       // §13 G / R / S — one registration point
    KiwiCon_RegisterCommands();         // §7 / §16 drawing tools + construction planes
    KiwiPrim_RegisterCommands();        // §16b Box / Cylinder / Sphere / Cone
    KiwiAdd_RegisterCommands();         // §16b the Shift+A add menu
    KiwiExtrude_RegisterCommands();     // §23 Extrude Region
    KiwiBevel_RegisterCommands();       // §25 Bevel Edge + Inset Face
    KiwiDupe_RegisterCommands();        // §25 Array (Linear) + Array (Radial)
    KiwiSelExt_RegisterCommands();      // §25 selection expansion
    KiwiSelConv_RegisterCommands();
    KiwiConSel_RegisterCommands();
    KiwiSplit_RegisterCommands();
    KiwiBool_RegisterCommands();
    KiwiLoft_RegisterCommands();
    KiwiAutoBool_RegisterCommands();
    KiwiMatch_RegisterCommands();
    KiwiTrim_RegisterCommands();
    KiwiJoin_RegisterCommands();
    KiwiUv_RegisterCommands();          // §26 texture shift/rotate/scale + Pick Texture
    KiwiWindows_RegisterCommands();
    KiwiOutliner_RegisterCommands();
    KiwiEntBrowser_RegisterCommands();
    KiwiModelBrowser_RegisterCommands(); // KIWI      — the Models window toggle
    KiwiSky_RegisterCommands();
    KiwiUvEd_RegisterCommands();
    KiwiImport_RegisterCommands();
    KiwiLaunch_RegisterCommands();
    KiwiCaulk_RegisterCommands();
    KiwiSection_RegisterCommands();
    KiwiSun_RegisterCommands();         //            — "Place Sun" (the worldspawn sun keys)
    KiwiLight_RegisterCommands();       //            — the selected-light helper window
    KiwiOffset_RegisterCommands();      // O       offset a construction chain
    KiwiFillet_RegisterCommands();
    KiwiPatchFillet_RegisterCommands();
    KiwiFocus_RegisterCommands();       // /       frame the selection
    KiwiVis_RegisterCommands();         // Ctrl+H  invert hidden
    Radiant_RegisterCommand( "KiwiRepeatLastCommand", 0, 0, KIWI_CMD_REPEAT_LAST );
    Radiant_RegisterCommand( "KiwiMatInfo", 0, 0, KIWI_CMD_MATINFO );
    Radiant_RegisterCommand( "KiwiModelInfo", 0, 0, KIWI_CMD_MODELINFO );
    Radiant_RegisterCommand( "KiwiInstBatch", 0, 0, KIWI_CMD_INSTBATCH );
    // Registry-only route to CMainFrame::OnSelectionMakehollow 0x425570 and
    // CSG_MakeHollow 0x47D3C0; the ported handler was already wired.
    Radiant_RegisterCommand( "CSGHollow", 0, 0, 32982 );
    Radiant_RegisterCommand( "KiwiClipCut", 0, 0, KIWI_CMD_CLIP_CUT );

    {
        // Duplicate-id aliases preserve classic rows while modern bindings address names.
        extern bool Radiant_RegisterCommandAlias( const char *name, byte vk, byte mods,
                                                  int commandId );   // mainfrm.cpp
        Radiant_RegisterCommandAlias( "KiwiDeleteSelection", 0, 0, 33003 );

        Radiant_RegisterCommandAlias( "KiwiGridDoublePage", 0, 0, KIWI_CMD_GRID_DOUBLE );
        Radiant_RegisterCommandAlias( "KiwiGridHalvePage",  0, 0, KIWI_CMD_GRID_HALVE );
    }
}

namespace
{
    // Only handled KIWI modeling/creation dispatches become repeatable.
    int s_lastCommand = 0;               // 0 = nothing recorded yet

    // Explicit exclusions are editor/view/selection actions, internal continuations,
    // diagnostics, and Repeat itself. New modeling ids default to repeatable.
    bool Repeatable( int id )
    {
        switch ( id )
        {
        case KIWI_CMD_SELMODE_POINT:                    // selection:mode:set:*
        case KIWI_CMD_SELMODE_EDGE:
        case KIWI_CMD_SELMODE_FACE:
        case KIWI_CMD_SELMODE_OBJECT:
        case KIWI_CMD_SELMODE_ALL:
        case KIWI_CMD_SELCONV_POINT:                    // selection:convert:*
        case KIWI_CMD_SELCONV_EDGE:
        case KIWI_CMD_SELCONV_FACE:
        case KIWI_CMD_SELCONV_OBJECT:
        case KIWI_CMD_PALETTE:                          // (KIWI's own; not a verb)
        case KIWI_CMD_ADD_MENU:
        case KIWI_CMD_GRID_HALVE:                       // viewport:grid:*
        case KIWI_CMD_GRID_DOUBLE:
        case KIWI_CMD_SNAP_TOGGLE:
        case KIWI_CMD_CPLANE_XY:                        // viewport:grid:selection kin
        case KIWI_CMD_CPLANE_XZ:
        case KIWI_CMD_CPLANE_YZ:
        case KIWI_CMD_CPLANE_FACE:
        case KIWI_CMD_CPLANE_VIEW:
        case KIWI_CMD_WINDOW_XY:                        // window / view toggles
        case KIWI_CMD_WINDOW_Z:
        case KIWI_CMD_WINDOW_TEXTURE:
        case KIWI_CMD_WINDOW_CONSOLE:
        case KIWI_CMD_WINDOW_SHELL:
        case KIWI_CMD_WINDOW_OUTLINER:
        case KIWI_CMD_WINDOW_ENTITIES:
        case KIWI_CMD_WINDOW_MODELS:                    // the Models tab — likewise
        case KIWI_CMD_WINDOW_SUN:                       // the Sun tab — likewise
        case KIWI_CMD_WINDOW_LIGHT:                     // the Light tab — likewise
        case KIWI_CMD_WINDOW_INSPECTOR:                 // the Inspector tab — likewise
        case KIWI_CMD_ENT_DROP:
        case KIWI_CMD_MODEL_DROP:
        case KIWI_CMD_PLASTICITY_PUSH:                 // external export, not a modelling replay
        case KIWI_CMD_VIEW_SHOW_GRID:
        case KIWI_CMD_VIEW_SHOW_AXES:
        case KIWI_CMD_VIEW_ORTHO:
        case KIWI_CMD_CONSTRUCT_UNDO:                   // edit:undo
        case KIWI_CMD_FOCUS_SELECTION:                  // viewport:focus
        case KIWI_CMD_VIEW_FACE:
        case KIWI_CMD_SECTION_TOGGLE:
        case KIWI_CMD_HIDE_INVERT:
        case KIWI_CMD_CONSTRUCT_HIDE:
        case KIWI_CMD_CONSTRUCT_UNHIDE:
        case KIWI_CMD_REPEAT_LAST:                      // edit:repeat-last-command
        case KIWI_CMD_CLIP_CUT:
        case KIWI_CMD_BUILD_RUN:
        case KIWI_CMD_SELFTEST:                         // a diagnostic, not a verb
            return false;
        default:
            return true;
        }
    }
}

bool KiwiCmd_HasRepeatable()
{
    return s_lastCommand != 0;
}

const char *KiwiCmd_RepeatLabel()
{
    if ( !s_lastCommand )
        return 0;
    const kiwiCommandInfo_t *info = KiwiCmd_Info( s_lastCommand );
    return info ? info->displayName : 0;
}

// Repeat re-enters ordinary dispatch so modal and instant semantics remain identical.
bool KiwiCmd_RepeatLast()
{
    const int id = s_lastCommand;
    if ( !id )
    {
        Sys_Printf( "Repeat: no command has been run yet.\n" );
        return false;
    }
    if ( !KiwiCmd_CanExecute( id ) )
    {
        const char *name = KiwiCmd_RepeatLabel();
        Sys_Printf( "Repeat: \"%s\" cannot run right now.\n", name ? name : "the last command" );
        return false;
    }
    return KiwiCmd_Dispatch( (unsigned int)id );
}

// Copy mutates nothing and ported Delete owns the complete undo record; do not nest
// another bracket. Empty selection must leave the clipboard unchanged.
void KiwiCmd_ClipCut()
{
    extern void Radiant_ExecCommand( unsigned int cmdId );   // mainfrm.cpp:4054

    if ( selected_brushes.next == &selected_brushes )
    {
        Sys_Printf( "Cut: nothing is selected - the clipboard is unchanged.\n" );
        return;
    }
    Radiant_ExecCommand( 33039u );      // Edit->Copy   (serialise to the clipboard)
    Radiant_ExecCommand( 33003u );      // Edit->Delete (its own complete undo record)
    Sys_Printf( "Cut: selection copied to the clipboard and deleted.\n" );
    g_nUpdateBits = -1;
}

static bool KiwiCmd_DispatchInner( unsigned int cmdId );

// Shared range predicate used by mainfrm's dispatch gate.
bool KiwiCmd_IsKiwiId( int id )
{
    return id >= KIWI_CMD_FIRST && id <= KIWI_CMD_LAST;
}

// Record only handled repeatable ids after dispatch.
bool KiwiCmd_Dispatch( unsigned int cmdId )
{
    const bool handled = KiwiCmd_DispatchInner( cmdId );
    if ( handled && Repeatable( (int)cmdId ) )
        s_lastCommand = (int)cmdId;
    return handled;
}

// Resolve context-sensitive B before any route so keys, palette, menus, and Repeat agree.
static bool KiwiCmd_DispatchInner( unsigned int cmdId )
{
    cmdId = (unsigned int)KiwiPatchFillet_ContextB( (int)cmdId );

    // Modal ids outrank every instant feature route; any id in this block means Start.
    if ( KiwiCmd_IsModalId( (int)cmdId ) )
        return KiwiCmd_Start( (int)cmdId );

    switch ( cmdId )
    {
    case KIWI_CMD_SELMODE_POINT:  SetModeMask( SEL_MASK_VERTEX );     return true;
    case KIWI_CMD_SELMODE_EDGE:   SetModeMask( SEL_MASK_EDGE );       return true;
    case KIWI_CMD_SELMODE_FACE:   SetModeMask( SEL_MASK_FACE );       return true;
    case KIWI_CMD_SELMODE_OBJECT: SetModeMask( SEL_MASK_OBJECT );     return true;
    case KIWI_CMD_SELMODE_ALL:    SetModeMask( SEL_MASK_EVERYTHING ); return true;
    case KIWI_CMD_PALETTE:        KiwiPalette_Toggle();               return true;
    case KIWI_CMD_GRID_HALVE:     StepGrid( false );                  return true;
    case KIWI_CMD_GRID_DOUBLE:    StepGrid( true );                   return true;
    case KIWI_CMD_SNAP_TOGGLE:
        KiwiSnap_SetShowMarkers( !KiwiSnap_ShowMarkers() );
        g_nUpdateBits |= 1;
        return true;
    case KIWI_CMD_REPEAT_LAST:    KiwiCmd_RepeatLast();               return true;
    case KIWI_CMD_CLIP_CUT:       KiwiCmd_ClipCut();                  return true;
    case KIWI_CMD_REMOVE_FACE:
        if ( g_activeCommand && g_activeCommand->PreemptIdle() )
            KiwiCmd_Cancel();
        if ( !KiwiBevel_RemoveFaceRestoreEdge() )
            Sys_Printf( "Remove Face: select exactly ONE brush face (mode 3).\n" );
        return true;
    default:
        // Feature dispatchers self-gate their ids. Keep this ownership chain and order intact.
        if ( KiwiCon_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiSelExt_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiSelConv_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiConSel_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiJoin_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiAutoBool_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiUv_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiAdd_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiWindows_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiDupe_DispatchInstant( cmdId ) )        // Shift+D duplicate + Move
            return true;
        if ( KiwiFocus_DispatchInstant( cmdId ) )       // /  frame  ·  Space  face
            return true;
        if ( KiwiSection_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiSun_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiVis_DispatchInstant( cmdId ) )         // Ctrl+H  invert hidden
            return true;
        if ( KiwiOutliner_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiEntBrowser_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiModelBrowser_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiSky_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiImport_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiLaunch_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiPlastBridge_DispatchInstant( cmdId ) )
            return true;
        if ( KiwiCaulk_DispatchInstant( cmdId ) )
            return true;
        if ( cmdId == (unsigned int)KIWI_CMD_MATINFO )
        {
            extern void KiwiMtl_InfoCommand();
            KiwiMtl_InfoCommand();
            return true;
        }
        if ( cmdId == (unsigned int)KIWI_CMD_MODELINFO )
        {
            extern void KiwiEdScene_ArmModelInfoDump();
            KiwiEdScene_ArmModelInfoDump();
            return true;
        }
        if ( cmdId == (unsigned int)KIWI_CMD_INSTBATCH )
        {
            extern void KiwiEdScene_ToggleInstBatching();
            KiwiEdScene_ToggleInstBatching();
            return true;
        }
        return false;                     // an unwired id in the reserved range
    }
}

// Post-tail for classic Paste/Clone only. Internal paste callers must not auto-enter
// a gesture; Map_ImportBuffer 0x487C90 already leaves pasted brushes selected.
// Mixed brush/construction output cannot share one Move arm, so it is left selected
// without auto-entry. TakeJustPasted is one-shot so Clone cannot mistake a stale
// line selection for paste output. A single-kind result enters Move PAUSED.
void KiwiCmd_AfterPaste()
{
    if ( !KiwiUX_ModernInput() )
        return;                             // classic profile: untouched behaviour
    if ( g_activeCommand )
        return;                             // never stomp a gesture already running

    const bool haveBrushes = !KiwiSel().items.empty()
                          || selected_brushes.next != &selected_brushes;

    const int  pastedLines = KiwiConClip_TakeJustPasted();
    const bool haveLines   = ( pastedLines > 0 );
    if ( !haveBrushes && !haveLines )
        return;                             // nothing landed — an empty clipboard
    if ( haveBrushes && haveLines )
    {
        Sys_Printf( "Paste: solids AND construction geometry were pasted, and one "
                    "Move gesture carries one kind at a time — nothing was "
                    "auto-entered.  Select one kind and press G.\n" );
        return;
    }

    if ( KiwiCmd_Start( KIWI_CMD_MOVE ) )
        KiwiCmd_Pause();
}

KiwiEditorCommand *KiwiCmd_Active()
{
    return g_activeCommand;
}

// Start resets numeric/snap state, installs fields before Begin, seeds LastCursor,
// and publishes Active before Begin so commands may query the framework.
bool KiwiCmd_Start( int commandId )
{
    KiwiEditorCommand *cmd = CommandForId( commandId );
    if ( !cmd )
        return false;

    if ( g_activeCommand )
        KiwiCmd_Cancel();

    if ( !cmd->CanExecute() )
        return false;

    KiwiNum_Reset();
    s_lastSnap = snap_result_t();
    s_hot      = true;

    {
        const kiwiNumField_t *fields = nullptr;
        const int nf = cmd->NumericFields( &fields );
        if ( nf > 0 && fields )
            KiwiNum_SetFields( fields, nf );
    }

    {
        int cx = 0, cy = 0;
        s_cursorHave = ImGuiShell_CameraPaintCursor( &cx, &cy, nullptr, nullptr );
        s_cursorX = cx;
        s_cursorY = cy;
        s_lastShift = false;
    }

    g_activeCommand = cmd;                 // set BEFORE Begin: Begin may query Active()
    if ( !cmd->Begin() )
    {
        g_activeCommand = nullptr;
        KiwiCmd_UndoCancel();              // self-guards when Begin opened nothing
        return false;
    }
    g_nUpdateBits |= 1;
    return true;
}

// Clear Active before re-entrant Commit, close undo before deferred deselection,
// reset numeric state, then drain any deferred start.
void KiwiCmd_Commit()
{
    KiwiEditorCommand *cmd = g_activeCommand;
    if ( !cmd )
        return;
    g_activeCommand = nullptr;             // clear FIRST: Commit may re-enter the layer
    cmd->Commit();
    KiwiCmd_UndoCommit();                  // no-op when the command opened no bracket
    DrainDeferredDeselect();
    KiwiNum_Reset();
    s_lastSnap = snap_result_t();
    DrainDeferred();
    g_nUpdateBits |= 1;
}

// Cancel restores state and drops both deferred start and deferred deselection.
void KiwiCmd_Cancel()
{
    KiwiEditorCommand *cmd = g_activeCommand;
    if ( !cmd )
        return;
    g_activeCommand = nullptr;
    cmd->Cancel();
    KiwiCmd_UndoCancel();                  // no-op when the command opened no bracket
    KiwiNum_Reset();
    s_lastSnap = snap_result_t();
    s_deferId = 0;
    s_deferDeselect = false;
    g_nUpdateBits |= 1;
}

// The latest handoff request is drained at the end of a successful commit.
void KiwiCmd_StartDeferred( int commandId, bool paused )
{
    s_deferId    = commandId;
    s_deferPause = paused;
}

// Idempotent request; selection remains intact until the undo tail is recorded.
void KiwiCmd_DeselectAfterCommit()
{
    s_deferDeselect = true;
}

// Click tools have no drag to park and must keep their rubber-band preview live.
void KiwiCmd_Pause()
{
    if ( !g_activeCommand || !s_hot )
        return;
    if ( g_activeCommand->WantsClicks() )
        return;
    s_hot = false;
    g_nUpdateBits |= 1;
}

// Rebase before the next MouseMove so cursor travel while parked cannot jump geometry.
void KiwiCmd_Resume()
{
    if ( !g_activeCommand || s_hot )
        return;
    s_hot = true;
    g_activeCommand->Rebase();
    g_nUpdateBits |= 1;
}

// Ctrl inverts the active snap context: transforms opt in, construction opts out,
// and pivot placement is always snapped. Poll physical state; never latch it.
bool KiwiCmd_SnapEngaged()
{
    const bool ctrl = ( ::GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) != 0;
    const KiwiEditorCommand::kiwiSnapCtx_t ctx =
        g_activeCommand ? g_activeCommand->SnapContext()
                        : KiwiEditorCommand::KSNAPCTX_TRANSFORM;
    if ( ctx == KiwiEditorCommand::KSNAPCTX_ALWAYS )
        return true;
    return ( ctx == KiwiEditorCommand::KSNAPCTX_CONSTRUCT ) != ctrl;
}

// Confirm source is scoped to the Enter-ladder dispatch and restored for re-entry.
namespace { bool s_confirmIsRmb = false; }

bool KiwiCmd_ConfirmIsRmb()
{
    return s_confirmIsRmb;
}

// RMB confirmation uses the Enter ladder so command-specific Enter vetoes also apply.
void KiwiCmd_Confirm()
{
    if ( !g_activeCommand )
        return;
    const bool prev = s_confirmIsRmb;      // nested confirms cannot lie to each other
    s_confirmIsRmb  = true;
    KiwiCmd_KeyDown( 0x0D, 0 );            // VK_RETURN, no modifiers
    s_confirmIsRmb  = prev;
}

// Key ladder, in order:
// 0) offer Tab to commands only when numeric cycling is a no-op;
// 1) numeric entry, including field Tab, outranks command keys;
// 2) Esc clears the focused numeric field before anything else;
// 3) command keys may consume Esc/Enter;
// 4) Esc cancels;
// 5) Enter advances a valid typed stage, swallows incomplete text, or commits.
// Every other key stays swallowed while a modal edit owns the gesture.
bool KiwiCmd_KeyDown( int vk, unsigned int mods )
{
    if ( !g_activeCommand )
        return false;

    if ( vk == 0x09 && KiwiNum_FieldCount() <= 1 )       // VK_TAB
    {
        if ( g_activeCommand->KeyDown( vk, mods ) )
        {
            g_nUpdateBits |= 1;
            return true;
        }
    }

    if ( KiwiNum_Key( vk, mods ) )
    {
        const int f = KiwiNum_Focus();
        g_activeCommand->NumericFieldChanged( f, KiwiNum_HasValueField( f ),
                                              KiwiNum_ValueWorldField( f ) );
        g_nUpdateBits |= 1;
        return true;
    }

    if ( vk == 0x1B && KiwiNum_Has() )      // VK_ESCAPE
    {
        const int f = KiwiNum_Focus();
        KiwiNum_ClearField( f );
        g_activeCommand->NumericFieldChanged( f, false, 0.0f );
        g_nUpdateBits |= 1;
        return true;
    }

    if ( g_activeCommand->KeyDown( vk, mods ) )
    {
        g_nUpdateBits |= 1;
        return true;
    }

    if ( vk == 0x1B )                       // VK_ESCAPE
    {
        KiwiCmd_Cancel();
        return true;
    }

    if ( vk == 0x0D )                       // VK_RETURN
    {
        if ( KiwiNum_Has() )
        {
            if ( !KiwiNum_HasValue() )
            {
                Sys_Printf( "%s: \"%s\" is not a finished value yet — finish it, "
                            "or clear it with Esc.\n",
                            g_activeCommand->Name(), KiwiNum_Text() );
                return true;
            }
            if ( g_activeCommand->AdvanceStage() )
            {
                g_nUpdateBits |= 1;
                return true;                // the gesture stays live
            }
        }
        KiwiCmd_Commit();
        return true;
    }

    return true;
}

// Latch cursor even while PAUSED; paused motion is consumed without pick/snap work.
// PickFlags drive both queries. SnapQueryAnchor may relocate only the snap ray/pixel;
// the pick remains under the cursor.
bool KiwiCmd_MouseMove( int imgX, int imgY )
{
    if ( !g_activeCommand )
        return false;

    s_cursorX    = imgX;
    s_cursorY    = imgY;
    s_cursorHave = true;

    if ( !s_hot )
        return true;

    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return true;                        // consumed; nothing to update

    const unsigned flags = g_activeCommand->PickFlags();

    int snapX = imgX, snapY = imgY;
    ray_t snapRay = ray;
    {
        int   ax = 0, ay = 0;
        ray_t anchorRay;
        if ( g_activeCommand->SnapQueryAnchor( &ax, &ay )
          && ( ax != imgX || ay != imgY )
          && Pick_RayFromImagePos( ax, ay, &anchorRay ) )
        {
            snapRay = anchorRay;             // written only on success
            snapX   = ax;
            snapY   = ay;
        }
    }
    KiwiSnap_Query( snapRay, snapX, snapY, &s_lastSnap, flags );
    const pick_result_t pick = Pick( ray, KiwiSel_GetModeMask(), flags );
    g_activeCommand->MouseMove( pick, s_lastSnap );
    return true;
}

// Camera-image coordinates use a top-left origin.
bool KiwiCmd_LastCursor( int *imgX, int *imgY )
{
    if ( !s_cursorHave )
        return false;
    if ( imgX ) *imgX = s_cursorX;
    if ( imgY ) *imgY = s_cursorY;
    return true;
}

bool KiwiCmd_LastShift()
{
    return s_lastShift;
}

bool KiwiCmd_WantsMarquee()
{
    return g_activeCommand != nullptr && g_activeCommand->WantsMarquee();
}

// Recheck opt-in at release because the command or stage may have changed mid-drag.
void KiwiCmd_Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift )
{
    if ( !g_activeCommand )
        return;
    if ( !g_activeCommand->WantsMarquee() )
        return;
    g_activeCommand->Marquee( x0, y0, x1, y1, crossing, shift );
}

// LMB only: MMB/RMB belong to camera navigation. The active command outranks
// selection so one press cannot both edit and select.
//
// PressIntercept runs before pause/resume with cursor+Shift latched. Idle reselect
// may consume a paused bare press or additive Shift press; Ctrl removal remains in
// selection handling. Any unclaimed paused LMB resumes broadly. Click tools place
// a point after MouseMove; other HOT presses park without resampling the cursor.
bool KiwiCmd_MouseButton( int btn, int imgX, int imgY, bool shift )
{
    if ( !g_activeCommand )
        return false;
    if ( btn != 0 )                         // only LMB; MMB/RMB stay with the camera
        return false;

    KiwiEditorCommand *cmd = g_activeCommand;

    s_cursorX    = imgX;
    s_cursorY    = imgY;
    s_cursorHave = true;
    s_lastShift  = shift;
    if ( cmd->PressIntercept( imgX, imgY ) )
        return true;

    if ( ( !s_hot || shift ) && cmd->IdlePressReselect( imgX, imgY, shift ) )
        return true;

    if ( !s_hot )
    {
        s_cursorX    = imgX;
        s_cursorY    = imgY;
        s_cursorHave = true;
        KiwiCmd_Resume();
        KiwiCmd_MouseMove( imgX, imgY );
        return true;
    }

    if ( cmd->WantsClicks() )
    {
        KiwiCmd_MouseMove( imgX, imgY );    // act at the point actually under the cursor
        if ( !g_activeCommand )
            return true;                    // the move ended the gesture under us
        if ( cmd->Click() )
            return true;                    // still running
        KiwiCmd_Commit();
        return true;
    }

    KiwiCmd_Pause();
    return true;
}

// Unified handle order is load-bearing: feed press, resume/rebase, open the command
// gate, rebase again, then feed the same pixel for a zero first delta.
bool KiwiCmd_HandleGrab( int imgX, int imgY )
{
    KiwiEditorCommand *cmd = g_activeCommand;
    if ( !cmd )
        return false;

    KiwiCmd_MouseMove( imgX, imgY );        // 1. the press pixel IS the grab point
    if ( !s_hot )
        KiwiCmd_Resume();                   // 2a. …which Rebase()s at that pixel
    else
        cmd->Rebase();                      // 2b. already hot: re-latch anyway
    cmd->HandleGrab( true );                // 3. open the command's own gate
    cmd->Rebase();                          // 4. the gate may have moved the origin
    KiwiCmd_MouseMove( imgX, imgY );        // 5. frame one: delta exactly zero
    return true;
}

// Only closes the command gate; callers own pause/commit/cancel.
void KiwiCmd_HandleRelease()
{
    if ( g_activeCommand )
        g_activeCommand->HandleGrab( false );
}

const snap_result_t &KiwiCmd_LastSnap()
{
    return s_lastSnap;
}

// One bounded batch holds both command overlay and snap marker. Commands may request
// a larger current budget, clamped to KCMD_LINE_BUDGET_MAX.
void KiwiCmd_DrawWorld()
{
    if ( !g_activeCommand )
        return;

    int budget = KCMD_LINE_BUDGET;
    const int want = g_activeCommand->LineBudget();
    if ( want > budget )
        budget = ( want > KCMD_LINE_BUDGET_MAX ) ? KCMD_LINE_BUDGET_MAX : want;
    KiwiLines_Begin( budget, 2 );
    g_activeCommand->DrawWorld();
    KiwiSnap_EmitMarker( s_lastSnap );
    KiwiLines_Flush();
}

// Funnel order is load-bearing:
// - bare modifiers pass through so ImGui's Win32 backend receives their messages;
// - open add/palette UI owns keys;
// - an unmoved auto-entered face gesture may yield to explicit context verbs;
// - G/R/S swaps commit moved gestures and cancel untouched ones;
// - Delete/End/view exceptions arbitrate parked face gestures;
// - otherwise an active modal command swallows the key;
// - idle Esc/delete/hide modes run before camera-fly swallowing.
//
// Creation-plane arming occurs before cancelling the parked face. Caulk yields
// without cancel because its texture handshake restores the gesture. View verbs
// yield only with no numeric text, preserving Space separators and '/' division.
bool KiwiUX_KeyFunnel( unsigned int vk )
{
    switch ( vk )
    {
    case 0x10: case 0x11: case 0x12:            // VK_SHIFT / VK_CONTROL / VK_MENU
    case 0xA0: case 0xA1:                       // VK_LSHIFT / VK_RSHIFT
    case 0xA2: case 0xA3:                       // VK_LCONTROL / VK_RCONTROL
    case 0xA4: case 0xA5:                       // VK_LMENU / VK_RMENU
    case 0x5B: case 0x5C:                       // VK_LWIN / VK_RWIN
        return false;
    default:
        break;
    }

    if ( KiwiAdd_IsOpen() )
    {
        if ( vk == 0x1B )                   // VK_ESCAPE
            KiwiAdd_Close();
        return true;
    }

    if ( KiwiPalette_IsOpen() )
    {
        if ( vk == 0x1B )                   // VK_ESCAPE
            KiwiPalette_Close();
        return true;
    }

    if ( g_activeCommand && g_activeCommand->PreemptIdle() )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( id && PreemptVerb( id ) && KiwiCmd_CanExecute( id ) )
        {
            if ( CreationVerb( id ) )
                KiwiCon_ArmSelectedFacePlane();
            KiwiCmd_Cancel();
            return false;                   // …and let the hotkey table run it
        }
    }

    if ( g_activeCommand )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( id && SwapVerb( id ) && g_activeCommand->CanSwapTo( id )
          && KiwiCmd_CanExecute( id ) )
        {
            const bool moved = g_activeCommand->GestureMoved();
            if ( moved ) KiwiCmd_Commit();
            else         KiwiCmd_Cancel();
            return false;                   // …and let the hotkey table run it
        }
    }

    if ( ( vk == 0x2E || vk == 0x08 )                            // VK_DELETE / VK_BACK
      && g_activeCommand && g_activeCommand->PreemptIdle()
      && !KiwiConSel_OwnsDelete()
      && KiwiBevel_CanRemoveFace() )
    {
        KiwiCmd_Cancel();
        if ( KiwiBevel_RemoveFaceRestoreEdge() )
            return true;
    }

    if ( g_activeCommand && g_activeCommand->PreemptIdle() )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( id == KIWI_CMD_CAULK_FACES && KiwiCmd_CanExecute( id ) )
            return false;                   // …the BH handshake owns the gesture
    }

    if ( g_activeCommand && g_activeCommand->PreemptIdle() && !KiwiNum_Has() )
    {
        const int id = LookupBinding( vk, CurrentMods() );
        if ( ( id == KIWI_CMD_VIEW_FACE || id == KIWI_CMD_FOCUS_SELECTION )
          && KiwiCmd_CanExecute( id ) )
            return false;                   // …the camera verb runs, the gesture stays
    }

    if ( g_activeCommand )
        return KiwiCmd_KeyDown( (int)vk, CurrentMods() );

    // Region and sun are selection peers: clear them but let classic deselect continue.
    if ( vk == 0x1B && KiwiRegion_HasSelection() )                   // VK_ESCAPE
        KiwiRegion_ClearSelection();

    if ( vk == 0x1B && KiwiSun_Selected() )                          // VK_ESCAPE
        KiwiSun_ClearSelection();

    // Patch-vertex and section-pick are modes: consume one Esc level at a time.
    if ( vk == 0x1B && KiwiPatchVerts_HandleEscape() )               // VK_ESCAPE
        return true;

    if ( vk == 0x1B && KiwiSection_HandleEscape() )                  // VK_ESCAPE
        return true;

    // Construction delete owns pure construction selection before classic Delete.
    if ( ( vk == 0x2E || vk == 0x08 ) && KiwiConSel_OwnsDelete() )   // VK_DELETE / VK_BACK
    {
        KiwiConSel_DeleteSelected();
        return true;
    }

    // Permanent, once-per-session diagnosis of legitimate patch selection gates.
    if ( vk == 0x48 && CurrentMods() == 0 )                          // H
        KiwiVis_ReportPatchGates( false );

    // Bare H hides construction first; mixed selections then fall through to classic Hide.
    if ( vk == 0x48 && CurrentMods() == 0 && KiwiConSel_OwnsHide() )  // H
    {
        KiwiConSel_HideSelected();
        if ( !KiwiVis_CanHide() )
            return true;                    // nothing brush-side to hide as well
        return false;                       // …and let 32923 hide the brushes too
    }

    // Exactly one brush face routes to restore-edge; all other selections use classic Delete.
    if ( ( vk == 0x2E || vk == 0x08 ) && KiwiBevel_CanRemoveFace() )  // VK_DELETE / VK_BACK
    {
        if ( KiwiBevel_RemoveFaceRestoreEdge() )
            return true;
    }

    // Below active-command handling: camera fly suppresses duplicate hotkeys but
    // never steals modal keys.
    if ( KiwiCam_FlySwallowKey( vk ) )
        return true;

    return false;
}

// Mirrors undo.cpp/drag.cpp. Snapshot selection before the first mutation and keep
// selected_brushes intact until EndBrushList records the AFTER state.
void KiwiCmd_UndoBegin( const char *operation )
{
    if ( s_undoOpen )
        return;                             // one bracket per gesture, always
    KiwiUndo_ArmSelectionSnapshot();
    Undo_ClearRedo();
    Undo_GeneralStart( operation );         // stores the POINTER — literals only
    Undo_AddBrushList( &selected_brushes );
    s_undoOpen = true;
}

// Cover one brush omitted from selected_brushes. Fixed-size owners are saved first;
// Undo_AddBrush self-deduplicates. Mirrors Undo_AddBrushList at undo.cpp 0x45E7C0.
void KiwiCmd_UndoCoverBrush( selbrush_t *node )
{
    if ( !node || !node->def )
        return;
    entity_s *owner = node->def->owner;
    if ( owner && owner->eclass && owner->eclass->fixedsize )
        Undo_AddEntity( (int)(intptr_t)owner );
    Undo_AddBrush( (entity_brush_s *)node->def );
}

void KiwiCmd_UndoCommit()
{
    if ( !s_undoOpen )
        return;
    s_undoOpen = false;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// Close before Undo_Undo, clear the generated redo, and suppress journal append hooks
// so a cancelled gesture leaves no undo or redo ticket.
void KiwiCmd_UndoCancel()
{
    if ( !s_undoOpen )
        return;
    s_undoOpen = false;
    KiwiUndo_SuppressBegin();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    Undo_Undo();
    Undo_ClearRedo();
    KiwiUndo_SuppressEnd();
    g_nUpdateBits = -1;
}
