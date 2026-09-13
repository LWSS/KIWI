#include <universal/q_shared.h>
#include <universal/q_parse.h>
#include <bgame/bg_weapon_fields.h>
#include "native_weapon.h"
#include "native_asset_files.h"
#include <database64/db_package.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>

struct LinkerWeaponSource
{
    WeaponDef weapon;
    snd_alias_list_t *bounce[29];
    float graphs[2][16][2];
    char name[64];
    char *text;
};

static bool Fail(char *error, size_t size, const char *field)
{
    if (size)
    {
        snprintf(error, size, "Cannot compile native weapon: %s", field);
    }
    return false;
}

static int EnumIndex(const char *value, const char *const *names, int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (!_stricmp(value, names[i]))
        {
            return i;
        }
    }
    return -1;
}

static bool Number(const char *value, double *number)
{
    char *end;
    errno = 0;
    *number = strtod(value, &end);
    return end != value && !*end && errno != ERANGE && isfinite(*number);
}

static bool Tags(const char *value, uint16_t *out, int count, LinkerWeaponString intern, void *context)
{
    if (!intern)
    {
        return false;
    }
    Com_BeginParseSession("weapon tags");
    bool ok = true;
    int index = 0;
    while (ok)
    {
        const char *token = Com_Parse(&value)->token;
        if (!token[0])
        {
            break;
        }
        const size_t length = strlen(token);
        char lower[64];
        if (index == count || length >= sizeof(lower))
        {
            ok = false;
            break;
        }
        for (size_t i = 0; i <= length; ++i)
        {
            lower[i] = token[i] >= 'A' && token[i] <= 'Z' ? token[i] + ('a' - 'A') : token[i];
        }
        out[index] = intern(lower, context);
        ok = out[index++] != 0;
    }
    Com_EndParseSession();
    // A notetrack mapping consists of a key followed by its sound name.
    return ok && (count != 32 || !(index & 1));
}

