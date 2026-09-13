#include <zlib/zlib.h>
#include <database64/fastfile_format.h>
#include <universal/q_shared.h>
#include "native_manifest.h"
#include "native_writer.h"
#include "native_anim.h"
#include "native_script_dependencies.h"
#include "native_image.h"
#include "native_sound.h"
#include "native_sound_alias_files.h"
#include "native_effect.h"
#include "native_font.h"
#include "native_stringtable.h"
#include "native_localize.h"
#include "native_menu.h"
#include "native_menu_dependencies.h"
#include "native_weapon.h"
#include "native_impact.h"
#include <gfx_d3d/fxprimitives.h>
#include <database64/db_weapon_layout.h>
#include "native_technique_files.h"
#include "native_light_files.h"
#include "native_physics.h"
#include "native_bsp.h"
#include "native_material_files.h"
#include "native_model_files.h"
#include "native_asset_files.h"
#include "native_world_files.h"
#include "native_world_assemble.h"
#include <xanim/xmodel.h>
#include <xanim/xanim.h>
#include <sound/snd_public.h>
#include <gfx_d3d/r_image.h>
#include <universal/com_sndalias.h>
#include <dynentity/DynEntity_client.h>
#include <database64/db_package.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

struct ManifestInput
{
    char *key;
    bool omitted;
    int line;
    char soundLevel[64];
    char soundGame[16];
    XAsset assets[5];
    XAsset *expandedAssets;
    int count;
    void *owned;
    void (*release)(void *);
};

struct ManifestEffectContext
{
    ManifestInput *inputs;
    int inputCount;
    unsigned int lightLookup;
    const char *level;
    const char *language;
    LinkerWorldSource *world;
    unsigned int remappedModelCount;
    bool borrowed;
};

struct ManifestScriptContext
{
    ManifestInput *inputs;
    int *count;
    int *table;
    int line;
    const char *soundLevel;
    const char *soundGame;
};

static bool QueueScriptAsset(const char *type, const char *path, void *value)
{
    ManifestScriptContext *context = (ManifestScriptContext *)value;
    char key[DB64_PACKAGE_PATH + 64];
    snprintf(key, sizeof(key), "%s:%s", type, path);
    uint32_t hash = 2166136261;
    for (const char *byte = key; *byte; ++byte)
    {
        hash = (hash ^ (uint8_t)*byte) * 16777619;
    }
    unsigned int slot = hash & 65535;
    while (context->table[slot] && strcmp(context->inputs[context->table[slot] - 1].key, key))
    {
        slot = (slot + 1) & 65535;
    }
    if (context->table[slot])
    {
        if (context->inputs[context->table[slot] - 1].omitted)
        {
            return true;
        }
        if (!strcmp(type, "sound") && context->soundGame && context->soundGame[0])
        {
            const ManifestInput *existing = &context->inputs[context->table[slot] - 1];
            return !strcmp(existing->soundGame, context->soundGame) &&
                   !strcmp(existing->soundLevel, context->soundLevel ? context->soundLevel : "");
        }
        return true;
    }
    if (*context->count >= 32768)
    {
        return false;
    }
    char *owned = _strdup(key);
    if (!owned)
    {
        return false;
    }
    ManifestInput *input = &context->inputs[(*context->count)++];
    input->key = owned;
    input->line = context->line;
    if (!strcmp(type, "sound"))
    {
        if (context->soundLevel)
        {
            snprintf(input->soundLevel, sizeof(input->soundLevel), "%s", context->soundLevel);
        }
        if (context->soundGame)
        {
            snprintf(input->soundGame, sizeof(input->soundGame), "%s", context->soundGame);
        }
    }
    context->table[slot] = *context->count;
    return true;
}

struct ManifestSoundTable
{
    ManifestScriptContext *scripts;
    const char *table;
};

static bool QueueSoundTableAlias(const char *name, void *value)
{
    ManifestSoundTable *context = (ManifestSoundTable *)value;
    char path[DB64_PACKAGE_PATH], normalized[DB64_PACKAGE_PATH];
    const int length = snprintf(path, sizeof(path), "%s/%s", context->table, name);
    return length > 0 && length < sizeof(path) &&
           DB64_NormalizePath(path, normalized, sizeof(normalized)) &&
           QueueScriptAsset("sound", normalized, context->scripts);
}

static bool QueueScript(const char *path, void *value)
{
    return QueueScriptAsset("rawfile", path, value);
}

static bool QueueSoundAlias(const char *name, void *value)
{
    ManifestScriptContext *context = (ManifestScriptContext *)value;
    if (!name || !name[0])
    {
        return true;
    }
    // An explicit table selection satisfies references to that alias as well.
    // Checking queued rows also terminates cycles before importing dependencies.
    for (int i = 0; i < *context->count; ++i)
    {
        const char *key = context->inputs[i].key;
        if (strncmp(key, "sound:", 6))
        {
            continue;
        }
        const char *alias = key + 6;
        const char *separator = strstr(alias, ".csv/");
        if (separator)
        {
            alias = separator + 5;
        }
        if (!_stricmp(alias, name))
        {
            return true;
        }
    }
    char normalized[DB64_PACKAGE_PATH];
    return DB64_NormalizePath(name, normalized, sizeof(normalized)) &&
           QueueScriptAsset("sound", normalized, context);
}

static bool QueueDiscoveredAsset(const char *type, const char *path, void *context)
{
    return !strcmp(type, "sound") ? QueueSoundAlias(path, context) : QueueScriptAsset(type, path, context);
}

static bool QueueMapDependencies(const char *root, const char *bsp, ManifestScriptContext *scripts)
{
    char base[DB64_PACKAGE_PATH];
    const size_t length = strlen(bsp);
    if (length < 7 || strcmp(bsp + length - 7, ".d3dbsp") || length >= sizeof(base))
    {
        return false;
    }
    memcpy(base, bsp, length - 7);
    base[length - 7] = 0;
    const char *name = strrchr(base, '/');
    name = name ? name + 1 : base;
    char paths[3][DB64_PACKAGE_PATH];
    const int lengths[] = {
        snprintf(paths[0], sizeof(paths[0]), "%s.gsc", base),
        snprintf(paths[1], sizeof(paths[1]), "%s_fx.gsc", base),
        snprintf(paths[2], sizeof(paths[2]), "maps/createfx/%s_fx.gsc", name)
    };
    for (int i = 0; i < 3; ++i)
    {
        if (lengths[i] < 0 || lengths[i] >= sizeof(paths[i]))
        {
            return false;
        }
        void *source = NULL;
        size_t size = 0;
        bool found = false;
        // Map scripts are optional in the engine. When present, their entire
        // statically referenced graph is required and imported on the next pass.
        if (Linker_ReadRawAssetFile(root, paths[i], &source, &size, INT_MAX, &found))
        {
            free(source);
            if (!QueueScript(paths[i], scripts))
            {
                return false;
            }
        }
        else if (found)
        {
            fprintf(stderr, "Cannot read native map script: %s\n", paths[i]);
            return false;
        }
    }
    return QueueScriptAsset("impactfx", name, scripts);
}

static void FreeImage(void *value)
{
    Linker_FreeImage((GfxImage *)value);
}
static void FreeMaterial(void *value)
{
    Linker_FreeCompiledMaterial((LinkerCompiledMaterial *)value);
}
struct ManifestFont
{
    Font_s *font;
    LinkerCompiledMaterial *materials[2];
    int count;
    const char *root;
    const char *language;
    char *error;
    size_t errorSize;
};
static Material *CompileFontMaterial(const char *name, void *value)
{
    ManifestFont *font = (ManifestFont *)value;
    if (font->count == 2 || !Linker_CompileMaterialFiles(font->root, name, IMAGE_TRACK_UI,
        &font->materials[font->count], font->error, font->errorSize, font->language))
    {
        return NULL;
    }
    return font->materials[font->count++]->material;
}
static void FreeLocalization(void *value)
{
    Linker_FreeLocalization((LinkerLocalization *)value);
}
static void FreeTechniqueSet(void *value)
{
    Linker_FreeCompiledTechniqueSet((LinkerCompiledTechniqueSet *)value);
}
static void FreeLightDefinition(void *value)
{
    Linker_FreeLightDefinition((GfxLightDef *)value);
}
static void FreeFont(void *value)
{
    ManifestFont *font = (ManifestFont *)value;
    for (int i = 0; i < font->count; ++i)
    {
        Linker_FreeCompiledMaterial(font->materials[i]);
    }
    free(font->font);
    free(font);
}
static void FreeSound(void *value)
{
    Linker_FreeSound((LoadedSound *)value);
}
static void FreeWorlds(void *value)
{
    Linker_FreeMapWorlds((LinkerMapWorlds *)value);
}

static void FreeSoundAlias(void *value)
{
    Linker_FreeSoundAlias((LinkerCompiledSoundAlias *)value);
}

static void FreeAnimation(void *value)
{
    Linker_FreeAnimation((LinkerCompiledAnimation *)value);
}

static void FreeRenderWorld(void *value)
{
    Linker_FreeWorldSource((LinkerWorldSource *)value);
}

struct ManifestMenu
{
    LinkerCompiledMenu *menu;
    XAsset *roots;
    LinkerCompiledMaterial *materials[4096];
    int materialCount;
    snd_alias_list_t *sounds[512];
    int soundCount;
    const char *root;
    const char *language;
    char *error;
    size_t errorSize;
};

