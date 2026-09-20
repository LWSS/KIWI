// .iwi writer; format and encoding contracts are documented in kiwi_iwi.h.
#include "stdafx.h"
#include "qe3.h"

#include <d3d9.h>
// D3DX_DEFAULT_NONPOW2 requires the umbrella header, not just <d3dx9tex.h>.
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

extern int Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118   int Sys_Printf(const char*,...)

// Keep the serialized 28-byte header in lockstep with GfxImageFileHeader.
static_assert( sizeof( GfxImageFileHeader ) == 28, "GfxImageFileHeader must be 28 bytes (r_image.h:118)" );

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

// KIWI (2026-09-17, user: "crash when pasting in that image. This is really bad, imagine
// losing all your progress to this"): D3DX9_43 access-violated twelve frames deep inside
// D3DXCreateTextureFromFileEx on a 4096x4096 picture.  That DLL is closed, unmaintained and
// is handed files the operator found anywhere, so NO call into it may be able to take the
// editor down: every entry goes through one of these SEH shims and a fault becomes an
// ordinary "conversion failed" result.  (Separate functions because MSVC forbids __try in
// a frame that owns C++ objects.)  `*crashed` tells the caller to say so.
HRESULT GuardedImageInfo( const char *path, D3DXIMAGE_INFO *info, bool *crashed )
{
    *crashed = false;
    __try
    {
        return ::D3DXGetImageInfoFromFileA( path, info );
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
        *crashed = true;
        return E_FAIL;
    }
}

HRESULT GuardedCreateTexture( const char *path, UINT w, UINT h, UINT mips, D3DFORMAT fmt,
                              D3DPOOL pool, DWORD filter, DWORD mipFilter,
                              IDirect3DTexture9 **out, bool *crashed )
{
    *crashed = false;
    *out = nullptr;
    __try
    {
        return ::D3DXCreateTextureFromFileExA( dx.device, path, w, h, mips, 0, fmt, pool,
                                               filter, mipFilter, 0, nullptr, nullptr, out );
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
        *crashed = true;
        *out = nullptr;                     // whatever D3DX half-built is abandoned, not released
        return E_FAIL;
    }
}

HRESULT GuardedLoadSurface( IDirect3DSurface9 *dst, IDirect3DSurface9 *src, DWORD filter, bool *crashed )
{
    *crashed = false;
    __try
    {
        return ::D3DXLoadSurfaceFromSurface( dst, nullptr, nullptr, src, nullptr, nullptr, filter, 0 );
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
        *crashed = true;
        return E_FAIL;
    }
}

bool IsPow2( int v ) { return v > 0 && ( v & ( v - 1 ) ) == 0; }

int NextPow2( int v )
{
    int p = 1;
    while ( p < v && p < 4096 )
        p <<= 1;
    return p;
}

// Match the reader's Image_CountMipmaps-derived level count.
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

// Source formats treated as carrying alpha.
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

// These literals invert the shader's [0,1] decode: x=A*4.08-2.08, y=G*4.06452-2.06452.
const float kNrmDecodeScaleX = 4.08f;
const float kNrmDecodeBiasX  = -2.08f;
const float kNrmDecodeScaleY = 4.06452f;
const float kNrmDecodeBiasY  = -2.06452f;

// A finite bound handles nz -> 0; the final byte clamp enforces the representable range.
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

// Convert RGB tangent-space normals in place to A=x slope and grayscale=y slope.
// A8R8G8B8 is B,G,R,A in memory, hence the byte indices 2,1,0,3.
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

