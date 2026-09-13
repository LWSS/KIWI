#include <universal/q_shared.h>
#include <ui/ui_shared.h>
#include <game/g_bsp.h>
#include <sound/snd_public.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_state.h>
#include <database64/db_image_assets.h>
#include <database64/db_render_world_layout.h>
#include <database64/db_render_light_grid.h>
#include <gfx_d3d/r_bsp.h>
#include <database64/db_shader_assets.h>
#include <database64/db_model_assets.h>
#include <database64/db_model_surfaces.h>
#include <xanim/xmodel.h>
#include <physics/phys_local.h>
#include <database64/db_physics_geometry.h>
#include <database64/db_animation_assets.h>
#include <database64/db_sound_assets.h>
#include <database64/db_font_assets.h>
#include <database64/db_sound_aliases.h>
#include <universal/com_sndalias.h>
#include <qcommon/qcommon.h>
#include <database64/db_clipmap_layout.h>
#include <math.h>
#include <gfx_d3d/fxprimitives.h>
#include <DynEntity/DynEntity_client.h>
#include <database64/db_effect_layout.h>
#include <database64/database.h>
#include <database64/fastfile_format.h>
#include "native_writer.h"
#include "native_header_padding.h"
#include <database64/db_shader_argument_layout.h>
#include <database64/db_menu_layout.h>
#include <database64/db_weapon_layout.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

struct NativeReference
{
    XAssetType type;
    const void *asset;
    uintptr_t token;
};

struct NativeWriter
{
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t position[9];
    size_t extent[9];
    unsigned int block;
    char *error;
    size_t errorSize;
    NativeReference *references;
    size_t referenceCount;
    size_t referenceCapacity;
    const ScriptStringList *strings;
    const XAsset *zoneAssets;
    int zoneAssetCount;
    const LinkerExternalAsset *external;
    size_t externalCount;
    bool (*visit)(int, const char *, void *);
    void *visitContext;
};

static const LinkerExternalAsset *ExternalAsset(const NativeWriter *writer, XAssetType type, const void *asset);
static bool WriteExternalAsset(NativeWriter *writer, XAssetType type, const void *asset);
static uintptr_t ReferenceToken(const NativeWriter *writer, XAssetType type, const void *asset);
static bool InsertReference(NativeWriter *writer, XAssetType type, const void *asset);

static uintptr_t InlineAssetToken(const NativeWriter *writer, XAssetType type, const void *asset)
{
    if (!asset)
    {
        return 0;
    }
    return ReferenceToken(writer, type, asset);
}

static const char *NativeAssetName(XAssetType type, const void *asset)
{
    if (!asset)
    {
        return NULL;
    }
    XAssetHeader header = {};
    header.data = (void *)asset;
    const char *name = NULL;
    switch (type)
    {
    case ASSET_TYPE_XMODELPIECES:
        name = header.xmodelPieces->name;
        break;
    case ASSET_TYPE_PHYSPRESET:
        name = header.physPreset->name;
        break;
    case ASSET_TYPE_XANIMPARTS:
        name = header.parts->name;
        break;
    case ASSET_TYPE_XMODEL:
        name = header.model->name;
        break;
    case ASSET_TYPE_MATERIAL:
        name = header.material->info.name;
        break;
    case ASSET_TYPE_TECHNIQUE_SET:
        name = header.techniqueSet->name;
        break;
    case ASSET_TYPE_IMAGE:
        name = header.image->name;
        break;
    case ASSET_TYPE_SOUND:
        name = header.sound->aliasName;
        break;
    case ASSET_TYPE_SOUND_CURVE:
        name = header.sndCurve->filename;
        break;
    case ASSET_TYPE_LOADED_SOUND:
        name = header.loadSnd->name;
        break;
    case ASSET_TYPE_CLIPMAP:
        name = header.clipMap->name;
        break;
    case ASSET_TYPE_CLIPMAP_PVS:
        name = header.clipMap->name;
        break;
    case ASSET_TYPE_COMWORLD:
        name = header.comWorld->name;
        break;
    case ASSET_TYPE_GAMEWORLD_SP:
        name = header.gameWorldSp->name;
        break;
    case ASSET_TYPE_GAMEWORLD_MP:
        name = header.gameWorldMp->name;
        break;
    case ASSET_TYPE_MAP_ENTS:
        name = header.mapEnts->name;
        break;
    case ASSET_TYPE_GFXWORLD:
        name = header.gfxWorld->name;
        break;
    case ASSET_TYPE_LIGHT_DEF:
        name = header.lightDef->name;
        break;
    case ASSET_TYPE_FONT:
        name = header.font->fontName;
        break;
    case ASSET_TYPE_MENULIST:
        name = header.menuList->name;
        break;
    case ASSET_TYPE_MENU:
        name = header.menu->window.name;
        break;
    case ASSET_TYPE_LOCALIZE_ENTRY:
        name = header.localize->name;
        break;
    case ASSET_TYPE_WEAPON:
        name = header.weapon->szInternalName;
        break;
    case ASSET_TYPE_SNDDRIVER_GLOBALS:
        name = header.sndDriverGlobals->name;
        break;
    case ASSET_TYPE_FX:
        name = header.fx->name;
        break;
    case ASSET_TYPE_IMPACT_FX:
        name = header.impactFx->name;
        break;
    case ASSET_TYPE_RAWFILE:
        name = header.rawfile->name;
        break;
    case ASSET_TYPE_STRINGTABLE:
        name = header.stringTable->name;
        break;
    default:
        break;
    }
    return name;
}

static bool VisitNativeAsset(NativeWriter *writer, XAssetType type, const void *asset)
{
    if (!writer->visit || ExternalAsset(writer, type, asset))
    {
        return true;
    }
    const char *name = NativeAssetName(type, asset);
    return !name || !name[0] || name[0] == ',' || writer->visit(type, name, writer->visitContext);
}

static bool Fail(NativeWriter *writer, const char *message)
{
    if (writer->errorSize)
    {
        snprintf(writer->error, writer->errorSize, "%s", message);
    }
    return false;
}

static bool Append(NativeWriter *writer, const void *data, size_t size)
{
    if (size > UINT32_MAX - writer->size)
    {
        return Fail(writer, "Native zone exceeds the current XFile size limit");
    }
    const size_t required = writer->size + size;
    if (required > writer->capacity)
    {
        const size_t capacity = required > writer->capacity * 2 ? required : writer->capacity * 2;
        uint8_t *next = (uint8_t *)realloc(writer->data, capacity);
        if (!next)
        {
            return Fail(writer, "Out of memory writing native zone");
        }
        writer->data = next;
        writer->capacity = capacity;
    }
    if (size)
    {
        memcpy(writer->data + writer->size, data, size);
    }
    writer->size = required;
    return true;
}

static bool Advance(NativeWriter *writer, size_t size)
{
    size_t *position = &writer->position[writer->block];
    if (size > INT_MAX - 4096 || *position > INT_MAX - 4096 - size)
    {
        return Fail(writer, "Native stream block exceeds the engine allocator limit");
    }
    *position += size;
    if (*position > writer->extent[writer->block])
    {
        writer->extent[writer->block] = *position;
    }
    return true;
}

static bool Align(NativeWriter *writer)
{
    return Advance(writer, (-writer->position[writer->block]) & (DB64_STREAM_ALIGNMENT - 1));
}

static bool Write(NativeWriter *writer, const void *data, size_t size)
{
    return Advance(writer, size) && Append(writer, data, size);
}

static bool String(NativeWriter *writer, const char *value)
{
    return !value || Write(writer, value, strlen(value) + 1);
}

static const char *StringToken(const char *value)
{
    return value ? (const char *)UINTPTR_MAX : NULL;
}

static bool Raw(NativeWriter *writer, const RawFile *input)
{
    if (!input || !input->name || input->len < 0 || input->len == INT_MAX || !input->buffer)
    {
        return Fail(writer, "Invalid rawfile input");
    }
    RawFile output = {};
    output.name = StringToken(input->name);
    output.len = input->len;
    output.buffer = (const char *)UINTPTR_MAX;
    writer->block = 0;
    const size_t temporaryStart = writer->position[0];
    if (!Align(writer) || !Write(writer, &output, sizeof(RawFile)))
    {
        return false;
    }
    writer->block = 4;
    const char terminator = 0;
    if (!String(writer, input->name) || !Write(writer, input->buffer, input->len) ||
        !Write(writer, &terminator, sizeof(char)))
    {
        return false;
    }
    // Block zero is temporary: asset registration copies the structure into
    // its typed pool. Its children stay in block four for the zone lifetime.
    writer->position[0] = temporaryStart;
    return true;
}

static bool Localize(NativeWriter *writer, const LocalizeEntry *input)
{
    if (!input || !input->name || !input->value)
    {
        return Fail(writer, "Invalid localization entry");
    }
    LocalizeEntry output = {};
    output.name = StringToken(input->name);
    output.value = StringToken(input->value);
    writer->block = 0;
    const size_t temporaryStart = writer->position[0];
    if (!Align(writer) || !Write(writer, &output, sizeof(LocalizeEntry)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->value) || !String(writer, input->name))
    {
        return false;
    }
    writer->position[0] = temporaryStart;
    return true;
}

static bool SoundDriverGlobals(NativeWriter *writer, const SndDriverGlobals *input)
{
    if (!input || !input->name || strcmp(input->name, "singleton"))
    {
        return Fail(writer, "Invalid sound driver globals");
    }
    SndDriverGlobals output = {StringToken(input->name)};
    writer->block = 0;
    const size_t temporaryStart = writer->position[0];
    if (!Align(writer) || !Write(writer, &output, sizeof(SndDriverGlobals)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name))
    {
        return false;
    }
    writer->position[0] = temporaryStart;
    return true;
}

static bool Table(NativeWriter *writer, const StringTable *input)
{
    if (!input || !input->name || input->rowCount < 0 || input->columnCount < 0 ||
        (input->rowCount && input->columnCount > INT_MAX / input->rowCount))
    {
        return Fail(writer, "Invalid stringtable dimensions");
    }
    const size_t count = (size_t)input->rowCount * input->columnCount;
    if (count > INT_MAX / sizeof(const char *) || (count && !input->values))
    {
        return Fail(writer, "Invalid stringtable values");
    }
    StringTable output = {};
    output.name = StringToken(input->name);
    output.rowCount = input->rowCount;
    output.columnCount = input->columnCount;
    output.values = count ? (const char **)UINTPTR_MAX : NULL;
    if (!Align(writer) || !Write(writer, &output, sizeof(StringTable)) || !String(writer, input->name))
    {
        return false;
    }
    if (count)
    {
        if (!Align(writer))
        {
            return false;
        }
        for (size_t i = 0; i < count; ++i)
        {
            const char *token = StringToken(input->values[i]);
            if (!Write(writer, &token, sizeof(const char *)))
            {
                return false;
            }
        }
        for (size_t i = 0; i < count; ++i)
        {
            if (!String(writer, input->values[i]))
            {
                return false;
            }
        }
    }
    return true;
}

static bool Entities(NativeWriter *writer, const MapEnts *input)
{
    if (ExternalAsset(writer, ASSET_TYPE_MAP_ENTS, input))
    {
        return WriteExternalAsset(writer, ASSET_TYPE_MAP_ENTS, input);
    }
    if (ReferenceToken(writer, ASSET_TYPE_MAP_ENTS, input) != UINTPTR_MAX - 1)
    {
        return true;
    }
    if (!input || !input->name || !input->entityString || input->numEntityChars <= 0 ||
        input->entityString[input->numEntityChars - 1])
    {
        return Fail(writer, "Invalid map entity string");
    }
    MapEnts output = {};
    output.name = StringToken(input->name);
    output.entityString = (char *)UINTPTR_MAX;
    output.numEntityChars = input->numEntityChars;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_MAP_ENTS, input) ||
        !Write(writer, &output, sizeof(MapEnts)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name) || !Write(writer, input->entityString, input->numEntityChars))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool CommonWorld(NativeWriter *writer, const ComWorld *input)
{
    if (!input || !input->name || input->primaryLightCount > 255 ||
        (!!input->primaryLights != (input->primaryLightCount != 0)))
    {
        return Fail(writer, "Invalid common world lights");
    }
    ComWorld output = {};
    output.name = StringToken(input->name);
    output.isInUse = input->isInUse;
    output.primaryLightCount = input->primaryLightCount;
    output.primaryLights = input->primaryLightCount ? (ComPrimaryLight *)UINTPTR_MAX : NULL;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !Write(writer, &output, sizeof(ComWorld)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name))
    {
        return false;
    }
    if (input->primaryLightCount)
    {
        if (!Align(writer))
        {
            return false;
        }
        for (uint i = 0; i < input->primaryLightCount; ++i)
        {
            ComPrimaryLight light = {};
            // All non-pointer fields occupy this prefix, without padding.
            static_assert(offsetof(ComPrimaryLight, defName) == 64);
            memcpy(&light, &input->primaryLights[i], offsetof(ComPrimaryLight, defName));
            light.unused = 0;
            light.defName = StringToken(input->primaryLights[i].defName);
            if (!Write(writer, &light, sizeof(ComPrimaryLight)))
            {
                return false;
            }
        }
        for (uint i = 0; i < input->primaryLightCount; ++i)
        {
            if (!String(writer, input->primaryLights[i].defName))
            {
                return false;
            }
        }
    }
    writer->position[0] = start;
    return true;
}

static bool MultiplayerWorld(NativeWriter *writer, const GameWorldMp *input)
{
    if (!input || !input->name)
    {
        return Fail(writer, "Invalid multiplayer world");
    }
    GameWorldMp output = {StringToken(input->name)};
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !Write(writer, &output, sizeof(GameWorldMp)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool Preset(NativeWriter *writer, XAssetType type, XAssetHeader header)
{
    if (ExternalAsset(writer, type, header.data))
    {
        return WriteExternalAsset(writer, type, header.data);
    }
    if (ReferenceToken(writer, type, header.data) != UINTPTR_MAX - 1)
    {
        return true;
    }
    const char *name;
    const char *prefix = NULL;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, type, header.data))
    {
        return false;
    }
    if (type == ASSET_TYPE_PHYSPRESET)
    {
        const PhysPreset *input = header.physPreset;
        if (!input || !input->name)
        {
            return Fail(writer, "Invalid physics preset input");
        }
        PhysPreset output = {};
        output.name = StringToken(input->name);
        output.type = input->type;
        output.mass = input->mass;
        output.bounce = input->bounce;
        output.friction = input->friction;
        output.bulletForceScale = input->bulletForceScale;
        output.explosiveForceScale = input->explosiveForceScale;
        output.sndAliasPrefix = StringToken(input->sndAliasPrefix);
        output.piecesSpreadFraction = input->piecesSpreadFraction;
        output.piecesUpwardVelocity = input->piecesUpwardVelocity;
        output.tempDefaultToCylinder = input->tempDefaultToCylinder;
        if (!Write(writer, &output, sizeof(PhysPreset)))
        {
            return false;
        }
        name = input->name;
        prefix = input->sndAliasPrefix;
    }
    else
    {
        const SndCurve *input = header.sndCurve;
        if (!input || !input->filename || input->knotCount < 2 || input->knotCount > ARRAY_COUNT(input->knots))
        {
            return Fail(writer, "Invalid sound curve input");
        }
        SndCurve output = {};
        output.filename = StringToken(input->filename);
        output.knotCount = input->knotCount;
        memcpy(output.knots, input->knots, input->knotCount * sizeof(float[2]));
        if (!Write(writer, &output, sizeof(SndCurve)))
        {
            return false;
        }
        name = input->filename;
    }
    writer->block = 4;
    if (!String(writer, name) || !String(writer, prefix))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static const LinkerExternalAsset *ExternalAsset(const NativeWriter *writer, XAssetType type, const void *asset)
{
    for (size_t i = 0; asset && i < writer->externalCount; ++i)
    {
        if (writer->external[i].type == type &&
            (writer->external[i].asset == asset || (!writer->external[i].asset &&
                NativeAssetName(type, asset) && !_stricmp(writer->external[i].name, NativeAssetName(type, asset)))))
        {
            return &writer->external[i];
        }
    }
    return NULL;
}

static bool WriteExternalAsset(NativeWriter *writer, XAssetType type, const void *asset)
{
    const LinkerExternalAsset *external = ExternalAsset(writer, type, asset);
    if (!external)
    {
        return true;
    }
    DB64ExternalAssetRecord record = {(int32_t)type, (uint32_t)strlen(external->name) + 1};
    writer->block = 4;
    return Write(writer, &record, sizeof(DB64ExternalAssetRecord)) && String(writer, external->name);
}

static uintptr_t ReferenceToken(const NativeWriter *writer, XAssetType type, const void *asset)
{
    if (ExternalAsset(writer, type, asset))
    {
        return DB64_EXTERNAL_ASSET_TOKEN;
    }
    for (size_t i = 0; i < writer->referenceCount; ++i)
    {
        if (writer->references[i].type != type)
        {
            continue;
        }
        if (writer->references[i].asset == asset)
        {
            return writer->references[i].token;
        }
        // Importers can own separate copies of a shared asset. The runtime
        // registry identifies assets by type/name, so serialize that definition once.
        const char *name = NativeAssetName(type, asset);
        const char *previousName = NativeAssetName(type, writer->references[i].asset);
        if (name && (name[0] || type == ASSET_TYPE_SOUND_CURVE) && previousName && !_stricmp(name, previousName))
        {
            return writer->references[i].token;
        }
    }
    return UINTPTR_MAX - 1;
}

static bool InsertReference(NativeWriter *writer, XAssetType type, const void *asset)
{
    if (!VisitNativeAsset(writer, type, asset))
    {
        return Fail(writer, "Cannot record native asset index");
    }
    if (writer->referenceCount == writer->referenceCapacity)
    {
        const size_t capacity = writer->referenceCapacity ? writer->referenceCapacity * 2 : 64;
        if (capacity > SIZE_MAX / sizeof(NativeReference))
        {
            return Fail(writer, "Too many native asset references");
        }
        NativeReference *next = (NativeReference *)realloc(writer->references, capacity * sizeof(NativeReference));
        if (!next)
        {
            return Fail(writer, "Out of memory recording native asset references");
        }
        writer->references = next;
        writer->referenceCapacity = capacity;
    }
    const uint previousBlock = writer->block;
    writer->block = 4;
    // Match DB_InsertPointer: reserve a pointer slot, with pointer alignment.
    if (!Advance(writer, (-writer->position[4]) & (alignof(void *) - 1)))
    {
        return false;
    }
    const uintptr_t token = (UINT64_C(4) << 60) + writer->position[4] + 1;
    if (!Advance(writer, sizeof(void *)))
    {
        return false;
    }
    NativeReference *reference = &writer->references[writer->referenceCount++];
    reference->type = type;
    reference->asset = asset;
    reference->token = token;
    writer->block = previousBlock;
    return true;
}

static bool ImageAsset(NativeWriter *writer, const GfxImage *input)
{
    if (ReferenceToken(writer, ASSET_TYPE_IMAGE, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_IMAGE, input);
    }
    if (!input || !DB64_ValidateImageLayout(input, input->texture.loadDef))
    {
        return Fail(writer,
                    "Invalid native image input; expected a compiler-owned load definition with complete pixels");
    }
    const GfxImageLoadDef *definition = input->texture.loadDef;
    GfxImage image = {};
    image.mapType = input->mapType;
    image.texture.loadDef = (GfxImageLoadDef *)UINTPTR_MAX;
    image.picmip = input->picmip;
    image.noPicmip = input->noPicmip;
    image.semantic = input->semantic;
    image.track = input->track;
    image.cardMemory = input->cardMemory;
    image.width = input->width;
    image.height = input->height;
    image.depth = input->depth;
    image.category = input->category;
    image.delayLoadPixels = false;
    image.name = StringToken(input->name);
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_IMAGE, input) || !Write(writer, &image, sizeof(GfxImage)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name))
    {
        return false;
    }
    writer->block = 0;
    // The texture loader nests another temporary scope below the image. Keep
    // the parent structure alive until its texture has been uploaded.
    if (!Align(writer) || !Write(writer, definition, offsetof(GfxImageLoadDef, data)) ||
        !Write(writer, definition->data, definition->resourceSize))
    {
        return false;
    }
    writer->position[0] = start;
    writer->block = 4;
    return true;
}

