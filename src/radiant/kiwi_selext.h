#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Smarter selection commands and the camera connected-selection hook.
// These commands change selection only and open no undo record.
//
// Unlike Region_BeginFromSingleBrush in select.cpp, touching grows from every
// selected brush and never deletes the seed. Bounds overlap mirrors
// Select_Touching_R (select.cpp, 0x490520): all axes use a 1-unit epsilon
// and Pick_BrushPickable admission.
// Classic Select_Connected (select.cpp, 0x490EC0) follows entity links;
// "Connected (touching)" here is geometric.
//
// Coplanar matches same-facing planes using the validator thresholds.
// Touching expands per typed-selected brush, including component-only owners;
// a union box would select the gaps between scattered seeds.
// Same material compares current-layer radMtl handles by pointer, then name.
// Active faces produce faces; other active items produce objects using face 0.
// Connected repeats touching to a fixed point under brush/pass caps.
//
// The camera hook belongs inside the modern-input gate and after modal commands.
// The ordinary first click establishes selection before this hook re-picks.

#define KSELX_PLANE_DOT        0.999f    // == KVALID_PLANE_DOT
#define KSELX_PLANE_DIST       0.01f     // == KVALID_PLANE_DIST, in world units
#define KSELX_TOUCH_EPS        1.0f      // Select_Touching_R epsilon, in world units
#define KSELX_MAX_FACES        4096      // coplanar/material addition cap
#define KSELX_MAX_CONNECTED    1000      // connected brush cap
#define KSELX_MAX_PASSES       64        // connected frontier cap

// Instant-command dispatch tail for the four ids (called by KiwiCmd_Dispatch).
bool KiwiSelExt_DispatchInstant( unsigned int commandId );
void KiwiSelExt_RegisterCommands();

// Palette availability predicates.
bool KiwiSelExt_CanCoplanar();   // any face item is present
bool KiwiSelExt_CanTouching();   // anything is selected
bool KiwiSelExt_CanMaterial();   // anything is selected
bool KiwiSelExt_CanConnected();  // anything is selected

// Camera-image coordinates; returns true when the double-click is consumed.
bool KiwiSelExt_CameraDoubleClick( int imgX, int imgY );

// The "Selection" block in the shell panel.
void KiwiSelExt_MenuItems();
