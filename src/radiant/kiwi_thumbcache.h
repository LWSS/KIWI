#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Executable-local cache: one .kthumb per model under <fs_basepath>/kiwi_cache/thumbs/.
// The header is followed by tightly packed BGRA8 rows, independent of D3D row pitch.

struct IDirect3DTexture9;

typedef unsigned __int64 kiwiThumbSourceHash_t;

enum kiwiThumbCacheFormat_t
{
    KIWI_THUMBCACHE_FORMAT_BGRA8 = 1,
};

enum kiwiThumbCacheLoadResult_t
{
    KIWI_THUMBCACHE_MISS = 0,
    KIWI_THUMBCACHE_HIT,
    KIWI_THUMBCACHE_RETRY,
};

void KiwiThumbCache_Init();

// FNV-1a-64 over the normalized model name and the size/mtime stamps of the
// xmodel plus the parts/surfs and parseable materials named by it.  An IWD member
// contributes its containing IWD's stamp.  Bounds come from that same header read.
bool KiwiThumbCache_SourceHash( const char *xmodelName,
                                kiwiThumbSourceHash_t *outHash,
                                float outMins[3], float outMaxs[3],
                                bool *outHaveBounds );

// Loads a match into a D3DPOOL_MANAGED texture. RETRY means a transient D3D
// upload failure; MISS asks the caller to render and replace the disk entry.
kiwiThumbCacheLoadResult_t KiwiThumbCache_Load(
    const char *xmodelName, kiwiThumbSourceHash_t sourceHash,
    unsigned renderVersion, unsigned width, unsigned height,
    kiwiThumbCacheFormat_t format, IDirect3DTexture9 **outTexture );

// Writes a successful readback.  `pixels` is BGRA8 and `rowPitch` may be wider
// than width*4; the disk payload is always tightly packed.
bool KiwiThumbCache_Write( const char *xmodelName,
                           kiwiThumbSourceHash_t sourceHash,
                           unsigned renderVersion,
                           unsigned width, unsigned height,
                           kiwiThumbCacheFormat_t format,
                           const void *pixels, unsigned rowPitch );

// Writers must invalidate after replacing xmodel/<name> or its parts/surfs;
// otherwise the cached source hash is not recomputed until the next process start.
void KiwiThumbCache_Invalidate( const char *xmodelName );
void KiwiThumbCache_InvalidateAll();

// Thumbnail memory owners sample this before an ImGui frame and release their
// live MANAGED textures there, never while a draw list may still reference one.
unsigned KiwiThumbCache_InvalidateSerial();