static bool LightDefinition(NativeWriter *writer, const GfxLightDef *input)
{
    if (!input || !input->name || !input->attenuation.image || input->lmapLookupStart < 1 ||
        input->lmapLookupStart >= 512 || input->attenuation.image->width + input->lmapLookupStart >= 512)
    {
        return Fail(writer, "Invalid native light definition or falloff allocation");
    }
    GfxLightDef output = {};
    output.name = StringToken(input->name);
    output.attenuation.image = (GfxImage *)ReferenceToken(writer, ASSET_TYPE_IMAGE, input->attenuation.image);
    output.attenuation.samplerState = input->attenuation.samplerState;
    output.lmapLookupStart = input->lmapLookupStart;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !Write(writer, &output, sizeof(GfxLightDef)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name) || !ImageAsset(writer, input->attenuation.image))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool Shader(NativeWriter *writer, const void *input, bool pixel)
{
    if (!input)
    {
        return true;
    }
    const char *name;
    const void *program;
    uint16_t words;
    if (!Align(writer))
    {
        return false;
    }
    if (pixel)
    {
        const MaterialPixelShader *source = (const MaterialPixelShader *)input;
        MaterialPixelShader output = {};
        output.name = StringToken(source->name);
        output.prog.loadDef.program = (void *)UINTPTR_MAX;
        output.prog.loadDef.programSize = source->prog.loadDef.programSize;
        output.prog.loadDef.loadForRenderer = source->prog.loadDef.loadForRenderer;
        name = source->name;
        program = source->prog.loadDef.program;
        words = source->prog.loadDef.programSize;
        if (!Write(writer, &output, sizeof(MaterialPixelShader)))
        {
            return false;
        }
    }
    else
    {
        const MaterialVertexShader *source = (const MaterialVertexShader *)input;
        MaterialVertexShader output = {};
        output.name = StringToken(source->name);
        output.prog.loadDef.program = (void *)UINTPTR_MAX;
        output.prog.loadDef.programSize = source->prog.loadDef.programSize;
        output.prog.loadDef.loadForRenderer = source->prog.loadDef.loadForRenderer;
        name = source->name;
        program = source->prog.loadDef.program;
        words = source->prog.loadDef.programSize;
        if (!Write(writer, &output, sizeof(MaterialVertexShader)))
        {
            return false;
        }
    }
    if (!name || !DB64_ValidateShaderProgram(program, words, pixel))
    {
        return Fail(writer, "Invalid native shader source");
    }
    return String(writer, name) && Align(writer) && Write(writer, program, words * sizeof(uint32_t));
}

static bool PassChildren(NativeWriter *writer, const MaterialPass *pass)
{
    if (pass->vertexDecl)
    {
        const MaterialVertexDeclaration *source = pass->vertexDecl;
        if (source->streamCount > ARRAY_COUNT(source->routing.data))
        {
            return Fail(writer, "Invalid native vertex stream count");
        }
        MaterialVertexDeclaration output = {};
        for (uint i = 0; i < source->streamCount; ++i)
        {
            if (source->routing.data[i].source >= STREAM_SRC_COUNT || source->routing.data[i].dest >= 12)
            {
                return Fail(writer, "Invalid native vertex stream routing");
            }
        }
        output.streamCount = source->streamCount;
        output.hasOptionalSource = source->hasOptionalSource;
        memcpy(output.routing.data, source->routing.data, source->streamCount * sizeof(MaterialStreamRouting));
        if (!Align(writer) || !Write(writer, &output, sizeof(MaterialVertexDeclaration)))
        {
            return false;
        }
    }
    if (!Shader(writer, pass->vertexShader, false) || !Shader(writer, pass->pixelShader, true))
    {
        return false;
    }
    const int count = pass->perPrimArgCount + pass->perObjArgCount + pass->stableArgCount;
    if (!!pass->args != (count != 0))
    {
        return Fail(writer, "Invalid native shader argument count");
    }
    if (count && !Align(writer))
    {
        return false;
    }
    for (int i = 0; i < count; ++i)
    {
        const MaterialShaderArgument *source = &pass->args[i];
        MaterialShaderArgument output = {};
        output.type = source->type;
        output.dest = source->dest;
        if (!DB64_ValidShaderArgument(source))
        {
            return Fail(writer, "Invalid native shader argument type");
        }
        if (source->type == MTL_ARG_LITERAL_VERTEX_CONST || source->type == MTL_ARG_LITERAL_PIXEL_CONST)
        {
            if (!source->u.literalConst)
            {
                return Fail(writer, "Missing native literal constant");
            }
            output.u.literalConst = (const float *)UINTPTR_MAX;
        }
        else if (source->type == MTL_ARG_CODE_VERTEX_CONST || source->type == MTL_ARG_CODE_PIXEL_CONST)
        {
            output.u.codeConst = source->u.codeConst;
        }
        else
        {
            output.u.nameHash = source->u.nameHash;
        }
        if (!Write(writer, &output, sizeof(MaterialShaderArgument)))
        {
            return false;
        }
    }
    for (int i = 0; i < count; ++i)
    {
        if (pass->args[i].type == MTL_ARG_LITERAL_VERTEX_CONST || pass->args[i].type == MTL_ARG_LITERAL_PIXEL_CONST)
        {
            if (!Align(writer) || !Write(writer, pass->args[i].u.literalConst, sizeof(float[4])))
            {
                return false;
            }
        }
    }
    return true;
}

static bool TechniqueSet(NativeWriter *writer, const MaterialTechniqueSet *input)
{
    if (ReferenceToken(writer, ASSET_TYPE_TECHNIQUE_SET, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_TECHNIQUE_SET, input);
    }
    if (!input || !input->name)
    {
        return Fail(writer, "Invalid native technique set");
    }
    MaterialTechniqueSet output = {};
    output.name = StringToken(input->name);
    output.worldVertFormat = input->worldVertFormat;
    for (uint i = 0; i < ARRAY_COUNT(output.techniques); ++i)
    {
        output.techniques[i] = input->techniques[i] ? (MaterialTechnique *)UINTPTR_MAX : NULL;
    }
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_TECHNIQUE_SET, input) ||
        !Write(writer, &output, sizeof(MaterialTechniqueSet)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name))
    {
        return false;
    }
    for (uint i = 0; i < ARRAY_COUNT(input->techniques); ++i)
    {
        const MaterialTechnique *technique = input->techniques[i];
        if (!technique)
        {
            continue;
        }
        if (!technique->name || !technique->passCount)
        {
            return Fail(writer, "Invalid native technique");
        }
        MaterialTechnique prefix = {};
        prefix.name = StringToken(technique->name);
        prefix.flags = technique->flags;
        prefix.passCount = technique->passCount;
        if (!Align(writer) || !Write(writer, &prefix, offsetof(MaterialTechnique, passArray)))
        {
            return false;
        }
        for (uint passIndex = 0; passIndex < technique->passCount; ++passIndex)
        {
            const MaterialPass *source = &technique->passArray[passIndex];
            MaterialPass pass = {};
            pass.vertexDecl = source->vertexDecl ? (MaterialVertexDeclaration *)UINTPTR_MAX : NULL;
            pass.vertexShader = source->vertexShader ? (MaterialVertexShader *)UINTPTR_MAX : NULL;
            pass.pixelShader = source->pixelShader ? (MaterialPixelShader *)UINTPTR_MAX : NULL;
            pass.perPrimArgCount = source->perPrimArgCount;
            pass.perObjArgCount = source->perObjArgCount;
            pass.stableArgCount = source->stableArgCount;
            pass.customSamplerFlags = source->customSamplerFlags;
            pass.args = source->args ? (MaterialShaderArgument *)UINTPTR_MAX : NULL;
            if (!Write(writer, &pass, sizeof(MaterialPass)))
            {
                return false;
            }
        }
        for (uint passIndex = 0; passIndex < technique->passCount; ++passIndex)
        {
            if (!PassChildren(writer, &technique->passArray[passIndex]))
            {
                return false;
            }
        }
        if (!String(writer, technique->name))
        {
            return false;
        }
    }
    writer->position[0] = start;
    return true;
}

static bool Water(NativeWriter *writer, const water_t *input)
{
    if (!input)
    {
        return true;
    }
    if (input->M <= 0 || input->N <= 0 || (input->M & (input->M - 1)) || (input->N & (input->N - 1)) ||
        (size_t)input->M > INT_MAX / sizeof(complex_s) / (size_t)input->N || !input->H0 || !input->wTerm ||
        !input->image)
    {
        return Fail(writer, "Invalid native water grid");
    }
    water_t output = {};
    output.H0 = (complex_s *)UINTPTR_MAX;
    output.wTerm = (float *)UINTPTR_MAX;
    output.M = input->M;
    output.N = input->N;
    output.Lx = input->Lx;
    output.Lz = input->Lz;
    output.gravity = input->gravity;
    output.windvel = input->windvel;
    memcpy(output.winddir, input->winddir, sizeof(float[2]));
    output.amplitude = input->amplitude;
    memcpy(output.codeConstant, input->codeConstant, sizeof(float[4]));
    output.image = (GfxImage *)ReferenceToken(writer, ASSET_TYPE_IMAGE, input->image);
    const size_t count = (size_t)input->M * input->N;
    return Align(writer) && Write(writer, &output, sizeof(water_t)) && Align(writer) &&
           Write(writer, input->H0, count * sizeof(complex_s)) && Align(writer) &&
           Write(writer, input->wTerm, count * sizeof(float)) && ImageAsset(writer, input->image);
}

static bool MaterialAsset(NativeWriter *writer, const Material *input)
{
    if (input && ReferenceToken(writer, ASSET_TYPE_MATERIAL, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_MATERIAL, input);
    }
    if (input && input->info.name && input->info.name[0] == ',')
    {
        if (!input->info.name[1] || input->techniqueSet || input->textureTable || input->constantTable ||
            input->stateBitsTable || input->textureCount || input->constantCount || input->stateBitsCount)
        {
            return Fail(writer, "Invalid native material reference");
        }
        Material reference = {};
        reference.info.name = StringToken(input->info.name);
        writer->block = 0;
        const size_t start = writer->position[0];
        if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_MATERIAL, input) ||
            !Write(writer, &reference, sizeof(Material)))
        {
            return false;
        }
        writer->block = 4;
        if (!String(writer, input->info.name))
        {
            return false;
        }
        writer->position[0] = start;
        return true;
    }
    if (!input || !input->info.name || !input->techniqueSet || (!!input->textureTable != (input->textureCount != 0)) ||
        (!!input->constantTable != (input->constantCount != 0)) ||
        (!!input->stateBitsTable != (input->stateBitsCount != 0)))
    {
        return Fail(writer, "Invalid native material input");
    }
    Material output = {};
    output.info.name = StringToken(input->info.name);
    output.info.gameFlags = input->info.gameFlags;
    output.info.sortKey = input->info.sortKey;
    output.info.textureAtlasRowCount = input->info.textureAtlasRowCount;
    output.info.textureAtlasColumnCount = input->info.textureAtlasColumnCount;
    output.info.drawSurf = input->info.drawSurf;
    output.info.drawSurf.fields.materialSortedIndex = 0;
    output.info.surfaceTypeBits = input->info.surfaceTypeBits;
    memcpy(output.stateBitsEntry, input->stateBitsEntry, sizeof(output.stateBitsEntry));
    output.textureCount = input->textureCount;
    output.constantCount = input->constantCount;
    output.stateBitsCount = input->stateBitsCount;
    output.stateFlags = input->stateFlags;
    output.cameraRegion = input->cameraRegion;
    output.techniqueSet = (MaterialTechniqueSet *)ReferenceToken(writer, ASSET_TYPE_TECHNIQUE_SET, input->techniqueSet);
    output.textureTable = input->textureCount ? (MaterialTextureDef *)UINTPTR_MAX : NULL;
    output.constantTable = input->constantCount ? (MaterialConstantDef *)UINTPTR_MAX : NULL;
    output.stateBitsTable = input->stateBitsCount ? (GfxStateBits *)UINTPTR_MAX : NULL;
    for (uint i = 0; i < ARRAY_COUNT(input->stateBitsEntry); ++i)
    {
        if (input->stateBitsEntry[i] != 255 && input->stateBitsEntry[i] >= input->stateBitsCount)
        {
            return Fail(writer, "Invalid native material state entry");
        }
    }
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_MATERIAL, input) ||
        !Write(writer, &output, sizeof(Material)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->info.name) || !TechniqueSet(writer, input->techniqueSet))
    {
        return false;
    }
    if (input->textureCount && !Align(writer))
    {
        return false;
    }
    // Headers precede child payloads. Patch each image token when visiting its
    // payload, after earlier textures have had a chance to introduce that image.
    const size_t textureHeaders = writer->size;
    for (uint i = 0; i < input->textureCount; ++i)
    {
        const MaterialTextureDef *source = &input->textureTable[i];
        MaterialTextureDef texture = {};
        texture.nameHash = source->nameHash;
        texture.nameStart = source->nameStart;
        texture.nameEnd = source->nameEnd;
        texture.samplerState = source->samplerState;
        texture.semantic = source->semantic;
        if (source->semantic == TS_WATER_MAP)
        {
            texture.u.water = source->u.water ? (water_t *)UINTPTR_MAX : NULL;
        }
        else
        {
            texture.u.image = source->u.image ? (GfxImage *)(UINTPTR_MAX - 1) : NULL;
        }
        if (!Write(writer, &texture, sizeof(MaterialTextureDef)))
        {
            return false;
        }
    }
    for (uint i = 0; i < input->textureCount; ++i)
    {
        const MaterialTextureDef *texture = &input->textureTable[i];
        if (texture->semantic == TS_WATER_MAP)
        {
            if (!Water(writer, texture->u.water))
            {
                return false;
            }
        }
        else if (texture->u.image)
        {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_IMAGE, texture->u.image);
            memcpy(writer->data + textureHeaders + i * sizeof(MaterialTextureDef) + offsetof(MaterialTextureDef, u),
                   &token, sizeof(uintptr_t));
            if (!ImageAsset(writer, texture->u.image))
            {
                return false;
            }
        }
    }
    if (input->constantCount &&
        (!Align(writer) || !Write(writer, input->constantTable, input->constantCount * sizeof(MaterialConstantDef))))
    {
        return false;
    }
    if (input->stateBitsCount &&
        (!Align(writer) || !Write(writer, input->stateBitsTable, input->stateBitsCount * sizeof(GfxStateBits))))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool Array(NativeWriter *writer, const void *data, size_t bytes, unsigned int alignment)
{
    return !bytes || (Advance(writer, (-writer->position[writer->block]) & alignment) && Write(writer, data, bytes));
}