static bool Field(LinkerWeaponSource *source, const cspField_t *field, char *value,
                  const char *const *animTypes, int animCount, LinkerWeaponAsset resolve,
                  LinkerWeaponString intern, void *context)
{
    uint8_t *address = (uint8_t *)&source->weapon + field->iOffset;
    const int type = field->iFieldType;
    const char *const *names = NULL;
    int count = 0;
    switch (type)
    {
    case WFT_WEAPONTYPE: names = szWeapTypeNames; count = 4; break;
    case WFT_WEAPONCLASS: names = szWeapClassNames; count = 10; break;
    case WFT_OVERLAYRETICLE: names = szWeapOverlayReticleNames; count = 2; break;
    case WFT_PENETRATE_TYPE: names = penetrateTypeNames; count = 4; break;
    case WFT_IMPACT_TYPE: names = impactTypeNames; count = 9; break;
    case WFT_STANCE: names = szWeapStanceNames; count = 3; break;
    case WFT_PROJ_EXPLOSION: names = szProjectileExplosionNames; count = 7; break;
    case WFT_OFFHAND_CLASS: names = offhandClassNames; count = 4; break;
    case WFT_ANIMTYPE: names = animTypes; count = animCount; break;
    case WFT_ACTIVE_RETICLE_TYPE: names = activeReticleNames; count = 3; break;
    case WFT_GUIDED_MISSILE_TYPE: names = guidedMissileNames; count = 4; break;
    case WFT_STICKINESS: names = stickinessNames; count = 4; break;
    case WFT_OVERLAYINTERFACE: names = overlayInterfaceNames; count = 3; break;
    case WFT_INVENTORYTYPE: names = szWeapInventoryTypeNames; count = 4; break;
    case WFT_FIRETYPE: names = szWeapFireTypeNames; count = 5; break;
    case WFT_AMMOCOUNTER_CLIPTYPE: names = ammoCounterClipNames; count = 7; break;
    case WFT_ICONRATIO_HUD: case WFT_ICONRATIO_AMMOCOUNTER: case WFT_ICONRATIO_KILL: case WFT_ICONRATIO_DPAD:
        names = weapIconRatioNames; count = 3; break;
    }
    if (names)
    {
        const int index = EnumIndex(value, names, count);
        memcpy(address, &index, sizeof(int));
        return index >= 0;
    }
    if (type == CSPFT_STRING)
    {
        memcpy(address, &value, sizeof(char *));
        return true;
    }
    if (type >= CSPFT_INT && type <= CSPFT_MILLISECONDS)
    {
        if (type == CSPFT_INT || type == CSPFT_QBOOLEAN)
        {
            char *end;
            errno = 0;
            const long number = strtol(value, &end, 10);
            if (end == value || *end || errno == ERANGE || number < INT_MIN || number > INT_MAX)
            {
                return false;
            }
            const int result = type == CSPFT_QBOOLEAN ? number != 0 : (int)number;
            memcpy(address, &result, sizeof(int));
            return true;
        }
        double number;
        if (!Number(value, &number))
        {
            return false;
        }
        if (type == CSPFT_FLOAT)
        {
            const float result = (float)number;
            memcpy(address, &result, sizeof(float));
            return isfinite(result);
        }
        if (type == CSPFT_MILLISECONDS)
        {
            number = (double)(float)number * 1000.0;
        }
        if (number < INT_MIN || number > INT_MAX)
        {
            return false;
        }
        const int result = (int)number;
        memcpy(address, &result, sizeof(int));
        return true;
    }
    if (type >= CSPFT_FX && type <= CSPFT_SOUND)
    {
        const XAssetType assetType = type == CSPFT_FX ? ASSET_TYPE_FX :
                                    type == CSPFT_XMODEL ? ASSET_TYPE_XMODEL :
                                    type == CSPFT_MATERIAL ? ASSET_TYPE_MATERIAL : ASSET_TYPE_SOUND;
        const void *asset = resolve ? resolve(assetType, value, true, context) : NULL;
        memcpy(address, &asset, sizeof(void *));
        return asset != NULL;
    }
    if (type == WFT_HIDETAGS)
    {
        return Tags(value, source->weapon.hideTags, 8, intern, context);
    }
    if (type == WFT_NOTETRACKSOUNDMAP)
    {
        uint16_t pairs[32] = {};
        if (!Tags(value, pairs, 32, intern, context))
        {
            return false;
        }
        for (int i = 0; i < 16; ++i)
        {
            source->weapon.notetrackSoundMapKeys[i] = pairs[2 * i];
            source->weapon.notetrackSoundMapValues[i] = pairs[2 * i + 1];
        }
        return true;
    }
    if (type == WFT_BOUNCE_SOUND && resolve)
    {
        static const char *surfaces[29] = {"default", "bark", "brick", "carpet", "cloth", "concrete", "dirt",
            "flesh", "foilage", "glass", "grass", "gravel", "ice", "metal", "mud", "paper", "plaster",
            "rock", "sand", "snow", "water", "wood", "asphalt", "ceramic", "plastic", "rubber", "cushion",
            "fruit", "paintedmetal"};
        for (int i = 0; i < 29; ++i)
        {
            char name[256];
            const int length = snprintf(name, sizeof(name), "%s_%s", value, surfaces[i]);
            if (length < 0 || length >= sizeof(name))
            {
                return false;
            }
            source->bounce[i] = (snd_alias_list_t *)resolve(ASSET_TYPE_SOUND, name, i == 0, context);
            if (!source->bounce[i])
            {
                source->bounce[i] = source->bounce[0];
            }
        }
        source->weapon.bounceSound = source->bounce;
        return source->bounce[0] != NULL;
    }
    return false;
}