static Material *CompileMenuMaterial(const char *name, void *context)
{
    ManifestMenu *owner = (ManifestMenu *)context;
    for (int i = 0; i < owner->materialCount; ++i)
    {
        const char *existing = owner->materials[i]->material->info.name;
        if (existing[0] == ',')
        {
            ++existing;
        }
        if (!_stricmp(existing, name[0] == ',' ? name + 1 : name))
        {
            return owner->materials[i]->material;
        }
    }
    if (owner->materialCount == ARRAY_COUNT(owner->materials))
    {
        return NULL;
    }
    LinkerCompiledMaterial *material = NULL;
    if (!Linker_CompileMaterialFiles(owner->root, name, IMAGE_TRACK_UI, &material, owner->error, owner->errorSize, owner->language))
    {
        return NULL;
    }
    owner->materials[owner->materialCount++] = material;
    return material->material;
}

static void FreeMenu(void *context)
{
    ManifestMenu *owner = (ManifestMenu *)context;
    Linker_FreeMenu(owner->menu);
    for (int i = 0; i < owner->materialCount; ++i)
    {
        Linker_FreeCompiledMaterial(owner->materials[i]);
    }
    for (int i = 0; i < owner->soundCount; ++i)
    {
        free(owner->sounds[i]);
    }
    free(owner->roots);
    free(owner);
}

static snd_alias_list_t *QueueMenuSound(const char *name, void *context)
{
    ManifestMenu *owner = (ManifestMenu *)context;
    if (!name || !name[0] || strlen(name) >= DB64_PACKAGE_PATH)
    {
        return NULL;
    }
    for (int i = 0; i < owner->soundCount; ++i)
    {
        if (!_stricmp(owner->sounds[i]->aliasName, name))
        {
            return owner->sounds[i];
        }
    }
    if (owner->soundCount == ARRAY_COUNT(owner->sounds))
    {
        return NULL;
    }
    // This name reference is replaced with the shared imported alias before serialization.
    snd_alias_list_t *sound = (snd_alias_list_t *)calloc(1, sizeof(snd_alias_list_t) + strlen(name) + 1);
    if (!sound)
    {
        return NULL;
    }
    char *ownedName = (char *)(sound + 1);
    strcpy(ownedName, name);
    sound->aliasName = ownedName;
    owner->sounds[owner->soundCount++] = sound;
    return sound;
}

static bool CompileMenuScriptDependency(const char *type, const char *name, void *context)
{
    if (!strcmp(type, "sound"))
    {
        return QueueMenuSound(name, context) != NULL;
    }
    return !strcmp(type, "material") && CompileMenuMaterial(name, context) != NULL;
}

static bool ResolveMenuSounds(ManifestMenu *owner, ManifestInput *inputs, int inputCount)
{
    MenuList *list = Linker_GetMenuList(owner->menu);
    for (int i = 0; i < owner->soundCount; ++i)
    {
        snd_alias_list_t *resolved = NULL;
        for (int input = 0; input < inputCount; ++input)
        {
            if (inputs[input].count == 1 && inputs[input].assets[0].type == ASSET_TYPE_SOUND &&
                !_stricmp(inputs[input].assets[0].header.sound->aliasName, owner->sounds[i]->aliasName))
            {
                resolved = inputs[input].assets[0].header.sound;
                break;
            }
        }
        if (!resolved)
        {
            return false;
        }
        for (int m = 0; m < list->menuCount; ++m)
        {
            menuDef_t *menu = list->menus[m];
            for (int item = 0; item < menu->itemCount; ++item)
            {
                if (menu->items[item]->focusSound == owner->sounds[i])
                {
                    menu->items[item]->focusSound = resolved;
                }
            }
        }
    }
    return true;
}

struct ManifestReferences
{
    LinkerWeaponSource *source;
    FxImpactTable *impact;
    XAsset references[512];
    int count;
    const char *root;
    char level[64];
    ManifestEffectContext *effects;
    ScriptStringList *strings;
    char *error;
    size_t errorSize;
    bool failed;
};

static const char *ReferencedAssetTypeName(XAssetType type)
{
    switch (type)
    {
    case ASSET_TYPE_XMODEL: return "xmodel";
    case ASSET_TYPE_MATERIAL: return "material";
    case ASSET_TYPE_FX: return "fx";
    case ASSET_TYPE_SOUND: return "sound";
    default: return NULL;
    }
}

static void FreeReferencedAssets(void *value)
{
    ManifestReferences *owner = (ManifestReferences *)value;
    Linker_FreeWeapon(owner->source);
    free(owner->impact);
    for (int i = 0; i < owner->count; ++i)
    {
        free(owner->references[i].header.data);
    }
    free(owner);
}

static const void *ReferenceNativeAsset(XAssetType type, const char *name, bool required, void *value)
{
    ManifestReferences *owner = (ManifestReferences *)value;
    char normalized[DB64_PACKAGE_PATH];
    if (!ReferencedAssetTypeName(type) || !DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        owner->failed = true;
        return NULL;
    }
    for (int i = 0; i < owner->count; ++i)
    {
        const char *existing;
        memcpy(&existing, owner->references[i].header.data, sizeof(const char *));
        if (type == owner->references[i].type && !_stricmp(existing, normalized))
        {
            return owner->references[i].header.data;
        }
    }
    if (!required && type == ASSET_TYPE_SOUND)
    {
        bool found = false;
        for (int i = 0; i < owner->effects->inputCount; ++i)
        {
            const char *key = owner->effects->inputs[i].key;
            if (!strncmp(key, "sound:", 6))
            {
                const char *alias = strstr(key + 6, ".csv/");
                alias = alias ? alias + 5 : key + 6;
                found = found || !_stricmp(alias, normalized);
            }
        }
        if (!found && !Linker_ProbeSoundAliasForZone(owner->root, normalized, owner->level, "all_mp",
                                                    &found, owner->error, owner->errorSize))
        {
            owner->failed = true;
            return NULL;
        }
        if (!found)
        {
            return NULL;
        }
    }
    if (owner->count == ARRAY_COUNT(owner->references))
    {
        owner->failed = true;
        return NULL;
    }
    const size_t size = type == ASSET_TYPE_XMODEL ? sizeof(XModel) :
                        type == ASSET_TYPE_MATERIAL ? sizeof(Material) :
                        type == ASSET_TYPE_FX ? sizeof(FxEffectDef) : sizeof(snd_alias_list_t);
    void *placeholder = calloc(1, size + strlen(normalized) + 1);
    if (!placeholder)
    {
        owner->failed = true;
        return NULL;
    }
    char *copy = (char *)placeholder + size;
    strcpy(copy, normalized);
    memcpy(placeholder, &copy, sizeof(char *));
    owner->references[owner->count].type = type;
    owner->references[owner->count++].header.data = placeholder;
    return placeholder;
}

static const FxEffectDef *ReferenceImpactEffect(const char *name, void *value)
{
    return (const FxEffectDef *)ReferenceNativeAsset(ASSET_TYPE_FX, name, true, value);
}

static uint16_t InternWeaponString(const char *name, void *value)
{
    return Linker_InternModelString(name, ((ManifestReferences *)value)->strings);
}

static bool QueueNamedWeaponDependency(const char *type, const char *name, ManifestScriptContext *context)
{
    if (!name || !name[0])
    {
        return true;
    }
    char normalized[DB64_PACKAGE_PATH];
    return DB64_NormalizePath(name, normalized, sizeof(normalized)) && QueueScriptAsset(type, normalized, context);
}

static bool QueueReferencedDependencies(ManifestReferences *owner, ManifestScriptContext *context)
{
    for (int i = 0; i < owner->count; ++i)
    {
        const char *name;
        memcpy(&name, owner->references[i].header.data, sizeof(const char *));
        if (!QueueDiscoveredAsset(ReferencedAssetTypeName(owner->references[i].type), name, context))
        {
            return false;
        }
    }
    const WeaponDef *weapon = Linker_GetWeapon(owner->source);
    if (owner->impact)
    {
        return true;
    }
    for (int i = 0; i < NUM_WEAP_ANIMS; ++i)
    {
        const char *name = weapon->szXAnims[i];
        if (!QueueNamedWeaponDependency("xanim", name, context))
        {
            return false;
        }
    }
    for (int i = 0; i < 16; ++i)
    {
        const uint16_t sound = weapon->notetrackSoundMapValues[i];
        if (sound && !QueueSoundAlias(owner->strings->strings[sound], context))
        {
            return false;
        }
    }
    return QueueNamedWeaponDependency("weapon", weapon->szAltWeaponName, context);
}

static bool ResolveReferencedDependencies(ManifestReferences *owner, const XAsset *assets, int count)
{
    WeaponDef *weapon = Linker_GetWeapon(owner->source);
    for (int ref = 0; ref < owner->count; ++ref)
    {
        const XAsset *reference = &owner->references[ref];
        const char *name;
        memcpy(&name, reference->header.data, sizeof(const char *));
        void *resolved = NULL;
        for (int i = 0; i < count; ++i)
        {
            if (assets[i].type == reference->type)
            {
                const char *candidate;
                memcpy(&candidate, assets[i].header.data, sizeof(const char *));
                if (!_stricmp(candidate, name))
                {
                    resolved = assets[i].header.data;
                    break;
                }
            }
        }
        if (!resolved)
        {
            return false;
        }
        if (owner->impact)
        {
            for (int entry = 0; entry < 12; ++entry)
            {
                FxImpactEntry *impact = &owner->impact->table[entry];
                for (int i = 0; i < 33; ++i)
                {
                    const FxEffectDef **effect = i < 29 ? &impact->nonflesh[i] : &impact->flesh[i - 29];
                    if (*effect == reference->header.fx)
                    {
                        *effect = (const FxEffectDef *)resolved;
                    }
                }
            }
            continue;
        }
        for (int field = 0; field < ARRAY_COUNT(db64WeaponFields); ++field)
        {
            const DB64WeaponField *layout = &db64WeaponFields[field];
            if (layout->type == DB64_WEAPON_STRING)
            {
                continue;
            }
            for (int i = 0; i < layout->count; ++i)
            {
                uint8_t *address = (uint8_t *)weapon + layout->offset + i * sizeof(void *);
                void *value;
                memcpy(&value, address, sizeof(void *));
                if (value == reference->header.data)
                {
                    memcpy(address, &resolved, sizeof(void *));
                }
            }
        }
        for (int i = 0; weapon->bounceSound && i < 29; ++i)
        {
            if (weapon->bounceSound[i] == reference->header.data)
            {
                weapon->bounceSound[i] = (snd_alias_list_t *)resolved;
            }
        }
    }
    return true;
}

