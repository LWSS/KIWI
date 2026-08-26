// kiwi_texgrave.cpp - the deferred-release graveyard; see kiwi_texgrave.h.
#include "stdafx.h"
#include "qe3.h"

#include <d3d9.h>

#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_gfx.h>

#include "kiwi_texgrave.h"

#include <string>
#include <vector>

namespace
{

std::vector<IDirect3DTexture9 *> s_dead;
std::vector<std::string>         s_reloads;

// Bound queue work per frame; reloads perform disk I/O and texture creation.
const size_t KIWI_TEXGRAVE_RELOADS_PER_FRAME  = 4;
const size_t KIWI_TEXGRAVE_RELEASES_PER_FRAME = 256;

}  // namespace

void KiwiTexGrave_Release( IDirect3DTexture9 *tex )
{
    if ( !tex )
        return;
    s_dead.push_back( tex );
}

void KiwiTexGrave_ReloadImage( const char *imageName )
{
    if ( !imageName || !imageName[0] )
        return;
    for ( size_t i = 0; i < s_reloads.size(); ++i )
        if ( s_reloads[i] == imageName )
            return;
    s_reloads.push_back( imageName );
}

void KiwiTexGrave_Drain()
{
    // Reload before this frame's UI can record Image_Reload's replacement texture.
    if ( !s_reloads.empty() )
    {
        const size_t n = ( s_reloads.size() < KIWI_TEXGRAVE_RELOADS_PER_FRAME )
                         ? s_reloads.size() : KIWI_TEXGRAVE_RELOADS_PER_FRAME;
        // Remove the batch first so reentrant reload requests remain queued.
        std::vector<std::string> work( s_reloads.begin(), s_reloads.begin() + n );
        s_reloads.erase( s_reloads.begin(), s_reloads.begin() + n );
        for ( size_t i = 0; i < work.size(); ++i )
            if ( GfxImage *img = Image_FindExisting( work[i].c_str() ) )
                Image_Reload( img );
    }

    // The previous frame has presented, so its retired UI textures are safe to release.
    const size_t dn = ( s_dead.size() < KIWI_TEXGRAVE_RELEASES_PER_FRAME )
                      ? s_dead.size() : KIWI_TEXGRAVE_RELEASES_PER_FRAME;
    for ( size_t i = 0; i < dn; ++i )
        if ( s_dead[i] )
            s_dead[i]->Release();
    s_dead.erase( s_dead.begin(), s_dead.begin() + dn );
}

void KiwiTexGrave_ReleaseForReset()
{
    // Device loss forbids reloads; ImGui draw data has already been invalidated.
    for ( size_t i = 0; i < s_dead.size(); ++i )
        if ( s_dead[i] )
            s_dead[i]->Release();
    s_dead.clear();
}
