#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "kiwi_thumbcache.h"

#include <d3d9.h>
#include <gfx_d3d/r_init.h>
#include <universal/com_files.h>

#include <float.h>
#include <set>
#include <string>
#include <vector>
#include <string.h>

extern int  Sys_Printf( const char *fmt, ... );
extern BOOL FS_UseSearchPath( const searchpath_s *search );

namespace
{
    const unsigned             KTHUMB_MAGIC        = 0x4D48544Bu; // "KTHM" bytes
    const unsigned             KTHUMB_FILE_VERSION = 1;
    const kiwiThumbSourceHash_t FNV64_OFFSET       = 14695981039346656037ui64;
    const kiwiThumbSourceHash_t FNV64_PRIME        = 1099511628211ui64;

#pragma pack(push, 1)
    struct kthumbHeader_t
    {
        unsigned             magic;
        unsigned             version;
        unsigned             width;
        unsigned             height;
        unsigned             format;
        kiwiThumbSourceHash_t sourceHash;
        unsigned             renderVersion;
    };
#pragma pack(pop)
    static_assert( sizeof( kthumbHeader_t ) == 32, "kthumb header layout" );

    bool     s_initialized        = false;
    bool     s_warnedWriteFailure = false;
    unsigned s_invalidateSerial   = 1;

    std::string NormalizeModelName( const char *name )
    {
        const char *start = name ? name : "";
        if ( _strnicmp( start, "xmodel", 6 ) == 0 &&
             ( start[6] == '/' || start[6] == '\\' ) )
            start += 7;

        std::string out;
        for ( const unsigned char *p = (const unsigned char *)start; *p; ++p )
        {
            unsigned char c = *p;
            if ( c == '\\' )
                c = '/';
            if ( c >= 'A' && c <= 'Z' )
                c = (unsigned char)( c - 'A' + 'a' );
            if ( c == '/' && out.empty() )
                continue;
            out.push_back( (char)c );
        }
        return out;
    }

    std::string NormalizePathForHash( const char *path )
    {
        std::string out;
        for ( const unsigned char *p = (const unsigned char *)( path ? path : "" ); *p; ++p )
        {
            unsigned char c = *p;
            if ( c == '\\' )
                c = '/';
            if ( c >= 'A' && c <= 'Z' )
                c = (unsigned char)( c - 'A' + 'a' );
            out.push_back( (char)c );
        }
        return out;
    }

    void HashBytes( kiwiThumbSourceHash_t &hash, const void *data, size_t size )
    {
        const unsigned char *p = (const unsigned char *)data;
        for ( size_t i = 0; i < size; ++i )
        {
            hash ^= p[i];
            hash *= FNV64_PRIME;
        }
    }

    void HashByte( kiwiThumbSourceHash_t &hash, unsigned char value )
    {
        HashBytes( hash, &value, 1 );
    }

    void HashString( kiwiThumbSourceHash_t &hash, const std::string &value )
    {
        if ( !value.empty() )
            HashBytes( hash, value.data(), value.size() );
        HashByte( hash, 0 );
    }

    void HashU64( kiwiThumbSourceHash_t &hash, kiwiThumbSourceHash_t value )
    {
        unsigned char bytes[8];
        for ( int i = 0; i < 8; ++i )
            bytes[i] = (unsigned char)( value >> ( i * 8 ) );
        HashBytes( hash, bytes, sizeof( bytes ) );
    }

    kiwiThumbSourceHash_t ModelId( const char *name )
    {
        const std::string normalized = NormalizeModelName( name );
        kiwiThumbSourceHash_t hash = FNV64_OFFSET;
        if ( !normalized.empty() )
            HashBytes( hash, normalized.data(), normalized.size() );
        return hash;
    }

    bool CacheDirectory( char *out, size_t outSize )
    {
        if ( !out || !outSize || !fs_basepath || !fs_basepath->current.string ||
             !fs_basepath->current.string[0] )
            return false;
        const int n = _snprintf( out, outSize, "%s\\kiwi_cache\\thumbs",
                                 fs_basepath->current.string );
        out[outSize - 1] = '\0';
        return n >= 0 && (size_t)n < outSize;
    }