static bool Fail(char *error, size_t size, const char *message)
{
    if (size)
    {
        snprintf(error, size, "%s", message);
    }
    return false;
}

static bool ReadFileBytes(const char *path, uint8_t **data, size_t *size, char *error, size_t errorSize,
                          size_t limit = 16 * 1024 * 1024)
{
    *data = NULL;
    *size = 0;
    FILE *file = NULL;
    if (fopen_s(&file, path, "rb"))
    {
        return Fail(error, errorSize, "Cannot open native asset input");
    }
    bool ok = _fseeki64(file, 0, SEEK_END) == 0;
    const int64_t length = ok ? _ftelli64(file) : -1;
    ok = length >= 0 && (uint64_t)length <= limit && _fseeki64(file, 0, SEEK_SET) == 0;
    if (ok)
    {
        *data = (uint8_t *)malloc((size_t)length + 1);
        ok = *data && fread(*data, 1, (size_t)length, file) == (size_t)length;
    }
    fclose(file);
    if (!ok)
    {
        free(*data);
        *data = NULL;
        return Fail(error, errorSize, "Cannot read native asset input or file is too large");
    }
    *size = (size_t)length;
    (*data)[length] = 0;
    return true;
}

// Sound rows optionally select a source level/game; all other rows have two fields.
static bool ParseRow(char *line, char *type, size_t typeSize, char *name, size_t nameSize,
                     char *level, char *game)
{
    char *cursor = line;
    level[0] = game[0] = 0;
    for (int field = 0; field < 4; ++field)
    {
        while (*cursor == ' ' || *cursor == '\t')
        {
            ++cursor;
        }
        char *out = field == 0 ? type : (field == 1 ? name : (field == 2 ? level : game));
        const size_t capacity = field == 0 ? typeSize : (field == 1 ? nameSize : (field == 2 ? 64 : 16));
        size_t length = 0;
        const bool quoted = *cursor == '"';
        if (quoted)
        {
            ++cursor;
        }
        bool closed = !quoted;
        while (*cursor)
        {
            if (quoted && *cursor == '"')
            {
                ++cursor;
                if (*cursor != '"')
                {
                    closed = true;
                    break;
                }
            }
            else if (!quoted && *cursor == ',')
            {
                break;
            }
            if (length + 1 >= capacity)
            {
                return false;
            }
            out[length++] = *cursor++;
        }
        if (!closed)
        {
            return false;
        }
        if (!quoted)
        {
            while (length && (out[length - 1] == ' ' || out[length - 1] == '\t'))
            {
                --length;
            }
        }
        out[length] = 0;
        while (*cursor == ' ' || *cursor == '\t')
        {
            ++cursor;
        }
        if (!length && !(field == 2 && !strcmp(type, "sound")))
        {
            return false;
        }
        if (field == 1 && !*cursor)
        {
            return true;
        }
        if (field == 1 && *cursor == ',')
        {
            const char *padding = cursor;
            while (*padding == ',' || *padding == ' ' || *padding == '\t')
            {
                ++padding;
            }
            if (!*padding)
            {
                return true;
            }
        }
        if (field < 3)
        {
            if (*cursor != ',')
            {
                return false;
            }
            ++cursor;
        }
        else if (*cursor)
        {
            return false;
        }
    }
    return !strcmp(type, "sound") && (!level[0] || DB64_ValidMapName(level)) &&
           (!strcmp(game, "all_mp") || !strcmp(game, "all_sp"));
}

struct ManifestExpansion
{
    char *text;
    size_t size, capacity;
    size_t sourceBytes;
    unsigned int includes;
    char stack[32][DB64_PACKAGE_PATH];
};
static bool ExpandManifest(const char *root, char *text, size_t size, ManifestExpansion *output,
                            int depth, char *error, size_t errorSize)
{
    if (depth == 32 || memchr(text, 0, size))
    {
        return Fail(error, errorSize, "Zone-source include depth exceeded or embedded NUL");
    }
    char *cursor = text;
    while (*cursor)
    {
        char *line = cursor;
        while (*cursor && *cursor != '\r' && *cursor != '\n')
        {
            ++cursor;
        }
        if (*cursor)
        {
            const char end = *cursor;
            *cursor++ = 0;
            if (end == '\r' && *cursor == '\n')
            {
                ++cursor;
            }
        }
        char type[64], name[DB64_PACKAGE_PATH], level[64], game[16];
        const bool parsed = ParseRow(line, type, sizeof(type), name, sizeof(name), level, game);
        if (parsed && (!strcmp(type, "include_console") || !strcmp(type, "include_ps3")))
        {
            continue;
        }
        if (parsed && (!strcmp(type, "include") || !strcmp(type, "include_pc")))
        {
            if (++output->includes > 4096)
            {
                return Fail(error, errorSize, "Too many zone-source includes");
            }
            char relative[DB64_PACKAGE_PATH], path[32768];
            const size_t length = strlen(name);
            const bool extension = length >= 4 && !_stricmp(name + length - 4, ".csv");
            const int count = snprintf(relative, sizeof(relative), "%s%s", name, extension ? "" : ".csv");
            if (count < 0 || count >= sizeof(relative) ||
                !DB64_NormalizePath(relative, output->stack[depth], sizeof(output->stack[depth])))
            {
                return Fail(error, errorSize, "Invalid zone-source include path");
            }
            for (int i = 0; i < depth; ++i)
            {
                if (!strcmp(output->stack[i], output->stack[depth]))
                {
                    return Fail(error, errorSize, "Cyclic zone-source include");
                }
            }
            const int pathLength = snprintf(path, sizeof(path), "%s/zone_source/%s", root, output->stack[depth]);
            uint8_t *child = NULL;
            size_t childSize = 0;
            if (pathLength < 0 || pathLength >= sizeof(path) || !ReadFileBytes(path, &child, &childSize, error, errorSize))
            {
                return Fail(error, errorSize, "Cannot read zone-source include");
            }
            output->sourceBytes += childSize;
            const bool valid = output->sourceBytes <= 64 * 1024 * 1024 ?
                ExpandManifest(root, (char *)child, childSize, output, depth + 1, error, errorSize) :
                Fail(error, errorSize, "Zone-source includes exceed 64 MiB of input");
            free(child);
            if (!valid)
            {
                return false;
            }
            continue;
        }
        const size_t length = strlen(line);
        const size_t required = output->size + length + 2;
        if (required > 16 * 1024 * 1024)
        {
            return Fail(error, errorSize, "Expanded zone-source exceeds 16 MiB");
        }
        if (required > output->capacity)
        {
            const size_t capacity = required > output->capacity * 2 ? required : output->capacity * 2;
            char *expanded = (char *)realloc(output->text, capacity);
            if (!expanded)
            {
                return Fail(error, errorSize, "Out of memory expanding zone-source includes");
            }
            output->text = expanded;
            output->capacity = capacity;
        }
        memcpy(output->text + output->size, line, length);
        output->size += length;
        output->text[output->size++] = '\n';
        output->text[output->size] = 0;
    }
    return true;
}

