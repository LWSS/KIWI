#include <universal/q_shared.h>
#include "native_font.h"
#include <gfx_d3d/r_font.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static bool FontError(char *error, size_t size, const char *message)
{
    if (size)
    {
        snprintf(error, size, "%s", message);
    }
    return false;
}

bool Linker_ImportFont(const void *data, size_t size, const char *name,
                       Material *(*resolve)(const char *, void *), void *context,
                       Font_s **result, char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (!data || size < 16 || size > 16 * 1024 * 1024 || !name || !name[0] || strlen(name) >= 256 || !resolve)
    {
        return FontError(error, errorSize, "Invalid raw font input");
    }
    uint32_t header[4];
    memcpy(header, data, sizeof(header));
    const size_t glyphBytes = (size_t)header[2] * sizeof(Glyph);
    if (!header[1] || header[1] > 65535 || header[2] < 96 || header[2] > 65536 || glyphBytes > size - 16 ||
        header[0] < 16 + glyphBytes || header[0] >= size || header[3] < 16 + glyphBytes || header[3] >= size)
    {
        return FontError(error, errorSize, "Invalid font dimensions, glyph count or string offsets");
    }
    const char *source = (const char *)data;
    const char *fontName = source + header[0];
    const char *materialName = source + header[3];
    if (!memchr(fontName, 0, size - header[0]) || !memchr(materialName, 0, size - header[3]) ||
        !fontName[0] || !materialName[0] || strlen(materialName) >= 58)
    {
        return FontError(error, errorSize, "Invalid font or material name");
    }
    const size_t nameSize = strlen(name) + 1;
    Font_s *font = (Font_s *)calloc(1, sizeof(Font_s) + glyphBytes + nameSize);
    if (!font)
    {
        return FontError(error, errorSize, "Out of memory importing font");
    }
    font->glyphs = (Glyph *)(font + 1);
    memcpy(font->glyphs, source + 16, glyphBytes);
    font->fontName = (char *)font->glyphs + glyphBytes;
    memcpy((void *)font->fontName, name, nameSize);
    font->pixelHeight = (int)header[1];
    font->glyphCount = (int)header[2];
    bool valid = true;
    for (int i = 0; i < font->glyphCount; ++i)
    {
        const Glyph *glyph = &font->glyphs[i];
        if ((i < 96 && glyph->letter != i + 32) ||
            (i > 96 && glyph->letter <= font->glyphs[i - 1].letter) ||
            !isfinite(glyph->s0) || !isfinite(glyph->t0) || !isfinite(glyph->s1) || !isfinite(glyph->t1))
        {
            valid = false;
            break;
        }
    }
    if (valid)
    {
        char glow[64];
        snprintf(glow, sizeof(glow), "%s_glow", materialName);
        font->material = resolve(materialName, context);
        font->glowMaterial = font->material ? resolve(glow, context) : NULL;
        valid = font->material && font->glowMaterial;
    }
    if (!valid)
    {
        free(font);
        return FontError(error, errorSize, "Invalid font glyphs or missing base/glow material");
    }
    *result = font;
    return true;
}
