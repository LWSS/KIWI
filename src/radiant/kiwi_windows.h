#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// One persisted flag per non-camera dock window drives ImGui p_open, RTT gating, and menu checks.
// Camera is excluded so the primary viewport cannot be hidden.
// Defaults live only in kiwi_windows.cpp's s_def table; enum notes below are descriptive.

// Shared by the [KiwiWindows] defaults reseed and kiwi_dock<N>.ini.
// Bump when defaults or dock placement changes so existing profiles receive both together.
// Version 17 adds Reference Images beside Decals in the lower-right dock node.
#define KIWI_LAYOUT_VERSION  17

enum kiwiWindow_t
{
    // Defaults mirror s_def. Default-open windows must also be placed by
    // ImGuiShell_BuildDefaultDockLayout or they can float over the camera.
    KIWI_WIN_XY = 0,        // "2D View"          (RTT_XY)      default ON
    KIWI_WIN_Z,             // "Z"                (RTT_Z)       default OFF
    KIWI_WIN_TEXTURE,       // "Textures"         (RTT_TEXTURE) default ON
    KIWI_WIN_CONSOLE,       // "Console"                        default ON
    KIWI_WIN_SHELL,         // "KIWI ImGui shell"               default OFF
    KIWI_WIN_OUTLINER,      // "Outliner"                       default ON
    KIWI_WIN_ENTITIES,      // "Entities"                       default ON
    // Static xmodels are previewed and placed as misc_model entities.
    KIWI_WIN_MODELS,        // "Models"                         default ON
    KIWI_WIN_SKY,           // "Sky"                            default ON
    KIWI_WIN_UVEDITOR,      // "UV editor"                      default ON
    KIWI_WIN_SUN,           // "Sun"                            default ON
    KIWI_WIN_LIGHT,         // "Light"                          default ON
    KIWI_WIN_INSPECTOR,     // "Inspector"                      default ON
    KIWI_WIN_DECALS,        // "Decals"                         default ON
    KIWI_WIN_REFIMAGES,     // "Reference Images"               default ON
    KIWI_WIN_COUNT,
};

// Runtime title; default-dock literals in imgui_shell.cpp must match it exactly.
const char *KiwiWindows_Title( kiwiWindow_t w );

bool  KiwiWindows_IsOpen ( kiwiWindow_t w );
// Live ImGui p_open pointer; CommitPending persists title-bar changes.
bool *KiwiWindows_OpenPtr( kiwiWindow_t w );
void  KiwiWindows_Set    ( kiwiWindow_t w, bool open );

// Call once after all ImGui windows to persist external changes and clear re-dock requests.
// JustOpened lets callers re-dock reopened windows instead of restoring floating positions.
void  KiwiWindows_CommitPending();
bool  KiwiWindows_JustOpened( kiwiWindow_t w );

// Append the native Windows popup; frameMenu is an opaque HMENU to keep windows.h out.
void  KiwiWindows_BuildMenu( void *frameMenu );
void  KiwiWindows_SyncMenu();

// Items are appended inside View, preserving menu-bar indices used by legacy code.
// View is popup index 2 in res/radiant.rc; Windows is appended after existing popups.
void  KiwiWindows_BuildViewMenu( void *frameMenu );
void  KiwiWindows_SyncViewMenu();

// Command registration and instant dispatch; kiwi_command.cpp calls both.
void  KiwiWindows_RegisterCommands();
bool  KiwiWindows_DispatchInstant( unsigned int cmdId );
