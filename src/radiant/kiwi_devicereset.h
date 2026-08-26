#pragma once
// Cached editor surf-cache handles become stale when Editor_VB_ReleaseForReset
// destroys the VB pool. Brush_InvalidateVis (0x478340) drops them without
// returning handles to that pool and arms the next rebuild.

// Drop handles reachable from the current brush lists before Editor_VB_ReleaseForReset.
// Safe with no map loaded and safe to call twice.
void KiwiDevice_InvalidateEditorSurfCache();

// DB_EnumXAssets is a no-op in Radiant, so unmanaged-image reset handling walks
// imageGlobals.imageHashTable and mirrors R_FreeLostImage/R_RebuildLostImage.
// Both operations are idempotent.
void KiwiDevice_ReleaseUnmanagedImages();   // before Reset()
void KiwiDevice_RebuildUnmanagedImages();   // after a Reset() that succeeded

// Recovery diagnostics consumed by the frame health watch.
void KiwiDevice_NoteCoopLevel( long hr );                    // R_TestDevice
// releasePass: 0 none, 1 full release, 2 second-chance release.
void KiwiDevice_NoteResetResult( long hr, int releasePass ); // R_ResetDevice

// Record only the first site/HRESULT per loss episode; rearm after a successful Reset.
void KiwiDevice_NoteLoss( const char *where, long hr );

// Called after EndPaint. Unauthorized paints are ignored; painted means the scene bracket ran.
// Throttle black-frame reports and, after ~15 s, attempt one rescue save and alert.
void KiwiDevice_FrameHealthWatch( struct HWND__ *frame, bool authorized, bool painted );