static bool Import(const char *root, const char *type, const char *name, ScriptStringList *strings,
                   ManifestEffectContext *effects, ManifestInput *input, char *error, size_t errorSize)
{
    char normalized[DB64_PACKAGE_PATH];
    if (!DB64_NormalizePath(name, normalized, sizeof(normalized)))
    {
        return Fail(error, errorSize, "Invalid native asset path");
    }
    if (!strcmp(type, "weapon") || !strcmp(type, "impactfx"))
    {
        ManifestReferences *owner = (ManifestReferences *)calloc(1, sizeof(ManifestReferences));
        if (!owner)
        {
            return Fail(error, errorSize, "Out of memory importing weapon");
        }
        input->owned = owner;
        input->release = FreeReferencedAssets;
        owner->root = root;
        owner->effects = effects;
        owner->strings = strings;
        owner->error = error;
        owner->errorSize = errorSize;
        const char *level = effects->level ? effects->level : "";
        if (effects->world && effects->world->metadata)
        {
            level = effects->world->metadata->entities.name;
            const char *slash = strrchr(level, '/');
            level = slash ? slash + 1 : level;
        }
        if (strlen(level) >= sizeof(owner->level))
        {
            return Fail(error, errorSize, "Weapon sound context is too long");
        }
        strcpy(owner->level, level);
        char *dot = strrchr(owner->level, '.');
        if (dot)
        {
            *dot = 0;
        }
        const bool impact = !strcmp(type, "impactfx");
        const bool compiled = impact ? Linker_CompileImpactFiles(root, normalized, ReferenceImpactEffect, owner,
                                         &owner->impact, error, errorSize) :
            Linker_CompileWeaponFile(root, normalized, ReferenceNativeAsset, InternWeaponString, owner,
                                    &owner->source, error, errorSize);
        if (!compiled || owner->failed)
        {
            if (errorSize && !error[0])
            {
                Fail(error, errorSize, "Cannot collect native weapon asset references");
            }
            return false;
        }
        input->count = 1;
        input->assets[0].type = impact ? ASSET_TYPE_IMPACT_FX : ASSET_TYPE_WEAPON;
        input->assets[0].header.data = impact ? (void *)owner->impact : (void *)Linker_GetWeapon(owner->source);
        return true;
    }
    if (!strcmp(type, "localize"))
    {
        char path[DB64_PACKAGE_PATH], normalizedPath[DB64_PACKAGE_PATH];
        const int length = snprintf(path, sizeof(path), "%s/localizedstrings/%s%s", effects->language, name,
            strlen(name) >= 4 && !_stricmp(name + strlen(name) - 4, ".str") ? "" : ".str");
        if (length < 0 || length >= sizeof(path) || !DB64_NormalizePath(path, normalizedPath, sizeof(normalizedPath)))
        {
            return Fail(error, errorSize, "Invalid localization source path");
        }
        void *data = NULL;
        size_t size = 0;
        bool found = false;
        if (!Linker_ReadRawAssetFile(root, normalizedPath, &data, &size, 16 * 1024 * 1024, &found))
        {
            if (found || !_stricmp(effects->language, "english"))
            {
                return Fail(error, errorSize, "Cannot read localization source");
            }
            snprintf(path, sizeof(path), "english/localizedstrings/%s%s", name,
                strlen(name) >= 4 && !_stricmp(name + strlen(name) - 4, ".str") ? "" : ".str");
            if (!Linker_ReadRawAssetFile(root, path, &data, &size, 16 * 1024 * 1024))
            {
                return Fail(error, errorSize, "Cannot read English localization fallback");
            }
        }
        LinkerLocalization *source = NULL;
        const bool ok = Linker_ImportLocalization(data, size, name, effects->language, &source, error, errorSize);
        free(data);
        if (ok)
        {
            input->owned = source;
            input->release = FreeLocalization;
            input->count = source->count;
            input->expandedAssets = source->assets;
        }
        return ok;
    }
    const char *directory = "";
    const char *extension = "";
    if (!strcmp(type, "menufile"))
    {
        ManifestMenu *owner = (ManifestMenu *)calloc(1, sizeof(ManifestMenu));
        if (!owner)
        {
            return Fail(error, errorSize, "Out of memory compiling menu");
        }
        owner->root = root;
        owner->language = effects->language;
        owner->error = error;
        owner->errorSize = errorSize;
        input->owned = owner;
        input->release = FreeMenu;
        if (!Linker_CompileMenu(root, normalized, CompileMenuMaterial, QueueMenuSound, owner, &owner->menu, error, errorSize))
        {
            return false;
        }
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_MENULIST;
        input->assets[0].header.menuList = Linker_GetMenuList(owner->menu);
        if (!Linker_VisitMenuScriptAssets(input->assets[0].header.menuList, CompileMenuScriptDependency,
                                          owner, error, errorSize))
        {
            return false;
        }
        for (int i = 0; i < input->assets[0].header.menuList->menuCount; ++i)
        {
            const char *sound = input->assets[0].header.menuList->menus[i]->soundName;
            if (sound && sound[0] && !QueueMenuSound(sound, owner))
            {
                return Fail(error, errorSize, "Cannot queue native menu loop sound");
            }
        }
        // Script-only materials have no typed pointer in the menu graph. Make them
        // roots; already referenced backgrounds reuse the writer's asset aliases.
        owner->roots = (XAsset *)calloc(owner->materialCount + 1, sizeof(XAsset));
        if (!owner->roots)
        {
            return Fail(error, errorSize, "Out of memory recording native menu materials");
        }
        owner->roots[0] = input->assets[0];
        for (int i = 0; i < owner->materialCount; ++i)
        {
            owner->roots[i + 1].type = ASSET_TYPE_MATERIAL;
            owner->roots[i + 1].header.material = owner->materials[i]->material;
        }
        input->expandedAssets = owner->roots;
        input->count = owner->materialCount + 1;
        return true;
    }
    if (!strcmp(type, "snddriverglobals"))
    {
        if (strcmp(normalized, "singleton"))
        {
            return Fail(error, errorSize, "Sound driver globals must be named singleton");
        }
        SndDriverGlobals *globals = (SndDriverGlobals *)malloc(sizeof(SndDriverGlobals));
        if (!globals)
        {
            return Fail(error, errorSize, "Out of memory creating sound driver globals");
        }
        globals->name = "singleton";
        input->owned = globals;
        input->release = free;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_SNDDRIVER_GLOBALS;
        input->assets[0].header.sndDriverGlobals = globals;
        return true;
    }
    if (!strcmp(type, "image") && normalized[0] == '$')
    {
        GfxImage *image = NULL;
        if (!Linker_CreateBuiltinImage(normalized, TS_FUNCTION, IMAGE_TRACK_MISC, &image, error, errorSize))
        {
            return false;
        }
        input->owned = image;
        input->release = FreeImage;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_IMAGE;
        input->assets[0].header.image = image;
        return true;
    }
    if (!strcmp(type, "techset"))
    {
        LinkerCompiledTechniqueSet *techniques = NULL;
        // State maps are compiled for validation; standalone technique assets do
        // not contain material state bits. Materials provide their own reference.
        const unsigned int reference[2] = {0x8000, 0};
        if (!Linker_CompileTechniqueSetFiles(root, normalized, reference, 0, &techniques, error, errorSize))
        {
            return false;
        }
        input->owned = techniques;
        input->release = FreeTechniqueSet;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_TECHNIQUE_SET;
        input->assets[0].header.techniqueSet = techniques->techniqueSet;
        return true;
    }
    if (!strcmp(type, "lightdef"))
    {
        GfxLightDef *light = NULL;
        unsigned int lookup = effects->lightLookup ? effects->lightLookup : 1;
        LinkerWorldSource *world = effects->world;
        for (unsigned int i = 0; world && i < world->lightDefCount; ++i)
        {
            GfxLightDef *existing = world->lightDefs[i];
            if (!_stricmp(existing->name, normalized))
            {
                input->count = 1;
                input->assets[0].type = ASSET_TYPE_LIGHT_DEF;
                input->assets[0].header.lightDef = existing;
                return true;
            }
            const unsigned int end = existing->lmapLookupStart + existing->attenuation.image->width + 2;
            if (lookup < end)
            {
                lookup = end;
            }
        }
        if (!Linker_CompileLightDefinition(root, normalized, lookup, &light, error, errorSize))
        {
            return false;
        }
        effects->lightLookup = lookup + light->attenuation.image->width + 2;
        input->owned = light;
        input->release = FreeLightDefinition;
        for (unsigned int i = 0; world && world->lightmaps && i < world->lightmaps->count; ++i)
        {
            if (!Linker_StampLightAttenuation(light, world->lightmaps->lightmaps[i].secondary, error, errorSize))
            {
                return false;
            }
        }
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_LIGHT_DEF;
        input->assets[0].header.lightDef = light;
        return true;
    }
    if (!strcmp(type, "sound"))
    {
        const char *separator = strstr(normalized, ".csv/");
        if ((separator && !separator[5]) || (!separator && strchr(normalized, '/')))
        {
            return Fail(error, errorSize, "Sound rows require alias_name or table.csv/alias_name");
        }
        const size_t tableLength = separator ? (size_t)(separator + 4 - normalized) : 0;
        char table[DB64_PACKAGE_PATH];
        const int length = snprintf(table, sizeof(table), "soundaliases/%.*s", (int)tableLength, normalized);
        if (length < 0 || length >= sizeof(table))
        {
            return Fail(error, errorSize, "Sound alias table path is too long");
        }
        char level[DB64_PACKAGE_PATH] = {};
        if (effects->level)
        {
            snprintf(level, sizeof(level), "%s", effects->level);
        }
        if (effects->world && effects->world->metadata)
        {
            const char *map = effects->world->metadata->entities.name;
            const char *base = strrchr(map, '/');
            base = base ? base + 1 : map;
            const size_t mapLength = strlen(base);
            if (mapLength >= sizeof(level))
            {
                return Fail(error, errorSize, "Sound alias map context is too long");
            }
            strcpy(level, base);
            char *dot = strrchr(level, '.');
            if (dot)
            {
                *dot = 0;
            }
        }
        LinkerCompiledSoundAlias *compiled = NULL;
        if (input->soundLevel[0] || input->soundGame[0])
        {
            strcpy(level, input->soundLevel);
        }
        if (!Linker_CompileSoundAliasForZone(root, separator ? table : NULL, separator ? separator + 5 : normalized,
                                            level, input->soundGame[0] ? input->soundGame : "all_mp",
                                            &compiled, error, errorSize, effects->language))
        {
            return false;
        }
        input->owned = compiled;
        input->release = FreeSoundAlias;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_SOUND;
        input->assets[0].header.sound = Linker_GetSoundAlias(compiled);
        return true;
    }
    if ((!strcmp(type, "fx") || !strcmp(type, "xmodel")) && !effects->world)
    {
        effects->world = (LinkerWorldSource *)calloc(1, sizeof(LinkerWorldSource));
        if (!effects->world)
        {
            return Fail(error, errorSize, "Out of memory creating native asset cache");
        }
        effects->world->modelStrings.count = 1;
        effects->world->modelStrings.strings = (const char **)calloc(65536, sizeof(const char *));
        if (!effects->world->modelStrings.strings)
        {
            return Fail(error, errorSize, "Out of memory creating native script string cache");
        }
    }
    if (!strcmp(type, "gfxworld") || !strcmp(type, "map") || !strcmp(type, "fx"))
    {
        LinkerWorldSource *world = NULL;
        const FxEffectDef *effect = NULL;
        if (!strcmp(type, "fx"))
        {
            world = effects->world;
            if (world->modelStrings.strings)
            {
                effect = Linker_CompileWorldEffect(root, world, normalized, error, errorSize);
            }
            else
            {
                Fail(error, errorSize, "Out of memory importing effect script strings");
            }
            if (!effect)
            {
                return false;
            }
        }
        else if (!Linker_CompileWorldSource(root, normalized, true, &world, error, errorSize))
        {
            return false;
        }
        if (!effect)
        {
            input->owned = world;
            input->release = FreeRenderWorld;
        }
        // World compilation owns a local bone-name table. Merge by text so rows
        // before or after the world can safely share the zone's script strings.
        for (unsigned int m = effect ? effects->remappedModelCount : 0; m < world->modelCount; ++m)
        {
            XModel *model = world->models[m]->model;
            for (unsigned int b = 0; b < model->numBones; ++b)
            {
                const unsigned int index = model->boneNames[b];
                if (!index || index >= (unsigned int)world->modelStrings.count)
                {
                    return Fail(error, errorSize, "Invalid compiled world bone name");
                }
                const uint16_t merged = Linker_InternModelString(world->modelStrings.strings[index], strings);
                if (!merged)
                {
                    return Fail(error, errorSize, "Cannot merge world bone names into native zone");
                }
                model->boneNames[b] = merged;
            }
        }
        if (effect)
        {
            effects->remappedModelCount = world->modelCount;
            input->assets[0].type = ASSET_TYPE_FX;
            input->assets[0].header.fx = (FxEffectDef *)effect;
        }
        else
        {
            input->assets[0].type = ASSET_TYPE_GFXWORLD;
            input->assets[0].header.gfxWorld = Linker_GetAssembledWorld(world->assembled);
            if (!effects->world)
            {
                effects->world = world;
                effects->remappedModelCount = world->modelCount;
                effects->borrowed = true;
            }
        }
        input->count = 1;
        if (!strcmp(type, "map"))
        {
            world->collision->name = world->metadata->entities.name;
            world->collision->mapEnts = &world->metadata->entities;
            input->assets[1].type = ASSET_TYPE_CLIPMAP;
            input->assets[1].header.clipMap = world->collision;
            input->assets[2].type = ASSET_TYPE_MAP_ENTS;
            input->assets[2].header.mapEnts = &world->metadata->entities;
            input->assets[3].type = ASSET_TYPE_COMWORLD;
            input->assets[3].header.comWorld = &world->metadata->common;
            input->assets[4].type = ASSET_TYPE_GAMEWORLD_MP;
            input->assets[4].header.gameWorldMp = &world->metadata->multiplayer;
            input->count = 5;
        }
        return true;
    }
    if (!strcmp(type, "xmodel"))
    {
        LinkerWorldSource *world = effects->world;
        XModel *model = Linker_CompileWorldModel(root, world, normalized, error, errorSize);
        if (!model)
        {
            return false;
        }
        for (unsigned int m = effects->remappedModelCount; m < world->modelCount; ++m)
        {
            XModel *added = world->models[m]->model;
            for (unsigned int b = 0; b < added->numBones; ++b)
            {
                const unsigned int index = added->boneNames[b];
                if (!index || index >= (unsigned int)world->modelStrings.count)
                {
                    return Fail(error, errorSize, "Invalid compiled model bone name");
                }
                const uint16_t merged = Linker_InternModelString(world->modelStrings.strings[index], strings);
                if (!merged)
                {
                    return Fail(error, errorSize, "Cannot merge model bone names into native zone");
                }
                added->boneNames[b] = merged;
            }
        }
        effects->remappedModelCount = world->modelCount;
        input->assets[0].type = ASSET_TYPE_XMODEL;
        input->assets[0].header.model = model;
        input->count = 1;
        return true;
    }
    if (!strcmp(type, "material"))
    {
        LinkerCompiledMaterial *material = NULL;
        if (!Linker_CompileMaterialFiles(root, normalized, IMAGE_TRACK_WORLD, &material, error, errorSize, effects->language))
        {
            return false;
        }
        input->assets[0].type = ASSET_TYPE_MATERIAL;
        input->assets[0].header.material = material->material;
        input->count = 1;
        input->owned = material;
        input->release = FreeMaterial;
        return true;
    }
    if (!strcmp(type, "soundcurve"))
    {
        directory = "soundaliases/";
        extension = ".vfcurve";
    }
    else if (!strcmp(type, "xanim"))
    {
        directory = "xanim/";
    }
    else if (!strcmp(type, "image"))
    {
        directory = "images/";
        extension = ".iwi";
    }
    else if (!strcmp(type, "loaded_sound"))
    {
        directory = "sound/";
    }
    else if (!strcmp(type, "physpreset"))
    {
        directory = "physic/";
    }
    else if (strcmp(type, "rawfile") && strcmp(type, "mapworlds") && strcmp(type, "font") && strcmp(type, "stringtable"))
    {
        return Fail(error, errorSize, "Native importer is not implemented for this asset type");
    }
    char path[32768];
    if (snprintf(path, sizeof(path), "%s%s%s", directory, normalized, extension) >= sizeof(path))
    {
        return Fail(error, errorSize, "Native input path is too long");
    }
    uint8_t *bytes = NULL;
    size_t size = 0;
    void *inputBytes = NULL;
    const bool localized = !strcmp(type, "font") || !strcmp(type, "loaded_sound") ||
                           (!strcmp(type, "rawfile") && !_strnicmp(path, "sound/", 6));
    if (!Linker_ReadLocalizedAssetFile(root, path, localized ? effects->language : NULL,
                                       &inputBytes, &size, INT_MAX))
    {
        return Fail(error, errorSize, "Cannot read native asset from raw files or IWD archives");
    }
    bytes = (uint8_t *)inputBytes;
    bool ok = false;
    if (!strcmp(type, "stringtable"))
    {
        StringTable *table = NULL;
        ok = Linker_ImportStringTable(bytes, size, normalized, &table, error, errorSize);
        input->owned = table;
        input->release = free;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_STRINGTABLE;
        input->assets[0].header.stringTable = table;
    }
    else if (!strcmp(type, "font"))
    {
        ManifestFont *font = (ManifestFont *)calloc(1, sizeof(ManifestFont));
        if (font)
        {
            font->root = root;
            font->language = effects->language;
            font->error = error;
            font->errorSize = errorSize;
            ok = Linker_ImportFont(bytes, size, normalized, CompileFontMaterial, font, &font->font, error, errorSize);
            input->owned = font;
            input->release = FreeFont;
            input->count = 1;
            input->assets[0].type = ASSET_TYPE_FONT;
            input->assets[0].header.font = font->font;
        }
        else
        {
            Fail(error, errorSize, "Out of memory importing font");
        }
    }
    else if (!strcmp(type, "soundcurve"))
    {
        SndCurve *curve = NULL;
        ok = Linker_ImportSoundCurve(bytes, size, normalized, &curve, error, errorSize);
        input->owned = curve;
        input->release = free;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_SOUND_CURVE;
        input->assets[0].header.sndCurve = curve;
    }
    else if (!strcmp(type, "xanim"))
    {
        LinkerCompiledAnimation *animation = NULL;
        ok = Linker_ImportAnimation(bytes, size, normalized, strings, &animation, error, errorSize);
        input->owned = animation;
        input->release = FreeAnimation;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_XANIMPARTS;
        input->assets[0].header.parts = Linker_GetAnimation(animation);
    }
    else if (!strcmp(type, "rawfile"))
    {
        const size_t nameSize = strlen(normalized) + 1;
        RawFile *raw = (RawFile *)calloc(1, sizeof(RawFile) + nameSize + size + 1);
        if (raw)
        {
            char *ownedName = (char *)(raw + 1);
            memcpy(ownedName, normalized, nameSize);
            raw->name = ownedName;
            raw->buffer = ownedName + nameSize;
            memcpy((void *)raw->buffer, bytes, size);
            raw->len = (int)size;
            input->assets[0].type = ASSET_TYPE_RAWFILE;
            input->assets[0].header.rawfile = raw;
            input->owned = raw;
            input->release = free;
            input->count = 1;
            ok = true;
        }
        else
        {
            Fail(error, errorSize, "Out of memory importing rawfile");
        }
    }
    else if (!strcmp(type, "physpreset"))
    {
        PhysPreset *preset = NULL;
        ok = Linker_ImportPhysicsPreset(bytes, size, normalized, &preset, error, errorSize);
        input->owned = preset;
        input->release = free;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_PHYSPRESET;
        input->assets[0].header.physPreset = preset;
    }
    else if (!strcmp(type, "image"))
    {
        GfxImage *image = NULL;
        ok = Linker_ImportImage(bytes, size, normalized, 0, 0, &image, error, errorSize);
        input->owned = image;
        input->release = FreeImage;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_IMAGE;
        input->assets[0].header.image = image;
    }
    else if (!strcmp(type, "loaded_sound"))
    {
        LoadedSound *sound = NULL;
        ok = Linker_ImportPcmWave(bytes, size, normalized, &sound, error, errorSize);
        input->owned = sound;
        input->release = FreeSound;
        input->count = 1;
        input->assets[0].type = ASSET_TYPE_LOADED_SOUND;
        input->assets[0].header.loadSnd = sound;
    }
    else
    {
        LinkerMapWorlds *worlds = NULL;
        ok = Linker_ImportMapWorlds(bytes, size, normalized, &worlds, error, errorSize);
        input->owned = worlds;
        input->release = FreeWorlds;
        input->count = 3;
        if (ok)
        {
            input->assets[0].type = ASSET_TYPE_MAP_ENTS;
            input->assets[0].header.mapEnts = &worlds->entities;
            input->assets[1].type = ASSET_TYPE_COMWORLD;
            input->assets[1].header.comWorld = &worlds->common;
            input->assets[2].type = ASSET_TYPE_GAMEWORLD_MP;
            input->assets[2].header.gameWorldMp = &worlds->multiplayer;
        }
    }
    free(bytes);
    return ok;
}