static bool AccuracyGraph(LinkerWeaponSource *source, const char *root, int index)
{
    const char *name = source->weapon.accuracyGraphName[index];
    if (!name || !name[0] || (source->weapon.weapType != WEAPTYPE_BULLET &&
                            source->weapon.weapType != WEAPTYPE_PROJECTILE))
    {
        return true;
    }
    char path[256];
    const int length = snprintf(path, sizeof(path), "accuracy/%s/%s", accuracyDirName[index], name);
    void *data = NULL;
    size_t size = 0;
    if (length < 0 || length >= sizeof(path) || !Linker_ReadRawAssetFile(root, path, &data, &size, 8192))
    {
        return false;
    }
    bool ok = size >= 15 && !memcmp(data, "WEAPONACCUFILE", 14) && !memchr(data, 0, size);
    if (ok)
    {
        const char *text = (const char *)data + 14;
        Com_BeginParseSession(path);
        double number;
        ok = Number(Com_Parse(&text)->token, &number) && number >= 1 && number <= 16 && number == floor(number);
        const int count = ok ? (int)number : 0;
        for (int i = 0; i < count && ok; ++i)
        {
            for (int j = 0; j < 2 && ok; ++j)
            {
                ok = Number(Com_Parse(&text)->token, &number) && isfinite((float)number);
                if (ok)
                {
                    source->graphs[index][i][j] = (float)number;
                }
            }
        }
        if (ok)
        {
            ok = !Com_Parse(&text)->token[0] && source->graphs[index][count - 1][0] == 1.0f;
            source->weapon.accuracyGraphKnots[index] = source->graphs[index];
            source->weapon.originalAccuracyGraphKnots[index] = source->graphs[index];
            source->weapon.accuracyGraphKnotCount[index] = count;
            source->weapon.originalAccuracyGraphKnotCount[index] = count;
        }
        Com_EndParseSession();
    }
    free(data);
    return ok;
}

WeaponDef *Linker_GetWeapon(LinkerWeaponSource *source)
{
    return source ? &source->weapon : NULL;
}
void Linker_FreeWeapon(LinkerWeaponSource *source)
{
    if (source)
    {
        free(source->text);
        free(source);
    }
}

