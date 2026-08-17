#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_iwi.h — KIWI-UX (ROUND BE): THE .iwi WRITER.
// ═════════════════════════════════════════════════════════════════════════════════════
// Pure I/O.  No ImGui, no editor state, no selection — decode an image file the OS knows
// how to read, and emit the ONE image container this engine loads.
//
// ── D-BE-A: WHY THERE IS NO CHOICE ABOUT THE CONTAINER ──────────────────────────────
// `Image_LoadFromFileWithReader` (r_image_load_obj.cpp:389) builds its path as
// `"images/" + image->name + ".iwi"` (:403) and nothing else in the editor or the game
// opens a .tga/.png/.dds by name.  So a dropped PNG has to BECOME an .iwi before any
// material can reference it.  This file is that conversion, and every field it writes is
// derived from the line of the reader that reads it — see D-BE-C.
//
// ── D-BE-B: WHY D3DX AND NOT WIC ────────────────────────────────────────────────────
// The Radiant target ALREADY links d3dx9 (scripts/radiant/CMakeLists.txt:116, ${D3DX_LIB},
// and r_material_load_obj.cpp:15 already includes <d3dx9shader.h>), so `D3DXCreateTexture-
// FromFileExA` is a dependency we are paying for regardless.  It reads TGA/PNG/JPG/BMP/DDS,
// converts to any D3DFORMAT, resamples to power-of-two and generates the whole mip chain
// in one call — every step this writer needs.  Adding WIC + a hand-rolled TGA reader beside
// an already-linked library that does the job better would be new code with new bugs for no
// new capability.  (The brief's WIC fallback was conditional on d3dx9 NOT being linked.)
//
// ── D-BE-C: THE .iwi LAYOUT, DERIVED FIELD BY FIELD FROM THE READER ─────────────────
//   struct GfxImageFileHeader (r_image.h:118, sizeof 0x1C = 28)
//     +0  char tag[3]      must be 'I','W','i'   — Image_ValidateHeader r_image.cpp:440
//     +3  u8   version     must be 6             — Image_ValidateHeader r_image.cpp:442
//     +4  u8   format      switch arm            — Image_LoadFromData  r_image_load_obj.cpp:542
//     +5  u8   flags       mip/picmip/cube bits  — Image_CountMipmaps  r_image.cpp:464,
//                                                  Image_Setup r_image.cpp:1403-1416,
//                                                  the noPicmip test r_image_load_obj.cpp:420
//     +6  s16  dimensions[3] (w,h,depth)         — Image_SetupFromFile r_image_load_obj.cpp:224-235
//     +12 s32  fileSizeForPicmip[4]              — the vassert r_image_load_obj.cpp:430 and
//                                                  readSize r_image_load_obj.cpp:431
//   then, IMMEDIATELY after the header, the mip data — SMALLEST MIP FIRST.  That order is
//   not lore: Image_LoadDxtc (:350) and Image_LoadBitmap (:506) both walk
//   `for (mipLevel = mipCount-1; mipLevel >= picmip; --mipLevel)` advancing `data` by the
//   size of the level they just consumed, and mipLevel == mipCount-1 is the 1x1 end of the
//   chain.  Each level is TIGHTLY PACKED (no row padding): the advance is exactly
//   `bytesPerBlock * ((h+3)>>2) * ((w+3)>>2)` (:364) / `bytesPerPixel * h * w` (:530).
//
//   fileSizeForPicmip[p] = 28 + sum over L = min(p, mipCount-1) .. mipCount-1 of size(L).
//   The `min` arm is what makes a NOMIPMAPS (mipCount == 1) file carry the same value in
//   all four slots, which is what the shipped set does.  This formula was validated against
//   every shipped .iwi in the data tree: 6199 of 6268 reproduce byte-exactly, and the 69
//   that do not are all cubemaps (flags & 4, x6 faces) or wavelet formats (6..10) — neither
//   of which this writer emits.
//
// ── D-BE-D: THE v1 POLICY, AND WHY ──────────────────────────────────────────────────
//   * format 1 = D3DFMT_A8R8G8B8, 4 bytes/pixel (Image_LoadFromData r_image_load_obj.cpp:545).
//     Lossless, alpha-carrying, and the byte order needs no swizzle: we LockRect a D3DX
//     A8R8G8B8 texture and the loader uploads into an A8R8G8B8 surface, so the bytes make
//     the same round trip they would in any engine-internal copy.
//   * Optional compression (a wizard checkbox): DXT1 = format 11 (8 bytes/block) when the
//     source has no alpha, DXT5 = format 13 (16 bytes/block) when it does — the two arms of
//     Image_LoadFromData :575 / :581, and the two most common formats in the shipped set
//     (1701 and 2766 files respectively).
//   * flags = 0: a FULL mip chain, picmip-able.  Not IMG_FLAG_NOMIPMAPS, because a
//     world texture without mips shimmers at distance; not IMG_FLAG_NOPICMIP, because the
//     reader sets noPicmip itself for anything under 32 px (:420-427) and a mapper's
//     r_picmip should still apply to imported art like it does to shipped art.
//   * POWER OF TWO IS ENFORCED.  Image_CountMipmaps (r_image.cpp:467) counts levels by
//     doubling until it reaches the larger dimension, which for a non-POT image yields ONE
//     MORE level than D3D's own CreateTexture chain (floor(log2)+1) — so the reader's last
//     Image_UploadData would ask for a mip level the texture does not have.  The wizard
//     warns and offers the resample; the writer refuses non-POT outright.
// ═════════════════════════════════════════════════════════════════════════════════════

