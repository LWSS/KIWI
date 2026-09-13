#include <universal/q_shared.h>
#include "database.h"
#include <game/g_bsp.h>

// ICF folded x86 functions with equal code/size, not equivalent native types.
static void DB_CheckAssetHeader(int type, const XAssetHeader *header)
{
    if (type < 0 || type >= ASSET_TYPE_COUNT || !header || !header->data)
    {
        Com_Error(ERR_DROP, "Invalid asset header/type %d", type);
    }
}

const char *DB_GetXAssetHeaderName(int type, const XAssetHeader *header)
{
    DB_CheckAssetHeader(type, header);
    const char *name = NULL;
    switch (type)
    {
    case ASSET_TYPE_XMODELPIECES:
        name = header->xmodelPieces->name;
        break;
    case ASSET_TYPE_PHYSPRESET:
        name = header->physPreset->name;
        break;
    case ASSET_TYPE_XANIMPARTS:
        name = header->parts->name;
        break;
    case ASSET_TYPE_XMODEL:
        name = header->model->name;
        break;
    case ASSET_TYPE_MATERIAL:
        name = header->material->info.name;
        break;
    case ASSET_TYPE_TECHNIQUE_SET:
        name = header->techniqueSet->name;
        break;
    case ASSET_TYPE_IMAGE:
        name = header->image->name;
        break;
    case ASSET_TYPE_SOUND:
        name = header->sound->aliasName;
        break;
    case ASSET_TYPE_SOUND_CURVE:
        name = header->sndCurve->filename;
        break;
    case ASSET_TYPE_LOADED_SOUND:
        name = header->loadSnd->name;
        break;
    case ASSET_TYPE_CLIPMAP:
        name = header->clipMap->name;
        break;
    case ASSET_TYPE_CLIPMAP_PVS:
        name = header->clipMap->name;
        break;
    case ASSET_TYPE_COMWORLD:
        name = header->comWorld->name;
        break;
    case ASSET_TYPE_GAMEWORLD_SP:
        name = header->gameWorldSp->name;
        break;
    case ASSET_TYPE_GAMEWORLD_MP:
        name = header->gameWorldMp->name;
        break;
    case ASSET_TYPE_MAP_ENTS:
        name = header->mapEnts->name;
        break;
    case ASSET_TYPE_GFXWORLD:
        name = header->gfxWorld->name;
        break;
    case ASSET_TYPE_LIGHT_DEF:
        name = header->lightDef->name;
        break;
    case ASSET_TYPE_FONT:
        name = header->font->fontName;
        break;
    case ASSET_TYPE_MENULIST:
        name = header->menuList->name;
        break;
    case ASSET_TYPE_MENU:
        name = header->menu->window.name;
        break;
    case ASSET_TYPE_LOCALIZE_ENTRY:
        name = header->localize->name;
        break;
    case ASSET_TYPE_WEAPON:
        name = header->weapon->szInternalName;
        break;
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
        name = header->sndDriverGlobals->name;
        break;
    case ASSET_TYPE_FX:
        name = header->fx->name;
        break;
    case ASSET_TYPE_IMPACT_FX:
        name = header->impactFx->name;
        break;
    case ASSET_TYPE_RAWFILE:
        name = header->rawfile->name;
        break;
    case ASSET_TYPE_STRINGTABLE:
        name = header->stringTable->name;
        break;
    default:
        break;
    }
    if (!name)
    {
        Com_Error(ERR_DROP, "Asset type %d has no name", type);
    }
    return name;
}

const char *DB_GetXAssetName(const XAsset *asset)
{
    if (!asset)
    {
        Com_Error(ERR_DROP, "Null asset");
    }
    return DB_GetXAssetHeaderName(asset->type, &asset->header);
}

