#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Persisted UX toggles and their settings UI.

#include "stdafx.h"
#include "kiwi_ux.h"
#include "kiwi_camera.h"
#include "kiwi_fmt.h"
#include "kiwi_gizmo.h"
#include "kiwi_grid.h"            // Grid-snap preference API.
#include "kiwi_hints.h"
#include "kiwi_keymap.h"
#include "kiwi_plastbridge.h"
#include "kiwi_snap.h"
#include "kiwi_units.h"
#include "kiwi_selection.h"
#include "kiwi_viewcube.h"
#include "kiwi_windows.h"         // Native View-menu checkbox synchronization.
#include "radiant_registry.h"

#include <imgui/imgui.h>

extern int g_nUpdateBits;                  // engine_stubs.cpp:773 (0x25D5A74)

namespace
{
    const char *KUX_SECTION = "KiwiUX";

    struct kuxFlag_t
    {
        const char *entry;
        int         value;                 // -1 = not loaded yet
        int         defVal;
    };

    kuxFlag_t s_modernInput = { "ModernInput", -1, 1 };
    kuxFlag_t s_showGrid    = { "ShowGrid",    -1, 1 };
    kuxFlag_t s_showAxes    = { "ShowAxes",    -1, 1 };
    kuxFlag_t s_showHover   = { "ShowHover",   -1, 1 };
    kuxFlag_t s_showTris    = { "ShowTriCount", -1, 1 };

    bool Get( kuxFlag_t &f )
    {
        if ( f.value < 0 )
            f.value = Radiant_ProfileGetInt( KUX_SECTION, f.entry, f.defVal ) ? 1 : 0;
        return f.value != 0;
    }

    void Set( kuxFlag_t &f, bool on )
    {
        const int v = on ? 1 : 0;
        if ( f.value == v )
            return;
        f.value = v;
        Radiant_ProfileSetInt( KUX_SECTION, f.entry, v );
    }
}

bool KiwiUX_ModernInput()              { return Get( s_modernInput ); }
void KiwiUX_SetModernInput( bool on )  { Set( s_modernInput, on ); }
bool KiwiUX_ShowGrid()                 { return Get( s_showGrid ); }
void KiwiUX_SetShowGrid( bool on )     { Set( s_showGrid, on ); g_nUpdateBits |= 1; }
bool KiwiUX_ShowAxes()                 { return Get( s_showAxes ); }
void KiwiUX_SetShowAxes( bool on )     { Set( s_showAxes, on ); g_nUpdateBits |= 1; }
bool KiwiUX_ShowHover()                { return Get( s_showHover ); }
void KiwiUX_SetShowHover( bool on )    { Set( s_showHover, on ); g_nUpdateBits |= 1; }
bool KiwiUX_ShowTriCount()             { return Get( s_showTris ); }
void KiwiUX_SetShowTriCount( bool on ) { Set( s_showTris, on ); g_nUpdateBits |= 1; }

