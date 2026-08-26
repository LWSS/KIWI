#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Defers texture release until ImGui no longer retains the pointer in RenderDrawData.

struct IDirect3DTexture9;

// Transfers ownership of tex; null is ignored and the caller must not use it afterward.
void KiwiTexGrave_Release( IDirect3DTexture9 *tex );

// Copies imageName and re-resolves it when a later reload runs; duplicates collapse.
void KiwiTexGrave_ReloadImage( const char *imageName );

// Call once at ImGui frame start, before UI records texture pointers; drain bounded
// reload and release batches in that order.
void KiwiTexGrave_Drain();

// Device-loss path: release all queued textures without running reloads; idempotent.
void KiwiTexGrave_ReleaseForReset();
