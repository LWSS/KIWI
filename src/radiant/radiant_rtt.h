#pragma once
// radiant_rtt.h — Phase 5: render each editor viewport into an offscreen D3D9 texture
// that ImGui samples as an image, replacing the native-child-HWND-per-viewport scheme.
// See RADIANT_UI_REWORK_PLAN.md (PHASE 5 — RTT VIEWPORTS).
struct IDirect3DTexture9;

enum rttViewport_t
{
    RTT_CAMERA = 0,
    RTT_XY,
    RTT_Z,
    RTT_TEXTURE,
    RTT_COUNT
};

// Ensure viewport `id`'s render-target texture exists at w×h (recreates on size change),
// point the engine's FRAME_BUFFER at it, and suppress Present. Returns false if the target
// couldn't be set up (skip the viewport's draw). Pair every true with RTT_End().
bool RTT_Begin( rttViewport_t id, int w, int h );
void RTT_End();

// The sampleable texture for ImGui::Image( (ImTextureID)RTT_GetTexture(id), ... ). May be
// null before the first successful RTT_Begin for that id.
IDirect3DTexture9 *RTT_GetTexture( rttViewport_t id );

// Device-loss bracket: release every RT texture (recreated lazily on the next RTT_Begin).
// Called from R_ReleaseForShutdownOrReset (r_init.cpp).
void RTT_ReleaseForReset();
