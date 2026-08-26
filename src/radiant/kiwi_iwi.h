#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// D3DX-backed writer for images/<name>.iwi; pure I/O with no editor state.
// GfxImageFileHeader is 28-byte little-endian data (r_image.h:118):
//   +0  char tag[3]           'I','W','i'                    Image_ValidateHeader r_image.cpp:440
//   +3  u8   version          6                              r_image.cpp:442
//   +4  u8   format           kiwiIwiFormat_t                r_image_load_obj.cpp:542
//   +5  u8   flags            0x2 no mips, 0x4 cube, 0x8 volume, 0x20 legacy normals (reader-untested)
//   +6  s16  dimensions[3]    w, h, depth
//   +12 s32  fileSizeForPicmip[4]
// This writer emits flags=0 for a full picmippable chain, tightly packed smallest-first:
//   DXT: bytesPerBlock*((h+3)>>2)*((w+3)>>2); ARGB8: 4*h*w
//   fileSizeForPicmip[p] = 28 + sum(size(L), L=min(p,mipCount-1)..mipCount-1)
// Power-of-two dimensions are required: the reader's doubling count gives a non-POT image
// one more level than D3D's chain.
//
// Normal maps store slopes x=nx/nz in A and y=ny/nz in grayscale (R=G=B). The shader
// reconstructs normalize(x*T+y*B+N) after x=A*4.08-2.08 and y=G*4.06452-2.06452.
// Mips are filtered from source normals before encoding; source green is never flipped.
// Specular maps store reflection color in RGB and gloss in A; shader LOD=6-8*A (1=sharpest).
// Normal/specular select DXT5 to preserve their channels; sub-4 dimensions fall back to ARGB8.

#include <d3d9.h>
#include <stddef.h>

// The IWI format bytes this writer can emit (the Image_LoadFromData arms it targets).
enum kiwiIwiFormat_t
{
    KIWI_IWI_ARGB8 = 1,    // D3DFMT_A8R8G8B8, 4 B/pixel  (r_image_load_obj.cpp:545)
    KIWI_IWI_DXT1  = 11,   // D3DFMT_DXT1,     8 B/block  (r_image_load_obj.cpp:575)
    KIWI_IWI_DXT5  = 13,   // D3DFMT_DXT5,    16 B/block  (r_image_load_obj.cpp:581)
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
    KIWI_IWI_ENC_NORMAL   = 1,   // RGB tangent-space normal -> alpha=x, grayscale=y
    KIWI_IWI_ENC_SPECULAR = 2,   // RGB = reflection color, A = gloss, verbatim
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

// Probe dimensions, alpha capability, and POT status without a D3D device.
bool KiwiIwi_Probe( const char *srcPath, kiwiIwiSource_t *out, char *err, size_t errSz );

// Managed/reset-proof wizard preview; caller releases it, null on failure.
IDirect3DTexture9 *KiwiIwi_CreatePreview( const char *srcPath );

// Decode srcPath and write virtual qpath to raw/ for map-compiler visibility.
// Returns false with a message in err; short writes are deleted.
bool KiwiIwi_WriteFromFile( const char *srcPath, const char *qpath,
                            const kiwiIwiOptions_t *opt, kiwiIwiResult_t *out,
                            char *err, size_t errSz );

// Stage 1: reopen through engine FS and validate header, mip count, and loader size bounds.
// Stage 2 (decode/upload) lives in kiwi_matwriter.
bool KiwiIwi_VerifyOnDisk( const char *qpath, char *err, size_t errSz );

// The accepted source extensions, for the drop filter and the file dialog.
bool KiwiIwi_IsAcceptedExtension( const char *path );
const char *KiwiIwi_AcceptedExtensionList();   // "*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.dds"