static bool WriteAtomic(const char *path, const uint8_t *data, size_t size, char *error, size_t errorSize)
{
    char temporary[32768];
    if (snprintf(temporary, sizeof(temporary), "%s.%lu.%llu.tmp", path, GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64()) >= sizeof(temporary))
    {
        return Fail(error, errorSize, "Native output path is too long");
    }
    HANDLE file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return Fail(error, errorSize, "Cannot create native output temporary file");
    }
    bool ok = true;
    size_t offset = 0;
    while (offset < size)
    {
        const DWORD chunk = (DWORD)((size - offset) > 1048576 ? 1048576 : size - offset);
        DWORD written = 0;
        if (!WriteFile(file, data + offset, chunk, &written, NULL) || written != chunk)
        {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok)
    {
        ok = FlushFileBuffers(file) != 0;
    }
    CloseHandle(file);
    if (ok)
    {
        ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!ok)
    {
        DeleteFileA(temporary);
        return Fail(error, errorSize, "Cannot commit native fastfile output");
    }
    return true;
}

struct ManifestAssetIndex
{
    LinkerExternalAsset *entries;
    size_t count;
};
struct NativeAssetIndexHeader
{
    char magic[8];
    uint32_t version, count;
    uint64_t fileSize;
    uint32_t crc, reserved;
};
static_assert(sizeof(NativeAssetIndexHeader) == 32);