static bool SurfaceChildren(NativeWriter *writer, const XSurface *surface)
{
    unsigned int blends = 0;
    for (int i = 0; i < 4; ++i)
    {
        blends += (2 * i + 1) * surface->vertInfo.vertCount[i];
    }
    if (!Array(writer, surface->vertInfo.vertsBlend, blends * sizeof(uint16_t), 1))
    {
        return false;
    }
    writer->block = 7;
    if (!Array(writer, surface->verts0, surface->vertCount * sizeof(GfxPackedVertex), 15))
    {
        return false;
    }
    writer->block = 4;
    if (surface->vertListCount && !Align(writer))
    {
        return false;
    }
    for (unsigned int i = 0; i < surface->vertListCount; ++i)
    {
        XRigidVertList list = {};
        list.boneOffset = surface->vertList[i].boneOffset;
        list.vertCount = surface->vertList[i].vertCount;
        list.triOffset = surface->vertList[i].triOffset;
        list.triCount = surface->vertList[i].triCount;
        list.collisionTree = surface->vertList[i].collisionTree ? (XSurfaceCollisionTree *)UINTPTR_MAX : NULL;
        if (!Write(writer, &list, sizeof(XRigidVertList)))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < surface->vertListCount; ++i)
    {
        const XSurfaceCollisionTree *tree = surface->vertList[i].collisionTree;
        if (!tree)
        {
            continue;
        }
        XSurfaceCollisionTree output = {};
        memcpy(output.trans, tree->trans, sizeof(output.trans));
        memcpy(output.scale, tree->scale, sizeof(output.scale));
        output.nodeCount = tree->nodeCount;
        output.leafCount = tree->leafCount;
        output.nodes = tree->nodeCount ? (XSurfaceCollisionNode *)UINTPTR_MAX : NULL;
        output.leafs = tree->leafCount ? (XSurfaceCollisionLeaf *)UINTPTR_MAX : NULL;
        if (!Array(writer, &output, sizeof(XSurfaceCollisionTree), 15) ||
            !Array(writer, tree->nodes, tree->nodeCount * sizeof(XSurfaceCollisionNode), 15) ||
            !Array(writer, tree->leafs, tree->leafCount * sizeof(XSurfaceCollisionLeaf), 1))
        {
            return false;
        }
    }
    writer->block = 8;
    if (!Array(writer, surface->triIndices, 3 * surface->triCount * sizeof(uint16_t), 15))
    {
        return false;
    }
    writer->block = 4;
    return true;
}

static bool PhysicsPlane(NativeWriter *writer, const cplane_s *plane)
{
    cplane_s output = {};
    memcpy(output.normal, plane->normal, sizeof(output.normal));
    output.dist = plane->dist;
    output.type = plane->normal[0] == 1 ? 0 : (plane->normal[1] == 1 ? 1 : (plane->normal[2] == 1 ? 2 : 3));
    for (int i = 0; i < 3; ++i)
    {
        if (plane->normal[i] < 0)
        {
            output.signbits |= 1 << i;
        }
    }
    return Write(writer, &output, sizeof(cplane_s));
}

static bool PhysicsGeometry(NativeWriter *writer, const PhysGeomList *input)
{
    if (!input)
    {
        return true;
    }
    if (!DB64_ValidatePhysicsGeometry(input, writer->error, writer->errorSize))
    {
        return false;
    }
    PhysGeomList output = {};
    output.count = input->count;
    output.geoms = input->count ? (PhysGeomInfo *)UINTPTR_MAX : NULL;
    output.mass = input->mass;
    if (!Array(writer, &output, sizeof(PhysGeomList), 15) || (input->count && !Align(writer)))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->count; ++i)
    {
        PhysGeomInfo geom = {};
        geom.brush = input->geoms[i].brush ? (BrushWrapper *)UINTPTR_MAX : NULL;
        geom.type = input->geoms[i].type;
        memcpy(geom.orientation, input->geoms[i].orientation, sizeof(geom.orientation));
        memcpy(geom.offset, input->geoms[i].offset, sizeof(geom.offset));
        memcpy(geom.halfLengths, input->geoms[i].halfLengths, sizeof(geom.halfLengths));
        if (!Write(writer, &geom, sizeof(PhysGeomInfo)))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < input->count; ++i)
    {
        const BrushWrapper *brush = input->geoms[i].brush;
        if (!brush)
        {
            continue;
        }
        BrushWrapper encoded = {};
        memcpy(encoded.mins, brush->mins, sizeof(encoded.mins));
        memcpy(encoded.maxs, brush->maxs, sizeof(encoded.maxs));
        encoded.contents = brush->contents;
        encoded.numsides = brush->numsides;
        encoded.sides = brush->numsides ? (cbrushside_t *)UINTPTR_MAX : NULL;
        encoded.planes = brush->numsides ? (cplane_s *)UINTPTR_MAX : NULL;
        memcpy(encoded.axialMaterialNum, brush->axialMaterialNum, sizeof(encoded.axialMaterialNum));
        memcpy(encoded.firstAdjacentSideOffsets, brush->firstAdjacentSideOffsets,
               sizeof(encoded.firstAdjacentSideOffsets));
        memcpy(encoded.edgeCount, brush->edgeCount, sizeof(encoded.edgeCount));
        encoded.totalEdgeCount = brush->totalEdgeCount;
        encoded.baseAdjacentSide = brush->totalEdgeCount ? (uint8_t *)UINTPTR_MAX : NULL;
        if (!Array(writer, &encoded, sizeof(BrushWrapper), 15) || (brush->numsides && !Align(writer)))
        {
            return false;
        }
        const uintptr_t planes = (UINT64_C(4) << 60) + writer->position[4] + 1;
        for (unsigned int planeIndex = 0; planeIndex < brush->numsides; ++planeIndex)
        {
            if (!PhysicsPlane(writer, &brush->planes[planeIndex]))
            {
                return false;
            }
        }
        if (brush->numsides && !Align(writer))
        {
            return false;
        }
        for (unsigned int sideIndex = 0; sideIndex < brush->numsides; ++sideIndex)
        {
            const cbrushside_t *side = &brush->sides[sideIndex];
            cbrushside_t sideOutput = {};
            sideOutput.plane = (cplane_s *)UINTPTR_MAX;
            for (unsigned int planeIndex = 0; planeIndex < brush->numsides; ++planeIndex)
            {
                if (side->plane == &brush->planes[planeIndex])
                {
                    sideOutput.plane = (cplane_s *)(planes + planeIndex * sizeof(cplane_s));
                    break;
                }
            }
            sideOutput.materialNum = side->materialNum;
            sideOutput.firstAdjacentSideOffset = side->firstAdjacentSideOffset;
            sideOutput.edgeCount = side->edgeCount;
            if (!Write(writer, &sideOutput, sizeof(cbrushside_t)))
            {
                return false;
            }
        }
        for (unsigned int sideIndex = 0; sideIndex < brush->numsides; ++sideIndex)
        {
            bool shared = false;
            for (unsigned int planeIndex = 0; planeIndex < brush->numsides; ++planeIndex)
            {
                if (brush->sides[sideIndex].plane == &brush->planes[planeIndex])
                {
                    shared = true;
                    break;
                }
            }
            if (!shared && (!Align(writer) || !PhysicsPlane(writer, brush->sides[sideIndex].plane)))
            {
                return false;
            }
        }
        if (!Array(writer, brush->baseAdjacentSide, brush->totalEdgeCount * sizeof(uint8_t), 0))
        {
            return false;
        }
    }
    return true;
}

static bool ModelAsset(NativeWriter *writer, const XModel *input)
{
    if (!DB64_ValidateModelHeader(input, writer->error, writer->errorSize))
    {
        return false;
    }
    if (ReferenceToken(writer, ASSET_TYPE_XMODEL, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_XMODEL, input);
    }
    const unsigned int children = input->numBones - input->numRootBones;
    for (unsigned int i = 0; i < input->numBones; ++i)
    {
        if (!writer->strings || input->boneNames[i] >= writer->strings->count)
        {
            return Fail(writer, "Native model bone name is not a zone script-string index");
        }
    }
    for (unsigned int i = 0; i < children; ++i)
    {
        if (!input->parentList[i] || input->parentList[i] > i + input->numRootBones)
        {
            return Fail(writer, "Native model has an invalid parent bone");
        }
    }
    for (unsigned int i = 0; i < input->numsurfs; ++i)
    {
        if (!DB64_ValidateModelSurface(&input->surfs[i], input->numBones, writer->error, writer->errorSize))
        {
            return false;
        }
    }
    XModel output = {};
    output.name = StringToken(input->name);
    output.numBones = input->numBones;
    output.numRootBones = input->numRootBones;
    output.numsurfs = input->numsurfs;
    output.lodRampType = input->lodRampType;
    output.boneNames = input->boneNames ? (uint16_t *)UINTPTR_MAX : NULL;
    output.parentList = input->parentList ? (uint8_t *)UINTPTR_MAX : NULL;
    output.quats = input->quats ? (int16_t *)UINTPTR_MAX : NULL;
    output.trans = input->trans ? (float *)UINTPTR_MAX : NULL;
    output.partClassification = input->partClassification ? (uint8_t *)UINTPTR_MAX : NULL;
    output.baseMat = input->baseMat ? (DObjAnimMat *)UINTPTR_MAX : NULL;
    output.surfs = input->surfs ? (XSurface *)UINTPTR_MAX : NULL;
    output.materialHandles = input->materialHandles ? (Material **)UINTPTR_MAX : NULL;
    memcpy(output.lodInfo, input->lodInfo, sizeof(output.lodInfo));
    for (int i = 0; i < 4; ++i)
    {
        output.lodInfo[i].smcIndexPlusOne = 0;
        output.lodInfo[i].smcAllocBits = 0;
        output.lodInfo[i].unused = 0;
    }
    output.collSurfs = input->collSurfs ? (XModelCollSurf_s *)UINTPTR_MAX : NULL;
    output.numCollSurfs = input->numCollSurfs;
    output.contents = input->contents;
    output.boneInfo = input->boneInfo ? (XBoneInfo *)UINTPTR_MAX : NULL;
    output.radius = input->radius;
    memcpy(output.mins, input->mins, sizeof(output.mins));
    memcpy(output.maxs, input->maxs, sizeof(output.maxs));
    output.numLods = input->numLods;
    output.collLod = input->collLod;
    output.flags = input->flags;
    output.physPreset = (PhysPreset *)InlineAssetToken(writer, ASSET_TYPE_PHYSPRESET, input->physPreset);
    output.physGeoms = input->physGeoms ? (PhysGeomList *)UINTPTR_MAX : NULL;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_XMODEL, input) || !Write(writer, &output, sizeof(XModel)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name) || !Array(writer, input->boneNames, input->numBones * sizeof(uint16_t), 1) ||
        !Array(writer, input->parentList, children * sizeof(uint8_t), 0) ||
        !Array(writer, input->quats, 4 * children * sizeof(int16_t), 1) ||
        !Array(writer, input->trans, 3 * children * sizeof(float), 15) ||
        !Array(writer, input->partClassification, input->numBones * sizeof(uint8_t), 0) ||
        !Array(writer, input->baseMat, input->numBones * sizeof(DObjAnimMat), 15))
    {
        return false;
    }
    if (input->numsurfs && !Align(writer))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->numsurfs; ++i)
    {
        const XSurface *surface = &input->surfs[i];
        XSurface encoded = {};
        encoded.tileMode = surface->tileMode;
        encoded.deformed = surface->deformed;
        encoded.vertCount = surface->vertCount;
        encoded.triCount = surface->triCount;
        encoded.baseTriIndex = surface->baseTriIndex;
        encoded.baseVertIndex = surface->baseVertIndex;
        encoded.triIndices = surface->triIndices ? (uint16_t *)UINTPTR_MAX : NULL;
        memcpy(encoded.vertInfo.vertCount, surface->vertInfo.vertCount, sizeof(encoded.vertInfo.vertCount));
        encoded.vertInfo.vertsBlend = surface->vertInfo.vertsBlend ? (uint16_t *)UINTPTR_MAX : NULL;
        encoded.verts0 = surface->verts0 ? (GfxPackedVertex *)UINTPTR_MAX : NULL;
        encoded.vertListCount = surface->vertListCount;
        encoded.vertList = surface->vertList ? (XRigidVertList *)UINTPTR_MAX : NULL;
        memcpy(encoded.partBits, surface->partBits, sizeof(encoded.partBits));
        if (!Write(writer, &encoded, sizeof(XSurface)))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < input->numsurfs; ++i)
    {
        if (!SurfaceChildren(writer, &input->surfs[i]))
        {
            return false;
        }
    }
    if (input->numsurfs && !Align(writer))
    {
        return false;
    }
    const size_t materialHeaders = writer->size;
    for (unsigned int i = 0; i < input->numsurfs; ++i)
    {
        const uintptr_t token = 0;
        if (!Write(writer, &token, sizeof(Material *)))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < input->numsurfs; ++i)
    {
        const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_MATERIAL, input->materialHandles[i]);
        memcpy(writer->data + materialHeaders + i * sizeof(Material *), &token, sizeof(uintptr_t));
        if (!MaterialAsset(writer, input->materialHandles[i]))
        {
            return false;
        }
    }
    if (input->numCollSurfs && !Align(writer))
    {
        return false;
    }
    for (int i = 0; i < input->numCollSurfs; ++i)
    {
        const XModelCollSurf_s *surface = &input->collSurfs[i];
        if (surface->numCollTris < 0 || surface->numCollTris > INT_MAX / sizeof(XModelCollTri_s) ||
            (!!surface->collTris != (surface->numCollTris != 0)) || surface->boneIdx < (surface->contents ? 0 : -1) ||
            surface->boneIdx >= input->numBones)
        {
            return Fail(writer, "Invalid native model collision surface");
        }
        XModelCollSurf_s encoded = {};
        encoded.collTris = surface->collTris ? (XModelCollTri_s *)UINTPTR_MAX : NULL;
        encoded.numCollTris = surface->numCollTris;
        memcpy(encoded.mins, surface->mins, sizeof(encoded.mins));
        memcpy(encoded.maxs, surface->maxs, sizeof(encoded.maxs));
        encoded.boneIdx = surface->boneIdx;
        encoded.contents = surface->contents;
        encoded.surfFlags = surface->surfFlags;
        if (!Write(writer, &encoded, sizeof(XModelCollSurf_s)))
        {
            return false;
        }
    }
    for (int i = 0; i < input->numCollSurfs; ++i)
    {
        if (!Array(writer, input->collSurfs[i].collTris, input->collSurfs[i].numCollTris * sizeof(XModelCollTri_s), 15))
        {
            return false;
        }
    }
    if (!Array(writer, input->boneInfo, input->numBones * sizeof(XBoneInfo), 15))
    {
        return false;
    }
    if (input->physPreset)
    {
        XAssetHeader header = {};
        header.physPreset = input->physPreset;
        if (!Preset(writer, ASSET_TYPE_PHYSPRESET, header))
        {
            return false;
        }
    }
    if (!PhysicsGeometry(writer, input->physGeoms))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool AnimationIndices(NativeWriter *writer, const XAnimDynamicIndices *indices, unsigned int size,
                             unsigned int frames)
{
    unsigned int previous = 0;
    for (unsigned int i = 0; i <= size; ++i)
    {
        const unsigned int index = frames < 256 ? indices->_1[i] : indices->_2[i];
        if (index > frames || (i && index <= previous))
        {
            return Fail(writer, "Invalid native animation delta frame index");
        }
        previous = index;
    }
    return Write(writer, indices, (size + 1) * (frames < 256 ? sizeof(uint8_t) : sizeof(uint16_t)));
}

static bool AnimationDelta(NativeWriter *writer, const XAnimDeltaPart *input, unsigned int frames)
{
    if (!input)
    {
        return true;
    }
    XAnimDeltaPart output = {};
    output.trans = input->trans ? (XAnimPartTrans *)UINTPTR_MAX : NULL;
    output.quat = input->quat ? (XAnimDeltaPartQuat *)UINTPTR_MAX : NULL;
    if (!Array(writer, &output, sizeof(XAnimDeltaPart), 15))
    {
        return false;
    }
    if (input->trans)
    {
        const XAnimPartTrans *trans = input->trans;
        if (trans->size > frames || trans->smallTrans > 1)
        {
            return Fail(writer, "Invalid native animation delta translation count");
        }
        XAnimPartTrans encoded = {};
        encoded.size = trans->size;
        encoded.smallTrans = trans->smallTrans;
        if (!Array(writer, &encoded, offsetof(XAnimPartTrans, u), 15))
        {
            return false;
        }
        if (trans->size)
        {
            if (!trans->u.frames.frames._1)
            {
                return Fail(writer, "Missing native animation translation frames");
            }
            XAnimPartTransFrames data = {};
            memcpy(data.mins, trans->u.frames.mins, sizeof(data.mins));
            memcpy(data.size, trans->u.frames.size, sizeof(data.size));
            data.frames._1 = (uint8_t(*)[3])UINTPTR_MAX;
            if (!Write(writer, &data, offsetof(XAnimPartTransFrames, indices)) ||
                !AnimationIndices(writer, &trans->u.frames.indices, trans->size, frames) ||
                !Array(writer, trans->u.frames.frames._1,
                       (trans->size + 1) * 3 * (trans->smallTrans ? sizeof(uint8_t) : sizeof(uint16_t)),
                       trans->smallTrans ? 0 : 15))
            {
                return false;
            }
        }
        else if (!Write(writer, trans->u.frame0, sizeof(float[3])))
        {
            return false;
        }
    }
    if (input->quat)
    {
        const XAnimDeltaPartQuat *quat = input->quat;
        if (quat->size > frames)
        {
            return Fail(writer, "Invalid native animation delta rotation count");
        }
        XAnimDeltaPartQuat encoded = {};
        encoded.size = quat->size;
        if (!Array(writer, &encoded, offsetof(XAnimDeltaPartQuat, u), 15))
        {
            return false;
        }
        if (quat->size)
        {
            if (!quat->u.frames.frames)
            {
                return Fail(writer, "Missing native animation rotation frames");
            }
            const uintptr_t token = UINTPTR_MAX;
            if (!Write(writer, &token, sizeof(void *)) ||
                !AnimationIndices(writer, &quat->u.frames.indices, quat->size, frames) ||
                !Array(writer, quat->u.frames.frames, (quat->size + 1) * sizeof(int16_t[2]), 15))
            {
                return false;
            }
        }
        else if (!Write(writer, quat->u.frame0, sizeof(int16_t[2])))
        {
            return false;
        }
    }
    return true;
}

