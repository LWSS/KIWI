// kiwi_texgrave.cpp - the deferred-release graveyard; see kiwi_texgrave.h.
#include "stdafx.h"
#include "qe3.h"

#include <d3d9.h>

#include <gfx_d3d/r_image.h>        // Image_FindExisting (r_image.h:155) / Image_Reload (r_image.h:426)
#include <gfx_d3d/r_gfx.h>          // GfxImage

#include "kiwi_texgrave.h"

#include <string>
#include <vector>

namespace
{

std::vector<IDirect3DTexture9 *> s_dead;
std::vector<std::string>         s_reloads;

// The drain is BOUNDED: a batch import queues one reload per file and Image_Reload is a
// disk read plus a texture create, so the remainder stays queued for the next frame.
const size_t KIWI_TEXGRAVE_RELOADS_PER_FRAME  = 4;
const size_t KIWI_TEXGRAVE_RELEASES_PER_FRAME = 256;

}  // namespace

void KiwiTexGrave_Release( IDirect3DTexture9 *tex )
{
    if ( !tex )
        return;                      // "release my optional handle" is a legal no-op
    s_dead.push_back( tex );
}

void KiwiTexGrave_ReloadImage( const char *imageName )
{
    if ( !imageName || !imageName[0] )
        return;
    // Collapse duplicates: two overwrite imports of the same name in one frame.
    for ( size_t i = 0; i < s_reloads.size(); ++i )
        if ( s_reloads[i] == imageName )
            return;
    s_reloads.push_back( imageName );
}

void KiwiTexGrave_Drain()
{
    // ENGINE WORK FIRST, and by name: Image_Reload swaps the image's live texture, so it
    // must run BEFORE this frame's UI can record the pointer.
    if ( !s_reloads.empty() )
    {
        const size_t n = ( s_reloads.size() < KIWI_TEXGRAVE_RELOADS_PER_FRAME )
                         ? s_reloads.size() : KIWI_TEXGRAVE_RELOADS_PER_FRAME;
        // Erased BEFORE the work runs, so Image_Reload cannot see a queue still draining.
        std::vector<std::string> work( s_reloads.begin(), s_reloads.begin() + n );
        s_reloads.erase( s_reloads.begin(), s_reloads.begin() + n );
        for ( size_t i = 0; i < work.size(); ++i )
            if ( GfxImage *img = Image_FindExisting( work[i].c_str() ) )
                Image_Reload( img );
    }

    // Then the textures the UI retired last frame; their draw data has been presented.
    const size_t dn = ( s_dead.size() < KIWI_TEXGRAVE_RELEASES_PER_FRAME )
                      ? s_dead.size() : KIWI_TEXGRAVE_RELEASES_PER_FRAME;
    for ( size_t i = 0; i < dn; ++i )
        if ( s_dead[i] )
            s_dead[i]->Release();
    s_dead.erase( s_dead.begin(), s_dead.begin() + dn );
}

void KiwiTexGrave_ReleaseForReset()
{
    // Releases only - a lost device cannot create the replacement texture Image_Reload
    // needs.  The draw data was already thrown away by ImGui_ImplDX9_InvalidateDeviceObjects.
    for ( size_t i = 0; i < s_dead.size(); ++i )
        if ( s_dead[i] )
            s_dead[i]->Release();
    s_dead.clear();
}