static bool IndexNativeAsset(int type, const char *name, void *context)
{
    ManifestAssetIndex *index = (ManifestAssetIndex *)context;
    if (type < 0 || type >= ASSET_TYPE_COUNT || !name || !name[0] || strlen(name) >= 1024 || name[0] == ',')
    {
        return false;
    }
    for (size_t i = 0; i < index->count; ++i)
    {
        if (index->entries[i].type == type && !_stricmp(index->entries[i].name, name))
        {
            return true;
        }
    }
    if (index->count == 32768)
    {
        return false;
    }
    LinkerExternalAsset *entries = (LinkerExternalAsset *)realloc(index->entries,
        (index->count + 1) * sizeof(LinkerExternalAsset));
    if (!entries)
    {
        return false;
    }
    index->entries = entries;
    char *copy = _strdup(name);
    if (!copy)
    {
        return false;
    }
    entries[index->count++] = {type, NULL, copy};
    return true;
}

static void FreeAssetIndex(ManifestAssetIndex *index)
{
    for (size_t i = 0; i < index->count; ++i)
    {
        free((void *)index->entries[i].name);
    }
    free(index->entries);
}

static bool WriteAssetIndex(const char *output, const uint8_t *zone, size_t zoneSize,
                           const ManifestAssetIndex *index, char *error, size_t errorSize)
{
    char path[32768];
    if (snprintf(path, sizeof(path), "%s.assets", output) >= sizeof(path) || zoneSize > UINT_MAX)
    {
        return Fail(error, errorSize, "Native asset index path or zone is too large");
    }
    size_t size = sizeof(NativeAssetIndexHeader);
    for (size_t i = 0; i < index->count; ++i)
    {
        size += sizeof(DB64ExternalAssetRecord) + strlen(index->entries[i].name) + 1;
    }
    uint8_t *bytes = (uint8_t *)malloc(size);
    if (!bytes)
    {
        return Fail(error, errorSize, "Out of memory writing native asset index");
    }
    NativeAssetIndexHeader header = {};
    memcpy(header.magic, "KIWIIDX2", 8);
    header.version = DB64_FASTFILE_VERSION;
    header.count = (uint32_t)index->count;
    header.fileSize = zoneSize;
    header.crc = crc32(0, zone, (uInt)zoneSize);
    memcpy(bytes, &header, sizeof(NativeAssetIndexHeader));
    size_t offset = sizeof(NativeAssetIndexHeader);
    for (size_t i = 0; i < index->count; ++i)
    {
        DB64ExternalAssetRecord record = {index->entries[i].type, (uint32_t)strlen(index->entries[i].name) + 1};
        memcpy(bytes + offset, &record, sizeof(DB64ExternalAssetRecord));
        offset += sizeof(DB64ExternalAssetRecord);
        memcpy(bytes + offset, index->entries[i].name, record.nameSize);
        offset += record.nameSize;
    }
    const bool ok = WriteAtomic(path, bytes, size, error, errorSize);
    free(bytes);
    return ok;
}

static bool NativeZoneFingerprint(const char *path, uint64_t *size, uint32_t *crc)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        return false;
    }
    DB64FilePrefix prefix;
    bool ok = fread(&prefix, 1, sizeof(prefix), file) == sizeof(prefix) &&
              !memcmp(prefix.magic, DB64_FASTFILE_MAGIC, sizeof(prefix.magic)) &&
              prefix.version == DB64_FASTFILE_VERSION;
    *size = sizeof(prefix);
    *crc = ok ? crc32(0, (const Bytef *)&prefix, sizeof(prefix)) : 0;
    uint8_t bytes[65536];
    while (ok)
    {
        const size_t count = fread(bytes, 1, sizeof(bytes), file);
        *size += count;
        *crc = crc32(*crc, bytes, (uInt)count);
        ok = *size <= UINT_MAX && !ferror(file);
        if (count < sizeof(bytes))
        {
            break;
        }
    }
    return fclose(file) == 0 && ok;
}

static bool ReadIgnoredZone(const char *root, const char *output, const char *language, const char *zoneName,
                            ManifestAssetIndex *index, char *error, size_t errorSize, bool optional = false)
{
    if (!DB64_ValidMapName(zoneName))
    {
        return Fail(error, errorSize, "Invalid ignored zone name");
    }
    char zonePath[32768], indexPath[32768];
    const char *slash = strrchr(output, '/'), *backslash = strrchr(output, '\\');
    if (backslash && (!slash || backslash > slash))
    {
        slash = backslash;
    }
    const size_t directorySize = slash ? (size_t)(slash - output + 1) : 0;
    if (directorySize > 32000 || snprintf(zonePath, sizeof(zonePath), "%.*s%s.ff",
        (int)directorySize, output, zoneName) >= sizeof(zonePath))
    {
        return Fail(error, errorSize, "Ignored zone path is too long");
    }
    if (!_stricmp(zonePath, output))
    {
        return Fail(error, errorSize, "A native zone cannot ignore itself");
    }
    if (snprintf(indexPath, sizeof(indexPath), "%s.assets", zonePath) >= sizeof(indexPath))
    {
        return false;
    }
    if (GetFileAttributesA(indexPath) == INVALID_FILE_ATTRIBUTES)
    {
        if (snprintf(zonePath, sizeof(zonePath), "%s/zone/%s/%s.ff", root, language, zoneName) >= sizeof(zonePath) ||
            snprintf(indexPath, sizeof(indexPath), "%s.assets", zonePath) >= sizeof(indexPath))
        {
            return false;
        }
    }
    if (optional && GetFileAttributesA(indexPath) == INVALID_FILE_ATTRIBUTES)
    {
        return true;
    }
    char fullZone[32768], fullOutput[32768];
    const DWORD zoneLength = GetFullPathNameA(zonePath, sizeof(fullZone), fullZone, NULL);
    const DWORD outputLength = GetFullPathNameA(output, sizeof(fullOutput), fullOutput, NULL);
    if (!zoneLength || zoneLength >= sizeof(fullZone) || !outputLength || outputLength >= sizeof(fullOutput) ||
        !_stricmp(fullZone, fullOutput))
    {
        return Fail(error, errorSize, "A native zone cannot ignore itself or use an invalid path");
    }
    uint8_t *bytes = NULL;
    size_t size = 0;
    uint64_t zoneSize = 0;
    uint32_t zoneCrc = 0;
    bool ok = ReadFileBytes(indexPath, &bytes, &size, error, errorSize, 64 * 1024 * 1024) &&
              NativeZoneFingerprint(zonePath, &zoneSize, &zoneCrc);
    NativeAssetIndexHeader header = {};
    if (ok && size >= sizeof(header))
    {
        memcpy(&header, bytes, sizeof(header));
    }
    ok = ok && size >= sizeof(header) && !memcmp(header.magic, "KIWIIDX2", 8) &&
         header.version == DB64_FASTFILE_VERSION && !header.reserved && header.count <= 32768 &&
         header.fileSize == zoneSize && header.crc == zoneCrc;
    size_t offset = sizeof(header);
    for (uint32_t i = 0; ok && i < header.count; ++i)
    {
        DB64ExternalAssetRecord record;
        if (size - offset < sizeof(record))
        {
            ok = false;
            break;
        }
        memcpy(&record, bytes + offset, sizeof(record));
        offset += sizeof(record);
        ok = record.nameSize >= 2 && record.nameSize <= 1024 && record.nameSize <= size - offset;
        if (ok)
        {
            const char *name = (const char *)bytes + offset;
            ok = !name[record.nameSize - 1] && !memchr(name, 0, record.nameSize - 1) &&
                 IndexNativeAsset(record.type, name, index);
            offset += record.nameSize;
        }
    }
    ok = ok && offset == size;
    free(bytes);
    if (!ok)
    {
        return Fail(error, errorSize, "Ignored zone needs a matching native .ff and .ff.assets index; build it first");
    }
    return true;
}