    bool CacheQPath( const char *modelName, char *out, size_t outSize )
    {
        if ( !out || !outSize || NormalizeModelName( modelName ).empty() )
            return false;
        const int n = _snprintf( out, outSize, "thumbs/model_%016I64x.kthumb",
                                 ModelId( modelName ) );
        out[outSize - 1] = '\0';
        return n >= 0 && (size_t)n < outSize;
    }

    bool CacheOSPath( const char *modelName, char *out, size_t outSize )
    {
        char dir[MAX_PATH];
        if ( !CacheDirectory( dir, sizeof( dir ) ) || !out || !outSize )
            return false;
        const int n = _snprintf( out, outSize, "%s\\model_%016I64x.kthumb",
                                 dir, ModelId( modelName ) );
        out[outSize - 1] = '\0';
        return n >= 0 && (size_t)n < outSize;
    }

    void EnsureCacheDirectories()
    {
        if ( !fs_basepath || !fs_basepath->current.string || !fs_basepath->current.string[0] )
            return;
        char parent[MAX_PATH];
        const int parentLen = _snprintf( parent, sizeof( parent ), "%s\\kiwi_cache",
                                         fs_basepath->current.string );
        parent[sizeof( parent ) - 1] = '\0';
        if ( parentLen < 0 || parentLen >= (int)sizeof( parent ) )
            return;
        ::CreateDirectoryA( parent, nullptr );

        char dir[MAX_PATH];
        if ( CacheDirectory( dir, sizeof( dir ) ) )
            ::CreateDirectoryA( dir, nullptr );
    }

    void CacheStats( unsigned *outFiles, kiwiThumbSourceHash_t *outBytes )
    {
        if ( outFiles )
            *outFiles = 0;
        if ( outBytes )
            *outBytes = 0;
        char dir[MAX_PATH];
        if ( !CacheDirectory( dir, sizeof( dir ) ) )
            return;
        char pattern[MAX_PATH];
        const int n = _snprintf( pattern, sizeof( pattern ), "%s\\*.kthumb", dir );
        pattern[sizeof( pattern ) - 1] = '\0';
        if ( n < 0 || n >= (int)sizeof( pattern ) )
            return;

        WIN32_FIND_DATAA data;
        HANDLE find = ::FindFirstFileA( pattern, &data );
        if ( find == INVALID_HANDLE_VALUE )
            return;
        do
        {
            if ( ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 )
                continue;
            if ( outFiles )
                ++*outFiles;
            if ( outBytes )
                *outBytes += ( (kiwiThumbSourceHash_t)data.nFileSizeHigh << 32 ) |
                             (kiwiThumbSourceHash_t)data.nFileSizeLow;
        } while ( ::FindNextFileA( find, &data ) );
        ::FindClose( find );
    }

    struct sourceStamp_t
    {
        enum kind_t { MISSING, LOOSE, IWD } kind = MISSING;
        std::string container;
        kiwiThumbSourceHash_t size = 0;
        kiwiThumbSourceHash_t mtime = 0;
    };

