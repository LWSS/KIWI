#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Radiant precompiled header in (Phase 4: MFC is gone — plain Win32 only).
#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

// ── Plain Win32 head ─────────────────────────────────────────────────────────────
// No WIN32_LEAN_AND_MEAN (the old afx path never defined it either — code relies on
// the full <windows.h> surface). The core's former CString/CStringList/CMap/CFile
// usage is now std::string / std::vector / std::map / FILE*, and registry persistence
// is Radiant_Profile* (radiant_registry.h) — the kisak_mfc_shim.h stopgap is deleted.
// The 4005 suppression stays: the DXSDK June-2010 include dir (on this target's
// include path) shadows several modern Windows SDK headers and redefines their
// macros with the same values but a different token spelling.
#pragma warning(push)
#pragma warning(disable: 4005)
#include <windows.h>
#include <commctrl.h>
#pragma warning(pop)

// Standard C/C++
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

// D3D9 (renderer bridge in later phases)
#include <d3d9.h>
#include <d3dx9.h>

// GDI+ (uses std::min/std::max)
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>

// Resource IDs
#include "res/resource.h"

// Engine base types (uint/ushort/byte/vec_t + the IDA compat macros). Every engine TU
// includes q_shared.h first; editor TUs cannot, because the Win32 head must lead (see
// above), so it goes here instead - stdafx.h is the first include of every editor TU, and this
// still lands before qe3.h/qedefs.h and the kisak headers they pull in, all of which
// use those base types.
#include <universal/q_shared.h>

// Editor object model (plane_t, winding_t, vec3_t, brush_t, face_t, qeglobals_t, …).
// GtkRadiant 1.6 stdafx.h also includes qe3.h as the universal editor type header.
// Must come AFTER <windows.h> (needs HWND/HINSTANCE) and resource.h (ICON IDs).
#include "qe3.h"