static bool AnimationAsset(NativeWriter *writer, const XAnimParts *input)
{
    if (!DB64_ValidateAnimationHeader(input, writer->error, writer->errorSize))
    {
        return false;
    }
    if (ReferenceToken(writer, ASSET_TYPE_XANIMPARTS, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_XANIMPARTS, input);
    }
    for (unsigned int i = 0; i < input->boneCount[9]; ++i)
    {
        if (!writer->strings || input->names[i] >= writer->strings->count)
        {
            return Fail(writer, "Native animation bone name is not a zone script-string index");
        }
    }
    for (unsigned int i = 0; i < input->notifyCount; ++i)
    {
        if (!writer->strings || input->notify[i].name >= writer->strings->count || !isfinite(input->notify[i].time) ||
            input->notify[i].time < 0 || input->notify[i].time > 1)
        {
            return Fail(writer, "Invalid native animation notify");
        }
    }
    XAnimParts output = {};
    output.name = StringToken(input->name);
    output.dataByteCount = input->dataByteCount;
    output.dataShortCount = input->dataShortCount;
    output.dataIntCount = input->dataIntCount;
    output.randomDataByteCount = input->randomDataByteCount;
    output.randomDataShortCount = input->randomDataShortCount;
    output.randomDataIntCount = input->randomDataIntCount;
    output.numframes = input->numframes;
    output.isDefault = input->isDefault;
    output.bLoop = input->bLoop;
    output.bDelta = input->bDelta;
    memcpy(output.boneCount, input->boneCount, sizeof(output.boneCount));
    output.notifyCount = input->notifyCount;
    output.assetType = input->assetType;
    output.indexCount = input->indexCount;
    output.framerate = input->framerate;
    output.frequency = input->frequency;
    output.names = input->names ? (uint16_t *)UINTPTR_MAX : NULL;
    output.notify = input->notify ? (XAnimNotifyInfo *)UINTPTR_MAX : NULL;
    output.deltaPart = input->deltaPart ? (XAnimDeltaPart *)UINTPTR_MAX : NULL;
    output.dataByte = input->dataByte ? (uint8_t *)UINTPTR_MAX : NULL;
    output.dataShort = input->dataShort ? (int16_t *)UINTPTR_MAX : NULL;
    output.dataInt = input->dataInt ? (int *)UINTPTR_MAX : NULL;
    output.randomDataByte = input->randomDataByte ? (uint8_t *)UINTPTR_MAX : NULL;
    output.randomDataShort = input->randomDataShort ? (int16_t *)UINTPTR_MAX : NULL;
    output.randomDataInt = input->randomDataInt ? (int *)UINTPTR_MAX : NULL;
    output.indices.data = input->indices.data ? (void *)UINTPTR_MAX : NULL;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_XANIMPARTS, input) ||
        !Write(writer, &output, sizeof(XAnimParts)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name) || !Array(writer, input->names, input->boneCount[9] * sizeof(uint16_t), 1) ||
        (input->notifyCount && !Align(writer)))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->notifyCount; ++i)
    {
        XAnimNotifyInfo notify = {};
        notify.name = input->notify[i].name;
        notify.time = input->notify[i].time;
        if (!Write(writer, &notify, sizeof(XAnimNotifyInfo)))
        {
            return false;
        }
    }
    if (!AnimationDelta(writer, input->deltaPart, input->numframes) ||
        !Array(writer, input->dataByte, input->dataByteCount * sizeof(uint8_t), 0) ||
        !Array(writer, input->dataShort, input->dataShortCount * sizeof(int16_t), 1) ||
        !Array(writer, input->dataInt, input->dataIntCount * sizeof(int), 15) ||
        !Array(writer, input->randomDataShort, input->randomDataShortCount * sizeof(int16_t), 1) ||
        !Array(writer, input->randomDataByte, input->randomDataByteCount * sizeof(uint8_t), 0) ||
        !Array(writer, input->randomDataInt, input->randomDataIntCount * sizeof(int), 15) ||
        !Array(writer, input->indices.data,
               input->indexCount * (input->numframes < 256 ? sizeof(uint8_t) : sizeof(uint16_t)),
               input->numframes < 256 ? 0 : 1))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool SoundAsset(NativeWriter *writer, const LoadedSound *input)
{
    if (!DB64_ValidateLoadedSound(input, writer->error, writer->errorSize))
    {
        return false;
    }
    if (ReferenceToken(writer, ASSET_TYPE_LOADED_SOUND, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_LOADED_SOUND, input);
    }
    LoadedSound output = {};
    output.name = StringToken(input->name);
    output.sound.info.format = input->sound.info.format;
    output.sound.info.data_len = input->sound.info.data_len;
    output.sound.info.rate = input->sound.info.rate;
    output.sound.info.bits = input->sound.info.bits;
    output.sound.info.channels = input->sound.info.channels;
    output.sound.info.samples = input->sound.info.samples;
    output.sound.info.block_size = input->sound.info.block_size;
    output.sound.data = (uint8_t *)UINTPTR_MAX;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_LOADED_SOUND, input) ||
        !Write(writer, &output, sizeof(LoadedSound)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name))
    {
        return false;
    }
    writer->block = 0;
    if (!Write(writer, input->sound.data, input->sound.info.data_len))
    {
        return false;
    }
    writer->position[0] = start;
    writer->block = 4;
    return true;
}

static bool MenuPointer(NativeWriter *writer, size_t offset, uintptr_t token)
{
    memcpy(writer->data + offset, &token, sizeof(uintptr_t));
    return true;
}

static bool MenuString(NativeWriter *writer, size_t offset, const char *value)
{
    MenuPointer(writer, offset, value ? UINTPTR_MAX : 0);
    return String(writer, value);
}

static bool MenuMaterial(NativeWriter *writer, size_t offset, const Material *value)
{
    MenuPointer(writer, offset, value ? ReferenceToken(writer, ASSET_TYPE_MATERIAL, value) : 0);
    return !value || MaterialAsset(writer, value);
}

static bool MenuStatement(NativeWriter *writer, size_t offset, const statement_s *value)
{
    if (!DB64_ValidateMenuStatement(value))
    {
        return Fail(writer, "Invalid native menu statement");
    }
    MenuPointer(writer, offset + offsetof(statement_s, entries), value->entries ? UINTPTR_MAX : 0);
    if (!value->entries)
    {
        return true;
    }
    if (!Align(writer))
    {
        return false;
    }
    const size_t pointers = writer->size;
    if (!Write(writer, value->entries, value->numEntries * sizeof(expressionEntry *)))
    {
        return false;
    }
    for (int i = 0; i < value->numEntries; ++i)
    {
        const expressionEntry *entry = value->entries[i];
        if (!DB64_ValidateMenuExpression(entry))
        {
            return Fail(writer, "Invalid native menu expression");
        }
        MenuPointer(writer, pointers + i * sizeof(expressionEntry *), UINTPTR_MAX);
        if (!Align(writer) || !Write(writer, entry, sizeof(expressionEntry)))
        {
            return false;
        }
        if (entry->type == 1 && entry->data.operand.dataType == VAL_STRING &&
            !MenuString(writer, writer->size - sizeof(expressionEntry) + offsetof(expressionEntry, data) +
                offsetof(Operand, internals), entry->data.operand.internals.string))
        {
            return false;
        }
    }
    return true;
}

static bool MenuKeys(NativeWriter *writer, size_t offset, const ItemKeyHandler *keys)
{
    unsigned int count = 0;
    while (keys)
    {
        if (++count > 256)
        {
            return Fail(writer, "Too many or cyclic native menu key handlers");
        }
        MenuPointer(writer, offset, UINTPTR_MAX);
        if (!Align(writer) || !Write(writer, keys, sizeof(ItemKeyHandler)))
        {
            return false;
        }
        const size_t header = writer->size - sizeof(ItemKeyHandler);
        if (!MenuString(writer, header + offsetof(ItemKeyHandler, action), keys->action))
        {
            return false;
        }
        offset = header + offsetof(ItemKeyHandler, next);
        keys = keys->next;
    }
    return MenuPointer(writer, offset, 0);
}

static bool MenuWindow(NativeWriter *writer, size_t offset, const windowDef_t *window)
{
    return MenuString(writer, offset + offsetof(windowDef_t, name), window->name) &&
           MenuString(writer, offset + offsetof(windowDef_t, group), window->group) &&
           MenuMaterial(writer, offset + offsetof(windowDef_t, background), window->background);
}

static bool FontAsset(NativeWriter *writer, const Font_s *input)
{
    if (!DB64_ValidateFont(input, true))
    {
        return Fail(writer, "Invalid native font");
    }
    if (ReferenceToken(writer, ASSET_TYPE_FONT, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_FONT, input);
    }
    Font_s output = {};
    output.fontName = StringToken(input->fontName);
    output.pixelHeight = input->pixelHeight;
    output.glyphCount = input->glyphCount;
    output.material = (Material *)ReferenceToken(writer, ASSET_TYPE_MATERIAL, input->material);
    output.glowMaterial = (Material *)ReferenceToken(writer, ASSET_TYPE_MATERIAL, input->glowMaterial);
    output.glyphs = (Glyph *)UINTPTR_MAX;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_FONT, input) || !Write(writer, &output, sizeof(Font_s)))
    {
        return false;
    }
    const size_t fontHeader = writer->size - sizeof(Font_s);
    writer->block = 4;
    if (!String(writer, input->fontName) || !MaterialAsset(writer, input->material))
    {
        return false;
    }
    const uintptr_t glowToken = ReferenceToken(writer, ASSET_TYPE_MATERIAL, input->glowMaterial);
    memcpy(writer->data + fontHeader + offsetof(Font_s, glowMaterial), &glowToken, sizeof(uintptr_t));
    if (!MaterialAsset(writer, input->glowMaterial) ||
        !Array(writer, input->glyphs, input->glyphCount * sizeof(Glyph), 3))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool SoundAliases(NativeWriter *writer, const snd_alias_list_t *input)
{
    if (!input || !input->aliasName || input->count < 0 || input->count > INT_MAX / sizeof(snd_alias_t) ||
        (!!input->head != (input->count != 0)))
    {
        return Fail(writer, "Invalid native sound alias list");
    }
    if (ReferenceToken(writer, ASSET_TYPE_SOUND, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_SOUND, input);
    }
    snd_alias_list_t output = {};
    output.aliasName = StringToken(input->aliasName);
    output.count = input->count;
    output.head = input->count ? (snd_alias_t *)UINTPTR_MAX : NULL;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_SOUND, input) ||
        !Write(writer, &output, sizeof(snd_alias_list_t)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->aliasName) || (input->count && !Align(writer)))
    {
        return false;
    }
    const size_t aliasHeaders = writer->size;
    for (int i = 0; i < input->count; ++i)
    {
        const snd_alias_t *alias = &input->head[i];
        if (!alias->aliasName || !alias->soundFile)
        {
            return Fail(writer, "Native sound alias has no name or sound file");
        }
        snd_alias_t encoded = *alias;
        Linker_ClearPadding_snd_alias_t(&encoded);
        encoded.aliasName = StringToken(alias->aliasName);
        encoded.subtitle = StringToken(alias->subtitle);
        encoded.secondaryAliasName = StringToken(alias->secondaryAliasName);
        encoded.chainAliasName = StringToken(alias->chainAliasName);
        encoded.soundFile = (SoundFile *)UINTPTR_MAX;
        encoded.volumeFalloffCurve = (SndCurve *)InlineAssetToken(writer, ASSET_TYPE_SOUND_CURVE, alias->volumeFalloffCurve);
        encoded.speakerMap = alias->speakerMap ? (SpeakerMap *)UINTPTR_MAX : NULL;
        if (!Write(writer, &encoded, sizeof(snd_alias_t)))
        {
            return false;
        }
    }
    for (int i = 0; i < input->count; ++i)
    {
        const snd_alias_t *alias = &input->head[i];
        if (!String(writer, alias->aliasName) || !String(writer, alias->subtitle) ||
            !String(writer, alias->secondaryAliasName) || !String(writer, alias->chainAliasName))
        {
            return false;
        }
        const SoundFile *file = alias->soundFile;
        if (file->exists > 1 || (file->type != SAT_LOADED && file->type != SAT_STREAMED))
        {
            return Fail(writer, "Invalid native sound file type");
        }
        SoundFile encoded = {};
        encoded.type = file->type;
        encoded.exists = file->exists;
        if (file->type == SAT_LOADED)
        {
            if (file->exists && !file->u.loadSnd)
            {
                return Fail(writer, "Native sound file marked present has no loaded sound");
            }
            encoded.u.loadSnd = file->u.loadSnd
                                    ? (LoadedSound *)ReferenceToken(writer, ASSET_TYPE_LOADED_SOUND, file->u.loadSnd)
                                    : NULL;
        }
        else
        {
            if (!file->u.streamSnd.filename.info.raw.name)
            {
                return Fail(writer, "Native streamed sound has no filename");
            }
            encoded.u.streamSnd.filename.info.raw.dir = StringToken(file->u.streamSnd.filename.info.raw.dir);
            encoded.u.streamSnd.filename.info.raw.name = StringToken(file->u.streamSnd.filename.info.raw.name);
        }
        if (!Array(writer, &encoded, sizeof(SoundFile), 15))
        {
            return false;
        }
        if (file->type == SAT_LOADED)
        {
            if (file->u.loadSnd && !SoundAsset(writer, file->u.loadSnd))
            {
                return false;
            }
        }
        else if (!String(writer, file->u.streamSnd.filename.info.raw.dir) ||
                 !String(writer, file->u.streamSnd.filename.info.raw.name))
        {
            return false;
        }
        if (alias->volumeFalloffCurve)
        {
            MenuPointer(writer, aliasHeaders + i * sizeof(snd_alias_t) + offsetof(snd_alias_t, volumeFalloffCurve),
                        InlineAssetToken(writer, ASSET_TYPE_SOUND_CURVE, alias->volumeFalloffCurve));
            XAssetHeader header = {};
            header.sndCurve = alias->volumeFalloffCurve;
            if (!Preset(writer, ASSET_TYPE_SOUND_CURVE, header))
            {
                return false;
            }
        }
        if (alias->speakerMap)
        {
            if (!DB64_ValidateSpeakerMap(alias->speakerMap, writer->error, writer->errorSize))
            {
                return false;
            }
            SpeakerMap map = {};
            map.name = StringToken(alias->speakerMap->name);
            map.isDefault = alias->speakerMap->isDefault;
            memcpy(map.channelMaps, alias->speakerMap->channelMaps, sizeof(map.channelMaps));
            if (!Array(writer, &map, sizeof(SpeakerMap), 15) || !String(writer, alias->speakerMap->name))
            {
                return false;
            }
        }
    }
    writer->position[0] = start;
    return true;
}

static bool ClipPointer(NativeWriter *writer, const void *pointer, const void *base, size_t bytes, size_t required,
                        uintptr_t token, void *output)
{
    uintptr_t encoded = 0;
    if (pointer)
    {
        const uintptr_t offset = (uintptr_t)pointer - (uintptr_t)base;
        if (!base || offset > bytes || required > bytes - offset)
        {
            return Fail(writer, "Native clipmap pointer is outside its owning array");
        }
        encoded = token + offset;
    }
    else if (required)
    {
        return Fail(writer, "Native clipmap is missing a required shared pointer");
    }
    memcpy(output, &encoded, sizeof(uintptr_t));
    return true;
}

static uintptr_t ClipArrayToken(const NativeWriter *writer, int alignment)
{
    const size_t position = (writer->position[4] + alignment) & ~(size_t)alignment;
    return (UINT64_C(4) << 60) + position + 1;
}

static bool ClipBrush(NativeWriter *writer, const cbrush_t *input, const clipMap_t *map, uintptr_t sides,
                      uintptr_t edges)
{
    cbrush_t output = *input;
    if (input->numsides > 250 ||
        !ClipPointer(writer, input->sides, map->brushsides, map->numBrushSides * sizeof(cbrushside_t),
                     input->numsides * sizeof(cbrushside_t), sides, &output.sides))
    {
        return Fail(writer, "Invalid native clipmap brush sides");
    }
    size_t edgeBytes = 0;
    for (unsigned int i = 0; i < input->numsides + 6; ++i)
    {
        const int offset =
            i < 6 ? input->firstAdjacentSideOffsets[i & 1][i >> 1] : input->sides[i - 6].firstAdjacentSideOffset;
        const unsigned int count = i < 6 ? input->edgeCount[i & 1][i >> 1] : input->sides[i - 6].edgeCount;
        if (count && offset < 0)
        {
            return Fail(writer, "Negative native clipmap brush edge offset");
        }
        if (count && (size_t)offset + count > edgeBytes)
        {
            edgeBytes = (size_t)offset + count;
        }
    }
    if (!ClipPointer(writer, input->baseAdjacentSide, map->brushEdges, map->numBrushEdges, edgeBytes, edges,
                     &output.baseAdjacentSide))
    {
        return false;
    }
    return Write(writer, &output, sizeof(cbrush_t));
}