    bool FileStamp( const char *path, sourceStamp_t &stamp )
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if ( !path || !path[0] ||
             !::GetFileAttributesExA( path, GetFileExInfoStandard, &data ) ||
             ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 )
            return false;
        stamp.size = ( (kiwiThumbSourceHash_t)data.nFileSizeHigh << 32 ) |
                     (kiwiThumbSourceHash_t)data.nFileSizeLow;
        stamp.mtime = ( (kiwiThumbSourceHash_t)data.ftLastWriteTime.dwHighDateTime << 32 ) |
                      (kiwiThumbSourceHash_t)data.ftLastWriteTime.dwLowDateTime;
        return true;
    }

    bool IwdContains( const iwd_t *iwd, const char *qpath )
    {
        if ( !iwd || !qpath || !iwd->hashSize || !iwd->hashTable )
            return false;
        const int hash = FS_HashFileName( qpath, (int)iwd->hashSize );
        for ( fileInIwd_s *file = iwd->hashTable[hash]; file; file = file->next )
            if ( !FS_FilenameCompare( file->name, qpath ) )
                return true;
        return false;
    }

    sourceStamp_t ResolveSource( const char *qpath )
    {
        sourceStamp_t stamp;
        if ( !qpath || !qpath[0] )
            return stamp;
        for ( searchpath_s *search = fs_searchpaths; search; search = search->next )
        {
            if ( !FS_UseSearchPath( search ) )
                continue;
            if ( search->iwd && IwdContains( search->iwd, qpath ) )
            {
                stamp.kind = sourceStamp_t::IWD;
                stamp.container = search->iwd->iwdFilename;
                FileStamp( stamp.container.c_str(), stamp );
                return stamp;
            }
            if ( search->dir )
            {
                char osPath[256];
                FS_BuildOSPath( search->dir->path, search->dir->gamedir, qpath, osPath );
                if ( FileStamp( osPath, stamp ) )
                {
                    stamp.kind = sourceStamp_t::LOOSE;
                    stamp.container = osPath;
                    return stamp;
                }
            }
        }
        return stamp;
    }

    void HashSourceFile( kiwiThumbSourceHash_t &hash, const std::string &qpath )
    {
        HashString( hash, qpath );
        const sourceStamp_t stamp = ResolveSource( qpath.c_str() );
        HashByte( hash, (unsigned char)stamp.kind );
        HashString( hash, NormalizePathForHash( stamp.container.c_str() ) );
        HashU64( hash, stamp.size );
        HashU64( hash, stamp.mtime );
    }

    bool SkipBytes( const unsigned char *&cursor, const unsigned char *end, size_t size )
    {
        if ( size > (size_t)( end - cursor ) )
            return false;
        cursor += size;
        return true;
    }

    bool ReadInt( const unsigned char *&cursor, const unsigned char *end, int &out )
    {
        if ( !SkipBytes( cursor, end, sizeof( out ) ) )
            return false;
        memcpy( &out, cursor - sizeof( out ), sizeof( out ) );
        return true;
    }

    bool ReadUShort( const unsigned char *&cursor, const unsigned char *end,
                     unsigned short &out )
    {
        if ( !SkipBytes( cursor, end, sizeof( out ) ) )
            return false;
        memcpy( &out, cursor - sizeof( out ), sizeof( out ) );
        return true;
    }

    bool ReadCString( const unsigned char *&cursor, const unsigned char *end,
                      std::string &out )
    {
        if ( cursor >= end )
            return false;
        const void *zero = memchr( cursor, 0, (size_t)( end - cursor ) );
        if ( !zero )
            return false;
        const unsigned char *finish = (const unsigned char *)zero;
        out.assign( (const char *)cursor, (size_t)( finish - cursor ) );
        cursor = finish + 1;
        return true;
    }

    bool ParseMaterialNames( const unsigned char *cursor, const unsigned char *end,
                             const std::string lodNames[4],
                             std::set<std::string> &outMaterials )
    {
        int collSurfCount = 0;
        if ( !ReadInt( cursor, end, collSurfCount ) || collSurfCount < 0 )
            return false;
        for ( int i = 0; i < collSurfCount; ++i )
        {
            int triangleCount = 0;
            if ( !ReadInt( cursor, end, triangleCount ) || triangleCount <= 0 ||
                 (size_t)triangleCount > (size_t)( end - cursor ) / 48 )
                return false;
            if ( !SkipBytes( cursor, end, (size_t)triangleCount * 48 ) ||
                 !SkipBytes( cursor, end, 6 * sizeof( float ) + 3 * sizeof( int ) ) )
                return false;
        }

        std::set<std::string> materials;
        for ( int lod = 0; lod < 4; ++lod )
        {
            if ( lodNames[lod].empty() )
                continue;
            unsigned short surfaceCount = 0;
            if ( !ReadUShort( cursor, end, surfaceCount ) )
                return false;
            for ( unsigned int surf = 0; surf < surfaceCount; ++surf )
            {
                std::string material;
                if ( !ReadCString( cursor, end, material ) )
                    return false;
                material = NormalizePathForHash( material.c_str() );
                if ( material == "$default" )
                    material = "$default3d";
                if ( !material.empty() )
                    materials.insert( material );
            }
        }
        outMaterials.swap( materials );
        return true;
    }

    bool ParseXModelHeader( const void *data, size_t size, std::string lodNames[4],
                            std::set<std::string> &materialNames,
                            float mins[3], float maxs[3] )
    {
        if ( !data || size < 2 + 1 + 6 * sizeof( float ) )
            return false;
        const unsigned char *cursor = (const unsigned char *)data;
        const unsigned char *end = cursor + size;
        unsigned short version = 0;
        memcpy( &version, cursor, sizeof( version ) );
        cursor += sizeof( version );
        if ( version != 25 || !SkipBytes( cursor, end, 1 ) )
            return false;
        for ( int i = 0; i < 3; ++i )
        {
            if ( !SkipBytes( cursor, end, sizeof( float ) ) )
                return false;
            memcpy( &mins[i], cursor - sizeof( float ), sizeof( float ) );
        }
        for ( int i = 0; i < 3; ++i )
        {
            if ( !SkipBytes( cursor, end, sizeof( float ) ) )
                return false;
            memcpy( &maxs[i], cursor - sizeof( float ), sizeof( float ) );
        }
        std::string physicsPreset;
        if ( !ReadCString( cursor, end, physicsPreset ) )
            return false;
        for ( int i = 0; i < 4; ++i )
        {
            if ( !SkipBytes( cursor, end, sizeof( float ) ) ||
                 !ReadCString( cursor, end, lodNames[i] ) )
                return false;
            lodNames[i] = NormalizePathForHash( lodNames[i].c_str() );
        }
        if ( !SkipBytes( cursor, end, sizeof( int ) ) )
            return false;
        // Material stamps are best-effort; model/parts/surfs remain the required set.
        ParseMaterialNames( cursor, end, lodNames, materialNames );
        return true;
    }

    bool BoundsValid( const float mins[3], const float maxs[3] )
    {
        for ( int i = 0; i < 3; ++i )
            if ( !_finite( mins[i] ) || !_finite( maxs[i] ) || maxs[i] < mins[i] )
                return false;
        return true;
    }

    bool ReadExact( HANDLE file, void *buffer, unsigned bytes )
    {
        DWORD got = 0;
        return ::ReadFile( file, buffer, bytes, &got, nullptr ) && got == bytes;
    }

    void DeleteCacheOSPath( const char *path )
    {
        if ( path && path[0] )
            ::DeleteFileA( path );
    }

    void BumpInvalidateSerial()
    {
        ++s_invalidateSerial;
        if ( !s_invalidateSerial )
            s_invalidateSerial = 1;
    }
}

