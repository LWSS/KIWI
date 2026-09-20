#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// See kiwi_mathdrcache.h.  Boot-time only, single-threaded.

#include "stdafx.h"
#include "kiwi_mathdrcache.h"

#include <universal/com_files.h>    // fs_basepath, fs_searchpaths, searchpath_s, directory_t

#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

extern int Sys_Printf( const char *fmt, ... );

namespace
{
    const unsigned KMH_MAGIC   = 0x4844574Bu;      // "KWDH"
    const unsigned KMH_VERSION = 1u;

    struct kmhStamp_t
    {
        unsigned long long size;
        unsigned long long mtime;                   // FILETIME, 100 ns ticks
    };

    struct kmhEntry_t
    {
        kmhStamp_t                 stamp;
        bool                       good;            // false = the file yields no full header
        std::vector<unsigned char> raw;
        bool                       seen;            // asked for this boot: survives the rewrite
    };

    unsigned                                     s_rawSize = 0;
    bool                                         s_active  = false;
    bool                                         s_dirty   = false;
    int                                          s_hits = 0, s_misses = 0;
    DWORD                                        s_startTick = 0;
    std::unordered_map<std::string, kmhStamp_t>  s_disk;      // lower-case name -> what is on disk now
    std::unordered_map<std::string, kmhEntry_t>  s_cache;

    std::string Lower( const char *s )
    {
        std::string out( s ? s : "" );
        for ( size_t i = 0; i < out.size(); ++i )
            if ( out[i] >= 'A' && out[i] <= 'Z' )
                out[i] = (char)( out[i] - 'A' + 'a' );
        return out;
    }

    bool CachePath( char *out, size_t outSize, bool makeDir )
    {
        if ( !fs_basepath || !fs_basepath->current.string || !fs_basepath->current.string[0] )
            return false;
        char dir[MAX_PATH];
        if ( _snprintf( dir, sizeof( dir ), "%s\\kiwi_cache", fs_basepath->current.string ) < 0 )
            return false;
        dir[sizeof( dir ) - 1] = '\0';
        if ( makeDir )
            ::CreateDirectoryA( dir, nullptr );
        const int n = _snprintf( out, outSize, "%s\\material_headers.bin", dir );
        out[outSize - 1] = '\0';
        return n > 0 && (size_t)n < outSize;
    }

