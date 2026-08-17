#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_texcache.h — KIWI-UX (CLEANUP, C-65): the by-name texture cache, once.
//
// kiwi_entthumb.cpp (entity-class thumbnails) and kiwi_skybox.cpp (cube-face
// tiles) each carried the SAME three things: the entry struct — down to the same
// `// resolved once; never retried` comment on the same `failed` flag — the same
// std::map<std::string, entry>, and the same release loop.  Both are reached from
// the same two lines of radiant_rtt.cpp's reset teardown, so any future rule (an
// LRU cap; "a lost device must not cache the failure", which C-67 already needed)
// had to be written twice.
//
// WHAT IS **NOT** HERE, deliberately: the BUILD.  The two differ in what they
// render, in how they report a failure and in how they tell a transient device
// loss from a real one, and those are the parts that carry the argument.  Each
// file keeps its own lookup/insert arms over this storage.
//
// Header-only.  `failed == true` with `tex == nullptr` means "asked and answered,
// do not ask again"; an absent key means "never asked".
// ─────────────────────────────────────────────────────────────────────────────

#include <d3d9.h>
#include <map>
#include <string>

struct kiwiTexEntry_t
{
    IDirect3DTexture9 *tex    = nullptr;
    bool               failed = false;   // resolved once; never retried
};

typedef std::map< std::string, kiwiTexEntry_t > kiwiTexCache_t;

// Release every live texture and empty the map.  Idempotent, and safe to call
// from a device-reset path — it touches nothing but the cache's own references.
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