void KiwiThumbCache_Init()
{
    if ( s_initialized )
        return;
    s_initialized = true;
    EnsureCacheDirectories();
    unsigned files = 0;
    kiwiThumbSourceHash_t bytes = 0;
    CacheStats( &files, &bytes );
    Sys_Printf( "thumb cache: %u files, %.2f MB\n", files,
                (double)bytes / ( 1024.0 * 1024.0 ) );
}

bool KiwiThumbCache_ResolveModelSource( const char *xmodelName,
                                        char *outContainer, int containerSize,
                                        bool *outLoose )
{
    if ( outContainer && containerSize > 0 )
        outContainer[0] = '\0';
    if ( outLoose )
        *outLoose = false;
    const std::string model = NormalizeModelName( xmodelName );
    if ( model.empty() || !fs_searchpaths )
        return false;
    // Same walk the source hash uses, so the Models browser reveals the file the
    // loader would actually read rather than a shadowed copy further down.
    const sourceStamp_t stamp =
        ResolveSource( ( std::string( "xmodel/" ) + model ).c_str() );
    if ( stamp.kind == sourceStamp_t::MISSING )
        return false;
    if ( outContainer && containerSize > 0 )
    {
        const size_t copied =
            stamp.container.copy( outContainer, (size_t)containerSize - 1 );
        outContainer[copied] = '\0';
    }
    if ( outLoose )
        *outLoose = stamp.kind == sourceStamp_t::LOOSE;
    return true;
}