    // Every loose file under <searchpath>\materials, first search path wins (FS search order).
    void IndexDisk()
    {
        s_disk.clear();
        for ( searchpath_s *sp = fs_searchpaths; sp; sp = sp->next )
        {
            if ( !sp->dir )
                continue;                               // an .iwd: no per-file open cost there
            char pattern[MAX_PATH * 2];
            _snprintf( pattern, sizeof( pattern ), "%s\\%s\\materials\\*", sp->dir->path, sp->dir->gamedir );
            pattern[sizeof( pattern ) - 1] = '\0';
            for ( char *c = pattern; *c; ++c )
                if ( *c == '/' ) *c = '\\';
            WIN32_FIND_DATAA fd;
            HANDLE h = ::FindFirstFileExA( pattern, FindExInfoBasic, &fd, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH );
            if ( h == INVALID_HANDLE_VALUE )
                continue;
            do
            {
                if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
                    continue;
                const std::string key = Lower( fd.cFileName );
                if ( s_disk.find( key ) != s_disk.end() )
                    continue;                           // an earlier search path owns this name
                kmhStamp_t st;
                st.size  = ( (unsigned long long)fd.nFileSizeHigh << 32 ) | fd.nFileSizeLow;
                st.mtime = ( (unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32 )
                         | fd.ftLastWriteTime.dwLowDateTime;
                s_disk[key] = st;
            }
            while ( ::FindNextFileA( h, &fd ) );
            ::FindClose( h );
        }
    }

    void LoadCache()
    {
        s_cache.clear();
        char path[MAX_PATH];
        if ( !CachePath( path, sizeof( path ), false ) )
            return;
        FILE *f = fopen( path, "rb" );
        if ( !f )
            return;
        unsigned head[4] = { 0, 0, 0, 0 };
        if ( fread( head, sizeof( unsigned ), 4, f ) == 4
          && head[0] == KMH_MAGIC && head[1] == KMH_VERSION && head[2] == s_rawSize && head[3] < 200000u )
        {
            for ( unsigned i = 0; i < head[3]; ++i )
            {
                unsigned short len = 0;
                char name[512];
                kmhEntry_t e;
                unsigned char good = 0;
                if ( fread( &len, sizeof( len ), 1, f ) != 1 || len == 0 || len >= sizeof( name )
                  || fread( name, 1, len, f ) != len
                  || fread( &e.stamp, sizeof( e.stamp ), 1, f ) != 1
                  || fread( &good, 1, 1, f ) != 1 )
                    break;
                name[len] = '\0';
                e.good = good != 0;
                e.seen = false;
                e.raw.resize( s_rawSize );
                if ( fread( e.raw.data(), 1, s_rawSize, f ) != s_rawSize )
                    break;
                s_cache[name] = e;
            }
        }
        fclose( f );
    }

    void SaveCache()
    {
        char path[MAX_PATH], tmp[MAX_PATH + 8];
        if ( !CachePath( path, sizeof( path ), true ) )
            return;
        _snprintf( tmp, sizeof( tmp ), "%s.tmp", path );
        tmp[sizeof( tmp ) - 1] = '\0';
        FILE *f = fopen( tmp, "wb" );
        if ( !f )
            return;
        unsigned count = 0;
        for ( auto it = s_cache.begin(); it != s_cache.end(); ++it )
            if ( it->second.seen )
                ++count;
        const unsigned head[4] = { KMH_MAGIC, KMH_VERSION, s_rawSize, count };
        bool ok = fwrite( head, sizeof( unsigned ), 4, f ) == 4;
        for ( auto it = s_cache.begin(); ok && it != s_cache.end(); ++it )
        {
            if ( !it->second.seen )
                continue;                               // the material file is gone: drop it
            const unsigned short len = (unsigned short)it->first.size();
            const unsigned char good = it->second.good ? 1 : 0;
            ok = fwrite( &len, sizeof( len ), 1, f ) == 1
              && fwrite( it->first.data(), 1, len, f ) == len
              && fwrite( &it->second.stamp, sizeof( kmhStamp_t ), 1, f ) == 1
              && fwrite( &good, 1, 1, f ) == 1
              && fwrite( it->second.raw.data(), 1, s_rawSize, f ) == s_rawSize;
        }
        fclose( f );
        if ( ok )
            ::MoveFileExA( tmp, path, MOVEFILE_REPLACE_EXISTING );   // never a half-written cache
        else
            ::DeleteFileA( tmp );
    }
}

void KiwiMatHdr_Begin( unsigned rawSize )
{
    s_rawSize   = rawSize;
    s_active    = rawSize > 0 && rawSize <= 4096;
    s_dirty     = false;
    s_hits = s_misses = 0;
    s_startTick = ::GetTickCount();
    if ( !s_active )
        return;
    IndexDisk();
    LoadCache();
}

kiwiMatHdr_t KiwiMatHdr_Get( const char *name, void *rawOut )
{
    if ( !s_active || !name || !rawOut )
        return KMATHDR_MISS;
    const std::string key = Lower( name );
    auto disk = s_disk.find( key );
    if ( disk == s_disk.end() )
        return KMATHDR_MISS;                            // not a loose file: nothing to validate against
    auto it = s_cache.find( key );
    if ( it == s_cache.end() || it->second.stamp.size != disk->second.size
      || it->second.stamp.mtime != disk->second.mtime || it->second.raw.size() != s_rawSize )
    {
        ++s_misses;
        return KMATHDR_MISS;
    }
    it->second.seen = true;
    ++s_hits;
    if ( !it->second.good )
        return KMATHDR_BAD;
    memcpy( rawOut, it->second.raw.data(), s_rawSize );
    return KMATHDR_HIT;
}

void KiwiMatHdr_Put( const char *name, const void *raw )
{
    if ( !s_active || !name )
        return;
    const std::string key = Lower( name );
    auto disk = s_disk.find( key );
    if ( disk == s_disk.end() )
        return;                                         // cannot be validated later: do not cache
    kmhEntry_t e;
    e.stamp = disk->second;
    e.good  = raw != nullptr;
    e.seen  = true;
    e.raw.assign( s_rawSize, 0 );
    if ( raw )
        memcpy( e.raw.data(), raw, s_rawSize );
    s_cache[key] = e;
    s_dirty = true;
}

void KiwiMatHdr_End()
{
    if ( !s_active )
        return;
    // entries nobody asked for this boot belong to deleted materials: rewrite without them
    for ( auto it = s_cache.begin(); it != s_cache.end() && !s_dirty; ++it )
        if ( !it->second.seen )
            s_dirty = true;
    if ( s_dirty )
        SaveCache();
    // Quiet on the normal boot (everything served from the cache); one line only when
    // material files were actually read, i.e. the cache was filled or refreshed.
    if ( s_dirty )
        Sys_Printf( "Load_Materials: %i header%s from kiwi_cache\\material_headers.bin, %i read from disk, %u ms "
                    "(cache rewritten)\n", s_hits, s_hits == 1 ? "" : "s", s_misses,
                    (unsigned)( ::GetTickCount() - s_startTick ) );
    s_disk.clear();
    s_cache.clear();
    s_active = false;
}