void DB_SetXAssetName(XAsset *asset, const char *name)
{
    if (!asset || !name)
    {
        Com_Error(ERR_DROP, "Invalid asset name assignment");
    }
    DB_CheckAssetHeader(asset->type, &asset->header);
    switch (asset->type)
    {
    case ASSET_TYPE_XMODELPIECES:
        asset->header.xmodelPieces->name = name;
        return;
    case ASSET_TYPE_PHYSPRESET:
        asset->header.physPreset->name = name;
        return;
    case ASSET_TYPE_XANIMPARTS:
        asset->header.parts->name = name;
        return;
    case ASSET_TYPE_XMODEL:
        asset->header.model->name = name;
        return;
    case ASSET_TYPE_MATERIAL:
        asset->header.material->info.name = name;
        return;
    case ASSET_TYPE_TECHNIQUE_SET:
        asset->header.techniqueSet->name = name;
        return;
    case ASSET_TYPE_IMAGE:
        asset->header.image->name = name;
        return;
    case ASSET_TYPE_SOUND:
        asset->header.sound->aliasName = name;
        return;
    case ASSET_TYPE_SOUND_CURVE:
        asset->header.sndCurve->filename = name;
        return;
    case ASSET_TYPE_LOADED_SOUND:
        asset->header.loadSnd->name = name;
        return;
    case ASSET_TYPE_CLIPMAP:
        asset->header.clipMap->name = name;
        return;
    case ASSET_TYPE_CLIPMAP_PVS:
        asset->header.clipMap->name = name;
        return;
    case ASSET_TYPE_COMWORLD:
        asset->header.comWorld->name = name;
        return;
    case ASSET_TYPE_GAMEWORLD_SP:
        asset->header.gameWorldSp->name = name;
        return;
    case ASSET_TYPE_GAMEWORLD_MP:
        asset->header.gameWorldMp->name = name;
        return;
    case ASSET_TYPE_MAP_ENTS:
        asset->header.mapEnts->name = name;
        return;
    case ASSET_TYPE_GFXWORLD:
        asset->header.gfxWorld->name = name;
        return;
    case ASSET_TYPE_LIGHT_DEF:
        asset->header.lightDef->name = name;
        return;
    case ASSET_TYPE_FONT:
        asset->header.font->fontName = name;
        return;
    case ASSET_TYPE_MENULIST:
        asset->header.menuList->name = name;
        return;
    case ASSET_TYPE_MENU:
        asset->header.menu->window.name = name;
        return;
    case ASSET_TYPE_LOCALIZE_ENTRY:
        asset->header.localize->name = name;
        return;
    case ASSET_TYPE_WEAPON:
        asset->header.weapon->szInternalName = name;
        return;
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
        asset->header.sndDriverGlobals->name = name;
        return;
    case ASSET_TYPE_FX:
        ((FxEffectDef *)asset->header.fx)->name = name;
        return;
    case ASSET_TYPE_IMPACT_FX:
        asset->header.impactFx->name = name;
        return;
    case ASSET_TYPE_RAWFILE:
        asset->header.rawfile->name = name;
        return;
    case ASSET_TYPE_STRINGTABLE:
        asset->header.stringTable->name = name;
        return;
    default:
        Com_Error(ERR_DROP, "Asset type %d has no name field", asset->type);
        break;
    }
}

int DB_GetXAssetTypeSize(int type)
{
    switch (type)
    {
    case ASSET_TYPE_XMODELPIECES:
        return sizeof(XModelPieces);
    case ASSET_TYPE_PHYSPRESET:
        return sizeof(PhysPreset);
    case ASSET_TYPE_XANIMPARTS:
        return sizeof(XAnimParts);
    case ASSET_TYPE_XMODEL:
        return sizeof(XModel);
    case ASSET_TYPE_MATERIAL:
        return sizeof(Material);
    case ASSET_TYPE_TECHNIQUE_SET:
        return sizeof(MaterialTechniqueSet);
    case ASSET_TYPE_IMAGE:
        return sizeof(GfxImage);
    case ASSET_TYPE_SOUND:
        return sizeof(snd_alias_list_t);
    case ASSET_TYPE_SOUND_CURVE:
        return sizeof(SndCurve);
    case ASSET_TYPE_LOADED_SOUND:
        return sizeof(LoadedSound);
    case ASSET_TYPE_CLIPMAP:
        return sizeof(clipMap_t);
    case ASSET_TYPE_CLIPMAP_PVS:
        return sizeof(clipMap_t);
    case ASSET_TYPE_COMWORLD:
        return sizeof(ComWorld);
    case ASSET_TYPE_GAMEWORLD_SP:
        return sizeof(GameWorldSp);
    case ASSET_TYPE_GAMEWORLD_MP:
        return sizeof(GameWorldMp);
    case ASSET_TYPE_MAP_ENTS:
        return sizeof(MapEnts);
    case ASSET_TYPE_GFXWORLD:
        return sizeof(GfxWorld);
    case ASSET_TYPE_LIGHT_DEF:
        return sizeof(GfxLightDef);
    case ASSET_TYPE_FONT:
        return sizeof(Font_s);
    case ASSET_TYPE_MENULIST:
        return sizeof(MenuList);
    case ASSET_TYPE_MENU:
        return sizeof(menuDef_t);
    case ASSET_TYPE_LOCALIZE_ENTRY:
        return sizeof(LocalizeEntry);
    case ASSET_TYPE_WEAPON:
        return sizeof(WeaponDef);
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
        return sizeof(SndDriverGlobals);
    case ASSET_TYPE_FX:
        return sizeof(FxEffectDef);
    case ASSET_TYPE_IMPACT_FX:
        return sizeof(FxImpactTable);
    case ASSET_TYPE_RAWFILE:
        return sizeof(RawFile);
    case ASSET_TYPE_STRINGTABLE:
        return sizeof(StringTable);
    default:
        Com_Error(ERR_DROP, "Invalid asset type %d", type);
        return 0;
    }
}

const char *DB_GetXAssetTypeName(uint type)
{
    if (type >= ASSET_TYPE_COUNT)
    {
        Com_Error(ERR_DROP, "Invalid asset type %u", type);
    }
    return g_assetNames[type];
}