static bool ModelPiecesAsset(NativeWriter *writer, const XModelPieces *input);
static bool EffectName(NativeWriter *writer, const FxEffectDef *effect);

static bool DynamicEntities(NativeWriter *writer, const clipMap_t *map)
{
    for (int group = 0; group < 2; ++group)
    {
        const size_t count = map->dynEntCount[group];
        if (count && !Align(writer))
        {
            return false;
        }
        const size_t headers = writer->size;
        for (size_t i = 0; i < count; ++i)
        {
            const DynEntityDef *source = &map->dynEntDefList[group][i];
            if (!DB64_ValidateDynamicEntity(source, group, map->numSubModels))
            {
                return Fail(writer, "Invalid native dynamic entity definition");
            }
            DynEntityDef entity = {};
            entity.type = source->type;
            entity.pose = source->pose;
            entity.brushModel = source->brushModel;
            entity.physicsBrushModel = source->physicsBrushModel;
            entity.health = source->health;
            entity.mass = source->mass;
            entity.contents = source->contents;
            entity.destroyFx = source->destroyFx ? (FxEffectDef *)UINTPTR_MAX : NULL;
            entity.destroyPieces = NULL;
            entity.physPreset = (PhysPreset *)InlineAssetToken(writer, ASSET_TYPE_PHYSPRESET, source->physPreset);
            if (!Write(writer, &entity, sizeof(DynEntityDef)))
            {
                return false;
            }
        }
        for (size_t i = 0; i < count; ++i)
        {
            const DynEntityDef *entity = &map->dynEntDefList[group][i];
            uintptr_t token = entity->xModel ? ReferenceToken(writer, ASSET_TYPE_XMODEL, entity->xModel) : 0;
            memcpy(writer->data + headers + i * sizeof(DynEntityDef) + offsetof(DynEntityDef, xModel), &token,
                   sizeof(uintptr_t));
            if ((entity->xModel && !ModelAsset(writer, entity->xModel)) || !EffectName(writer, entity->destroyFx))
            {
                return false;
            }
            token = entity->destroyPieces ? ReferenceToken(writer, ASSET_TYPE_XMODELPIECES, entity->destroyPieces) : 0;
            memcpy(writer->data + headers + i * sizeof(DynEntityDef) + offsetof(DynEntityDef, destroyPieces), &token,
                   sizeof(uintptr_t));
            XAssetHeader preset = {};
            preset.physPreset = entity->physPreset;
            if (entity->destroyPieces && !ModelPiecesAsset(writer, entity->destroyPieces))
            {
                return false;
            }
            MenuPointer(writer, headers + i * sizeof(DynEntityDef) + offsetof(DynEntityDef, physPreset),
                        InlineAssetToken(writer, ASSET_TYPE_PHYSPRESET, entity->physPreset));
            if (entity->physPreset && !Preset(writer, ASSET_TYPE_PHYSPRESET, preset))
            {
                return false;
            }
        }
    }
    writer->block = 1;
    for (int kind = 0; kind < 3; ++kind)
    {
        for (int group = 0; group < 2; ++group)
        {
            const size_t stride = kind == 0   ? sizeof(DynEntityPose)
                                  : kind == 1 ? sizeof(DynEntityClient)
                                              : sizeof(DynEntityColl);
            if (map->dynEntCount[group] && (!Align(writer) || !Advance(writer, map->dynEntCount[group] * stride)))
            {
                return false;
            }
        }
    }
    writer->block = 4;
    return true;
}

static bool ClipMapAsset(NativeWriter *writer, const clipMap_t *input)
{
    if (!DB64_ValidateClipMapHeader(input, writer->error, writer->errorSize) ||
        !DB64_ValidateCollisionTrees(input, writer->error, writer->errorSize) ||
        !DB64_ValidateBspNodes(input, writer->error, writer->errorSize))
    {
        return false;
    }
    if (input->mapEnts && (!input->mapEnts->name || strcmp(input->name, input->mapEnts->name)))
    {
        return Fail(writer, "Native clipmap entities belong to a different map");
    }
    if (ReferenceToken(writer, ASSET_TYPE_CLIPMAP, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_CLIPMAP, input);
    }
    clipMap_t output = *input;
    output.name = StringToken(input->name);
#define CLIP_POINTER(field, type) output.field = input->field ? (type)UINTPTR_MAX : NULL
    CLIP_POINTER(planes, cplane_s *);
    CLIP_POINTER(staticModelList, cStaticModel_s *);
    CLIP_POINTER(materials, dmaterial_t *);
    CLIP_POINTER(brushsides, cbrushside_t *);
    CLIP_POINTER(brushEdges, uint8_t *);
    CLIP_POINTER(nodes, cNode_t *);
    CLIP_POINTER(leafs, cLeaf_t *);
    CLIP_POINTER(leafbrushes, uint16_t *);
    CLIP_POINTER(leafbrushNodes, cLeafBrushNode_s *);
    CLIP_POINTER(leafsurfaces, uint *);
    CLIP_POINTER(verts, float(*)[3]);
    CLIP_POINTER(triIndices, uint16_t *);
    CLIP_POINTER(triEdgeIsWalkable, uint8_t *);
    CLIP_POINTER(borders, CollisionBorder *);
    CLIP_POINTER(partitions, CollisionPartition *);
    CLIP_POINTER(aabbTrees, CollisionAabbTree *);
    CLIP_POINTER(cmodels, cmodel_t *);
    CLIP_POINTER(brushes, cbrush_t *);
    CLIP_POINTER(visibility, uint8_t *);
    output.mapEnts = (MapEnts *)InlineAssetToken(writer, ASSET_TYPE_MAP_ENTS, input->mapEnts);
    CLIP_POINTER(box_brush, cbrush_t *);
#undef CLIP_POINTER
    for (int group = 0; group < 2; ++group)
    {
        output.dynEntDefList[group] = input->dynEntCount[group] ? (DynEntityDef *)UINTPTR_MAX : NULL;
        output.dynEntPoseList[group] = input->dynEntCount[group] ? (DynEntityPose *)UINTPTR_MAX : NULL;
        output.dynEntClientList[group] = input->dynEntCount[group] ? (DynEntityClient *)UINTPTR_MAX : NULL;
        output.dynEntCollList[group] = input->dynEntCount[group] ? (DynEntityColl *)UINTPTR_MAX : NULL;
    }
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_CLIPMAP, input) ||
        !Write(writer, &output, sizeof(clipMap_t)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name))
    {
        return false;
    }
    const uintptr_t planes = ClipArrayToken(writer, 15);
    if (!Array(writer, input->planes, input->planeCount * sizeof(cplane_s), 15) ||
        (input->numStaticModels && !Align(writer)))
    {
        return false;
    }
    const size_t modelHeaders = writer->size;
    for (unsigned int i = 0; i < input->numStaticModels; ++i)
    {
        cStaticModel_s model = input->staticModelList[i];
        memset(&model.writable, 0, sizeof(model.writable));
        model.xmodel = NULL;
        if (!Write(writer, &model, sizeof(cStaticModel_s)))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < input->numStaticModels; ++i)
    {
        const XModel *model = input->staticModelList[i].xmodel;
        const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_XMODEL, model);
        memcpy(writer->data + modelHeaders + i * sizeof(cStaticModel_s) + offsetof(cStaticModel_s, xmodel), &token,
               sizeof(uintptr_t));
        if (!ModelAsset(writer, model))
        {
            return false;
        }
    }
    // Material -1 is the empty surface used by the temporary box hull.
    const dmaterial_t emptyMaterial = {};
    if (input->numMaterials && !Array(writer, &emptyMaterial, sizeof(dmaterial_t), 15))
    {
        return false;
    }
    if (!Array(writer, input->materials, input->numMaterials * sizeof(dmaterial_t), 0))
    {
        return false;
    }
    const uintptr_t sides = ClipArrayToken(writer, 15);
    if (input->numBrushSides && !Align(writer))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->numBrushSides; ++i)
    {
        cbrushside_t side = input->brushsides[i];
        if (!ClipPointer(writer, side.plane, input->planes, input->planeCount * sizeof(cplane_s), sizeof(cplane_s),
                         planes, &side.plane) ||
            !Write(writer, &side, sizeof(cbrushside_t)))
        {
            return false;
        }
    }
    const uintptr_t edges = ClipArrayToken(writer, 0);
    if (!Array(writer, input->brushEdges, input->numBrushEdges, 0) || (input->numNodes && !Align(writer)))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->numNodes; ++i)
    {
        cNode_t node = input->nodes[i];
        if (!ClipPointer(writer, node.plane, input->planes, input->planeCount * sizeof(cplane_s), sizeof(cplane_s),
                         planes, &node.plane) ||
            !Write(writer, &node, sizeof(cNode_t)))
        {
            return false;
        }
    }
    if (!Array(writer, input->leafs, input->numLeafs * sizeof(cLeaf_t), 15))
    {
        return false;
    }
    const uintptr_t leafBrushes = ClipArrayToken(writer, 1);
    if (!Array(writer, input->leafbrushes, input->numLeafBrushes * sizeof(uint16_t), 1) ||
        (input->leafbrushNodesCount && !Align(writer)))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->leafbrushNodesCount; ++i)
    {
        cLeafBrushNode_s node = input->leafbrushNodes[i];
        if (node.leafBrushCount > 0 &&
            !ClipPointer(writer, node.data.leaf.brushes, input->leafbrushes, input->numLeafBrushes * sizeof(uint16_t),
                         node.leafBrushCount * sizeof(uint16_t), leafBrushes, &node.data.leaf.brushes))
        {
            return false;
        }
        if (!Write(writer, &node, sizeof(cLeafBrushNode_s)))
        {
            return false;
        }
    }
    if (!Array(writer, input->leafsurfaces, input->numLeafSurfaces * sizeof(uint), 15) ||
        !Array(writer, input->verts, input->vertCount * sizeof(float[3]), 15) ||
        !Array(writer, input->triIndices, input->triCount * sizeof(uint16_t[3]), 1) ||
        !Array(writer, input->triEdgeIsWalkable, DB64_ClipMapWalkableBytes(input->triCount), 0))
    {
        return false;
    }
    const uintptr_t borders = ClipArrayToken(writer, 15);
    if (!Array(writer, input->borders, input->borderCount * sizeof(CollisionBorder), 15) ||
        (input->partitionCount && !Align(writer)))
    {
        return false;
    }
    for (int i = 0; i < input->partitionCount; ++i)
    {
        CollisionPartition partition = input->partitions[i];
        if (!ClipPointer(writer, partition.borders, input->borders, input->borderCount * sizeof(CollisionBorder),
                         partition.borderCount * sizeof(CollisionBorder), borders, &partition.borders) ||
            !Write(writer, &partition, sizeof(CollisionPartition)))
        {
            return false;
        }
    }
    if (!Array(writer, input->aabbTrees, input->aabbTreeCount * sizeof(CollisionAabbTree), 15) ||
        !Array(writer, input->cmodels, input->numSubModels * sizeof(cmodel_t), 15) ||
        (input->numBrushes && !Align(writer)))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->numBrushes; ++i)
    {
        if (!ClipBrush(writer, &input->brushes[i], input, sides, edges))
        {
            return false;
        }
    }
    if (input->visibility && !Array(writer, input->visibility, (size_t)input->numClusters * input->clusterBytes, 0))
    {
        return false;
    }
    if (input->mapEnts && !Entities(writer, input->mapEnts))
    {
        return false;
    }
    if (input->box_brush && (!Align(writer) || !ClipBrush(writer, input->box_brush, input, sides, edges)))
    {
        return false;
    }
    if (!DynamicEntities(writer, input))
    {
        return false;
    }
    writer->position[0] = start;
    return true;
}

static bool ModelPiecesAsset(NativeWriter *writer, const XModelPieces *input)
{
    if (!input || !input->name || input->numpieces < 0 || input->numpieces > INT_MAX / sizeof(XModelPiece) ||
        (!!input->pieces != (input->numpieces != 0)))
    {
        return Fail(writer, "Invalid native model-piece list");
    }
    if (ReferenceToken(writer, ASSET_TYPE_XMODELPIECES, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_XMODELPIECES, input);
    }
    XModelPieces output = {};
    output.name = StringToken(input->name);
    output.numpieces = input->numpieces;
    output.pieces = input->pieces ? (XModelPiece *)UINTPTR_MAX : NULL;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_XMODELPIECES, input) ||
        !Write(writer, &output, sizeof(XModelPieces)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name) || (input->numpieces && !Align(writer)))
    {
        return false;
    }
    const size_t headers = writer->size;
    for (int i = 0; i < input->numpieces; ++i)
    {
        XModelPiece piece = {};
        memcpy(piece.offset, input->pieces[i].offset, sizeof(piece.offset));
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(piece.offset[axis]))
            {
                return Fail(writer, "Invalid native model piece offset");
            }
        }
        if (!Write(writer, &piece, sizeof(XModelPiece)))
        {
            return false;
        }
    }
    for (int i = 0; i < input->numpieces; ++i)
    {
        const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_XMODEL, input->pieces[i].model);
        memcpy(writer->data + headers + i * sizeof(XModelPiece) + offsetof(XModelPiece, model), &token,
               sizeof(uintptr_t));
        if (!ModelAsset(writer, input->pieces[i].model))
        {
            return false;
        }
    }
    writer->position[0] = start;
    return true;
}

static bool EffectName(NativeWriter *writer, const FxEffectDef *effect)
{
    if (!effect)
    {
        return true;
    }
    for (int i = 0; i < writer->zoneAssetCount; ++i)
    {
        if (writer->zoneAssets[i].type == ASSET_TYPE_FX && writer->zoneAssets[i].header.fx == effect)
        {
            return effect->name && effect->name[0] ? String(writer, effect->name)
                                                   : Fail(writer, "Effect dependency has no name");
        }
    }
    return Fail(writer, "Effect dependency is missing from the native zone asset list");
}

static bool EffectVisual(NativeWriter *writer, const FxElemVisuals *visual, int type, size_t offset)
{
    uintptr_t token = 0;
    if (visual->anonymous)
    {
        if (type == 5)
        {
            token = ReferenceToken(writer, ASSET_TYPE_XMODEL, visual->model);
        }
        else if (type == 8 || type == 10)
        {
            token = UINTPTR_MAX;
        }
        else if (type == 6 || type == 7)
        {
            return Fail(writer, "Light effect has a visual pointer");
        }
        else
        {
            token = ReferenceToken(writer, ASSET_TYPE_MATERIAL, visual->material);
        }
    }
    memcpy(writer->data + offset, &token, sizeof(uintptr_t));
    if (!token)
    {
        return true;
    }
    switch (type)
    {
    case 5:
        return ModelAsset(writer, visual->model);
    case 8:
        return String(writer, visual->soundName);
    case 10:
        return EffectName(writer, visual->effectDef.handle);
    default:
        return MaterialAsset(writer, visual->material);
    }
}

static bool EffectAsset(NativeWriter *writer, const FxEffectDef *input)
{
    size_t count;
    if (!DB64_EffectElementCount(input, &count) || !input->name[0])
    {
        return Fail(writer, "Invalid native effect header");
    }
    if (ReferenceToken(writer, ASSET_TYPE_FX, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_FX, input);
    }
    FxEffectDef output = *input;
    output.name = StringToken(input->name);
    output.elemDefs = count ? (FxElemDef *)UINTPTR_MAX : NULL;
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_FX, input) ||
        !Write(writer, &output, sizeof(FxEffectDef)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name) || (count && !Align(writer)))
    {
        return false;
    }
    const size_t headers = writer->size;
    for (size_t i = 0; i < count; ++i)
    {
        const FxElemDef *element = &input->elemDefs[i];
        if (!DB64_ValidateEffectElement(element))
        {
            return Fail(writer, "Invalid native effect element");
        }
        FxElemDef copy = *element;
        Linker_ClearPadding_FxElemDef(&copy);
        copy.velSamples = element->velSamples ? (FxElemVelStateSample *)UINTPTR_MAX : NULL;
        copy.visSamples = element->visSamples ? (FxElemVisStateSample *)UINTPTR_MAX : NULL;
        copy.visuals.instance.anonymous = element->visuals.instance.anonymous ? (void *)UINTPTR_MAX : NULL;
        copy.effectOnImpact.name = element->effectOnImpact.handle ? (const char *)UINTPTR_MAX : NULL;
        copy.effectOnDeath.name = element->effectOnDeath.handle ? (const char *)UINTPTR_MAX : NULL;
        copy.effectEmitted.name = element->effectEmitted.handle ? (const char *)UINTPTR_MAX : NULL;
        copy.trailDef = element->trailDef ? (FxTrailDef *)UINTPTR_MAX : NULL;
        if (!Write(writer, &copy, sizeof(FxElemDef)))
        {
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i)
    {
        const FxElemDef *element = &input->elemDefs[i];
        if ((element->velSamples &&
             !Array(writer, element->velSamples, ((size_t)element->velIntervalCount + 1) * sizeof(FxElemVelStateSample),
                    15)) ||
            (element->visSamples &&
             !Array(writer, element->visSamples,
                    ((size_t)element->visStateIntervalCount + 1) * sizeof(FxElemVisStateSample), 15)))
        {
            return false;
        }
        if (element->elemType == 9 || element->visualCount > 1)
        {
            const size_t visualCount = element->elemType == 9 ? element->visualCount * 2 : element->visualCount;
            if (visualCount && !Align(writer))
            {
                return false;
            }
            const size_t visualHeaders = writer->size;
            const uintptr_t empty = 0;
            for (size_t j = 0; j < visualCount; ++j)
            {
                if (!Write(writer, &empty, sizeof(uintptr_t)))
                {
                    return false;
                }
            }
            for (size_t j = 0; j < visualCount; ++j)
            {
                FxElemVisuals visual = {};
                if (element->elemType == 9)
                {
                    visual.material = element->visuals.markArray[j / 2].materials[j % 2];
                }
                else
                {
                    visual = element->visuals.array[j];
                }
                if (!EffectVisual(writer, &visual, element->elemType == 9 ? 0 : element->elemType,
                                  visualHeaders + j * sizeof(FxElemVisuals)))
                {
                    return false;
                }
            }
        }
        else if (!EffectVisual(writer, &element->visuals.instance, element->elemType,
                               headers + i * sizeof(FxElemDef) + offsetof(FxElemDef, visuals)))
        {
            return false;
        }
        if (!EffectName(writer, element->effectOnImpact.handle) || !EffectName(writer, element->effectOnDeath.handle) ||
            !EffectName(writer, element->effectEmitted.handle))
        {
            return false;
        }
        if (element->trailDef)
        {
            if (!DB64_ValidateTrail(element->trailDef, true))
            {
                return Fail(writer, "Invalid native effect trail");
            }
            FxTrailDef trail = *element->trailDef;
            trail.verts = trail.vertCount ? (FxTrailVertex *)UINTPTR_MAX : NULL;
            trail.inds = trail.indCount ? (uint16_t *)UINTPTR_MAX : NULL;
            if (!Array(writer, &trail, sizeof(FxTrailDef), 15) ||
                !Array(writer, element->trailDef->verts, trail.vertCount * sizeof(FxTrailVertex), 15) ||
                !Array(writer, element->trailDef->inds, trail.indCount * sizeof(uint16_t), 1))
            {
                return false;
            }
        }
    }
    writer->position[0] = start;
    return true;
}

