// kiwi_iwi.cpp — the .iwi writer (mechanical half).  The layout and the encodings are in
// kiwi_iwi.h.
#include "stdafx.h"
#include "qe3.h"

#include <d3d9.h>
// The umbrella header, not just <d3dx9tex.h>: D3DX_DEFAULT_NONPOW2 is only defined there.
#include <d3dx9.h>                  // D3DXGetImageInfoFromFileA / D3DXCreateTextureFromFileExA
#include <gfx_d3d/r_init.h>         // dx (dx.device)
#include <gfx_d3d/r_image.h>        // GfxImageFileHeader / Image_ValidateHeader / Image_CountMipmapsForFile
#include <universal/com_files.h>    // FS_FOpenFileWrite / FS_Write / FS_Read / FS_FCloseFile / FS_Delete

#include "kiwi_iwi.h"

#include <math.h>                   // sqrt, for the normal-map encode
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported / cross-file entry points (each verified against its definition) ─────────
extern int Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118   int Sys_Printf(const char*,...)

// The 28 this writer spells out in a dozen places IS sizeof(GfxImageFileHeader); if the
// struct grows, this fires instead of the writer silently emitting a short header.
static_assert( sizeof( GfxImageFileHeader ) == 28, "GfxImageFileHeader must be 28 bytes (r_image.h:118)" );

// ── local helpers ──────────────────────────────────────────────────────────────────
namespace
{

void SetErr( char *err, size_t errSz, const char *fmt, ... )
{
    if ( !err || errSz == 0 )
        return;
    va_list ap;
    va_start( ap, fmt );
    _vsnprintf( err, errSz - 1, fmt, ap );
    va_end( ap );
    err[errSz - 1] = '\0';
}

bool IsPow2( int v ) { return v > 0 && ( v & ( v - 1 ) ) == 0; }

int NextPow2( int v )
{
    int p = 1;
    while ( p < v && p < 4096 )
        p <<= 1;
    return p;
}

// Image_CountMipmaps transcribed: the count the READER derives from the header we write, so
// the writer must produce exactly this many levels.
int CountMipmaps( unsigned char imageFlags, int width, int height, int depth )
{
    if ( ( imageFlags & 2 ) != 0 )                      // IMG_FLAG_NOMIPMAPS
        return 1;
    int mipCount = 1;
    for ( int mipRes = 1; mipRes < width || mipRes < height || mipRes < depth; mipRes *= 2 )
        ++mipCount;
    return mipCount;
}

// The reader's per-level advance, verbatim.
int LevelSize( int format, int w, int h )
{
    if ( format == KIWI_IWI_DXT1 )
        return 8 * ( ( h + 3 ) >> 2 ) * ( ( w + 3 ) >> 2 );
    if ( format == KIWI_IWI_DXT5 )
        return 16 * ( ( h + 3 ) >> 2 ) * ( ( w + 3 ) >> 2 );
    return 4 * h * w;                                   // KIWI_IWI_ARGB8
}

int MipDim( int base, int level )
{
    const int v = base >> level;
    return v > 1 ? v : 1;
}

_D3DFORMAT D3DFormatFor( int iwiFormat )
{
    if ( iwiFormat == KIWI_IWI_DXT1 ) return D3DFMT_DXT1;
    if ( iwiFormat == KIWI_IWI_DXT5 ) return D3DFMT_DXT5;
    return D3DFMT_A8R8G8B8;
}

// D3DX reports a source format; we only need "does it carry alpha".  The list is the set
// D3DXGetImageInfoFromFileA can report for the file types we accept.
bool FormatHasAlpha( _D3DFORMAT f )
{
    switch ( f )
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_A8L8:
    case D3DFMT_A4L4:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        return true;
    default:
        return false;
    }
}