static bool CompileManifestText(const char *root, const char *manifest, uint8_t *text, size_t textSize,
                                const char *output, bool compress, char *error, size_t errorSize, const char *level,
                                const char *language, bool mapZone = false)
{
    if (!language || !DB64_ValidMapName(language))
    {
        free(text);
        return Fail(error, errorSize, "Invalid native localization language");
    }
    ManifestExpansion expanded = {};
    const bool expandedOk = ExpandManifest(root, (char *)text, textSize, &expanded, 0, error, errorSize);
    free(text);
    if (!expandedOk)
    {
        free(expanded.text);
        return false;
    }
    text = (uint8_t *)(expanded.text ? expanded.text : _strdup(""));
    textSize = expanded.size;
    if (!text)
    {
        return Fail(error, errorSize, "Out of memory expanding zone-source");
    }
    if (!level || (level[0] && !DB64_ValidMapName(level)))
    {
        free(text);
        return Fail(error, errorSize, "Invalid native sound load-spec level");
    }
    if (memchr(text, 0, textSize))
    {
        free(text);
        return Fail(error, errorSize, "NUL byte in native manifest");
    }
    ManifestInput *inputs = (ManifestInput *)calloc(32768, sizeof(ManifestInput));
    int *rowTable = (int *)calloc(65536, sizeof(int));
    XAsset *assets = (XAsset *)calloc(32768, sizeof(XAsset));
    ScriptStringList strings = {1, (const char **)calloc(65536, sizeof(const char *))};
    if (!inputs || !rowTable || !assets || !strings.strings)
    {
        free(inputs);
        free(rowTable);
        free(assets);
        free(text);
        free(strings.strings);
        return Fail(error, errorSize, "Out of memory reading native manifest");
    }
    ManifestAssetIndex ignored = {}, index = {};
    int inputCount = 0;
    ManifestEffectContext effects = {};
    effects.level = level;
    effects.language = language;
    int assetCount = 0;
    int lineNumber = 0;
    bool ok = true;
    if (mapZone)
    {
        // These zones remain loaded during gameplay. UI assets do not.
        const char *commonZones[] = {"code_post_gfx_mp", "localized_code_post_gfx_mp",
                                    "common_mp", "localized_common_mp"};
        for (int i = 0; i < ARRAY_COUNT(commonZones) && ok; ++i)
        {
            ok = ReadIgnoredZone(root, output, language, commonZones[i], &ignored, error, errorSize, true);
        }
    }
    char *cursor = (char *)text;
    while (*cursor && ok)
    {
        ++lineNumber;
        char *line = cursor;
        while (*cursor && *cursor != '\r' && *cursor != '\n')
        {
            ++cursor;
        }
        if (*cursor)
        {
            const char delimiter = *cursor;
            *cursor++ = 0;
            if (delimiter == '\r' && *cursor == '\n')
            {
                ++cursor;
            }
        }
        while (*line == ' ' || *line == '\t')
        {
            ++line;
        }
        if (!*line || *line == '#' || (line[0] == '/' && line[1] == '/'))
        {
            continue;
        }
        char type[64], name[DB64_PACKAGE_PATH];
        char soundLevel[64], soundGame[16];
        if (!ParseRow(line, type, sizeof(type), name, sizeof(name), soundLevel, soundGame))
        {
            ok = Fail(error, errorSize, "Expected type,name or sound,name,level,all_mp|all_sp");
            break;
        }
        if (!strcmp(type, "ignore"))
        {
            ok = ReadIgnoredZone(root, output, language, name, &ignored, error, errorSize);
            continue;
        }
        const bool omitted = !strcmp(type, "omit");
        if (omitted)
        {
            char *separator = strchr(name, ':');
            if (!separator || separator == name || separator - name >= sizeof(type) || !separator[1])
            {
                ok = Fail(error, errorSize, "Expected omit,type:name");
                break;
            }
            memcpy(type, name, separator - name);
            type[separator - name] = 0;
            memmove(name, separator + 1, strlen(separator + 1) + 1);
            if (strcmp(type, "sound") && strcmp(type, "rawfile") && strcmp(type, "xmodel") &&
                strcmp(type, "weapon") && strcmp(type, "fx") && strcmp(type, "material") && strcmp(type, "impactfx"))
            {
                ok = Fail(error, errorSize, "Unsupported omitted asset type");
                break;
            }
        }
        char normalized[DB64_PACKAGE_PATH];
        if (!DB64_NormalizePath(name, normalized, sizeof(normalized)))
        {
            ok = Fail(error, errorSize, "Invalid native asset path");
            break;
        }
        if (!strcmp(type, "sound") && soundGame[0] && !strstr(normalized, ".csv/"))
        {
            char table[DB64_PACKAGE_PATH], relative[DB64_PACKAGE_PATH];
            const size_t length = strlen(normalized);
            const bool extension = length >= 4 && !strcmp(normalized + length - 4, ".csv");
            const int count = snprintf(table, sizeof(table), "%s%s", normalized, extension ? "" : ".csv");
            const int pathLength = snprintf(relative, sizeof(relative), "soundaliases/%s", table);
            ManifestScriptContext scripts = {inputs, &inputCount, rowTable, lineNumber, soundLevel, soundGame};
            ManifestSoundTable context = {&scripts, table};
            bool found = false;
            ok = count > 0 && count < sizeof(table) && pathLength > 0 && pathLength < sizeof(relative) &&
                 Linker_VisitSoundTable(root, relative, soundLevel, soundGame, QueueSoundTableAlias,
                                       &context, &found, error, errorSize);
            if (!ok || found)
            {
                if (!ok && (!errorSize || !error[0]))
                {
                    Fail(error, errorSize, "Cannot expand sound table or conflicting sound selection");
                }
                continue;
            }
        }
        // Stock zone sources use mp/name; the engine registers weapons by the bare name.
        if (!strcmp(type, "weapon") && !strncmp(normalized, "mp/", 3))
        {
            memmove(normalized, normalized + 3, strlen(normalized + 3) + 1);
            if (!normalized[0] || strchr(normalized, '/'))
            {
                ok = Fail(error, errorSize, "Invalid multiplayer weapon source path");
                break;
            }
        }
        if (!strcmp(type, "sndcurve"))
        {
            strcpy(type, "soundcurve");
        }
        char key[sizeof(type) + sizeof(normalized) + 1];
        snprintf(key, sizeof(key), "%s:%s", type, normalized);
        uint32_t hash = 2166136261;
        for (const char *byte = key; *byte; ++byte)
        {
            hash = (hash ^ (uint8_t)*byte) * 16777619;
        }
        unsigned int slot = hash & 65535;
        while (rowTable[slot] && strcmp(inputs[rowTable[slot] - 1].key, key))
        {
            slot = (slot + 1) & 65535;
        }
        if (rowTable[slot])
        {
            ManifestInput *existing = &inputs[rowTable[slot] - 1];
            if (omitted || existing->omitted)
            {
                existing->omitted = true;
                continue;
            }
            if (strcmp(existing->soundLevel, soundLevel) || strcmp(existing->soundGame, soundGame))
            {
                ok = Fail(error, errorSize, "Conflicting sound variant selection for duplicate manifest row");
            }
            continue;
        }
        if (inputCount >= 32768)
        {
            ok = Fail(error, errorSize, "Too many native manifest assets");
            break;
        }
        ManifestInput *input = &inputs[inputCount++];
        input->omitted = omitted;
        input->line = lineNumber;
        strcpy(input->soundLevel, soundLevel);
        strcpy(input->soundGame, soundGame);
        input->key = _strdup(key);
        if (!input->key)
        {
            ok = Fail(error, errorSize, "Out of memory indexing native manifest");
            break;
        }
        rowTable[slot] = inputCount;
    }
    // Build the map first so explicit effects share its compiler-owned assets,
    // independent of where the map row appeared in the source CSV.
    Linker_BeginSoundTableCache(root);
    for (int pass = 0; ok && pass < 2; ++pass)
    {
        for (int i = 0; ok && i < inputCount; ++i)
        {
            ManifestInput *input = &inputs[i];
            if (input->omitted)
            {
                if (pass == 1)
                {
                    fprintf(stderr, "Omitted by zone source: %s\n", input->key);
                }
                continue;
            }
            const char *separator = strchr(input->key, ':');
            char type[64];
            const size_t typeSize = (size_t)(separator - input->key);
            memcpy(type, input->key, typeSize);
            type[typeSize] = 0;
            const bool world = !strcmp(type, "map") || !strcmp(type, "gfxworld");
            if (world != (pass == 0))
            {
                continue;
            }
            lineNumber = input->line;
            effects.inputs = inputs;
            effects.inputCount = inputCount;
            if (world || !(i % 128))
            {
                printf("Importing native asset %d/%d: %s\n", i + 1, inputCount, input->key);
                fflush(stdout);
            }
            ok = Import(root, type, separator + 1, &strings, &effects, input, error, errorSize);
            if (!ok)
            {
                fprintf(stderr, "Native asset import failed: %s\n", input->key);
                break;
            }
            if (input->count > 32768 - assetCount)
            {
                ok = Fail(error, errorSize, "Too many native manifest assets");
                break;
            }
            const XAsset *imported = input->expandedAssets ? input->expandedAssets : input->assets;
            memcpy(assets + assetCount, imported, input->count * sizeof(XAsset));
            assetCount += input->count;
            for (int assetIndex = 0; ok && assetIndex < input->count; ++assetIndex)
            {
                ManifestScriptContext dependencies = {inputs, &inputCount, rowTable, input->line};
                const XAsset *asset = &imported[assetIndex];
                if (asset->type == ASSET_TYPE_FX)
                {
                    ok = Linker_VisitEffectSounds(asset->header.fx, QueueSoundAlias, &dependencies);
                }
                else if (asset->type == ASSET_TYPE_CLIPMAP)
                {
                    const clipMap_t *map = asset->header.clipMap;
                    for (int group = 0; ok && group < 2; ++group)
                    {
                        for (int entity = 0; ok && entity < map->dynEntCount[group]; ++entity)
                        {
                            ok = Linker_VisitEffectSounds(map->dynEntDefList[group][entity].destroyFx,
                                                         QueueSoundAlias, &dependencies);
                        }
                    }
                }
                if (!ok)
                {
                    Fail(error, errorSize, "Cannot queue effect sound dependencies");
                }
            }
            if (!strcmp(type, "weapon") || !strcmp(type, "impactfx"))
            {
                ManifestScriptContext dependencies = {inputs, &inputCount, rowTable, input->line};
                if (!QueueReferencedDependencies((ManifestReferences *)input->owned, &dependencies))
                {
                    ok = Fail(error, errorSize, "Cannot queue native weapon dependencies");
                }
            }
            if (!strcmp(type, "menufile"))
            {
                const ManifestMenu *owner = (const ManifestMenu *)input->owned;
                ManifestScriptContext dependencies = {inputs, &inputCount, rowTable, input->line};
                for (int sound = 0; ok && sound < owner->soundCount; ++sound)
                {
                    const int previousCount = inputCount;
                    const char *name = owner->sounds[sound]->aliasName;
                    ok = QueueSoundAlias(name, &dependencies);
                    if (ok && inputCount > previousCount)
                    {
                        bool menuSound = false;
                        ok = Linker_ProbeSoundAliasForZone(root, name, "menu", "all_mp", &menuSound,
                                                          error, errorSize);
                        if (ok && menuSound)
                        {
                            strcpy(inputs[previousCount].soundLevel, "menu");
                        }
                    }
                }
                if (!ok)
                {
                    Fail(error, errorSize, "Cannot queue native menu sound dependencies");
                }
            }
            if (input->count == 1 && input->assets[0].type == ASSET_TYPE_SOUND)
            {
                ManifestScriptContext dependencies = {inputs, &inputCount, rowTable, input->line,
                                                      input->soundLevel, input->soundGame};
                const snd_alias_list_t *aliases = input->assets[0].header.sound;
                for (int variant = 0; ok && variant < aliases->count; ++variant)
                {
                    const snd_alias_t *alias = &aliases->head[variant];
                    if (alias->soundFile->type == SAT_STREAMED)
                    {
                        const StreamFileNameRaw *file = &alias->soundFile->u.streamSnd.filename.info.raw;
                        const bool directory = file->dir && file->dir[0] && strcmp(file->dir, ".");
                        char path[DB64_PACKAGE_PATH], normalized[DB64_PACKAGE_PATH];
                        const int length = snprintf(path, sizeof(path), "sound/%s%s%s",
                            directory ? file->dir : "", directory ? "/" : "", file->name);
                        if (length < 0 || length >= sizeof(path) ||
                            !DB64_NormalizePath(path, normalized, sizeof(normalized)) ||
                            !QueueScriptAsset("rawfile", normalized, &dependencies))
                        {
                            ok = Fail(error, errorSize, "Cannot queue streamed sound bytes");
                            break;
                        }
                    }
                    if (!QueueSoundAlias(alias->secondaryAliasName, &dependencies) ||
                        !QueueSoundAlias(alias->chainAliasName, &dependencies))
                    {
                        ok = Fail(error, errorSize, "Cannot queue sound alias dependencies");
                    }
                }
            }
            if (!strcmp(type, "map"))
            {
                ManifestScriptContext scripts = {inputs, &inputCount, rowTable, input->line};
                if (!QueueMapDependencies(root, separator + 1, &scripts))
                {
                    ok = Fail(error, errorSize, "Cannot queue native map dependencies");
                }
            }
            if (input->count == 1 && input->assets[0].type == ASSET_TYPE_RAWFILE)
            {
                const RawFile *raw = input->assets[0].header.rawfile;
                const size_t length = strlen(raw->name);
                if (length >= 4 && !_stricmp(raw->name + length - 4, ".gsc"))
                {
                    ManifestScriptContext scripts = {inputs, &inputCount, rowTable, input->line};
                    ok = Linker_ScanScriptAssetDependencies(raw->buffer, raw->len, QueueScript, QueueDiscoveredAsset,
                                                            &scripts, error, errorSize);
                    if (!ok)
                    {
                        fprintf(stderr, "Script dependency discovery failed: %s\n", raw->name);
                    }
                }
            }
        }
    }
    Linker_EndSoundTableCache();
    uint8_t *encoded = NULL;
    size_t encodedSize = 0;
    for (int i = 0; ok && i < inputCount; ++i)
    {
        if (inputs[i].omitted)
        {
            continue;
        }
        if ((!strncmp(inputs[i].key, "weapon:", 7) || !strncmp(inputs[i].key, "impactfx:", 9)) &&
            !ResolveReferencedDependencies((ManifestReferences *)inputs[i].owned, assets, assetCount))
        {
            ok = Fail(error, errorSize, "Cannot resolve native weapon dependencies");
        }
        if (!strncmp(inputs[i].key, "menufile:", 9) &&
            !ResolveMenuSounds((ManifestMenu *)inputs[i].owned, inputs, inputCount))
        {
            ok = Fail(error, errorSize, "Cannot resolve native menu sound dependencies");
        }
    }
    // G_PrintFastFileErrors looks up a rawfile named after the zone. Failed
    // builds are not published, so a successful build has an empty report.
    char reportName[DB64_PACKAGE_PATH];
    RawFile report = {};
    if (ok)
    {
        const char *base = output;
        for (const char *p = output; *p; ++p)
        {
            if (*p == '/' || *p == '\\')
            {
                base = p + 1;
            }
        }
        size_t length = strlen(base);
        if (length > 3 && !_stricmp(base + length - 3, ".ff"))
        {
            length -= 3;
        }
        if (!length || length >= sizeof(reportName))
        {
            ok = Fail(error, errorSize, "Invalid fastfile build-report name");
        }
        else
        {
            memcpy(reportName, base, length);
            reportName[length] = 0;
            bool haveReport = false;
            for (int i = 0; i < assetCount; ++i)
            {
                if (assets[i].type == ASSET_TYPE_RAWFILE &&
                    !_stricmp(assets[i].header.rawfile->name, reportName))
                {
                    haveReport = true;
                    break;
                }
            }
            if (!haveReport)
            {
                if (assetCount == 32768)
                {
                    ok = Fail(error, errorSize, "No asset slot for fastfile build report");
                }
                else
                {
                    report.name = reportName;
                    report.buffer = "";
                    assets[assetCount].type = ASSET_TYPE_RAWFILE;
                    assets[assetCount++].header.rawfile = &report;
                }
            }
        }
    }
    if (ok)
    {
        printf("Serializing %d native root assets\n", assetCount);
        fflush(stdout);
        ok = Linker_BuildNativeZoneWithStrings(assets, assetCount, &strings, compress, &encoded, &encodedSize, error,
                                               errorSize, ignored.entries, ignored.count, IndexNativeAsset, &index);
    }
    if (ok)
    {
        ok = WriteAtomic(output, encoded, encodedSize, error, errorSize) &&
             WriteAssetIndex(output, encoded, encodedSize, &index, error, errorSize);
    }
    if (ok)
    {
        printf("Native fastfile: %d assets, %zu bytes: %s\n", assetCount, encodedSize, output);
    }
    else
    {
        fprintf(stderr, "Native manifest %s, at or before line %d\n", manifest, lineNumber);
    }
    free(encoded);
    FreeAssetIndex(&ignored);
    FreeAssetIndex(&index);
    for (int i = 0; i < inputCount; ++i)
    {
        free(inputs[i].key);
        if (inputs[i].release)
        {
            inputs[i].release(inputs[i].owned);
        }
    }
    free(inputs);
    free(rowTable);
    if (!effects.borrowed)
    {
        Linker_FreeWorldSource(effects.world);
    }
    for (int i = 1; i < strings.count; ++i)
    {
        free((void *)strings.strings[i]);
    }
    free(strings.strings);
    free(assets);
    free(text);
    return ok;
}