bool Linker_CompileWeaponFile(const char *root, const char *name, LinkerWeaponAsset resolve,
                             LinkerWeaponString intern, void *context, LinkerWeaponSource **result,
                             char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    char normalized[64];
    if (!root || !name || !DB64_NormalizePath(name, normalized, sizeof(normalized)) || strchr(normalized, '/'))
    {
        return Fail(error, errorSize, "invalid weapon name");
    }
    LinkerWeaponSource *source = (LinkerWeaponSource *)calloc(1, sizeof(LinkerWeaponSource));
    if (!source)
    {
        return Fail(error, errorSize, "out of memory");
    }
    strcpy(source->name, normalized);
    char path[96];
    snprintf(path, sizeof(path), "weapons/mp/%s", normalized);
    size_t size = 0;
    bool ok = Linker_ReadRawAssetFile(root, path, (void **)&source->text, &size, 10249) && size >= 10 &&
              !memcmp(source->text, "WEAPONFILE", 10) && !memchr(source->text, 0, size);
    source->weapon.szInternalName = source->name;
    for (int i = 0; i < ARRAY_COUNT(weaponDefFields); ++i)
    {
        if (weaponDefFields[i].iFieldType == CSPFT_STRING)
        {
            const char *empty = "";
            memcpy((uint8_t *)&source->weapon + weaponDefFields[i].iOffset, &empty, sizeof(const char *));
        }
    }
    void *animData = NULL;
    size_t animSize = 0;
    char animNames[64][64];
    const char *animTypes[64];
    int animCount = 0;
    if (ok)
    {
        ok = Linker_ReadRawAssetFile(root, "mp/playeranimtypes.txt", &animData, &animSize, 8192) &&
             !memchr(animData, 0, animSize);
    }
    if (ok)
    {
        const char *text = (const char *)animData;
        Com_BeginParseSession("mp/playeranimtypes.txt");
        while (ok)
        {
            const char *token = Com_Parse(&text)->token;
            if (!token[0])
            {
                break;
            }
            if (animCount == 64 || strlen(token) >= 64)
            {
                ok = false;
                break;
            }
            strcpy(animNames[animCount], token);
            animTypes[animCount] = animNames[animCount];
            ++animCount;
        }
        Com_EndParseSession();
        ok = ok && animCount != 0;
    }
    free(animData);
    char *cursor = ok ? source->text + 10 : NULL;
    if (cursor && *cursor == '\\')
    {
        ++cursor;
    }
    bool seen[502] = {};
    while (ok && *cursor)
    {
        char *key = cursor;
        char *value = strchr(key, '\\');
        if (!value)
        {
            ok = false;
            break;
        }
        *value++ = 0;
        cursor = strchr(value, '\\');
        if (cursor)
        {
            *cursor++ = 0;
        }
        else
        {
            cursor = value + strlen(value);
        }
        if (!key[0] || strpbrk(key, "\";") || strpbrk(value, "\";"))
        {
            ok = false;
            break;
        }
        for (int i = 0; i < ARRAY_COUNT(weaponDefFields); ++i)
        {
            if (!_stricmp(key, weaponDefFields[i].szName))
            {
                if (!seen[i] && value[0])
                {
                    ok = Field(source, &weaponDefFields[i], value, animTypes, animCount, resolve, intern, context);
                    if (!ok)
                    {
                        Fail(error, errorSize, key);
                    }
                }
                seen[i] = true;
                break;
            }
        }
    }
    WeaponDef *weapon = &source->weapon;
    if (ok && _stricmp(name, "defaultweapon_mp"))
    {
        if (!weapon->viewLastShotEjectEffect)
        {
            weapon->viewLastShotEjectEffect = weapon->viewShellEjectEffect;
        }
        if (!weapon->worldLastShotEjectEffect)
        {
            weapon->worldLastShotEjectEffect = weapon->worldShellEjectEffect;
        }
        snd_alias_list_t **sounds[] = {&weapon->raiseSound, &weapon->putawaySound, &weapon->pickupSound,
                                       &weapon->ammoPickupSound, &weapon->emptyFireSound};
        const char *defaults[] = {"weap_raise", "weap_putaway", "weap_pickup", "weap_ammo_pickup", "weap_dryfire_smg_npc"};
        for (int i = 0; i < 5; ++i)
        {
            if (!*sounds[i])
            {
                *sounds[i] = resolve ? (snd_alias_list_t *)resolve(ASSET_TYPE_SOUND, defaults[i], true, context) : NULL;
                ok = ok && *sounds[i] != NULL;
            }
        }
    }
    if (ok)
    {
        weapon->fOOPosAnimLength[0] = 1.0f / (weapon->iAdsTransInTime > 0 ? weapon->iAdsTransInTime : 300);
        weapon->fOOPosAnimLength[1] = 1.0f / (weapon->iAdsTransOutTime > 0 ? weapon->iAdsTransOutTime : 500);
        if (weapon->fMaxDamageRange <= 0.0f)
        {
            weapon->fMaxDamageRange = 999999.0f;
        }
        if (weapon->fMinDamageRange <= 0.0f)
        {
            weapon->fMinDamageRange = 999999.12f;
        }
        ok = weapon->enemyCrosshairRange <= 15000.0f &&
             (weapon->weapType != WEAPTYPE_PROJECTILE || (weapon->iProjectileSpeed > 0 &&
              weapon->destabilizationCurvatureMax >= 0.0f && weapon->destabilizationCurvatureMax < 1000000000.0f &&
              weapon->destabilizationRateTime >= 0.0f));
        ok = ok && AccuracyGraph(source, root, 0) && AccuracyGraph(source, root, 1);
        const char *names[] = {weapon->szAmmoName, weapon->szClipName};
        for (int i = 0; i < 2; ++i)
        {
            for (char *text = (char *)names[i]; *text; ++text)
            {
                if (*text >= 'A' && *text <= 'Z')
                {
                    *text += 'a' - 'A';
                }
            }
        }
    }
    if (!ok)
    {
        Linker_FreeWeapon(source);
        if (!errorSize || !error[0])
        {
            Fail(error, errorSize, name);
        }
        return false;
    }
    *result = source;
    return true;
}