// Settings UI.
void KiwiUX_DrawSettings()
{
    ImGui::SeparatorText( "KIWI UX" );

    bool modern = KiwiUX_ModernInput();
    if ( ImGui::Checkbox( "Modern input (orbit / look / dolly / marquee)", &modern ) )
        KiwiUX_SetModernInput( modern );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "OFF restores the classic camera dispatch exactly:\n"
                           "MMB, RMB and the wheel go back to the legacy handlers,\n"
                           "LMB drag is the classic 3D drag-select again, and the\n"
                           "keyboard fly and the gizmo stop taking input.\n"
                           "ON: MMB orbit, RMB drag TRUCK (Shift+MMB and Shift+RMB\n"
                           "too), Alt+RMB drag mouselook, wheel dolly, arrows fly.\n"
                           "RMB CLICK confirms a live command, or opens the classic\n"
                           "context menu when nothing is running." );

    // Persist first, then restamp the matching native View-menu check item.
    bool grid = KiwiUX_ShowGrid();
    if ( ImGui::Checkbox( "Ground grid", &grid ) )
    {
        KiwiUX_SetShowGrid( grid );
        KiwiWindows_SyncViewMenu();
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "ROUND AJ: the ground lattice is drawn in ORTHO only.\n"
                           "In perspective ('P') it is suppressed outright — a\n"
                           "workaround while the perspective aliasing is open, not\n"
                           "the end state.  The world axes are unaffected, and so\n"
                           "is grid SNAPPING (its own checkbox, below and on the\n"
                           "grid pill in the viewport's top right)." );
    bool axes = KiwiUX_ShowAxes();
    if ( ImGui::Checkbox( "World axes", &axes ) )
    {
        KiwiUX_SetShowAxes( axes );
        KiwiWindows_SyncViewMenu();
    }
    // Shared with the viewport grid pill; both controls use one persisted preference.
    bool gridSnap = KiwiGrid_SnapEnabled();
    if ( ImGui::Checkbox( "Snap to grid", &gridSnap ) )
        KiwiGrid_SetSnapEnabled( gridSnap );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "OFF disables the grid quantiser AND the light grid\n"
                           "magnet everywhere (moves, extrudes, splits, clones).\n"
                           "Geometry / face / construction snapping is untouched,\n"
                           "and CTRL still suppresses everything for one gesture." );
    bool hover = KiwiUX_ShowHover();
    if ( ImGui::Checkbox( "Hover highlight", &hover ) )
        KiwiUX_SetShowHover( hover );

    // Marker visibility does not change each command's Ctrl-controlled snap engagement.
    bool snapMarkers = KiwiSnap_ShowMarkers();
    if ( ImGui::Checkbox( "Snap marker + label", &snapMarkers ) )
    {
        KiwiSnap_SetShowMarkers( snapMarkers );
        g_nUpdateBits |= 1;
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Shown while a modal command is running.\n"
                           "Hold CTRL to suppress snapping itself." );

    bool gizmo = KiwiGizmo_Show();
    if ( ImGui::Checkbox( "Transform gizmos", &gizmo ) )
        KiwiGizmo_SetShow( gizmo );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "The move handles and the rotate rings.\n"
                           "Shakeout D: they appear only WHILE the matching\n"
                           "command is running - the arrows during G, the three\n"
                           "rings during R - and nothing is drawn on an idle\n"
                           "selection.  Dragging a handle or a ring runs that\n"
                           "SAME command, with its axis or plane already locked.\n"
                           "OFF leaves G and R fully usable without them." );

    bool cube = KiwiViewCube_Show();
    if ( ImGui::Checkbox( "Orientation view-cube", &cube ) )
    {
        KiwiViewCube_SetShow( cube );
        g_nUpdateBits |= 1;
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "The axis widget in the camera's top-right corner.\n"
                           "Click a ball to look along that axis from the\n"
                           "current pivot and distance." );

    bool hints = KiwiHints_Show();
    if ( ImGui::Checkbox( "Hotkey hints panel", &hints ) )
    {
        KiwiHints_SetShow( hints );
        g_nUpdateBits |= 1;
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "The contextual key list on the camera's right edge.\n"
                           "Every key shown is read LIVE from the command table,\n"
                           "so remaps and keymap-profile switches show at once." );

    // A logarithmic scale keeps both 0.1x and 50x usable.
    float fly = KiwiCam_FlySpeedScale();
    ImGui::SetNextItemWidth( 220.0f );
    if ( ImGui::SliderFloat( "Fly speed x", &fly, 0.1f, 50.0f, "%.1fx",
                             ImGuiSliderFlags_Logarithmic ) )
        KiwiCam_SetFlySpeedScale( fly );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Multiplier over the classic MoveSpeed preference,\n"
                           "which the ARROW-KEY fly reads as units per second\n"
                           "(stock 350).  Shift boosts while RMB is held." );

    KiwiKeymap_DrawSettings();

    KiwiPlastBridge_DrawSettings();

    // Display-unit preferences only; neither mutates map geometry.
    float spacing = KiwiUnits_GridSpacingInches();
    ImGui::SetNextItemWidth( 110.0f );
    if ( ImGui::InputFloat( "Grid spacing (in)", &spacing, 0.0f, 0.0f, KIWI_FMT_FLOAT,
                            ImGuiInputTextFlags_EnterReturnsTrue ) )
    {
        KiwiUnits_SetGridSpacingInches( spacing );
        g_nUpdateBits |= 1;
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Any positive value — not restricted to powers of two.\n"
                           "Separate from the classic 1..256 grid (that still drives\n"
                           "the ported snap sites)." );

    float perInch = KiwiUnits_PerInch();
    ImGui::SetNextItemWidth( 110.0f );
    if ( ImGui::InputFloat( "Units per inch", &perInch, 0.0f, 0.0f, KIWI_FMT_FLOAT,
                            ImGuiInputTextFlags_EnterReturnsTrue ) )
    {
        KiwiUnits_SetPerInch( perInch );
        g_nUpdateBits |= 1;
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Display-only conversion constant (CoD: 1 unit = 1 inch).\n"
                           "Never stored in geometry or the .map." );

    // Selection-mode readout, mirroring the in-viewport chips.
    const sel_mask_t m = KiwiSel_GetModeMask();
    ImGui::TextDisabled( "Select mode: %s%s%s%s",
                         ( m & SEL_MASK_VERTEX ) ? "point " : "",
                         ( m & SEL_MASK_EDGE   ) ? "edge "  : "",
                         ( m & SEL_MASK_FACE   ) ? "face "  : "",
                         ( m & SEL_MASK_OBJECT ) ? "object" : "" );
}