// ── the normal-map encode ──────────────────────────────────────────────────────────
// The pixel shader's own literal, `def c1, 4.08, 4.06452, -2.08, -2.06452`, decoding
//     x = alpha * SCALE_X + BIAS_X          y = green * SCALE_Y + BIAS_Y
// from the sampler's [0,1] values.  Spelled as the shader spells them so the inversion
// below reads as an inversion and not as two magic numbers.
const float kNrmDecodeScaleX = 4.08f;
const float kNrmDecodeBiasX  = -2.08f;
const float kNrmDecodeScaleY = 4.06452f;
const float kNrmDecodeBiasY  = -2.06452f;

// Slopes beyond the representable window (alpha 0..255 spans x in [-2.08, +2.0]) clamp at
// the byte anyway; this bound only keeps the float->int conversion finite for a degenerate
// nz -> 0 pixel.
const float kNrmSlopeLimit = 4.0f;

unsigned char EncodeNormalByte( float slope, float decodeScale, float decodeBias )
{
    if ( slope < -kNrmSlopeLimit ) slope = -kNrmSlopeLimit;
    if ( slope >  kNrmSlopeLimit ) slope =  kNrmSlopeLimit;
    const float raw = ( slope - decodeBias ) / decodeScale;      // back to the sampler's [0,1]
    int v = (int)( raw * 255.0f + 0.5f );
    if ( v < 0 )   v = 0;
    if ( v > 255 ) v = 255;
    return (unsigned char)v;
}

// One A8R8G8B8 mip level in place: a conventional RGB tangent-space normal becomes the
// engine's two-channel form (alpha = x slope, R=G=B = y slope).  A8R8G8B8 is 0xAARRGGBB in
// a DWORD, i.e. B,G,R,A in memory order, which is why the byte indices read 2,1,0,3.
void EncodeNormalLevel( unsigned char *bits, int pitch, int w, int h )
{
    for ( int y = 0; y < h; ++y )
    {
        unsigned char *row = bits + (size_t)pitch * (size_t)y;
        for ( int x = 0; x < w; ++x )
        {
            unsigned char *p = row + 4 * x;
            float nx = (float)p[2] / 127.5f - 1.0f;
            float ny = (float)p[1] / 127.5f - 1.0f;
            float nz = (float)p[0] / 127.5f - 1.0f;
            const float len = (float)sqrt( (double)( nx * nx + ny * ny + nz * nz ) );
            if ( len > 1e-6f )
            {
                nx /= len;
                ny /= len;
                nz /= len;
            }
            else
            {
                nx = 0.0f;
                ny = 0.0f;
                nz = 1.0f;
            }
            // A pixel facing away from the surface has no slope representation at all.
            if ( nz < 1.0f / 256.0f )
                nz = 1.0f / 256.0f;

            const unsigned char a = EncodeNormalByte( nx / nz, kNrmDecodeScaleX, kNrmDecodeBiasX );
            const unsigned char g = EncodeNormalByte( ny / nz, kNrmDecodeScaleY, kNrmDecodeBiasY );
            p[0] = g;
            p[1] = g;
            p[2] = g;
            p[3] = a;
        }
    }
}

}  // namespace

// ── PROBE ──────────────────────────────────────────────────────────────────────────
bool KiwiIwi_Probe( const char *srcPath, kiwiIwiSource_t *out, char *err, size_t errSz )
{
    if ( !srcPath || !srcPath[0] || !out )
    {
        SetErr( err, errSz, "no source path" );
        return false;
    }

    D3DXIMAGE_INFO info;
    memset( &info, 0, sizeof( info ) );
    const HRESULT hr = ::D3DXGetImageInfoFromFileA( srcPath, &info );
    if ( FAILED( hr ) )
    {
        SetErr( err, errSz, "not a readable image (D3DXGetImageInfoFromFile 0x%08X)", (unsigned)hr );
        return false;
    }
    if ( info.Width == 0 || info.Height == 0 )
    {
        SetErr( err, errSz, "image has a zero dimension" );
        return false;
    }
    if ( info.Width > 4096 || info.Height > 4096 )
    {
        SetErr( err, errSz, "image is %ux%u; the limit is 4096", info.Width, info.Height );
        return false;
    }
    if ( info.ResourceType != D3DRTYPE_TEXTURE )
    {
        // A .dds can carry a cubemap or a volume texture; this writer emits 2D only.
        SetErr( err, errSz, "only 2D images are supported (this file is a cube/volume texture)" );
        return false;
    }

    out->width        = (int)info.Width;
    out->height       = (int)info.Height;
    out->hasAlpha     = FormatHasAlpha( info.Format );
    out->isPowerOfTwo = IsPow2( out->width ) && IsPow2( out->height );
    out->potWidth     = NextPow2( out->width );
    out->potHeight    = NextPow2( out->height );
    return true;
}

