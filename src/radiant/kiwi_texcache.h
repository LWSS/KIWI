#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Shared by-name cache for entity thumbnails and skybox face copies.
// A present key records a completed attempt; absence leaves the request eligible.

#include <d3d9.h>
#include <map>
#include <string>

struct kiwiTexEntry_t
{
    IDirect3DTexture9 *tex    = nullptr;
    bool               failed = false;   // null result; suppress retries until cache clear
};

typedef std::map< std::string, kiwiTexEntry_t > kiwiTexCache_t;

// Release cache-owned COM references before clearing success and failure entries.
inline void KiwiTexCache_ReleaseAll( kiwiTexCache_t &cache )
{
    for ( kiwiTexCache_t::iterator it = cache.begin(); it != cache.end(); ++it )
    {
        if ( it->second.tex )
            it->second.tex->Release();
        it->second.tex = nullptr;
    }
    cache.clear();
}