bool KiwiIwi_Probe( const char *srcPath, kiwiIwiSource_t *out, char *err, size_t errSz )
{
    if ( !srcPath || !srcPath[0] || !out )
    {
        SetErr( err, errSz, "no source path" );
        return false;
    }

    D3DXIMAGE_INFO info;
    memset( &info, 0, sizeof( info ) );
    bool crashed = false;
    const HRESULT hr = GuardedImageInfo( srcPath, &info, &crashed );
    if ( crashed )
    {
        SetErr( err, errSz, "D3DX crashed reading the image header (caught; the file is malformed or unsupported)" );
        return false;
    }
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

// Managed textures survive device resets without an RTT_ReleaseForReset hook.
IDirect3DTexture9 *KiwiIwi_CreatePreview( const char *srcPath )
{
    if ( !srcPath || !srcPath[0] || !dx.device )
        return nullptr;

    IDirect3DTexture9 *tex = nullptr;
    bool crashed = false;
    const HRESULT hr = GuardedCreateTexture(
        srcPath,
        D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2,   // keep the true aspect in the preview
        1,                                            // one level
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        D3DX_FILTER_TRIANGLE | D3DX_FILTER_DITHER, D3DX_FILTER_NONE,
        &tex, &crashed );
    if ( crashed )
        Sys_Printf( "IWI preview: D3DX crashed decoding '%s' (caught); no preview.\n", srcPath );
    if ( FAILED( hr ) )
        return nullptr;
    return tex;
}

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

    // Normal/specular ignore "Compress" and select DXT5; sub-4 dimensions fall back below.
    const int encoding = opt ? opt->encoding : KIWI_IWI_ENC_COLOR;
    int format = KIWI_IWI_ARGB8;
    if ( encoding != KIWI_IWI_ENC_COLOR )
        format = KIWI_IWI_DXT5;
    else if ( opt && opt->compress )
        format = src.hasAlpha ? KIWI_IWI_DXT5 : KIWI_IWI_DXT1;
    // D3D rounds sub-4 DXT dimensions to a full block; use ARGB8 to preserve header dimensions.
    if ( format != KIWI_IWI_ARGB8 && ( width < 4 || height < 4 ) )
        format = KIWI_IWI_ARGB8;

    const unsigned char flags = 0;                         // full mip chain, picmip-able
    const int mipCount = CountMipmaps( flags, width, height, 1 );

    // SYSTEMMEM textures are lock-only/reset-proof; explicit mipCount must match the reader.
    // Normal maps build ARGB8 mips from source normals before slope encoding and conversion.
    // Dithering is disabled because it perturbs slopes.
    const bool  twoStage    = ( encoding == KIWI_IWI_ENC_NORMAL );
    // A source already at the target size needs no resample at all: D3DX_FILTER_NONE skips
    // the triangle filter's full-image float buffers (a quarter gigabyte at 4096x4096),
    // which is where the 2026-09-17 crash sat.  Only a real resize pays for TRIANGLE.
    DWORD resampleFlt = resampled ? D3DX_FILTER_TRIANGLE : D3DX_FILTER_NONE;
    if ( !twoStage && resampled )
        resampleFlt |= D3DX_FILTER_DITHER;

    IDirect3DTexture9 *tex = nullptr;
    bool crashed = false;
    const D3DFORMAT loadFmt = twoStage ? D3DFMT_A8R8G8B8 : D3DFormatFor( format );
    HRESULT hr = GuardedCreateTexture( srcPath, (UINT)width, (UINT)height, (UINT)mipCount, loadFmt,
                                       D3DPOOL_SYSTEMMEM, resampleFlt, D3DX_FILTER_BOX, &tex, &crashed );
    if ( crashed )
    {
        // One gentler retry: the cheapest filters D3DX has, no dithering.
        Sys_Printf( "IWI writer: D3DX crashed decoding '%s' (caught); retrying with plain filters.\n", srcPath );
        hr = GuardedCreateTexture( srcPath, (UINT)width, (UINT)height, (UINT)mipCount, loadFmt,
                                   D3DPOOL_SYSTEMMEM, resampled ? D3DX_FILTER_LINEAR : D3DX_FILTER_NONE,
                                   D3DX_FILTER_BOX, &tex, &crashed );
        if ( crashed )
        {
            SetErr( err, errSz, "D3DX crashed decoding the image twice (caught; the editor is unharmed). "
                                "Re-save it smaller or as a different format" );
            return false;
        }
    }
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
            // Convert already-sized levels without filtering.
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
                bool surfCrashed = false;
                if ( FAILED( tex->GetSurfaceLevel( (UINT)L, &sSrc ) )
                     || FAILED( packed->GetSurfaceLevel( (UINT)L, &sDst ) )
                     || FAILED( GuardedLoadSurface( sDst, sSrc, D3DX_FILTER_NONE, &surfCrashed ) ) )
                {
                    SetErr( err, errSz, "normal-map compression %s on mip level %d",
                            surfCrashed ? "crashed inside D3DX (caught)" : "failed", L );
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

    // Size the file exactly as the reader does.
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

    // Serialize smallest mip first, matching the reader's descending mip walk.
    // Repack driver-pitched rows without padding.
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

    // Write <fs_homepath>/raw/<qpath>; map compilers search raw/raw_shared/devraw, not main.
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

// Validate the on-disk header through the engine reader.
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

    // The validator checks tag/version and reports its own error.
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
    // The cast selects the loader's const overload; r_image.h also declares non-const twins.
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