static void WorldPointer(NativeWriter *writer, size_t field, uintptr_t token)
{
    memcpy(writer->data + field, &token, sizeof(uintptr_t));
}

static bool WorldArray(NativeWriter *writer, size_t field, const void *data, size_t count, size_t stride,
                       unsigned int alignment, size_t *headers = NULL, bool runtime = false)
{
    if (count > INT_MAX / stride || (!!data != (count != 0)))
    {
        return Fail(writer, "Invalid native world array dimensions");
    }
    WorldPointer(writer, field, count ? UINTPTR_MAX : 0);
    if (headers)
    {
        *headers = writer->size;
    }
    if (!count)
    {
        return true;
    }
    writer->block = runtime ? 1 : 4;
    const bool ok = Advance(writer, (-writer->position[writer->block]) & alignment) &&
                    (runtime ? Advance(writer, count * stride) : Write(writer, data, count * stride));
    writer->block = 4;
    return ok;
}

static bool WorldAssetReference(NativeWriter *writer, size_t field, XAssetType type, const void *asset)
{
    const uintptr_t token = !asset                         ? 0
                            : type == ASSET_TYPE_LIGHT_DEF ? UINTPTR_MAX
                                                           : ReferenceToken(writer, type, asset);
    WorldPointer(writer, field, token);
    if (!asset)
    {
        return true;
    }
    switch (type)
    {
    case ASSET_TYPE_IMAGE:
        return ImageAsset(writer, (const GfxImage *)asset);
    case ASSET_TYPE_MATERIAL:
        return MaterialAsset(writer, (const Material *)asset);
    case ASSET_TYPE_XMODEL:
        return ModelAsset(writer, (const XModel *)asset);
    case ASSET_TYPE_LIGHT_DEF:
        return LightDefinition(writer, (const GfxLightDef *)asset);
    default:
        return Fail(writer, "Unsupported render-world dependency");
    }
}

static bool RenderWorldAsset(NativeWriter *writer, const GfxWorld *input)
{
    DB64WorldRuntimeLayout layout;
    if (!input || !input->name || !input->baseName || input->reflectionProbeCount > 256 ||
        input->lightmapCount < 0 || input->lightmapCount > 31 ||
        !DB64_ValidLightGridData(&input->lightGrid, input->primaryLightCount) ||
        !DB64_ValidateWorldRuntimeCounts(input, writer->error, writer->errorSize) ||
        !DB64_GetWorldRuntimeLayout(input, &layout, writer->error, writer->errorSize))
    {
        return Fail(writer, "Invalid native render world");
    }
    if (ReferenceToken(writer, ASSET_TYPE_GFXWORLD, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_GFXWORLD, input);
    }
    const unsigned int sortedCount = input->dpvs.staticSurfaceCount + input->dpvs.staticSurfaceCountNoDecal;
    if (input->cullGroupCount < 0 || (!!input->dpvs.cullGroups != (input->cullGroupCount != 0)))
    {
        return Fail(writer, "Invalid native cull-group array");
    }
    for (int i = 0; i < input->cullGroupCount; ++i)
    {
        const GfxCullGroup *group = &input->dpvs.cullGroups[i];
        if (!group->surfaceCount && group->startSurfIndex == -1)
        {
            continue;
        }
        if (group->startSurfIndex < 0 || group->surfaceCount < 0 || (unsigned int)group->startSurfIndex > sortedCount ||
            (unsigned int)group->surfaceCount > sortedCount - group->startSurfIndex)
        {
            return Fail(writer, "Invalid native cull-group surface span");
        }
    }
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_GFXWORLD, input))
    {
        return false;
    }
    const size_t header = writer->size;
    GfxWorld output = *input;
    output.name = StringToken(input->name);
    output.baseName = StringToken(input->baseName);
    output.vd.worldVb = NULL;
    output.vld.layerVb = NULL;
    output.dpvs.usageCount = 0;
    if (!Write(writer, &output, sizeof(GfxWorld)))
    {
        return false;
    }
    writer->block = 4;
#define WORLD_ARRAY(field, count, type, alignment)                                                                     \
    if (!WorldArray(writer, header + offsetof(GfxWorld, field), input->field, count, sizeof(type), alignment))         \
    {                                                                                                                  \
        return false;                                                                                                  \
    }
#define WORLD_RUNTIME(field, count, type, alignment)                                                                   \
    if (!WorldArray(writer, header + offsetof(GfxWorld, field), input->field, count, sizeof(type), alignment, NULL,    \
                    true))                                                                                             \
    {                                                                                                                  \
        return false;                                                                                                  \
    }
#define WORLD_ASSET(field, type)                                                                                       \
    if (!WorldAssetReference(writer, header + offsetof(GfxWorld, field), type, input->field))                          \
    {                                                                                                                  \
        return false;                                                                                                  \
    }
    if (!String(writer, input->name) || !String(writer, input->baseName))
    {
        return false;
    }
    WORLD_ARRAY(indices, input->indexCount, uint16_t, 1);
    WORLD_ARRAY(skyStartSurfs, input->skySurfCount, int, 15);
    WORLD_ASSET(skyImage, ASSET_TYPE_IMAGE);
    size_t children;
    if (!WorldArray(writer, header + offsetof(GfxWorld, sunLight), input->sunLight, input->sunLight ? 1 : 0,
                    sizeof(GfxLight), 15, &children) ||
        (input->sunLight &&
         !WorldAssetReference(writer, children + offsetof(GfxLight, def), ASSET_TYPE_LIGHT_DEF, input->sunLight->def)))
    {
        return false;
    }
    if (!WorldArray(writer, header + offsetof(GfxWorld, reflectionProbes), input->reflectionProbes,
                    input->reflectionProbeCount, sizeof(GfxReflectionProbe), 15, &children))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->reflectionProbeCount; ++i)
    {
        if (!WorldAssetReference(
                writer, children + i * sizeof(GfxReflectionProbe) + offsetof(GfxReflectionProbe, reflectionImage),
                ASSET_TYPE_IMAGE, input->reflectionProbes[i].reflectionImage))
        {
            return false;
        }
    }
    WORLD_RUNTIME(reflectionProbeTextures, input->reflectionProbeCount, GfxTexture, 15);
    WORLD_ARRAY(dpvsPlanes.planes, input->planeCount, cplane_s, 15);
    WORLD_ARRAY(dpvsPlanes.nodes, input->nodeCount, uint16_t, 1);
    WORLD_RUNTIME(dpvsPlanes.sceneEntCellBits, layout.sceneEntCellWords, uint, 15);
    const uintptr_t cellToken = ClipArrayToken(writer, 15);
    size_t cells;
    if (!WorldArray(writer, header + offsetof(GfxWorld, cells), input->cells, input->dpvsPlanes.cellCount,
                    sizeof(GfxCell), 15, &cells))
    {
        return false;
    }
    for (int c = 0; c < input->dpvsPlanes.cellCount; ++c)
    {
        const GfxCell *cell = &input->cells[c];
        const size_t cellHeader = cells + c * sizeof(GfxCell);
        size_t trees, portals;
        if (!WorldArray(writer, cellHeader + offsetof(GfxCell, aabbTree), cell->aabbTree, cell->aabbTreeCount,
                        sizeof(GfxAabbTree), 15, &trees))
        {
            return false;
        }
        for (int t = 0; t < cell->aabbTreeCount; ++t)
        {
            if (!WorldArray(writer, trees + t * sizeof(GfxAabbTree) + offsetof(GfxAabbTree, smodelIndexes),
                            cell->aabbTree[t].smodelIndexes, cell->aabbTree[t].smodelIndexCount, sizeof(uint16_t), 1))
            {
                return false;
            }
        }
        if (!WorldArray(writer, cellHeader + offsetof(GfxCell, portals), cell->portals, cell->portalCount,
                        sizeof(GfxPortal), 15, &portals))
        {
            return false;
        }
        for (int p = 0; p < cell->portalCount; ++p)
        {
            const GfxPortal *portal = &cell->portals[p];
            const uintptr_t offset = (uintptr_t)portal->cell - (uintptr_t)input->cells;
            if (offset >= (size_t)input->dpvsPlanes.cellCount * sizeof(GfxCell) || offset % sizeof(GfxCell))
            {
                return Fail(writer, "Native portal points outside the world cells");
            }
            const size_t portalHeader = portals + p * sizeof(GfxPortal);
            memset(writer->data + portalHeader + offsetof(GfxPortal, writable), 0, sizeof(GfxPortalWritable));
            WorldPointer(writer, portalHeader + offsetof(GfxPortal, cell), cellToken + offset);
            if (!WorldArray(writer, portalHeader + offsetof(GfxPortal, vertices), portal->vertices, portal->vertexCount,
                            sizeof(float[3]), 15))
            {
                return false;
            }
        }
        if (!WorldArray(writer, cellHeader + offsetof(GfxCell, cullGroups), cell->cullGroups, cell->cullGroupCount,
                        sizeof(int), 15) ||
            !WorldArray(writer, cellHeader + offsetof(GfxCell, reflectionProbes), cell->reflectionProbes,
                        cell->reflectionProbeCount, sizeof(uint8_t), 0))
        {
            return false;
        }
    }
    if (!WorldArray(writer, header + offsetof(GfxWorld, lightmaps), input->lightmaps, input->lightmapCount,
                    sizeof(GfxLightmapArray), 15, &children))
    {
        return false;
    }
    for (int i = 0; i < input->lightmapCount; ++i)
    {
        if (!WorldAssetReference(writer, children + i * sizeof(GfxLightmapArray) + offsetof(GfxLightmapArray, primary),
                                 ASSET_TYPE_IMAGE, input->lightmaps[i].primary) ||
            !WorldAssetReference(writer,
                                 children + i * sizeof(GfxLightmapArray) + offsetof(GfxLightmapArray, secondary),
                                 ASSET_TYPE_IMAGE, input->lightmaps[i].secondary))
        {
            return false;
        }
    }
    if (input->lightGrid.rowAxis > 1 ||
        input->lightGrid.maxs[input->lightGrid.rowAxis] < input->lightGrid.mins[input->lightGrid.rowAxis])
    {
        return Fail(writer, "Invalid native light-grid row dimensions");
    }
    WORLD_ARRAY(lightGrid.rowDataStart,
                input->lightGrid.maxs[input->lightGrid.rowAxis] - input->lightGrid.mins[input->lightGrid.rowAxis] + 1,
                uint16_t, 1);
    WORLD_ARRAY(lightGrid.rawRowData, input->lightGrid.rawRowDataSize, uint8_t, 0);
    WORLD_ARRAY(lightGrid.entries, input->lightGrid.entryCount, GfxLightGridEntry, 15);
    WORLD_ARRAY(lightGrid.colors, input->lightGrid.colorCount, GfxLightGridColors, 15);
    WORLD_RUNTIME(lightmapPrimaryTextures, input->lightmapCount, GfxTexture, 15);
    WORLD_RUNTIME(lightmapSecondaryTextures, input->lightmapCount, GfxTexture, 15);
    WORLD_ARRAY(models, input->modelCount, GfxBrushModel, 15);
    if (!WorldArray(writer, header + offsetof(GfxWorld, materialMemory), input->materialMemory,
                    input->materialMemoryCount, sizeof(MaterialMemory), 15, &children))
    {
        return false;
    }
    for (int i = 0; i < input->materialMemoryCount; ++i)
    {
        if (!WorldAssetReference(writer, children + i * sizeof(MaterialMemory) + offsetof(MaterialMemory, material),
                                 ASSET_TYPE_MATERIAL, input->materialMemory[i].material))
        {
            return false;
        }
    }
    WORLD_ARRAY(vd.vertices, input->vertexCount, GfxWorldVertex, 15);
    WORLD_ARRAY(vld.data, input->vertexLayerDataSize, uint8_t, 0);
    WORLD_ASSET(sun.spriteMaterial, ASSET_TYPE_MATERIAL);
    WORLD_ASSET(sun.flareMaterial, ASSET_TYPE_MATERIAL);
    WORLD_ASSET(outdoorImage, ASSET_TYPE_IMAGE);
    WORLD_RUNTIME(cellCasterBits, layout.cellCasterWords, uint, 15);
    WORLD_RUNTIME(sceneDynModel, input->dpvsDyn.dynEntClientCount[0], GfxSceneDynModel, 15);
    WORLD_RUNTIME(sceneDynBrush, input->dpvsDyn.dynEntClientCount[1], GfxSceneDynBrush, 15);
    WORLD_RUNTIME(primaryLightEntityShadowVis, layout.primaryLightEntityShadowWords, uint, 15);
    WORLD_RUNTIME(primaryLightDynEntShadowVis[0], layout.primaryLightDynEntShadowWords[0], uint, 15);
    WORLD_RUNTIME(primaryLightDynEntShadowVis[1], layout.primaryLightDynEntShadowWords[1], uint, 15);
    WORLD_RUNTIME(nonSunPrimaryLightForModelDynEnt, input->dpvsDyn.dynEntClientCount[0], uint8_t, 0);
    if (!WorldArray(writer, header + offsetof(GfxWorld, shadowGeom), input->shadowGeom,
                    input->shadowGeom ? input->primaryLightCount : 0, sizeof(GfxShadowGeometry), 15, &children))
    {
        return false;
    }
    for (unsigned int i = 0; input->shadowGeom && i < input->primaryLightCount; ++i)
    {
        const GfxShadowGeometry *shadow = &input->shadowGeom[i];
        if (!WorldArray(writer, children + i * sizeof(GfxShadowGeometry) + offsetof(GfxShadowGeometry, sortedSurfIndex),
                        shadow->sortedSurfIndex, shadow->surfaceCount, sizeof(uint16_t), 1) ||
            !WorldArray(writer, children + i * sizeof(GfxShadowGeometry) + offsetof(GfxShadowGeometry, smodelIndex),
                        shadow->smodelIndex, shadow->smodelCount, sizeof(uint16_t), 1))
        {
            return false;
        }
    }
    if (!WorldArray(writer, header + offsetof(GfxWorld, lightRegion), input->lightRegion,
                    input->lightRegion ? input->primaryLightCount : 0, sizeof(GfxLightRegion), 15, &children))
    {
        return false;
    }
    for (unsigned int i = 0; input->lightRegion && i < input->primaryLightCount; ++i)
    {
        const GfxLightRegion *region = &input->lightRegion[i];
        size_t hulls;
        if (!WorldArray(writer, children + i * sizeof(GfxLightRegion) + offsetof(GfxLightRegion, hulls), region->hulls,
                        region->hullCount, sizeof(GfxLightRegionHull), 15, &hulls))
        {
            return false;
        }
        for (unsigned int h = 0; h < region->hullCount; ++h)
        {
            if (!WorldArray(writer, hulls + h * sizeof(GfxLightRegionHull) + offsetof(GfxLightRegionHull, axis),
                            region->hulls[h].axis, region->hulls[h].axisCount, sizeof(GfxLightRegionAxis), 15))
            {
                return false;
            }
        }
    }
    for (int view = 0; view < 3; ++view)
    {
        WORLD_RUNTIME(dpvs.smodelVisData[view], input->dpvs.smodelCount, uint8_t, 0);
    }
    for (int view = 0; view < 3; ++view)
    {
        WORLD_RUNTIME(dpvs.surfaceVisData[view], input->dpvs.staticSurfaceCount, uint8_t, 0);
    }
    WORLD_RUNTIME(dpvs.lodData, 2 * layout.smodelVisWords, uint, 127);
    WORLD_ARRAY(dpvs.sortedSurfIndex, input->dpvs.staticSurfaceCount + input->dpvs.staticSurfaceCountNoDecal, uint16_t,
                1);
    WORLD_ARRAY(dpvs.smodelInsts, input->dpvs.smodelCount, GfxStaticModelInst, 15);
    if (!WorldArray(writer, header + offsetof(GfxWorld, dpvs.surfaces), input->dpvs.surfaces, input->surfaceCount,
                    sizeof(GfxSurface), 15, &children))
    {
        return false;
    }
    for (int i = 0; i < input->surfaceCount; ++i)
    {
        if (!WorldAssetReference(writer, children + i * sizeof(GfxSurface) + offsetof(GfxSurface, material),
                                 ASSET_TYPE_MATERIAL, input->dpvs.surfaces[i].material))
        {
            return false;
        }
    }
    WORLD_ARRAY(dpvs.cullGroups, input->cullGroupCount, GfxCullGroup, 15);
    if (!WorldArray(writer, header + offsetof(GfxWorld, dpvs.smodelDrawInsts), input->dpvs.smodelDrawInsts,
                    input->dpvs.smodelCount, sizeof(GfxStaticModelDrawInst), 15, &children))
    {
        return false;
    }
    for (unsigned int i = 0; i < input->dpvs.smodelCount; ++i)
    {
        if (!WorldAssetReference(
                writer, children + i * sizeof(GfxStaticModelDrawInst) + offsetof(GfxStaticModelDrawInst, model),
                ASSET_TYPE_XMODEL, input->dpvs.smodelDrawInsts[i].model))
        {
            return false;
        }
    }
    WORLD_RUNTIME(dpvs.surfaceMaterials, input->dpvs.staticSurfaceCount, GfxDrawSurf, 15);
    WORLD_RUNTIME(dpvs.surfaceCastsSunShadow, layout.surfaceVisWords, uint, 127);
    WORLD_RUNTIME(dpvsDyn.dynEntCellBits[0], layout.dynEntCellWords[0], uint, 15);
    WORLD_RUNTIME(dpvsDyn.dynEntCellBits[1], layout.dynEntCellWords[1], uint, 15);
    for (int view = 0; view < 3; ++view)
    {
        WORLD_RUNTIME(dpvsDyn.dynEntVisData[0][view], layout.dynEntVisBytes[0], uint8_t, 15);
        WORLD_RUNTIME(dpvsDyn.dynEntVisData[1][view], layout.dynEntVisBytes[1], uint8_t, 15);
    }