// ── PREVIEW ────────────────────────────────────────────────────────────────────────
// D3DPOOL_MANAGED deliberately: a managed texture survives a device reset unaided, so the
// preview needs NO entry in the RTT_ReleaseForReset list.  One mip level.
IDirect3DTexture9 *KiwiIwi_CreatePreview( const char *srcPath )
{
    if ( !srcPath || !srcPath[0] || !dx.device )
        return nullptr;

    IDirect3DTexture9 *tex = nullptr;
    const HRESULT hr = ::D3DXCreateTextureFromFileExA(
        dx.device, srcPath,
        D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2,   // keep the true aspect in the preview
        1,                                            // one level
        0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        D3DX_FILTER_TRIANGLE | D3DX_FILTER_DITHER, D3DX_FILTER_NONE,
        0, nullptr, nullptr, &tex );
    if ( FAILED( hr ) )
        return nullptr;
    return tex;
}

// ── WRITE ──────────────────────────────────────────────────────────────────────────
bool KiwiIwi_WriteFromFile( const char *srcPath, const char *qpath,
                            const kiwiIwiOptions_t *opt, kiwiIwiResult_t *out,
                            char *err, size_t errSz )
{
    if ( out )
        memset( out, 0, sizeof( *out ) );

    kiwiIwiSource_t src;
    if ( !KiwiIwi_Probe( srcPath, &src, err, errSz ) )
        return false;

    if ( !dx.device )
    {
        SetErr( err, errSz, "no D3D device (the renderer is not up)" );
        return false;
    }

    // ── dimensions ──────────────────────────────────────────────────────────────────
    int width  = src.width;
    int height = src.height;
    bool resampled = false;
    if ( !src.isPowerOfTwo )
    {
        if ( !opt || !opt->resampleToPot )
        {
            SetErr( err, errSz,
                    "%dx%d is not a power of two (see kiwi_iwi.h D-BE-D); enable the resample option",
                    src.width, src.height );
            return false;
        }
        width     = src.potWidth;
        height    = src.potHeight;
        resampled = true;
    }

    // ── format ──────────────────────────────────────────────────────────────────────
    // The normal and specular slots are ALWAYS DXT5, so "Compress" does not apply to them.
    const int encoding = opt ? opt->encoding : KIWI_IWI_ENC_COLOR;
    int format = KIWI_IWI_ARGB8;
    if ( encoding != KIWI_IWI_ENC_COLOR )
        format = KIWI_IWI_DXT5;
    else if ( opt && opt->compress )
        format = src.hasAlpha ? KIWI_IWI_DXT5 : KIWI_IWI_DXT1;
    // A DXT block is 4x4; anything narrower rounds UP inside D3D and stops matching the
    // reader's ((w+3)>>2) advance for the last levels.  Fall back to uncompressed.
    if ( format != KIWI_IWI_ARGB8 && ( width < 4 || height < 4 ) )
        format = KIWI_IWI_ARGB8;

    const unsigned char flags = 0;                         // full mip chain, picmip-able
    const int mipCount = CountMipmaps( flags, width, height, 1 );

    // ── decode ──────────────────────────────────────────────────────────────────────
    // D3DPOOL_SYSTEMMEM: never drawn, only LockRect'd, and cannot be lost by a device reset
    // mid-import.  MipLevels is passed EXPLICITLY so the chain D3DX builds is the one the
    // reader will count from our header — the invariant checked immediately below.
    // A NORMAL map is decoded into A8R8G8B8 first (whole mip chain filtered from the SOURCE
    // normals), then each level is encoded in place and converted to the final format.
    // D3DX_FILTER_DITHER is dropped on that path: dithering a normal map scatters its slopes.
    const bool  twoStage    = ( encoding == KIWI_IWI_ENC_NORMAL );
    const DWORD resampleFlt = twoStage ? D3DX_FILTER_TRIANGLE
                                       : ( D3DX_FILTER_TRIANGLE | D3DX_FILTER_DITHER );

    IDirect3DTexture9 *tex = nullptr;
    HRESULT hr = ::D3DXCreateTextureFromFileExA(
        dx.device, srcPath,
        (UINT)width, (UINT)height, (UINT)mipCount,
        0, twoStage ? D3DFMT_A8R8G8B8 : D3DFormatFor( format ), D3DPOOL_SYSTEMMEM,
        resampleFlt,                                 // resample filter
        D3DX_FILTER_BOX,                             // mip filter
        0, nullptr, nullptr, &tex );
    if ( FAILED( hr ) || !tex )
    {
        SetErr( err, errSz, "decode failed (D3DXCreateTextureFromFileEx 0x%08X)", (unsigned)hr );
        return false;
    }

    if ( twoStage )
    {
        for ( int L = 0; L < mipCount; ++L )
        {
            D3DLOCKED_RECT lr;
            memset( &lr, 0, sizeof( lr ) );
            if ( FAILED( tex->LockRect( (UINT)L, &lr, nullptr, 0 ) ) || !lr.pBits )
            {
                SetErr( err, errSz, "LockRect failed encoding normal-map mip level %d", L );
                tex->Release();
                return false;
            }
            EncodeNormalLevel( (unsigned char *)lr.pBits, lr.Pitch,
                               MipDim( width, L ), MipDim( height, L ) );
            tex->UnlockRect( (UINT)L );
        }

        if ( format != KIWI_IWI_ARGB8 )
        {
            // Compress level by level.  D3DX_FILTER_NONE makes this a pure format conversion
            // of an already-correctly-sized surface.
            IDirect3DTexture9 *packed = nullptr;
            hr = ::D3DXCreateTexture( dx.device, (UINT)width, (UINT)height, (UINT)mipCount,
                                      0, D3DFormatFor( format ), D3DPOOL_SYSTEMMEM, &packed );
            if ( FAILED( hr ) || !packed )
            {
                SetErr( err, errSz, "could not create the DXT5 target (D3DXCreateTexture 0x%08X)",
                        (unsigned)hr );
                tex->Release();
                return false;
            }
            bool convOk = true;
            for ( int L = 0; L < mipCount && convOk; ++L )
            {
                IDirect3DSurface9 *sSrc = nullptr;
                IDirect3DSurface9 *sDst = nullptr;
                if ( FAILED( tex->GetSurfaceLevel( (UINT)L, &sSrc ) )
                     || FAILED( packed->GetSurfaceLevel( (UINT)L, &sDst ) )
                     || FAILED( ::D3DXLoadSurfaceFromSurface( sDst, nullptr, nullptr,
                                                              sSrc, nullptr, nullptr,
                                                              D3DX_FILTER_NONE, 0 ) ) )
                {
                    SetErr( err, errSz, "normal-map compression failed on mip level %d", L );
                    convOk = false;
                }
                if ( sDst )
                    sDst->Release();
                if ( sSrc )
                    sSrc->Release();
            }
            tex->Release();
            if ( !convOk )
            {
                packed->Release();
                return false;
            }
            tex = packed;
        }
    }

    D3DSURFACE_DESC desc;
    memset( &desc, 0, sizeof( desc ) );
    if ( FAILED( tex->GetLevelDesc( 0, &desc ) )
         || (int)desc.Width != width || (int)desc.Height != height
         || (int)tex->GetLevelCount() != mipCount )
    {
        SetErr( err, errSz,
                "D3DX produced %ux%u/%u levels, not %dx%d/%d - refusing to write a header the reader would disagree with",
                desc.Width, desc.Height, tex->GetLevelCount(), width, height, mipCount );
        tex->Release();
        return false;
    }

    // ── size the file, exactly as the reader will ──────────────────────────────────
    std::vector<int> levelSize( (size_t)mipCount, 0 );
    int dataBytes = 0;
    for ( int L = 0; L < mipCount; ++L )
    {
        levelSize[(size_t)L] = LevelSize( format, MipDim( width, L ), MipDim( height, L ) );
        dataBytes += levelSize[(size_t)L];
    }

    std::vector<unsigned char> blob( (size_t)( 28 + dataBytes ), 0 );
    unsigned char *p = &blob[0];

    p[0] = 'I'; p[1] = 'W'; p[2] = 'i';                     // Image_ValidateHeader r_image.cpp:440
    p[3] = 6;                                               // Image_ValidateHeader r_image.cpp:442
    p[4] = (unsigned char)format;                           // Image_LoadFromData   r_image_load_obj.cpp:542
    p[5] = flags;                                           // Image_CountMipmaps / Image_Setup
    *(short *)( p +  6 ) = (short)width;                    // Image_SetupFromFile  r_image_load_obj.cpp:224
    *(short *)( p +  8 ) = (short)height;                   //                      :228
    *(short *)( p + 10 ) = (short)1;                        //                      :232 (depth)
    for ( int pm = 0; pm < 4; ++pm )
    {
        const int first = ( pm < mipCount - 1 ) ? pm : mipCount - 1;
        int total = 28;
        for ( int L = first; L < mipCount; ++L )
            total += levelSize[(size_t)L];
        *(int *)( p + 12 + 4 * pm ) = total;                // readSize r_image_load_obj.cpp:431
    }

    // ── mip data, SMALLEST FIRST, tightly packed ────────────────────────────────────
    // The reader starts at mipLevel == mipCount-1 (the 1x1 end) and advances by exactly the
    // level size, so the file must present the levels in that order with no padding.
    // LockRect hands us rows at the driver's own pitch, hence the repack.
    unsigned char *dst = p + 28;
    bool copyOk = true;
    for ( int L = mipCount - 1; L >= 0 && copyOk; --L )
    {
        const int w = MipDim( width, L );
        const int h = MipDim( height, L );
        const int rows      = ( format == KIWI_IWI_ARGB8 ) ? h : ( ( h + 3 ) >> 2 );
        const int rowBytes  = ( format == KIWI_IWI_ARGB8 ) ? 4 * w
                            : ( ( format == KIWI_IWI_DXT1 ) ? 8 : 16 ) * ( ( w + 3 ) >> 2 );

        D3DLOCKED_RECT lr;
        memset( &lr, 0, sizeof( lr ) );
        if ( FAILED( tex->LockRect( (UINT)L, &lr, nullptr, D3DLOCK_READONLY ) ) || !lr.pBits )
        {
            SetErr( err, errSz, "LockRect failed on mip level %d", L );
            copyOk = false;
            break;
        }
        const unsigned char *srcRow = (const unsigned char *)lr.pBits;
        for ( int r = 0; r < rows; ++r )
        {
            memcpy( dst, srcRow, (size_t)rowBytes );
            dst    += rowBytes;
            srcRow += lr.Pitch;
        }
        tex->UnlockRect( (UINT)L );
    }
    tex->Release();
    if ( !copyOk )
        return false;

    if ( (int)( dst - p ) != 28 + dataBytes )
    {
        SetErr( err, errSz, "internal: packed %d bytes, expected %d", (int)( dst - p ), 28 + dataBytes );
        return false;
    }

    // ── write ───────────────────────────────────────────────────────────────────────
    // <fs_homepath>/raw/<qpath>, directories created on the way.  raw/ rather than
    // fs_gamedir ("main"): the map compilers' searchpaths are raw/raw_shared/devraw
    // only, so an import into main/ is invisible to them.
    const int h = FS_FOpenFileWriteToDir( qpath, "raw" );
    if ( !h )
    {
        SetErr( err, errSz, "could not open '%s' for writing", qpath );
        return false;
    }
    const unsigned wrote = FS_Write( (const char *)p, (unsigned)( 28 + dataBytes ), h );
    FS_FCloseFile( h );
    if ( wrote != (unsigned)( 28 + dataBytes ) )
    {
        FS_DeleteInDir( (char*)qpath, (char *)"raw" );
        SetErr( err, errSz, "short write to '%s' (%u of %d bytes)", qpath, wrote, 28 + dataBytes );
        return false;
    }

    if ( out )
    {
        out->width     = width;
        out->height    = height;
        out->mipCount  = mipCount;
        out->format    = format;
        out->fileSize  = 28 + dataBytes;
        out->resampled = resampled;
    }
    return true;
}