bool KiwiThumbCache_SourceHash( const char *xmodelName,
                                kiwiThumbSourceHash_t *outHash,
                                float outMins[3], float outMaxs[3],
                                bool *outHaveBounds )
{
    if ( outHash )
        *outHash = 0;
    if ( outHaveBounds )
        *outHaveBounds = false;
    const std::string model = NormalizeModelName( xmodelName );
    if ( model.empty() || !outHash || !fs_searchpaths )
        return false;

    kiwiThumbSourceHash_t hash = FNV64_OFFSET;
    HashString( hash, model );
    const std::string modelQPath = std::string( "xmodel/" ) + model;
    HashSourceFile( hash, modelQPath );

    void *buffer = nullptr;
    const int fileSize = FS_ReadFile( modelQPath.c_str(), &buffer );
    std::string lodNames[4];
    std::set<std::string> materialNames;
    float mins[3] = { 0.0f, 0.0f, 0.0f };
    float maxs[3] = { 0.0f, 0.0f, 0.0f };
    const bool parsed = fileSize > 0 && buffer &&
                        ParseXModelHeader( buffer, (size_t)fileSize,
                                           lodNames, materialNames, mins, maxs );
    if ( buffer )
        FS_FreeFile( (char *)buffer );

    HashByte( hash, parsed ? 1 : 0 );
    if ( parsed )
    {
        // The loader precaches parts from LOD 0, but surfs from every non-empty LOD.
        if ( !lodNames[0].empty() )
            HashSourceFile( hash, std::string( "xmodelparts/" ) + lodNames[0] );
        std::set<std::string> seenSurfs;
        for ( int i = 0; i < 4; ++i )
            if ( !lodNames[i].empty() && seenSurfs.insert( lodNames[i] ).second )
                HashSourceFile( hash, std::string( "xmodelsurfs/" ) + lodNames[i] );
        for ( std::set<std::string>::const_iterator it = materialNames.begin();
              it != materialNames.end(); ++it )
            HashSourceFile( hash, std::string( "materials/" ) + *it );

        if ( BoundsValid( mins, maxs ) )
        {
            if ( outMins && outMaxs )
                for ( int i = 0; i < 3; ++i )
                {
                    outMins[i] = mins[i];
                    outMaxs[i] = maxs[i];
                }
            if ( outHaveBounds )
                *outHaveBounds = true;
        }
    }
    *outHash = hash;
    return true;
}