#undef WORLD_ARRAY
#undef WORLD_RUNTIME
#undef WORLD_ASSET
    writer->position[0] = start;
    return true;
}

static bool MenuItemData(NativeWriter *writer, size_t field, const itemDef_s *item)
{
    MenuPointer(writer, field, item->typeData.data ? UINTPTR_MAX : 0);
    if (!item->typeData.data)
    {
        return true;
    }
    if (item->dataType == 13)
    {
        return String(writer, item->typeData.enumDvarName);
    }
    size_t size;
    switch (item->dataType)
    {
    case 6:
        if (!DB64_ValidateMenuListBox(item->typeData.listBox))
        {
            return Fail(writer, "Invalid native menu list-box");
        }
        size = sizeof(listBoxDef_s);
        break;
    case 12:
        if (!DB64_ValidateMenuMulti(item->typeData.multi))
        {
            return Fail(writer, "Invalid native menu multi-choice data");
        }
        size = sizeof(multiDef_s);
        break;
    case 0: case 4: case 9: case 10: case 11: case 14: case 16: case 17: case 18:
        size = sizeof(editFieldDef_s);
        break;
    default:
        if (writer->errorSize)
        {
            snprintf(writer->error, writer->errorSize, "Unexpected native menu item data: %s (type %d, dataType %d)",
                     item->window.name ? item->window.name : "unnamed", item->type, item->dataType);
        }
        return false;
    }
    if (!Align(writer) || !Write(writer, item->typeData.data, size))
    {
        return false;
    }
    const size_t header = writer->size - size;
    if (item->dataType == 6)
    {
        return MenuString(writer, header + offsetof(listBoxDef_s, doubleClick), item->typeData.listBox->doubleClick) &&
               MenuMaterial(writer, header + offsetof(listBoxDef_s, selectIcon), item->typeData.listBox->selectIcon);
    }
    if (item->dataType == 12)
    {
        for (int i = 0; i < 32; ++i)
        {
            if (!MenuString(writer, header + offsetof(multiDef_s, dvarList) + i * sizeof(const char *),
                            item->typeData.multi->dvarList[i]))
            {
                return false;
            }
        }
        for (int i = 0; i < 32; ++i)
        {
            if (!MenuString(writer, header + offsetof(multiDef_s, dvarStr) + i * sizeof(const char *),
                            item->typeData.multi->dvarStr[i]))
            {
                return false;
            }
        }
    }
    return true;
}

static bool MenuItem(NativeWriter *writer, const itemDef_s *item)
{
    if (!item || !Align(writer) || !Write(writer, item, sizeof(itemDef_s)))
    {
        return Fail(writer, "Invalid native menu item");
    }
    const size_t header = writer->size - sizeof(itemDef_s);
    Linker_ClearPadding_itemDef_s(writer->data + header);
    MenuPointer(writer, header + offsetof(itemDef_s, parent), 0);
    if (!MenuWindow(writer, header + offsetof(itemDef_s, window), &item->window))
    {
        return false;
    }
#define MENU_ITEM_STRING(field) \
    if (!MenuString(writer, header + offsetof(itemDef_s, field), item->field)) { return false; }
    MENU_ITEM_STRING(text);
    MENU_ITEM_STRING(mouseEnterText);
    MENU_ITEM_STRING(mouseExitText);
    MENU_ITEM_STRING(mouseEnter);
    MENU_ITEM_STRING(mouseExit);
    MENU_ITEM_STRING(action);
    MENU_ITEM_STRING(onAccept);
    MENU_ITEM_STRING(onFocus);
    MENU_ITEM_STRING(leaveFocus);
    MENU_ITEM_STRING(dvar);
    MENU_ITEM_STRING(dvarTest);
    if (!MenuKeys(writer, header + offsetof(itemDef_s, onKey), item->onKey))
    {
        return false;
    }
    MENU_ITEM_STRING(enableDvar);
#undef MENU_ITEM_STRING
    MenuPointer(writer, header + offsetof(itemDef_s, focusSound),
                item->focusSound ? ReferenceToken(writer, ASSET_TYPE_SOUND, item->focusSound) : 0);
    if ((item->focusSound && !SoundAliases(writer, item->focusSound)) ||
        !MenuItemData(writer, header + offsetof(itemDef_s, typeData), item))
    {
        return false;
    }
#define MENU_ITEM_STATEMENT(field) \
    if (!MenuStatement(writer, header + offsetof(itemDef_s, field), &item->field)) { return false; }
    MENU_ITEM_STATEMENT(visibleExp);
    MENU_ITEM_STATEMENT(textExp);
    MENU_ITEM_STATEMENT(materialExp);
    MENU_ITEM_STATEMENT(rectXExp);
    MENU_ITEM_STATEMENT(rectYExp);
    MENU_ITEM_STATEMENT(rectWExp);
    MENU_ITEM_STATEMENT(rectHExp);
    MENU_ITEM_STATEMENT(forecolorAExp);
#undef MENU_ITEM_STATEMENT
    return true;
}

static bool MenuAsset(NativeWriter *writer, XAssetType type, const void *data)
{
    if (!data || (type == ASSET_TYPE_MENU ? !DB64_ValidateMenuHeader((const menuDef_t *)data) :
                                          !DB64_ValidateMenuListHeader((const MenuList *)data)))
    {
        return Fail(writer, "Invalid native menu asset");
    }
    if (ReferenceToken(writer, type, data) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, type, data);
    }
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, type, data))
    {
        return false;
    }
    const size_t header = writer->size;
    if (!Write(writer, data, type == ASSET_TYPE_MENU ? sizeof(menuDef_t) : sizeof(MenuList)))
    {
        return false;
    }
    if (type == ASSET_TYPE_MENU)
    {
        Linker_ClearPadding_menuDef_t(writer->data + header);
    }
    writer->block = 4;
    if (type == ASSET_TYPE_MENULIST)
    {
        const MenuList *list = (const MenuList *)data;
        if (!MenuString(writer, header + offsetof(MenuList, name), list->name))
        {
            return false;
        }
        MenuPointer(writer, header + offsetof(MenuList, menus), list->menus ? UINTPTR_MAX : 0);
        if (list->menus)
        {
            if (!Align(writer))
            {
                return false;
            }
            const size_t pointers = writer->size;
            if (!Write(writer, list->menus, list->menuCount * sizeof(menuDef_t *)))
            {
                return false;
            }
            for (int i = 0; i < list->menuCount; ++i)
            {
                MenuPointer(writer, pointers + i * sizeof(menuDef_t *), ReferenceToken(writer, ASSET_TYPE_MENU, list->menus[i]));
                if (!MenuAsset(writer, ASSET_TYPE_MENU, list->menus[i]))
                {
                    return false;
                }
            }
        }
    }
    else
    {
        const menuDef_t *menu = (const menuDef_t *)data;
        if (!MenuWindow(writer, header + offsetof(menuDef_t, window), &menu->window))
        {
            return false;
        }
#define MENU_STRING(field) \
        if (!MenuString(writer, header + offsetof(menuDef_t, field), menu->field)) { return false; }
#define MENU_STATEMENT(field) \
        if (!MenuStatement(writer, header + offsetof(menuDef_t, field), &menu->field)) { return false; }
        MENU_STRING(font);
        MENU_STRING(onOpen);
        MENU_STRING(onClose);
        MENU_STRING(onESC);
        if (!MenuKeys(writer, header + offsetof(menuDef_t, onKey), menu->onKey))
        {
            return false;
        }
        MENU_STATEMENT(visibleExp);
        MENU_STRING(allowedBinding);
        MENU_STRING(soundName);
        MENU_STATEMENT(rectXExp);
        MENU_STATEMENT(rectYExp);
#undef MENU_STRING
#undef MENU_STATEMENT
        MenuPointer(writer, header + offsetof(menuDef_t, items), menu->items ? UINTPTR_MAX : 0);
        if (menu->items)
        {
            if (!Align(writer))
            {
                return false;
            }
            const size_t pointers = writer->size;
            if (!Write(writer, menu->items, menu->itemCount * sizeof(itemDef_s *)))
            {
                return false;
            }
            for (int i = 0; i < menu->itemCount; ++i)
            {
                MenuPointer(writer, pointers + i * sizeof(itemDef_s *), UINTPTR_MAX);
                if (!MenuItem(writer, menu->items[i]))
                {
                    return false;
                }
            }
        }
    }
    writer->position[0] = start;
    return true;
}

static bool WeaponGraph(NativeWriter *writer, size_t field, const float (*knots)[2], int count)
{
    MenuPointer(writer, field, knots ? UINTPTR_MAX : 0);
    for (int i = 0; i < count; ++i)
    {
        if (!isfinite(knots[i][0]) || !isfinite(knots[i][1]))
        {
            return Fail(writer, "Invalid native weapon accuracy graph knot");
        }
    }
    return !knots || (Align(writer) && Write(writer, knots, count * sizeof(float[2])));
}

static bool WeaponAsset(NativeWriter *writer, const WeaponDef *input)
{
    if (!DB64_ValidateWeaponHeader(input) || !input->szInternalName[0])
    {
        return Fail(writer, "Invalid native weapon header");
    }
    if (ReferenceToken(writer, ASSET_TYPE_WEAPON, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_WEAPON, input);
    }
    const uint16_t *tags[] = {input->hideTags, input->notetrackSoundMapKeys, input->notetrackSoundMapValues};
    const int counts[] = {8, 16, 16};
    for (int group = 0; group < 3; ++group)
    {
        for (int i = 0; i < counts[group]; ++i)
        {
            if (tags[group][i] && (!writer->strings || tags[group][i] >= writer->strings->count ||
                                    !writer->strings->strings[tags[group][i]]))
            {
                return Fail(writer, "Native weapon tag is not a zone script-string index");
            }
        }
    }
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_WEAPON, input))
    {
        return false;
    }
    const size_t header = writer->size;
    if (!Write(writer, input, sizeof(WeaponDef)))
    {
        return false;
    }
    Linker_ClearPadding_WeaponDef(writer->data + header);
    writer->block = 4;
    for (int i = 0; i < ARRAY_COUNT(db64WeaponFields); ++i)
    {
        const DB64WeaponField *field = &db64WeaponFields[i];
        for (int j = 0; j < field->count; ++j)
        {
            const size_t offset = field->offset + j * sizeof(void *);
            const void *value;
            memcpy(&value, (const uint8_t *)input + offset, sizeof(void *));
            if (field->type == DB64_WEAPON_STRING)
            {
                if (!MenuString(writer, header + offset, (const char *)value))
                {
                    return false;
                }
                continue;
            }
            const XAssetType type = field->type == DB64_WEAPON_MODEL ? ASSET_TYPE_XMODEL :
                                    field->type == DB64_WEAPON_MATERIAL ? ASSET_TYPE_MATERIAL :
                                    field->type == DB64_WEAPON_EFFECT ? ASSET_TYPE_FX : ASSET_TYPE_SOUND;
            MenuPointer(writer, header + offset, value ? ReferenceToken(writer, type, value) : 0);
            if (value)
            {
                const bool ok = type == ASSET_TYPE_XMODEL ? ModelAsset(writer, (const XModel *)value) :
                                type == ASSET_TYPE_MATERIAL ? MaterialAsset(writer, (const Material *)value) :
                                type == ASSET_TYPE_FX ? EffectAsset(writer, (const FxEffectDef *)value) :
                                SoundAliases(writer, (const snd_alias_list_t *)value);
                if (!ok)
                {
                    return false;
                }
            }
        }
    }
    MenuPointer(writer, header + offsetof(WeaponDef, bounceSound), input->bounceSound ? UINTPTR_MAX : 0);
    if (input->bounceSound)
    {
        if (!Align(writer) || !Write(writer, input->bounceSound, 29 * sizeof(snd_alias_list_t *)))
        {
            return false;
        }
        const size_t sounds = writer->size - 29 * sizeof(snd_alias_list_t *);
        for (int i = 0; i < 29; ++i)
        {
            const snd_alias_list_t *sound = input->bounceSound[i];
            MenuPointer(writer, sounds + i * sizeof(snd_alias_list_t *),
                        sound ? ReferenceToken(writer, ASSET_TYPE_SOUND, sound) : 0);
            if (sound && !SoundAliases(writer, sound))
            {
                return false;
            }
        }
    }
    for (int i = 0; i < 2; ++i)
    {
        if (!WeaponGraph(writer, header + offsetof(WeaponDef, accuracyGraphKnots) + i * sizeof(float (*)[2]),
                         input->accuracyGraphKnots[i], input->accuracyGraphKnotCount[i]) ||
            !WeaponGraph(writer, header + offsetof(WeaponDef, originalAccuracyGraphKnots) + i * sizeof(float (*)[2]),
                         input->originalAccuracyGraphKnots[i], input->originalAccuracyGraphKnotCount[i]))
        {
            return false;
        }
    }
    writer->position[0] = start;
    return true;
}

static bool ImpactAsset(NativeWriter *writer, const FxImpactTable *input)
{
    if (!input || !input->name || input->name[0] || !input->table)
    {
        return Fail(writer, "Invalid native impact-effect table");
    }
    if (ReferenceToken(writer, ASSET_TYPE_IMPACT_FX, input) != UINTPTR_MAX - 1)
    {
        return WriteExternalAsset(writer, ASSET_TYPE_IMPACT_FX, input);
    }
    FxImpactTable output = {StringToken(input->name), (FxImpactEntry *)UINTPTR_MAX};
    writer->block = 0;
    const size_t start = writer->position[0];
    if (!Align(writer) || !InsertReference(writer, ASSET_TYPE_IMPACT_FX, input) ||
        !Write(writer, &output, sizeof(FxImpactTable)))
    {
        return false;
    }
    writer->block = 4;
    if (!String(writer, input->name) || !Align(writer) || !Write(writer, input->table, 12 * sizeof(FxImpactEntry)))
    {
        return false;
    }
    const size_t entries = writer->size - 12 * sizeof(FxImpactEntry);
    for (int entry = 0; entry < 12; ++entry)
    {
        for (int i = 0; i < 33; ++i)
        {
            const FxEffectDef *effect = i < 29 ? input->table[entry].nonflesh[i] : input->table[entry].flesh[i - 29];
            const size_t offset = i < 29 ? offsetof(FxImpactEntry, nonflesh) + i * sizeof(const FxEffectDef *) :
                                           offsetof(FxImpactEntry, flesh) + (i - 29) * sizeof(const FxEffectDef *);
            MenuPointer(writer, entries + entry * sizeof(FxImpactEntry) + offset,
                        effect ? ReferenceToken(writer, ASSET_TYPE_FX, effect) : 0);
            if (effect && !EffectAsset(writer, effect))
            {
                return false;
            }
        }
    }
    writer->position[0] = start;
    return true;
}