bool Linker_CompileNativeManifest(const char *root, const char *manifest, const char *output, bool compress,
                                  char *error, size_t errorSize, const char *level, const char *language)
{
    uint8_t *text = NULL;
    size_t textSize = 0;
    if (!ReadFileBytes(manifest, &text, &textSize, error, errorSize))
    {
        return false;
    }
    return CompileManifestText(root, manifest, text, textSize, output, compress, error, errorSize, level, language);
}

bool Linker_CompileNativeMap(const char *root, const char *bsp, const char *manifest, const char *output,
                             bool compress, char *error, size_t errorSize, const char *language)
{
    char normalized[DB64_PACKAGE_PATH];
    if (!DB64_NormalizePath(bsp, normalized, sizeof(normalized)) || strchr(normalized, '"') ||
        strchr(normalized, '\r') || strchr(normalized, '\n'))
    {
        return Fail(error, errorSize, "Invalid native map path");
    }
    uint8_t *additional = NULL;
    size_t additionalSize = 0;
    if (manifest && !ReadFileBytes(manifest, &additional, &additionalSize, error, errorSize))
    {
        return false;
    }
    const size_t prefixSize = strlen(normalized) + 7; // map,"path" followed by newline
    uint8_t *text = (uint8_t *)malloc(prefixSize + additionalSize + 1);
    if (!text)
    {
        free(additional);
        return Fail(error, errorSize, "Out of memory preparing native map manifest");
    }
    snprintf((char *)text, prefixSize + 1, "map,\"%s\"\n", normalized);
    if (additionalSize)
    {
        memcpy(text + prefixSize, additional, additionalSize);
    }
    text[prefixSize + additionalSize] = 0;
    free(additional);
    return CompileManifestText(root, manifest ? manifest : bsp, text, prefixSize + additionalSize, output,
                               compress, error, errorSize, "", language, true);
}