#include <d3d9.h>
#include <stddef.h>

// The IWI format bytes this writer can emit (the Image_LoadFromData arms it targets).
enum kiwiIwiFormat_t
{
    KIWI_IWI_ARGB8 = 1,    // D3DFMT_A8R8G8B8, 4 bpp  (r_image_load_obj.cpp:545)
    KIWI_IWI_DXT1  = 11,   // D3DFMT_DXT1,     8 bpb  (r_image_load_obj.cpp:575)
    KIWI_IWI_DXT5  = 13,   // D3DFMT_DXT5,    16 bpb  (r_image_load_obj.cpp:581)
};

struct kiwiIwiSource_t
{
    int  width;          // source pixels, before any POT resample
    int  height;
    bool hasAlpha;       // source format carries an alpha channel
    bool isPowerOfTwo;   // both dimensions are powers of two
    int  potWidth;       // what the resample would produce (== width  when already POT)
    int  potHeight;
};

struct kiwiIwiOptions_t
{
    bool compress;       // emit DXT1/DXT5 instead of ARGB8
    bool resampleToPot;  // allow the writer to resize a non-POT source
};

struct kiwiIwiResult_t
{
    int width;           // dimensions actually written
    int height;
    int mipCount;
    int format;          // one of kiwiIwiFormat_t
    int fileSize;        // == fileSizeForPicmip[0]
    bool resampled;      // the source was not POT and we resized it
};

// Read just enough of `srcPath` to answer the wizard's questions (dimensions, alpha,
// POT-ness).  Uses D3DXGetImageInfoFromFileA — no device needed.
bool KiwiIwi_Probe( const char *srcPath, kiwiIwiSource_t *out, char *err, size_t errSz );

// A MANAGED (reset-proof, so it needs no RTT_ReleaseForReset hook) D3D texture of the
// source image, for the wizard preview.  Caller Release()s it.  Null on failure.
IDirect3DTexture9 *KiwiIwi_CreatePreview( const char *srcPath );

// Decode `srcPath` and write `qpath` (a virtual path, e.g. "images/foo.iwi") through
// FS_FOpenFileWrite.  Returns false with a reason in `err` on any failure; on failure it
// leaves no file behind.
bool KiwiIwi_WriteFromFile( const char *srcPath, const char *qpath,
                            const kiwiIwiOptions_t *opt, kiwiIwiResult_t *out,
                            char *err, size_t errSz );

// ROUND-TRIP GATE, STAGE 1.  Re-open the file we just wrote through the ENGINE's own
// filesystem and run the ENGINE's own header functions over it — Image_ValidateHeader
// (r_image.cpp:438) and Image_CountMipmapsForFile (r_image.cpp:471) — plus the two size
// invariants Image_LoadFromFileWithReader asserts (fileSize >= 28 at :408 and
// fileSizeForPicmip[0] == fileSize at :430).  Stage 2 (the real decode + upload) happens
// when the material registers and is checked by kiwi_matwriter.
bool KiwiIwi_VerifyOnDisk( const char *qpath, char *err, size_t errSz );

// The accepted source extensions, for the drop filter and the file dialog.
bool KiwiIwi_IsAcceptedExtension( const char *path );
const char *KiwiIwi_AcceptedExtensionList();   // "*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.dds"