static bool Build(NativeWriter *writer, const XAsset *assets, int count, const ScriptStringList *strings)
{
    writer->zoneAssets = assets;
    writer->zoneAssetCount = count;
    for (int i = 0; i < count; ++i)
    {
        if (assets[i].type != ASSET_TYPE_FX)
        {
            continue;
        }
        const FxEffectDef *effect = assets[i].header.fx;
        if (!effect || !effect->name || !effect->name[0])
        {
            return Fail(writer, "Native effect has no name");
        }
        for (int j = 0; j < i; ++j)
        {
            if (assets[j].type == ASSET_TYPE_FX && assets[j].header.fx != effect &&
                !_stricmp(assets[j].header.fx->name, effect->name))
            {
                return Fail(writer, "Different native effects share the same name");
            }
        }
    }
    const char *mapName = NULL;
    unsigned int worldTypes = 0;
    for (int i = 0; i < count; ++i)
    {
        const char *name = NULL;
        unsigned int kind = 0;
        if (assets[i].type == ASSET_TYPE_MAP_ENTS && assets[i].header.mapEnts)
        {
            name = assets[i].header.mapEnts->name;
            kind = 1;
        }
        else if ((assets[i].type == ASSET_TYPE_CLIPMAP || assets[i].type == ASSET_TYPE_CLIPMAP_PVS) &&
                 assets[i].header.clipMap)
        {
            name = assets[i].header.clipMap->name;
            kind = 8;
        }
        else if (assets[i].type == ASSET_TYPE_COMWORLD && assets[i].header.comWorld)
        {
            name = assets[i].header.comWorld->name;
            kind = 2;
        }
        else if (assets[i].type == ASSET_TYPE_GAMEWORLD_MP && assets[i].header.gameWorldMp)
        {
            name = assets[i].header.gameWorldMp->name;
            kind = 4;
        }
        else if (assets[i].type == ASSET_TYPE_GFXWORLD && assets[i].header.gfxWorld)
        {
            name = assets[i].header.gfxWorld->name;
            kind = 16;
        }
        if (kind)
        {
            if (!name || !name[0] || (mapName && strcmp(mapName, name)) || (worldTypes & kind))
            {
                return Fail(writer, "A native zone must contain at most one map, with one asset of each world type");
            }
            mapName = name;
            worldTypes |= kind;
        }
    }
    XFile file = {};
    writer->strings = strings;
    XAssetList list = {};
    if (strings)
    {
        if (strings->count < 0 || strings->count > 65536 || (!!strings->strings != (strings->count != 0)) ||
            (strings->count && strings->strings[0]))
        {
            return Fail(writer, "Invalid native script-string table");
        }
        list.stringList.count = strings->count;
        list.stringList.strings = strings->count ? (const char **)UINTPTR_MAX : NULL;
    }
    list.assetCount = count;
    list.assets = count ? (XAsset *)UINTPTR_MAX : NULL;
    if (!Append(writer, &file, sizeof(XFile)) || !Append(writer, &list, sizeof(XAssetList)))
    {
        return false;
    }
    writer->block = 4;
    if (list.stringList.count)
    {
        if (!Align(writer))
        {
            return false;
        }
        for (int i = 0; i < strings->count; ++i)
        {
            const char *token = StringToken(strings->strings[i]);
            if (!Write(writer, &token, sizeof(const char *)))
            {
                return false;
            }
        }
        for (int i = 0; i < strings->count; ++i)
        {
            if (!String(writer, strings->strings[i]))
            {
                return false;
            }
        }
    }
    if (count && !Align(writer))
    {
        return false;
    }
    const size_t assetHeaders = writer->size;
    for (int i = 0; i < count; ++i)
    {
        XAsset asset = {};
        asset.type = assets[i].type;
        asset.header.data = (void *)UINTPTR_MAX;
        if (!Write(writer, &asset, sizeof(XAsset)))
        {
            return false;
        }
    }
    for (int i = 0; i < count; ++i)
    {
        if (ExternalAsset(writer, assets[i].type, assets[i].header.data))
        {
            const uintptr_t token = DB64_EXTERNAL_ASSET_TOKEN;
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            if (!WriteExternalAsset(writer, assets[i].type, assets[i].header.data))
            {
                return false;
            }
            continue;
        }
        if (!VisitNativeAsset(writer, assets[i].type, assets[i].header.data))
        {
            return Fail(writer, "Cannot record native asset index");
        }
        bool ok;
        switch (assets[i].type)
        {
        case ASSET_TYPE_GFXWORLD: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_GFXWORLD, assets[i].header.gfxWorld);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = RenderWorldAsset(writer, assets[i].header.gfxWorld);
            break;
        }
        case ASSET_TYPE_FX: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_FX, assets[i].header.fx);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = EffectAsset(writer, assets[i].header.fx);
            break;
        }
        case ASSET_TYPE_XMODELPIECES: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_XMODELPIECES, assets[i].header.xmodelPieces);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = ModelPiecesAsset(writer, assets[i].header.xmodelPieces);
            break;
        }
        case ASSET_TYPE_CLIPMAP:
        case ASSET_TYPE_CLIPMAP_PVS: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_CLIPMAP, assets[i].header.clipMap);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = ClipMapAsset(writer, assets[i].header.clipMap);
            break;
        }
        case ASSET_TYPE_FONT: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_FONT, assets[i].header.font);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = FontAsset(writer, assets[i].header.font);
            break;
        }
        case ASSET_TYPE_SOUND: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_SOUND, assets[i].header.sound);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = SoundAliases(writer, assets[i].header.sound);
            break;
        }
        case ASSET_TYPE_LOADED_SOUND: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_LOADED_SOUND, assets[i].header.loadSnd);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = SoundAsset(writer, assets[i].header.loadSnd);
            break;
        }
        case ASSET_TYPE_XANIMPARTS: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_XANIMPARTS, assets[i].header.parts);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = AnimationAsset(writer, assets[i].header.parts);
            break;
        }
        case ASSET_TYPE_XMODEL: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_XMODEL, assets[i].header.model);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = ModelAsset(writer, assets[i].header.model);
            break;
        }
        case ASSET_TYPE_RAWFILE:
            ok = Raw(writer, assets[i].header.rawfile);
            break;
        case ASSET_TYPE_LOCALIZE_ENTRY:
            ok = Localize(writer, assets[i].header.localize);
            break;
        case ASSET_TYPE_IMPACT_FX: {
            MenuPointer(writer, assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header),
                        ReferenceToken(writer, ASSET_TYPE_IMPACT_FX, assets[i].header.impactFx));
            ok = ImpactAsset(writer, assets[i].header.impactFx);
            break;
        }
        case ASSET_TYPE_WEAPON: {
            MenuPointer(writer, assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header),
                        ReferenceToken(writer, ASSET_TYPE_WEAPON, assets[i].header.weapon));
            ok = WeaponAsset(writer, assets[i].header.weapon);
            break;
        }
        case ASSET_TYPE_MENU:
        case ASSET_TYPE_MENULIST: {
            MenuPointer(writer, assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header),
                        ReferenceToken(writer, assets[i].type, assets[i].header.data));
            ok = MenuAsset(writer, assets[i].type, assets[i].header.data);
            break;
        }
        case ASSET_TYPE_SNDDRIVER_GLOBALS:
            ok = SoundDriverGlobals(writer, assets[i].header.sndDriverGlobals);
            break;
        case ASSET_TYPE_STRINGTABLE:
            ok = Table(writer, assets[i].header.stringTable);
            break;
        case ASSET_TYPE_MAP_ENTS:
            MenuPointer(writer, assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header),
                        ReferenceToken(writer, ASSET_TYPE_MAP_ENTS, assets[i].header.mapEnts));
            ok = Entities(writer, assets[i].header.mapEnts);
            break;
        case ASSET_TYPE_COMWORLD:
            ok = CommonWorld(writer, assets[i].header.comWorld);
            break;
        case ASSET_TYPE_GAMEWORLD_MP:
            ok = MultiplayerWorld(writer, assets[i].header.gameWorldMp);
            break;
        case ASSET_TYPE_PHYSPRESET:
        case ASSET_TYPE_SOUND_CURVE:
            MenuPointer(writer, assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header),
                        ReferenceToken(writer, assets[i].type, assets[i].header.data));
            ok = Preset(writer, assets[i].type, assets[i].header);
            break;
        case ASSET_TYPE_IMAGE: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_IMAGE, assets[i].header.image);
            const size_t headerOffset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + headerOffset, &token, sizeof(uintptr_t));
            ok = ImageAsset(writer, assets[i].header.image);
            break;
        }
        case ASSET_TYPE_LIGHT_DEF:
            ok = LightDefinition(writer, assets[i].header.lightDef);
            break;
        case ASSET_TYPE_TECHNIQUE_SET: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_TECHNIQUE_SET, assets[i].header.techniqueSet);
            const size_t headerOffset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + headerOffset, &token, sizeof(uintptr_t));
            ok = TechniqueSet(writer, assets[i].header.techniqueSet);
            break;
        }
        case ASSET_TYPE_MATERIAL: {
            const uintptr_t token = ReferenceToken(writer, ASSET_TYPE_MATERIAL, assets[i].header.material);
            const size_t offset = assetHeaders + i * sizeof(XAsset) + offsetof(XAsset, header);
            memcpy(writer->data + offset, &token, sizeof(uintptr_t));
            ok = MaterialAsset(writer, assets[i].header.material);
            break;
        }
        default:
            return Fail(writer, "Native serialization is not yet implemented for this asset type");
        }
        if (!ok)
        {
            return false;
        }
    }
    file.size = (uint)(writer->size - sizeof(XFile));
    for (int i = 0; i < 9; ++i)
    {
        file.blockSize[i] = (uint)writer->extent[i];
    }
    memcpy(writer->data, &file, sizeof(XFile));
    return true;
}

static bool AddEffectDependency(NativeWriter *writer, XAsset **assets, int *count, const FxEffectDef *effect)
{
    if (!effect)
    {
        return true;
    }
    for (int i = 0; i < *count; ++i)
    {
        if ((*assets)[i].type == ASSET_TYPE_FX && (*assets)[i].header.fx == effect)
        {
            return true;
        }
    }
    if (*count >= 32767)
    {
        return Fail(writer, "Native effect dependencies exceed the zone asset limit");
    }
    XAsset *next = (XAsset *)realloc(*assets, ((size_t)*count + 1) * sizeof(XAsset));
    if (!next)
    {
        return Fail(writer, "Out of memory collecting effect dependencies");
    }
    *assets = next;
    next[*count].type = ASSET_TYPE_FX;
    next[*count].header.fx = effect;
    ++*count;
    return true;
}

static bool CollectEffects(NativeWriter *writer, XAsset **assets, int *count)
{
    // Appending to this work list visits each newly discovered effect once.
    // Pointer identity terminates cycles without consuming the C call stack.
    for (int i = 0; i < *count; ++i)
    {
        if (ExternalAsset(writer, (*assets)[i].type, (*assets)[i].header.data))
        {
            continue;
        }
        if ((*assets)[i].type == ASSET_TYPE_IMPACT_FX)
        {
            const FxImpactTable *table = (*assets)[i].header.impactFx;
            if (!table || !table->name || table->name[0] || !table->table)
            {
                return Fail(writer, "Invalid native impact-effect dependency table");
            }
            for (int previous = 0; previous < i; ++previous)
            {
                if ((*assets)[previous].type == ASSET_TYPE_IMPACT_FX && (*assets)[previous].header.impactFx != table)
                {
                    return Fail(writer, "A native zone can contain only one impact-effect table");
                }
            }
            for (int entry = 0; entry < 12; ++entry)
            {
                for (int effect = 0; effect < 33; ++effect)
                {
                    const FxEffectDef *value = effect < 29 ? table->table[entry].nonflesh[effect] :
                                                             table->table[entry].flesh[effect - 29];
                    if (!AddEffectDependency(writer, assets, count, value))
                    {
                        return false;
                    }
                }
            }
            continue;
        }
        if ((*assets)[i].type == ASSET_TYPE_WEAPON)
        {
            const WeaponDef *weapon = (*assets)[i].header.weapon;
            if (!DB64_ValidateWeaponHeader(weapon))
            {
                return Fail(writer, "Invalid native weapon dependency header");
            }
            for (int field = 0; field < ARRAY_COUNT(db64WeaponFields); ++field)
            {
                if (db64WeaponFields[field].type == DB64_WEAPON_EFFECT)
                {
                    const FxEffectDef *effect;
                    memcpy(&effect, (const uint8_t *)weapon + db64WeaponFields[field].offset,
                           sizeof(const FxEffectDef *));
                    if (!AddEffectDependency(writer, assets, count, effect))
                    {
                        return false;
                    }
                }
            }
            continue;
        }
        if ((*assets)[i].type == ASSET_TYPE_CLIPMAP || (*assets)[i].type == ASSET_TYPE_CLIPMAP_PVS)
        {
            const clipMap_t *map = (*assets)[i].header.clipMap;
            if (!DB64_ValidateClipMapHeader(map, writer->error, writer->errorSize))
            {
                return false;
            }
            for (int group = 0; group < 2; ++group)
            {
                for (int entity = 0; entity < map->dynEntCount[group]; ++entity)
                {
                    if (!AddEffectDependency(writer, assets, count, map->dynEntDefList[group][entity].destroyFx))
                    {
                        return false;
                    }
                }
            }
            continue;
        }
        if ((*assets)[i].type != ASSET_TYPE_FX)
        {
            continue;
        }
        const FxEffectDef *effect = (*assets)[i].header.fx;
        size_t elementCount;
        if (!DB64_EffectElementCount(effect, &elementCount))
        {
            return Fail(writer, "Invalid native effect dependency");
        }
        for (size_t j = 0; j < elementCount; ++j)
        {
            const FxElemDef *element = &effect->elemDefs[j];
            if (!DB64_ValidateEffectElement(element))
            {
                return Fail(writer, "Invalid native effect dependency element");
            }
            if (!AddEffectDependency(writer, assets, count, element->effectOnImpact.handle) ||
                !AddEffectDependency(writer, assets, count, element->effectOnDeath.handle) ||
                !AddEffectDependency(writer, assets, count, element->effectEmitted.handle))
            {
                return false;
            }
            if (element->elemType == 10)
            {
                for (int k = 0; k < element->visualCount; ++k)
                {
                    const FxElemVisuals *visual =
                        element->visualCount > 1 ? &element->visuals.array[k] : &element->visuals.instance;
                    if (!AddEffectDependency(writer, assets, count, visual->effectDef.handle))
                    {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool Linker_BuildNativeZone(const XAsset *assets, int count, bool compress, uint8_t **data, size_t *size, char *error,
                            size_t errorSize)
{
    return Linker_BuildNativeZoneWithStrings(assets, count, NULL, compress, data, size, error, errorSize);
}

bool Linker_BuildNativeZoneWithStrings(const XAsset *assets, int count, const ScriptStringList *strings, bool compress,
                                       uint8_t **data, size_t *size, char *error, size_t errorSize,
                                       const LinkerExternalAsset *external, size_t externalCount,
                                       bool (*visit)(int, const char *, void *), void *context)
{
    if (!data || !size || (!error && errorSize))
    {
        return false;
    }
    *data = NULL;
    *size = 0;
    NativeWriter writer = {};
    writer.error = error;
    writer.errorSize = errorSize;
    if (errorSize)
    {
        error[0] = 0;
    }
    if (count < 0 || count >= 32768 || (count && !assets))
    {
        return Fail(&writer, "Invalid native asset count");
    }
    if (externalCount > 32768 || (externalCount && !external))
    {
        return Fail(&writer, "Invalid native external asset count");
    }
    for (size_t i = 0; i < externalCount; ++i)
    {
        if (!external[i].name || !external[i].name[0] ||
            external[i].name[0] == ',' || strlen(external[i].name) >= 1024 ||
            external[i].type < 0 || external[i].type >= ASSET_TYPE_COUNT ||
            external[i].type == ASSET_TYPE_CLIPMAP_PVS || external[i].type == ASSET_TYPE_GAMEWORLD_SP ||
            external[i].type == ASSET_TYPE_UI_MAP || external[i].type == ASSET_TYPE_AITYPE ||
            external[i].type == ASSET_TYPE_MPTYPE || external[i].type == ASSET_TYPE_CHARACTER ||
            external[i].type == ASSET_TYPE_XMODELALIAS)
        {
            return Fail(&writer, "Invalid native external asset binding");
        }
    }
    writer.external = external;
    writer.externalCount = externalCount;
    writer.visit = visit;
    writer.visitContext = context;
    XAsset *zoneAssets = count ? (XAsset *)malloc((size_t)count * sizeof(XAsset)) : NULL;
    if (count && !zoneAssets)
    {
        return Fail(&writer, "Out of memory collecting zone assets");
    }
    if (count)
    {
        memcpy(zoneAssets, assets, (size_t)count * sizeof(XAsset));
    }
    bool ok = CollectEffects(&writer, &zoneAssets, &count) && Build(&writer, zoneAssets, count, strings);
    free(zoneAssets);
    writer.zoneAssets = NULL;
    if (ok)
    {
        // zlib 1.1.4 has no compressBound; this is its documented safe bound.
        const size_t bound = writer.size + writer.size / 1000 + 64;
        if (bound > ULONG_MAX)
        {
            ok = Fail(&writer, "Compressed native zone exceeds zlib size limit");
        }
        else
        {
            uint8_t *output = (uint8_t *)malloc(sizeof(DB64FilePrefix) + bound);
            if (!output)
            {
                ok = Fail(&writer, "Out of memory compressing native zone");
            }
            else
            {
                DB64FilePrefix prefix = {};
                memcpy(prefix.magic, DB64_FASTFILE_MAGIC, sizeof(prefix.magic));
                prefix.version = DB64_FASTFILE_VERSION;
                memcpy(output, &prefix, sizeof(DB64FilePrefix));
                uLongf compressedSize = (uLongf)bound;
                if (compress2(output + sizeof(DB64FilePrefix), &compressedSize, writer.data, (uLong)writer.size,
                              compress ? Z_BEST_COMPRESSION : Z_NO_COMPRESSION) != Z_OK)
                {
                    free(output);
                    ok = Fail(&writer, "Native zone compression failed");
                }
                else
                {
                    *data = output;
                    *size = sizeof(DB64FilePrefix) + compressedSize;
                }
            }
        }
    }
    free(writer.data);
    free(writer.references);
    return ok;
}