kiwiThumbCacheLoadResult_t KiwiThumbCache_Load(
    const char *xmodelName, kiwiThumbSourceHash_t sourceHash,
    unsigned renderVersion, unsigned width, unsigned height,
    kiwiThumbCacheFormat_t format, IDirect3DTexture9 **outTexture )
{
    if ( outTexture )
        *outTexture = nullptr;
    if ( !outTexture || !width || !height || format != KIWI_THUMBCACHE_FORMAT_BGRA8 )
        return KIWI_THUMBCACHE_MISS;
    KiwiThumbCache_Init();

    char path[MAX_PATH];
    if ( !CacheOSPath( xmodelName, path, sizeof( path ) ) )
        return KIWI_THUMBCACHE_MISS;
    HANDLE file = ::CreateFileA( path, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    if ( file == INVALID_HANDLE_VALUE )
        return KIWI_THUMBCACHE_MISS;

    kthumbHeader_t header;
    LARGE_INTEGER diskSize;
    const kiwiThumbSourceHash_t pixelBytes =
        (kiwiThumbSourceHash_t)width * (kiwiThumbSourceHash_t)height * 4ui64;
    const bool valid = ReadExact( file, &header, sizeof( header ) ) &&
                       ::GetFileSizeEx( file, &diskSize ) &&
                       header.magic == KTHUMB_MAGIC &&
                       header.version == KTHUMB_FILE_VERSION &&
                       header.width == width && header.height == height &&
                       header.format == (unsigned)format &&
                       header.sourceHash == sourceHash &&
                       header.renderVersion == renderVersion &&
                       diskSize.QuadPart == (LONGLONG)( sizeof( header ) + pixelBytes );
    if ( !valid || pixelBytes > 0xFFFFFFFFui64 )
    {
        ::CloseHandle( file );
        DeleteCacheOSPath( path );
        return KIWI_THUMBCACHE_MISS;
    }

    std::vector<unsigned char> pixels( (size_t)pixelBytes );
    const bool readPixels = ReadExact( file, &pixels[0], (unsigned)pixelBytes );
    ::CloseHandle( file );
    if ( !readPixels )
    {
        DeleteCacheOSPath( path );
        return KIWI_THUMBCACHE_MISS;
    }

    if ( !dx.device )
        return KIWI_THUMBCACHE_RETRY;
    IDirect3DTexture9 *texture = nullptr;
    if ( FAILED( dx.device->CreateTexture( width, height, 1, 0,
                                           D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                           &texture, nullptr ) ) || !texture )
        return KIWI_THUMBCACHE_RETRY;

    D3DLOCKED_RECT locked;
    memset( &locked, 0, sizeof( locked ) );
    if ( FAILED( texture->LockRect( 0, &locked, nullptr, 0 ) ) || !locked.pBits )
    {
        texture->Release();
        return KIWI_THUMBCACHE_RETRY;
    }
    const unsigned rowBytes = width * 4;
    for ( unsigned y = 0; y < height; ++y )
        memcpy( (unsigned char *)locked.pBits + (size_t)y * locked.Pitch,
                &pixels[(size_t)y * rowBytes], rowBytes );
    texture->UnlockRect( 0 );
    *outTexture = texture;
    return KIWI_THUMBCACHE_HIT;
}

bool KiwiThumbCache_Write( const char *xmodelName,
                           kiwiThumbSourceHash_t sourceHash,
                           unsigned renderVersion,
                           unsigned width, unsigned height,
                           kiwiThumbCacheFormat_t format,
                           const void *pixels, unsigned rowPitch )
{
    if ( NormalizeModelName( xmodelName ).empty() || !pixels || !width || !height ||
         format != KIWI_THUMBCACHE_FORMAT_BGRA8 || rowPitch < width * 4 )
        return false;
    KiwiThumbCache_Init();
    char qpath[96];
    if ( !CacheQPath( xmodelName, qpath, sizeof( qpath ) ) )
        return false;

    const int handle = FS_FOpenFileWriteToDir( qpath, "kiwi_cache" );
    if ( handle )
    {
        kthumbHeader_t header;
        header.magic         = KTHUMB_MAGIC;
        header.version       = KTHUMB_FILE_VERSION;
        header.width         = width;
        header.height        = height;
        header.format        = (unsigned)format;
        header.sourceHash    = sourceHash;
        header.renderVersion = renderVersion;

        bool ok = FS_Write( (const char *)&header, sizeof( header ), handle ) == sizeof( header );
        const unsigned rowBytes = width * 4;
        for ( unsigned y = 0; ok && y < height; ++y )
            ok = FS_Write( (const char *)pixels + (size_t)y * rowPitch,
                           rowBytes, handle ) == rowBytes;
        FS_FCloseFile( handle );
        if ( ok )
            return true;
        FS_DeleteInDir( qpath, (char *)"kiwi_cache" );
    }

    if ( !s_warnedWriteFailure )
    {
        s_warnedWriteFailure = true;
        Sys_Printf( "thumb cache: could not write '%s' (further write errors suppressed)\n",
                    qpath );
    }
    return false;
}

void KiwiThumbCache_Invalidate( const char *xmodelName )
{
    KiwiThumbCache_Init();
    char path[MAX_PATH];
    if ( CacheOSPath( xmodelName, path, sizeof( path ) ) )
        DeleteCacheOSPath( path );
    BumpInvalidateSerial();
}

void KiwiThumbCache_InvalidateAll()
{
    KiwiThumbCache_Init();
    char dir[MAX_PATH];
    unsigned removed = 0;
    if ( CacheDirectory( dir, sizeof( dir ) ) )
    {
        char pattern[MAX_PATH];
        const int n = _snprintf( pattern, sizeof( pattern ), "%s\\*.kthumb", dir );
        pattern[sizeof( pattern ) - 1] = '\0';
        if ( n >= 0 && n < (int)sizeof( pattern ) )
        {
            WIN32_FIND_DATAA data;
            HANDLE find = ::FindFirstFileA( pattern, &data );
            if ( find != INVALID_HANDLE_VALUE )
            {
                do
                {
                    if ( ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0 )
                        continue;
                    char path[MAX_PATH];
                    const int pathLen = _snprintf( path, sizeof( path ), "%s\\%s",
                                                   dir, data.cFileName );
                    path[sizeof( path ) - 1] = '\0';
                    if ( pathLen >= 0 && pathLen < (int)sizeof( path ) &&
                         ::DeleteFileA( path ) )
                        ++removed;
                } while ( ::FindNextFileA( find, &data ) );
                ::FindClose( find );
            }
        }
    }
    BumpInvalidateSerial();
    Sys_Printf( "thumb cache: cleared %u files\n", removed );
}

unsigned KiwiThumbCache_InvalidateSerial()
{
    return s_invalidateSerial;
}
