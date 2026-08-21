#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_iwi.h — the .iwi writer: decode an image file through D3DX and emit the one image
// container this engine loads ("images/<name>.iwi").  Pure I/O, no editor state.
//
// GfxImageFileHeader (r_image.h:118, sizeof 0x1C = 28), little-endian:
//   +0  char tag[3]           'I','W','i'                    Image_ValidateHeader r_image.cpp:440
//   +3  u8   version          must be 6                                          r_image.cpp:442
//   +4  u8   format           kiwiIwiFormat_t   Image_LoadFromData r_image_load_obj.cpp:542
//   +5  u8   flags            0x2 nomipmaps, 0x4 cube, 0x8 volume, 0x20 legacy-normals (untested
//                             by any reader).  This writer emits 0 = full mip chain, picmip-able.
//   +6  s16  dimensions[3]    w, h, depth
//   +12 s32  fileSizeForPicmip[4]
// Mip data follows immediately, SMALLEST MIP FIRST (the readers walk mipLevel = mipCount-1
// down to picmip).  Each level is tightly packed, no row padding:
//   DXT   bytesPerBlock * ((h+3)>>2) * ((w+3)>>2)
//   plain bytesPerPixel * h * w
//   fileSizeForPicmip[p] = 28 + sum over L = min(p, mipCount-1) .. mipCount-1 of size(L)
// Power-of-two is ENFORCED: Image_CountMipmaps (r_image.cpp:467) counts by doubling to the
// larger dimension, which for a non-POT image yields one MORE level than D3D's own chain.
//
// NORMAL MAPS: always DXT5, two channels, storing SLOPES x = nx/nz, y = ny/nz (not normal
// components); the shader reconstructs normalize(x*T + y*B + N) from
//   x = A*4.08 - 2.08 , y = G*4.06452 - 2.06452
// so the encode is  A = 62.5000*x + 130.0000 ,  G = 62.7381*y + 129.5238 , written as A into
// the DXT5 alpha block and R=G=B=G into the colour block (R and B are never sampled).  Mips
// are generated from the SOURCE normal image and each level encoded separately.
// KIWI: green is taken verbatim — +Y vs -Y is a property of the source art, not the format.
//
// SPECULAR MAPS: always DXT5, ordinary image — RGB = reflection colour, A = gloss (the shader
// uses lod = 6 - 8*alpha on the reflection cube, so 1 = sharpest).  DXT5 is forced so a source
// without alpha does not fall to DXT1 and silently drop the gloss channel.

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

// Which of the three material slots this image is being written for.
enum kiwiIwiEncoding_t
{
    KIWI_IWI_ENC_COLOR    = 0,   // DXT1/DXT5/ARGB8 by `compress` + source alpha
    KIWI_IWI_ENC_NORMAL   = 1,   // always DXT5; RGB tangent-space normal -> alpha=x, grey=y
    KIWI_IWI_ENC_SPECULAR = 2,   // always DXT5; RGB = reflection colour, A = gloss, verbatim
};

struct kiwiIwiOptions_t
{
    bool compress;       // emit DXT1/DXT5 instead of ARGB8   (COLOR encoding only)
    bool resampleToPot;  // allow the writer to resize a non-POT source
    int  encoding;       // kiwiIwiEncoding_t
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

// A MANAGED (reset-proof, so it needs no RTT_ReleaseForReset hook) D3D texture of the source
// image, for the wizard preview.  Caller Release()s it.  Null on failure.
IDirect3DTexture9 *KiwiIwi_CreatePreview( const char *srcPath );

// Decode `srcPath` and write `qpath` (a virtual path, e.g. "images/foo.iwi") through
// FS_FOpenFileWriteToDir into raw/, where the map compilers can see it.  Returns false
// with a reason in `err` on any failure; on failure it leaves no file behind.
bool KiwiIwi_WriteFromFile( const char *srcPath, const char *qpath,
                            const kiwiIwiOptions_t *opt, kiwiIwiResult_t *out,
                            char *err, size_t errSz );

// Round-trip gate stage 1: re-open the file we just wrote through the engine's filesystem and
// run Image_ValidateHeader + Image_CountMipmapsForFile over it, plus the two size invariants
// Image_LoadFromFileWithReader asserts.  Stage 2 (decode + upload) is in kiwi_matwriter.
bool KiwiIwi_VerifyOnDisk( const char *qpath, char *err, size_t errSz );

// The accepted source extensions, for the drop filter and the file dialog.
bool KiwiIwi_IsAcceptedExtension( const char *path );
const char *KiwiIwi_AcceptedExtensionList();   // "*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.dds"