// ── ROUND-TRIP GATE, STAGE 1: the engine's own header functions over our own file ───
bool KiwiIwi_VerifyOnDisk( const char *qpath, char *err, size_t errSz )
{
    int fh = 0;
    com_fileAccessed = 1;
    const int fileSize = (int)FS_FOpenFileRead( qpath, &fh );
    if ( !fh || fileSize < 0 )
    {
        SetErr( err, errSz, "the engine filesystem cannot find '%s' after writing it", qpath );
        return false;
    }
    if ( fileSize < (int)sizeof( GfxImageFileHeader ) )      // r_image_load_obj.cpp:408
    {
        FS_FCloseFile( fh );
        SetErr( err, errSz, "'%s' is %d bytes, shorter than the 28-byte header", qpath, fileSize );
        return false;
    }

    GfxImageFileHeader hdr;
    memset( &hdr, 0, sizeof( hdr ) );
    const unsigned got = FS_Read( (uint8_t *)&hdr, sizeof( hdr ), fh );
    FS_FCloseFile( fh );
    if ( got != sizeof( hdr ) )
    {
        SetErr( err, errSz, "could not read the header back from '%s'", qpath );
        return false;
    }

    // The engine's own validator — tag + version.  It prints its own Com_PrintError.
    if ( !Image_ValidateHeader( &hdr, qpath ) )
    {
        SetErr( err, errSz, "the engine rejected the header of '%s'", qpath );
        return false;
    }
    // fileSizeForPicmip[0] must equal the whole file (the loader vasserts this).
    if ( hdr.fileSizeForPicmip[0] != fileSize )
    {
        SetErr( err, errSz, "'%s': fileSizeForPicmip[0]=%d but the file is %d bytes",
                qpath, hdr.fileSizeForPicmip[0], fileSize );
        return false;
    }
    // The engine's own mip count, from our flags/dimensions.  The const-pointer overload is
    // the one the loader calls; r_image.h also declares a non-const int twin, hence the cast.
    const uint mips = Image_CountMipmapsForFile( (const GfxImageFileHeader *)&hdr );
    if ( mips < 1 )
    {
        SetErr( err, errSz, "'%s': the engine counts %u mip levels", qpath, mips );
        return false;
    }
    // readSize for the worst picmip the engine can ask for must stay inside the file.
    for ( int pm = 0; pm < 4; ++pm )
    {
        if ( hdr.fileSizeForPicmip[pm] < (int)sizeof( GfxImageFileHeader )
             || hdr.fileSizeForPicmip[pm] > fileSize )
        {
            SetErr( err, errSz, "'%s': fileSizeForPicmip[%d]=%d is outside [28,%d]",
                    qpath, pm, hdr.fileSizeForPicmip[pm], fileSize );
            return false;
        }
    }
    return true;
}

// ── EXTENSION FILTER ───────────────────────────────────────────────────────────────
bool KiwiIwi_IsAcceptedExtension( const char *path )
{
    if ( !path || !path[0] )
        return false;
    const char *dot = strrchr( path, '.' );
    if ( !dot )
        return false;
    static const char *kExt[] = { ".tga", ".png", ".jpg", ".jpeg", ".bmp", ".dds" };
    for ( int i = 0; i < (int)( sizeof( kExt ) / sizeof( kExt[0] ) ); ++i )
        if ( _stricmp( dot, kExt[i] ) == 0 )
            return true;
    return false;
}

const char *KiwiIwi_AcceptedExtensionList()
{
    return "*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.dds";
}
