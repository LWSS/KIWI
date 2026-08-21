#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
//  kiwi_texgrave.h - the deferred-release graveyard.  ImGui is RETAINED, so a texture
//  Released during UI build leaves RenderDrawData a dangling pointer; hand it here
//  instead and it dies at the start of the next ImGui frame, after that render.

struct IDirect3DTexture9;

// Hand a texture over instead of calling Release() on it.  Legal at any time; null is
// ignored.  The caller must drop its own pointer - the graveyard owns the reference.
void KiwiTexGrave_Release( IDirect3DTexture9 *tex );

// Queue Image_Reload( Image_FindExisting( imageName ) ) for the next ImGui frame.  The
// name is copied and re-resolved when the work runs.  Duplicates collapse.
void KiwiTexGrave_ReloadImage( const char *imageName );

// Run the queued engine work, then release every queued texture.  Called from ONE place:
// the top of the ImGui frame, before any UI can record an ImTextureID.
void KiwiTexGrave_Drain();

// Release every queued texture WITHOUT running any queued engine work - for the
// device-loss hook, where creating a texture is not allowed.  Idempotent.
void KiwiTexGrave_ReleaseForReset();
