#include "db_external_assets.h"
#include "db_font_assets.h"
#include "database.h"
#include "db_material_assets.h"

void DB64_LoadFontAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_FONT, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->font = (Font_s *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        Font_s *font = header->font;
        Load_Stream(true, (uint8_t *)font, sizeof(Font_s));
        if (!DB64_ValidateFont(font, false) || (uintptr_t)font->glyphs != UINTPTR_MAX)
        {
            Com_Error(ERR_DROP, "Invalid native font header");
        }
        DB_PushStreamPos(4);
        DB64_LoadAssetString(&font->fontName);
        DB64_LoadMaterialAsset((XAssetHeader *)&font->material, false);
        DB64_LoadMaterialAsset((XAssetHeader *)&font->glowMaterial, false);
        font->glyphs = (Glyph *)DB_AllocStreamPos(3);
        Load_Stream(true, (uint8_t *)font->glyphs, font->glyphCount * sizeof(Glyph));
        if (!DB64_ValidateFont(font, true))
        {
            Com_Error(ERR_DROP, "Invalid native font glyphs");
        }
        DB_PopStreamPos();
        Load_FontAsset(header);
        if (inserted)
        {
            *inserted = header->data;
        }
    }
    else if (token)
    {
        DB_ConvertOffsetToAlias((uintptr_t *)header);
    }
    DB_PopStreamPos();
}
