#pragma once
#include <universal/q_shared.h>
#include <gfx_d3d/r_font.h>
#include <math.h>
union XAssetHeader;
static inline bool DB64_ValidateFont(const Font_s *font, bool glyphs)
{
    if (!font || !font->fontName || font->pixelHeight <= 0 || font->pixelHeight > 65535 ||
        font->glyphCount < 96 || font->glyphCount > 65536 || !font->glyphs || !font->material || !font->glowMaterial)
    {
        return false;
    }
    for (int i = 0; glyphs && i < font->glyphCount; ++i)
    {
        const Glyph *g = &font->glyphs[i];
        if ((i < 96 && g->letter != i + 32) || (i > 96 && g->letter <= font->glyphs[i - 1].letter) ||
            !isfinite(g->s0) || !isfinite(g->t0) || !isfinite(g->s1) || !isfinite(g->t1))
        {
            return false;
        }
    }
    return true;
}
void DB64_LoadFontAsset(XAssetHeader *header, bool atStreamStart);
