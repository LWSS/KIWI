#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_utils.h>
#include "native_technique.h"
#include "native_shader.h"
#include <gfx_d3d/r_material_constant_sources.h>
#include <gfx_d3d/r_material_sampler_sources.h>
#include <gfx_d3d/r_material_state_sources.h>
#include <d3dx9shader.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *s_techniqueLabels[34] = {"depth prepass",
                                            "build floatz",
                                            "build shadowmap depth",
                                            "build shadowmap color",
                                            "unlit",
                                            "emissive",
                                            "emissive shadow",
                                            "lit",
                                            "lit sun",
                                            "lit sun shadow",
                                            "lit spot",
                                            "lit spot shadow",
                                            "lit omni",
                                            "lit omni shadow",
                                            "lit instanced",
                                            "lit instanced sun",
                                            "lit instanced sun shadow",
                                            "lit instanced spot",
                                            "lit instanced spot shadow",
                                            "lit instanced omni",
                                            "lit instanced omni shadow",
                                            "light spot",
                                            "light omni",
                                            "light spot shadow",
                                            "fakelight normal",
                                            "fakelight view",
                                            "sunlight preview",
                                            "case texture",
                                            "solid wireframe",
                                            "shaded wireframe",
                                            "shadowcookie caster",
                                            "shadowcookie receiver",
                                            "debug bumpmap",
                                            "debug bumpmap instanced"};
struct TechniqueCursor
{
    const char *data;
    size_t size;
    size_t position;
};
static bool TechniqueToken(TechniqueCursor *cursor, char *token, size_t capacity, bool *quoted)
{
    *quoted = false;
    token[0] = 0;
    while (cursor->position < cursor->size)
    {
        const size_t p = cursor->position;
        const char c = cursor->data[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            ++cursor->position;
            continue;
        }
        if (c == '/' && p + 1 < cursor->size && cursor->data[p + 1] == '/')
        {
            cursor->position += 2;
            while (cursor->position < cursor->size && cursor->data[cursor->position] != '\n')
            {
                ++cursor->position;
            }
            continue;
        }
        if (c == '/' && p + 1 < cursor->size && cursor->data[p + 1] == '*')
        {
            cursor->position += 2;
            while (cursor->position + 1 < cursor->size &&
                   !(cursor->data[cursor->position] == '*' && cursor->data[cursor->position + 1] == '/'))
            {
                ++cursor->position;
            }
            if (cursor->position + 1 >= cursor->size)
            {
                return false;
            }
            cursor->position += 2;
            continue;
        }
        break;
    }
    if (cursor->position == cursor->size)
    {
        return true;
    }
    *quoted = cursor->data[cursor->position] == '"';
    if (*quoted)
    {
        ++cursor->position;
    }
    size_t length = 0;
    while (cursor->position < cursor->size)
    {
        const char c = cursor->data[cursor->position];
        if (*quoted && c == '"')
        {
            ++cursor->position;
            token[length] = 0;
            return length != 0;
        }
        if (!*quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == '&' || c == '=' ||
                         c == ':' || c == ';' || c == '{' || c == '}' || c == '"'))
        {
            if (!length && (c == ',' || c == '&' || c == '=' || c == ':' || c == ';' || c == '{' || c == '}'))
            {
                token[length++] = c;
                ++cursor->position;
            }
            break;
        }
        if (length + 1 >= capacity || !c || c == '\r' || c == '\n')
        {
            return false;
        }
        token[length++] = c;
        ++cursor->position;
    }
    token[length] = 0;
    return !*quoted && length != 0;
}

bool Linker_ReadTechniqueSet(const void *data, size_t size, LinkerTechniqueSetSource *source, char *error,
                             size_t errorSize)
{
    if (!source || (!error && errorSize))
    {
        return false;
    }
    memset(source, 0, sizeof(LinkerTechniqueSetSource));
    TechniqueCursor cursor = {(const char *)data, size, 0};
    LinkerTechniqueSetSource result = {};
    bool valid = data && size && !memchr(data, 0, size);
    int labels[34];
    int labelCount = 0;
    char token[256];
    bool quoted;
    while (valid)
    {
        valid = TechniqueToken(&cursor, token, sizeof(token), &quoted);
        if (!valid || !token[0])
        {
            break;
        }
        if (quoted)
        {
            int label = 0;
            while (label < 34 && strcmp(token, s_techniqueLabels[label]))
            {
                ++label;
            }
            if (label == 34 || labelCount == 34)
            {
                valid = false;
                break;
            }
            labels[labelCount++] = label;
            valid = TechniqueToken(&cursor, token, sizeof(token), &quoted) && !quoted && !strcmp(token, ":");
        }
        else
        {
            if (!labelCount || !strcmp(token, ";") || !strcmp(token, ":"))
            {
                valid = false;
                break;
            }
            for (int i = 0; i < labelCount; ++i)
            {
                strcpy(result.techniques[labels[i]], token);
            }
            labelCount = 0;
            valid = TechniqueToken(&cursor, token, sizeof(token), &quoted) && !quoted && !strcmp(token, ";");
        }
    }
    valid = valid && !labelCount;
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid technique-set source near byte %zu", cursor.position);
        }
        return false;
    }
    *source = result;
    return true;
}

static bool TechniqueExpect(TechniqueCursor *cursor, const char *expected)
{
    char token[256];
    bool quoted;
    return TechniqueToken(cursor, token, sizeof(token), &quoted) && !quoted && !strcmp(token, expected);
}
static bool TechniqueBlock(TechniqueCursor *cursor, size_t *begin, size_t *size)
{
    if (!TechniqueExpect(cursor, "{"))
    {
        return false;
    }
    *begin = cursor->position;
    int depth = 1;
    while (depth)
    {
        const size_t before = cursor->position;
        char token[256];
        bool quoted;
        if (!TechniqueToken(cursor, token, sizeof(token), &quoted) || !token[0])
        {
            return false;
        }
        if (!quoted && !strcmp(token, "{"))
        {
            ++depth;
        }
        if (!quoted && !strcmp(token, "}"))
        {
            --depth;
            if (!depth)
            {
                *size = before - *begin;
            }
        }
    }
    return true;
}
static bool TechniqueShader(TechniqueCursor *cursor, const char *kind, char *name, int *version, size_t *begin,
                            size_t *size)
{
    char token[256];
    bool quoted;
    if (!TechniqueExpect(cursor, kind) || !TechniqueToken(cursor, token, sizeof(token), &quoted) || quoted)
    {
        return false;
    }
    if (!strcmp(token, "1.1"))
    {
        *version = 11;
    }
    else if (!strcmp(token, "2.0"))
    {
        *version = 20;
    }
    else if (!strcmp(token, "3.0"))
    {
        *version = 30;
    }
    else
    {
        return false;
    }
    return TechniqueToken(cursor, name, 256, &quoted) && quoted && TechniqueBlock(cursor, begin, size);
}

bool Linker_ReadTechnique(const void *data, size_t size, LinkerTechniqueSource **source, char *error, size_t errorSize)
{
    if (!source || (!error && errorSize))
    {
        return false;
    }
    *source = NULL;
    if (!data || !size || size > 16 * 1024 * 1024 || memchr(data, 0, size))
    {
        return false;
    }
    LinkerTechniqueSource *result = (LinkerTechniqueSource *)calloc(1, sizeof(LinkerTechniqueSource));
    if (!result)
    {
        return false;
    }
    TechniqueCursor cursor = {(const char *)data, size, 0};
    bool valid = true;
    char token[256];
    bool quoted;
    while (valid)
    {
        valid = TechniqueToken(&cursor, token, sizeof(token), &quoted);
        if (!valid || !token[0])
        {
            break;
        }
        if (quoted || strcmp(token, "{") || result->passCount == 32)
        {
            valid = false;
            break;
        }
        LinkerTechniquePassSource *pass = &result->passes[result->passCount++];
        valid = TechniqueExpect(&cursor, "stateMap") &&
                TechniqueToken(&cursor, pass->stateMap, sizeof(pass->stateMap), &quoted) && quoted &&
                TechniqueExpect(&cursor, ";") &&
                TechniqueShader(&cursor, "vertexShader", pass->vertexShader, &pass->vertexVersion,
                                &pass->vertexBindingsBegin, &pass->vertexBindingsSize) &&
                TechniqueShader(&cursor, "pixelShader", pass->pixelShader, &pass->pixelVersion,
                                &pass->pixelBindingsBegin, &pass->pixelBindingsSize);
        pass->routingBegin = cursor.position;
        bool closed = false;
        while (valid)
        {
            const size_t before = cursor.position;
            valid = TechniqueToken(&cursor, token, sizeof(token), &quoted) && token[0];
            if (!valid)
            {
                break;
            }
            if (!quoted && !strcmp(token, "}"))
            {
                pass->routingSize = before - pass->routingBegin;
                closed = true;
                break;
            }
            if (!quoted && !strcmp(token, "{"))
            {
                valid = false;
            }
        }
        valid = valid && closed;
    }
    valid = valid && result->passCount;
    if (valid)
    {
        result->text = (char *)malloc(size + 1);
        valid = result->text != NULL;
        if (valid)
        {
            memcpy(result->text, data, size);
            result->text[size] = 0;
            result->textSize = size;
        }
    }
    if (!valid)
    {
        free(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid technique pass source near byte %zu", cursor.position);
        }
        return false;
    }
    *source = result;
    return true;
}

bool Linker_ReadTechniqueBindings(const void *data, size_t size, LinkerTechniqueBindings *bindings, char *error,
                                  size_t errorSize)
{
    if (!bindings || (!error && errorSize))
    {
        return false;
    }
    memset(bindings, 0, sizeof(LinkerTechniqueBindings));
    if ((!data && size) || (size && memchr(data, 0, size)))
    {
        return false;
    }
    TechniqueCursor cursor = {(const char *)data, size, 0};
    LinkerTechniqueBindings *result = (LinkerTechniqueBindings *)calloc(1, sizeof(LinkerTechniqueBindings));
    if (!result)
    {
        return false;
    }
    bool valid = true;
    char token[256];
    bool quoted;
    while (valid)
    {
        valid = TechniqueToken(&cursor, token, sizeof(token), &quoted);
        if (!valid || !token[0])
        {
            break;
        }
        if (quoted || result->count == 64 || strpbrk(token, "{}:;="))
        {
            valid = false;
            break;
        }
        LinkerTechniqueBinding *binding = &result->entries[result->count++];
        strcpy(binding->destination, token);
        valid = TechniqueExpect(&cursor, "=");
        const size_t begin = cursor.position;
        bool expression = false, closed = false;
        while (valid)
        {
            const size_t before = cursor.position;
            valid = TechniqueToken(&cursor, token, sizeof(token), &quoted) && token[0];
            if (!valid)
            {
                break;
            }
            if (!quoted && !strcmp(token, ";"))
            {
                size_t first = begin, end = before;
                while (first < end && (cursor.data[first] == ' ' || cursor.data[first] == '\t' ||
                                       cursor.data[first] == '\r' || cursor.data[first] == '\n'))
                {
                    ++first;
                }
                while (end > first && (cursor.data[end - 1] == ' ' || cursor.data[end - 1] == '\t' ||
                                       cursor.data[end - 1] == '\r' || cursor.data[end - 1] == '\n'))
                {
                    --end;
                }
                valid = expression && end - first < sizeof(binding->expression);
                if (valid)
                {
                    memcpy(binding->expression, cursor.data + first, end - first);
                }
                closed = true;
                break;
            }
            valid = quoted || (!strpbrk(token, "{}:;="));
            expression = true;
        }
        valid = valid && closed;
    }
    if (valid)
    {
        *bindings = *result;
    }
    else if (errorSize)
    {
        snprintf(error, errorSize, "Invalid technique binding near byte %zu", cursor.position);
    }
    free(result);
    return valid;
}

bool Linker_BuildVertexRouting(const LinkerTechniqueBindings *bindings, const int *resourceDest,
                               MaterialVertexDeclaration *declaration, char *error, size_t errorSize)
{
    if (!declaration || (!error && errorSize))
    {
        return false;
    }
    memset(declaration, 0, sizeof(MaterialVertexDeclaration));
    static const char *destinations[12] = {"vertex.position",    "vertex.normal",      "vertex.color[0]",
                                           "vertex.color[1]",    "vertex.texcoord[0]", "vertex.texcoord[1]",
                                           "vertex.texcoord[2]", "vertex.texcoord[3]", "vertex.texcoord[4]",
                                           "vertex.texcoord[5]", "vertex.texcoord[6]", "vertex.texcoord[7]"};
    static const char *sources[9] = {
        "code.position",          "code.color",       "code.texcoord[0]", "code.normal",
        "code.tangent",           "code.texcoord[1]", "code.texcoord[2]", "code.normalTransform[0]",
        "code.normalTransform[1]"};
    bool assigned[12] = {};
    bool registers[16] = {};
    bool valid = bindings && resourceDest && bindings->count && bindings->count <= 16;
    MaterialVertexDeclaration result = {};
    for (unsigned int i = 0; valid && i < bindings->count; ++i)
    {
        const LinkerTechniqueBinding *binding = &bindings->entries[i];
        int dest = 0, source = 0;
        while (dest < 12 && strcmp(binding->destination, destinations[dest]))
        {
            ++dest;
        }
        while (source < 9 && strcmp(binding->expression, sources[source]))
        {
            ++source;
        }
        if (dest == 12 || source == 9 || assigned[dest] || resourceDest[dest] < 0 || resourceDest[dest] >= 16)
        {
            valid = false;
            break;
        }
        const int reg = resourceDest[dest];
        if (registers[reg])
        {
            valid = false;
            break;
        }
        assigned[dest] = true;
        registers[reg] = true;
        unsigned int insert = result.streamCount;
        while (insert &&
               (result.routing.data[insert - 1].source > source ||
                (result.routing.data[insert - 1].source == source && result.routing.data[insert - 1].dest > dest)))
        {
            result.routing.data[insert] = result.routing.data[insert - 1];
            --insert;
        }
        result.routing.data[insert].source = (uint8_t)source;
        result.routing.data[insert].dest = (uint8_t)dest;
        ++result.streamCount;
        result.hasOptionalSource = result.hasOptionalSource || source >= 5;
    }
    if (valid)
    {
        for (int i = 0; i < 12; ++i)
        {
            valid = valid && resourceDest[i] >= -1 && (resourceDest[i] == -1 || assigned[i]);
        }
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid, duplicate or missing vertex stream mapping");
        }
        return false;
    }
    *declaration = result;
    return true;
}

bool Linker_ResolveMaterialArgument(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                                    bool pixelShader, MaterialShaderArgument *argument, char *error, size_t errorSize)
{
    if (!argument || (!error && errorSize))
    {
        return false;
    }
    memset(argument, 0, sizeof(MaterialShaderArgument));
    bool valid = binding && parameter && !strcmp(binding->destination, parameter->name) &&
                 !strncmp(binding->expression, "material.", 9) && binding->expression[9] &&
                 parameter->registerCount == 1 && parameter->elements == 1;
    const char *name = valid ? binding->expression + 9 : NULL;
    uint32_t hash = 0;
    if (valid)
    {
        for (const char *p = name; *p; ++p)
        {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            {
                valid = false;
                break;
            }
            hash = (uint32_t)((int)*p | 0x20) ^ (33 * hash);
        }
    }
    MaterialShaderArgument result = {};
    if (valid)
    {
        if (parameter->registerSet == D3DXRS_SAMPLER)
        {
            valid = pixelShader && parameter->registerIndex < 16;
            result.type = MTL_ARG_MATERIAL_PIXEL_SAMPLER;
        }
        else if (parameter->registerSet == D3DXRS_FLOAT4)
        {
            valid = parameter->rows == 1 && parameter->columns >= 1 && parameter->columns <= 4 &&
                    parameter->registerIndex < (pixelShader ? 224 : 256) &&
                    (parameter->parameterClass == D3DXPC_SCALAR || parameter->parameterClass == D3DXPC_VECTOR);
            result.type = pixelShader ? MTL_ARG_MATERIAL_PIXEL_CONST : MTL_ARG_MATERIAL_VERTEX_CONST;
        }
        else
        {
            valid = false;
        }
        result.dest = (uint16_t)parameter->registerIndex;
        result.u.nameHash = hash;
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unsupported material shader argument");
        }
        return false;
    }
    *argument = result;
    return true;
}

static bool FindCodeConstant(const char *name, const CodeConstantSource *table, unsigned int *index)
{
    const size_t length = strcspn(name, ".[ ");
    if (!length)
    {
        return false;
    }
    for (unsigned int i = 0; table[i].name; ++i)
    {
        const CodeConstantSource *entry = &table[i];
        if (strlen(entry->name) != length || strncmp(name, entry->name, length))
        {
            continue;
        }
        const char *rest = name + length;
        if (entry->subtable)
        {
            return *rest == '.' && FindCodeConstant(rest + 1, entry->subtable, index);
        }
        unsigned int offset = 0;
        if (entry->arrayCount)
        {
            if (*rest++ != '[' || *rest < '0' || *rest > '9')
            {
                return false;
            }
            while (*rest >= '0' && *rest <= '9')
            {
                offset = offset * 10 + (*rest++ - '0');
                if (offset >= (unsigned int)entry->arrayCount)
                {
                    return false;
                }
            }
            if (*rest++ != ']')
            {
                return false;
            }
        }
        if (*rest)
        {
            return false;
        }
        *index = entry->source + offset * entry->arrayStride;
        return true;
    }
    return false;
}

bool Linker_FindCodeConstant(const char *expression, unsigned int *index)
{
    if (!index)
    {
        return false;
    }
    *index = 0;
    return expression && !strncmp(expression, "code.", 5) && FindCodeConstant(expression + 5, s_codeConsts, index);
}

static bool ResolveCodeIndex(unsigned int index, const LinkerShaderConstant *parameter, bool pixelShader,
                             MaterialShaderArgument *argument, char *error, size_t errorSize)
{
    if (!argument || (!error && errorSize))
    {
        return false;
    }
    memset(argument, 0, sizeof(MaterialShaderArgument));
    bool valid = parameter && index < 90 && parameter->registerSet == D3DXRS_FLOAT4 && parameter->elements == 1 &&
                 parameter->registerCount >= 1 && parameter->registerCount <= 4 &&
                 parameter->registerIndex < (pixelShader ? 224 : 256) &&
                 parameter->registerCount <= (pixelShader ? 224 : 256) - parameter->registerIndex;
    MaterialShaderArgument result = {};
    if (valid)
    {
        if (index < 58)
        {
            valid = parameter->registerCount == 1 && parameter->rows == 1 &&
                    (parameter->parameterClass == D3DXPC_SCALAR || parameter->parameterClass == D3DXPC_VECTOR);
        }
        else
        {
            valid =
                (parameter->parameterClass == D3DXPC_MATRIX_ROWS && parameter->registerCount <= parameter->rows) ||
                (parameter->parameterClass == D3DXPC_MATRIX_COLUMNS && parameter->registerCount <= parameter->columns);
            if (parameter->parameterClass == D3DXPC_MATRIX_COLUMNS)
            {
                index = ((index - 58) ^ 2) + 58;
            }
        }
        valid = valid && (!pixelShader || s_codeConstUpdateFreq[index] != MTL_UPDATE_PER_PRIM);
        result.type = pixelShader ? MTL_ARG_CODE_PIXEL_CONST : MTL_ARG_CODE_VERTEX_CONST;
        result.dest = (uint16_t)parameter->registerIndex;
        result.u.codeConst.index = (uint16_t)index;
        result.u.codeConst.firstRow = 0;
        result.u.codeConst.rowCount = (uint8_t)parameter->registerCount;
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unsupported code constant argument");
        }
        return false;
    }
    *argument = result;
    return true;
}

bool Linker_ResolveCodeArgument(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                                bool pixelShader, MaterialShaderArgument *argument, char *error, size_t errorSize)
{
    unsigned int index = 0;
    if (!binding || !parameter || strcmp(binding->destination, parameter->name) ||
        !Linker_FindCodeConstant(binding->expression, &index))
    {
        if (argument)
        {
            memset(argument, 0, sizeof(MaterialShaderArgument));
        }
        return false;
    }
    return ResolveCodeIndex(index, parameter, pixelShader, argument, error, errorSize);
}

static bool FindCodeSampler(const char *name, const CodeSamplerSource *table, MaterialTextureSource *source)
{
    const size_t length = strcspn(name, ".");
    for (unsigned int i = 0; table[i].name; ++i)
    {
        if (strlen(table[i].name) != length || strncmp(name, table[i].name, length))
        {
            continue;
        }
        if (name[length])
        {
            return table[i].subtable && FindCodeSampler(name + length + 1, table[i].subtable, source);
        }
        *source = table[i].source;
        return true;
    }
    return false;
}

bool Linker_ResolveCodeSampler(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                               MaterialShaderArgument *argument, char *error, size_t errorSize)
{
    if (!argument || (!error && errorSize))
    {
        return false;
    }
    memset(argument, 0, sizeof(MaterialShaderArgument));
    MaterialTextureSource source = TEXTURE_SRC_CODE_BLACK;
    const bool valid = binding && parameter && !strcmp(binding->destination, parameter->name) &&
                       !strncmp(binding->expression, "sampler.", 8) && parameter->registerSet == D3DXRS_SAMPLER &&
                       parameter->registerIndex < 16 && parameter->registerCount == 1 && parameter->elements == 1 &&
                       FindCodeSampler(binding->expression + 8, s_codeSamplers, &source);
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid code sampler argument");
        }
        return false;
    }
    argument->type = MTL_ARG_CODE_PIXEL_SAMPLER;
    argument->dest = (uint16_t)parameter->registerIndex;
    argument->u.codeSampler = source;
    return true;
}

static void SkipLiteralSpace(const char **text)
{
    while (**text == ' ' || **text == '\t' || **text == '\r' || **text == '\n')
    {
        ++*text;
    }
}
static bool ParseShaderLiteral(const char *text, float *literal)
{
    SkipLiteralSpace(&text);
    if (strncmp(text, "float", 5) || text[5] < '1' || text[5] > '4')
    {
        return false;
    }
    const int count = text[5] - '0';
    text += 6;
    SkipLiteralSpace(&text);
    if (*text++ != '(')
    {
        return false;
    }
    literal[0] = literal[1] = literal[2] = 0;
    literal[3] = 1;
    for (int i = 0; i < count; ++i)
    {
        SkipLiteralSpace(&text);
        char *end;
        literal[i] = strtof(text, &end);
        if (end == text || !isfinite(literal[i]))
        {
            return false;
        }
        text = end;
        SkipLiteralSpace(&text);
        if (*text++ != (i + 1 == count ? ')' : ','))
        {
            return false;
        }
    }
    SkipLiteralSpace(&text);
    return !*text;
}

bool Linker_ResolveLiteralArgument(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                                   bool pixelShader, float *literalStorage, MaterialShaderArgument *argument,
                                   char *error, size_t errorSize)
{
    if (!argument || !literalStorage || (!error && errorSize))
    {
        return false;
    }
    memset(argument, 0, sizeof(MaterialShaderArgument));
    memset(literalStorage, 0, sizeof(float[4]));
    float literal[4];
    const bool valid = binding && parameter && !strcmp(binding->destination, parameter->name) &&
                       parameter->registerSet == D3DXRS_FLOAT4 && parameter->elements == 1 &&
                       parameter->registerCount == 1 && parameter->registerIndex < (pixelShader ? 224 : 256) &&
                       parameter->rows == 1 && parameter->columns >= 1 && parameter->columns <= 4 &&
                       (parameter->parameterClass == D3DXPC_SCALAR || parameter->parameterClass == D3DXPC_VECTOR) &&
                       ParseShaderLiteral(binding->expression, literal);
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid literal shader argument");
        }
        return false;
    }
    memcpy(literalStorage, literal, sizeof(float[4]));
    argument->type = pixelShader ? MTL_ARG_LITERAL_PIXEL_CONST : MTL_ARG_LITERAL_VERTEX_CONST;
    argument->dest = (uint16_t)parameter->registerIndex;
    argument->u.literalConst = literalStorage;
    return true;
}

static int ArgumentFrequency(const MaterialShaderArgument *argument)
{
    if (argument->type == MTL_ARG_CODE_VERTEX_CONST || argument->type == MTL_ARG_CODE_PIXEL_CONST)
    {
        if (argument->u.codeConst.index >= 90)
        {
            return -1;
        }
        const int frequency = s_codeConstUpdateFreq[argument->u.codeConst.index];
        return argument->type == MTL_ARG_CODE_PIXEL_CONST && frequency != MTL_UPDATE_RARELY ? -1 : frequency;
    }
    if (argument->type == MTL_ARG_CODE_PIXEL_SAMPLER)
    {
        const unsigned int source = argument->u.codeSampler;
        return source < 27 ? s_codeSamplerUpdateFreq[source] : -1;
    }
    return argument->type <= MTL_ARG_LITERAL_PIXEL_CONST ? MTL_UPDATE_RARELY : -1;
}
static int ComparePassArguments(const void *left, const void *right)
{
    const MaterialShaderArgument *a = (const MaterialShaderArgument *)left;
    const MaterialShaderArgument *b = (const MaterialShaderArgument *)right;
    int difference = ArgumentFrequency(a) - ArgumentFrequency(b);
    if (difference)
    {
        return difference;
    }
    difference = a->type - b->type;
    if (difference)
    {
        return difference;
    }
    if (a->type == MTL_ARG_MATERIAL_VERTEX_CONST || a->type == MTL_ARG_MATERIAL_PIXEL_CONST ||
        a->type == MTL_ARG_MATERIAL_PIXEL_SAMPLER)
    {
        if (a->u.nameHash != b->u.nameHash)
        {
            return a->u.nameHash < b->u.nameHash ? -1 : 1;
        }
    }
    return (int)a->dest - b->dest;
}

bool Linker_BuildPassArguments(const MaterialShaderArgument *arguments, unsigned int count, MaterialPass *pass,
                               char *error, size_t errorSize)
{
    if (!pass || pass->args || (!arguments && count) || count > 64 || (!error && errorSize))
    {
        return false;
    }
    MaterialShaderArgument sorted[64];
    unsigned int counts[4] = {};
    bool occupied[3][256] = {};
    unsigned int customFlags = 0;
    bool valid = true;
    for (unsigned int i = 0; valid && i < count; ++i)
    {
        const MaterialShaderArgument *argument = &arguments[i];
        const int frequency = ArgumentFrequency(argument);
        if (frequency < 0 || frequency > 3)
        {
            valid = false;
            break;
        }
        ++counts[frequency];
        const bool sampler =
            argument->type == MTL_ARG_CODE_PIXEL_SAMPLER || argument->type == MTL_ARG_MATERIAL_PIXEL_SAMPLER;
        const bool vertex = argument->type == MTL_ARG_CODE_VERTEX_CONST ||
                            argument->type == MTL_ARG_MATERIAL_VERTEX_CONST ||
                            argument->type == MTL_ARG_LITERAL_VERTEX_CONST;
        const unsigned int bank = sampler ? 2 : vertex ? 0 : 1;
        const unsigned int limit = sampler ? 16 : vertex ? 256 : 224;
        const bool code = argument->type == MTL_ARG_CODE_VERTEX_CONST || argument->type == MTL_ARG_CODE_PIXEL_CONST;
        const unsigned int rows = code ? argument->u.codeConst.rowCount : 1;
        if (!rows || argument->dest >= limit || rows > limit - argument->dest ||
            (code && (argument->u.codeConst.firstRow + rows > 4 || (argument->u.codeConst.index < 58 && rows != 1))))
        {
            valid = false;
            break;
        }
        for (unsigned int row = 0; row < rows; ++row)
        {
            if (occupied[bank][argument->dest + row])
            {
                valid = false;
            }
            occupied[bank][argument->dest + row] = true;
        }
        if (argument->type == MTL_ARG_LITERAL_VERTEX_CONST || argument->type == MTL_ARG_LITERAL_PIXEL_CONST)
        {
            if (!argument->u.literalConst)
            {
                valid = false;
            }
            else
            {
                for (int j = 0; j < 4; ++j)
                {
                    valid = valid && isfinite(argument->u.literalConst[j]);
                }
            }
        }
        if (frequency == MTL_UPDATE_CUSTOM)
        {
            const unsigned int sources[3] = {26, 4, 5};
            unsigned int slot = 0;
            while (slot < 3 && sources[slot] != (unsigned int)argument->u.codeSampler)
            {
                ++slot;
            }
            if (slot == 3 || argument->dest != slot + 1 || (customFlags & (1 << slot)))
            {
                valid = false;
            }
            else
            {
                customFlags |= 1 << slot;
            }
        }
        sorted[i] = *argument;
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or overlapping technique argument registers");
        }
        return false;
    }
    qsort(sorted, count, sizeof(MaterialShaderArgument), ComparePassArguments);
    const unsigned int stored = count - counts[MTL_UPDATE_CUSTOM];
    MaterialShaderArgument *result =
        stored ? (MaterialShaderArgument *)calloc(stored, sizeof(MaterialShaderArgument) + sizeof(float[4])) : NULL;
    if (stored && !result)
    {
        return false;
    }
    float *literals = result ? (float *)(result + stored) : NULL;
    for (unsigned int i = 0; i < stored; ++i)
    {
        result[i] = sorted[i];
        if (result[i].type == MTL_ARG_LITERAL_VERTEX_CONST || result[i].type == MTL_ARG_LITERAL_PIXEL_CONST)
        {
            memcpy(literals, sorted[i].u.literalConst, sizeof(float[4]));
            result[i].u.literalConst = literals;
            literals += 4;
        }
    }
    pass->args = result;
    pass->perPrimArgCount = (uint8_t)counts[0];
    pass->perObjArgCount = (uint8_t)counts[1];
    pass->stableArgCount = (uint8_t)counts[2];
    pass->customSamplerFlags = (uint8_t)customFlags;
    return true;
}

static bool DefaultStageBinding(const LinkerShaderConstant *parameter, LinkerTechniqueBinding *binding)
{
    strcpy(binding->destination, parameter->name);
    if (parameter->registerSet == D3DXRS_FLOAT4)
    {
        const CodeConstantSource *tables[2] = {s_codeConsts, s_defaultCodeConsts};
        for (int t = 0; t < 2; ++t)
        {
            unsigned int index;
            if (FindCodeConstant(parameter->name, tables[t], &index))
            {
                snprintf(binding->expression, sizeof(binding->expression), "@constant:%u", index);
                return true;
            }
            for (unsigned int i = 0; tables[t][i].name; ++i)
            {
                const CodeConstantSource *entry = &tables[t][i];
                if (!entry->subtable && !strcmp(entry->name, parameter->name))
                {
                    snprintf(binding->expression, sizeof(binding->expression), "@constant:%u", entry->source);
                    return true;
                }
            }
        }
    }
    else if (parameter->registerSet == D3DXRS_SAMPLER)
    {
        for (unsigned int i = 0; s_defaultCodeSamplers[i].name; ++i)
        {
            if (!strcmp(s_defaultCodeSamplers[i].name, parameter->name))
            {
                // The default sampler table includes engine-only sources with no explicit sampler.* spelling.
                snprintf(binding->expression, sizeof(binding->expression), "@sampler:%u",
                         s_defaultCodeSamplers[i].source);
                return true;
            }
        }
    }
    return false;
}

static bool BuildExpandedStageArguments(const LinkerTechniqueBindings *bindings, const LinkerShaderConstant *parameters,
                                unsigned int parameterCount, bool pixelShader, MaterialShaderArgument **arguments,
                                unsigned int *count, char *error, size_t errorSize)
{
    if (!arguments || !count || (!error && errorSize))
    {
        return false;
    }
    *arguments = NULL;
    *count = 0;
    if (!bindings || bindings->count > 64 || parameterCount > 64 || (!parameters && parameterCount))
    {
        return false;
    }
    MaterialShaderArgument *result =
        parameterCount
            ? (MaterialShaderArgument *)calloc(parameterCount, sizeof(MaterialShaderArgument) + sizeof(float[4]))
            : NULL;
    if (parameterCount && !result)
    {
        return false;
    }
    float *literal = result ? (float *)(result + parameterCount) : NULL;
    bool valid = true;
    const char *failedParameter = "";
    const LinkerShaderConstant *failedDefinition = NULL;
    for (unsigned int p = 0; valid && p < parameterCount; ++p)
    {
        const LinkerShaderConstant *parameter = &parameters[p];
        failedParameter = parameter->name;
        failedDefinition = parameter;
        const LinkerTechniqueBinding *binding = NULL;
        for (unsigned int i = 0; i < bindings->count; ++i)
        {
            if (!strcmp(bindings->entries[i].destination, parameter->name))
            {
                if (binding)
                {
                    valid = false;
                }
                binding = &bindings->entries[i];
            }
        }
        if (!valid)
        {
            break;
        }
        LinkerTechniqueBinding defaultBinding = {};
        const bool isDefault = binding == NULL;
        if (!binding)
        {
            valid = DefaultStageBinding(parameter, &defaultBinding);
            binding = &defaultBinding;
        }
        if (!valid)
        {
            break;
        }
        if (!strncmp(binding->expression, "material.", 9))
        {
            valid = Linker_ResolveMaterialArgument(binding, parameter, pixelShader, &result[p], error, errorSize);
        }
        else if (!strncmp(binding->expression, "code.", 5))
        {
            valid = Linker_ResolveCodeArgument(binding, parameter, pixelShader, &result[p], error, errorSize);
        }
        else if (!strncmp(binding->expression, "sampler.", 8))
        {
            valid = pixelShader && Linker_ResolveCodeSampler(binding, parameter, &result[p], error, errorSize);
        }
        else if (isDefault && !strncmp(binding->expression, "@constant:", 10))
        {
            valid = ResolveCodeIndex((unsigned int)strtoul(binding->expression + 10, NULL, 10), parameter, pixelShader,
                                     &result[p], error, errorSize);
        }
        else if (isDefault && !strncmp(binding->expression, "@sampler:", 9))
        {
            valid = pixelShader && parameter->registerIndex < 16 && parameter->registerCount == 1 &&
                    parameter->elements == 1;
            result[p].type = MTL_ARG_CODE_PIXEL_SAMPLER;
            result[p].dest = (uint16_t)parameter->registerIndex;
            result[p].u.codeSampler = (MaterialTextureSource)strtoul(binding->expression + 9, NULL, 10);
        }
        else
        {
            valid = Linker_ResolveLiteralArgument(binding, parameter, pixelShader, literal + p * 4, &result[p], error,
                                                  errorSize);
        }
    }
    if (!valid)
    {
        free(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Cannot resolve %s shader parameter '%s'", pixelShader ? "pixel" : "vertex",
                     failedParameter);
            if (failedDefinition)
            {
                const size_t used = strlen(error);
                snprintf(error + used, errorSize - used, " (class %u, %ux%u, %u registers, %u elements)",
                         failedDefinition->parameterClass, failedDefinition->rows, failedDefinition->columns,
                         failedDefinition->registerCount, failedDefinition->elements);
            }
        }
        return false;
    }
    *arguments = result;
    *count = parameterCount;
    return true;
}

bool Linker_BuildStageArguments(const LinkerTechniqueBindings *bindings, const LinkerShaderConstant *parameters,
                                unsigned int parameterCount, bool pixelShader, MaterialShaderArgument **arguments,
                                unsigned int *count, char *error, size_t errorSize)
{
    if (!arguments || !count || (!error && errorSize))
    {
        return false;
    }
    *arguments = NULL;
    *count = 0;
    if (parameterCount > 64 || (parameterCount && !parameters))
    {
        return false;
    }
    LinkerShaderConstant expanded[64];
    unsigned int used = 0;
    for (unsigned int i = 0; i < parameterCount; ++i)
    {
        const LinkerShaderConstant *parameter = &parameters[i];
        const bool array = parameter->elements > 1;
        const unsigned int elements = array ? parameter->registerCount : 1;
        // CTAB keeps the declared array length even when optimization removes
        // trailing registers. Match R_SetParameterDefArray's actual register range.
        const bool validArray = !array ||
            (parameter->registerSet == D3DXRS_FLOAT4 && parameter->rows == 1 &&
             (parameter->parameterClass == D3DXPC_SCALAR || parameter->parameterClass == D3DXPC_VECTOR) &&
             elements && elements <= parameter->elements);
        if (!validArray || elements > ARRAY_COUNT(expanded) - used ||
            !memchr(parameter->name, 0, sizeof(parameter->name)))
        {
            if (errorSize)
            {
                snprintf(error, errorSize, "Unsupported or oversized shader constant array");
            }
            return false;
        }
        for (unsigned int element = 0; element < elements; ++element)
        {
            LinkerShaderConstant *value = &expanded[used++];
            *value = *parameter;
            if (array)
            {
                const int length = snprintf(value->name, sizeof(value->name), "%s[%u]", parameter->name, element);
                if (length < 0 || length >= sizeof(value->name) || element > UINT_MAX - value->registerIndex)
                {
                    return false;
                }
                value->registerIndex += element;
                value->registerCount = 1;
                value->elements = 1;
            }
        }
    }
    return BuildExpandedStageArguments(bindings, expanded, used, pixelShader, arguments, count, error, errorSize);
}

static bool TechniqueSpan(const LinkerTechniqueSource *source, size_t begin, size_t size,
                          LinkerTechniqueBindings *bindings, char *error, size_t errorSize)
{
    return begin <= source->textSize && size <= source->textSize - begin &&
           Linker_ReadTechniqueBindings(source->text + begin, size, bindings, error, errorSize);
}

unsigned short Linker_TechniquePassFlags(const MaterialPass *pass)
{
    unsigned short flags = 0;
    const unsigned int count = pass->perPrimArgCount + pass->perObjArgCount + pass->stableArgCount;
    for (unsigned int i = 0; i < count; ++i)
    {
        const MaterialShaderArgument *argument = &pass->args[i];
        if (argument->type == MTL_ARG_CODE_PIXEL_SAMPLER)
        {
            switch (argument->u.codeSampler)
            {
            case 10:
                flags |= 1;
                break;
            case 11:
                flags |= 2;
                break;
            case 18:
            case 19:
            case 20:
                flags |= 0x20;
                break;
            }
        }
        else if (argument->type == MTL_ARG_CODE_PIXEL_CONST && argument->u.codeConst.index == 4)
        {
            flags |= 0x10;
        }
    }
    return flags;
}

bool Linker_BuildTechniquePass(const LinkerTechniqueSource *source, unsigned int passIndex,
                               MaterialVertexShader *vertexShader, MaterialPixelShader *pixelShader, MaterialPass *pass,
                               char *error, size_t errorSize)
{
    if (!pass || (!error && errorSize))
    {
        return false;
    }
    memset(pass, 0, sizeof(MaterialPass));
    if (!source || !source->text || passIndex >= source->passCount || passIndex >= 32 || !vertexShader || !pixelShader)
    {
        return false;
    }
    const LinkerTechniquePassSource *input = &source->passes[passIndex];
    LinkerTechniqueBindings *bindings = (LinkerTechniqueBindings *)malloc(sizeof(LinkerTechniqueBindings));
    MaterialPass result = {};
    result.vertexShader = vertexShader;
    result.pixelShader = pixelShader;
    result.vertexDecl = (MaterialVertexDeclaration *)calloc(1, sizeof(MaterialVertexDeclaration));
    int resources[12];
    bool valid = bindings && result.vertexDecl &&
                 Linker_ReflectVertexInputs(vertexShader->prog.loadDef.program, vertexShader->prog.loadDef.programSize,
                                            resources, error, errorSize) &&
                 TechniqueSpan(source, input->routingBegin, input->routingSize, bindings, error, errorSize) &&
                 Linker_BuildVertexRouting(bindings, resources, result.vertexDecl, error, errorSize);
    MaterialShaderArgument *stageArguments[2] = {};
    unsigned int counts[2] = {};
    for (int stage = 0; valid && stage < 2; ++stage)
    {
        const bool pixel = stage != 0;
        const void *program =
            pixel ? (const void *)pixelShader->prog.loadDef.program : vertexShader->prog.loadDef.program;
        const size_t words = pixel ? pixelShader->prog.loadDef.programSize : vertexShader->prog.loadDef.programSize;
        LinkerShaderConstant *parameters = NULL;
        unsigned int parameterCount = 0;
        valid =
            Linker_ReflectShaderConstants(program, words, pixel, &parameters, &parameterCount, error, errorSize) &&
            TechniqueSpan(source, pixel ? input->pixelBindingsBegin : input->vertexBindingsBegin,
                          pixel ? input->pixelBindingsSize : input->vertexBindingsSize, bindings, error, errorSize) &&
            Linker_BuildStageArguments(bindings, parameters, parameterCount, pixel, &stageArguments[stage],
                                       &counts[stage], error, errorSize);
        free(parameters);
    }
    if (valid)
    {
        MaterialShaderArgument combined[64];
        valid = counts[0] + counts[1] <= 64;
        if (valid)
        {
            if (counts[0])
            {
                memcpy(combined, stageArguments[0], counts[0] * sizeof(MaterialShaderArgument));
            }
            if (counts[1])
            {
                memcpy(combined + counts[0], stageArguments[1], counts[1] * sizeof(MaterialShaderArgument));
            }
            valid = Linker_BuildPassArguments(combined, counts[0] + counts[1], &result, error, errorSize);
        }
    }
    free(bindings);
    free(stageArguments[0]);
    free(stageArguments[1]);
    if (!valid)
    {
        free(result.vertexDecl);
        free(result.args);
        return false;
    }
    *pass = result;
    return true;
}

static const MtlStateMapBitName *StateValue(const char *name, const MtlStateMapBitGroup *group)
{
    for (unsigned int i = 0; group->bitNames[i].name; ++i)
    {
        if (!strcmp(name, group->bitNames[i].name))
        {
            return &group->bitNames[i];
        }
    }
    return NULL;
}
static bool StateSection(TechniqueCursor *cursor, const MtlStateMapBitGroup *groups, const unsigned int *reference,
                         unsigned int *output)
{
    if (!TechniqueExpect(cursor, "{"))
    {
        return false;
    }
    bool pending = false, any = false, applied = false;
    char token[256];
    bool quoted;
    for (;;)
    {
        if (!TechniqueToken(cursor, token, sizeof(token), &quoted) || quoted || !token[0])
        {
            return false;
        }
        if (!strcmp(token, "}"))
        {
            return applied && !pending;
        }
        const MtlStateMapBitGroup *condition = NULL;
        for (unsigned int i = 0; s_stateMapSrcBitGroup[i].name; ++i)
        {
            if (!strcmp(token, s_stateMapSrcBitGroup[i].name))
            {
                condition = &s_stateMapSrcBitGroup[i];
                break;
            }
        }
        if (condition || !strcmp(token, "default"))
        {
            bool matches = true;
            if (!condition)
            {
                if (!TechniqueExpect(cursor, ":"))
                {
                    return false;
                }
            }
            while (condition)
            {
                if (!TechniqueExpect(cursor, "=") || !TechniqueExpect(cursor, "=") ||
                    !TechniqueToken(cursor, token, sizeof(token), &quoted) || quoted)
                {
                    return false;
                }
                const MtlStateMapBitName *value = StateValue(token, condition);
                if (!value)
                {
                    return false;
                }
                const int word = condition->stateBitsMask[0] ? 0 : 1;
                matches = matches && (reference[word] & condition->stateBitsMask[word]) == (unsigned int)value->bits;
                if (!TechniqueToken(cursor, token, sizeof(token), &quoted) || quoted)
                {
                    return false;
                }
                if (!strcmp(token, ":"))
                {
                    break;
                }
                if (strcmp(token, "&") || !TechniqueExpect(cursor, "&") ||
                    !TechniqueToken(cursor, token, sizeof(token), &quoted) || quoted)
                {
                    return false;
                }
                condition = NULL;
                for (unsigned int i = 0; s_stateMapSrcBitGroup[i].name; ++i)
                {
                    if (!strcmp(token, s_stateMapSrcBitGroup[i].name))
                    {
                        condition = &s_stateMapSrcBitGroup[i];
                        break;
                    }
                }
                if (!condition)
                {
                    return false;
                }
            }
            pending = true;
            any = any || matches;
            continue;
        }
        if (!pending)
        {
            return false;
        }
        unsigned int set[2] = {}, clear[2] = {};
        if (strcmp(token, "passthrough"))
        {
            for (unsigned int i = 0; groups[i].name; ++i)
            {
                if (i &&
                    (!TechniqueExpect(cursor, ",") || !TechniqueToken(cursor, token, sizeof(token), &quoted) || quoted))
                {
                    return false;
                }
                const MtlStateMapBitName *value = StateValue(token, &groups[i]);
                if (!value)
                {
                    return false;
                }
                const int word = groups[i].stateBitsMask[0] ? 0 : 1;
                set[word] |= value->bits;
                clear[word] |= groups[i].stateBitsMask[word];
            }
        }
        if (!TechniqueExpect(cursor, ";"))
        {
            return false;
        }
        if (any && !applied)
        {
            for (int word = 0; word < 2; ++word)
            {
                output[word] = (output[word] & ~clear[word]) | set[word];
            }
            applied = true;
        }
        pending = any = false;
    }
}

bool Linker_ApplyStateMap(const void *data, size_t size, const unsigned int *referenceBits, unsigned int toolFlags,
                          unsigned int *stateBits, char *error, size_t errorSize)
{
    if (!data || !referenceBits || !stateBits || (!error && errorSize) || memchr(data, 0, size))
    {
        return false;
    }
    static const char *names[10] = {"alphaTest",  "blendFunc",  "separateAlphaBlendFunc", "cullFace", "depthTest",
                                    "depthWrite", "colorWrite", "polygonOffset",          "stencil",  "wireframe"};
    const MtlStateMapBitGroup *groups[10] = {s_stateMapDstAlphaTestBitGroup,      s_stateMapDstBlendFuncRgbBitGroup,
                                             s_stateMapDstBlendFuncAlphaBitGroup, s_stateMapDstCullFaceBitGroup,
                                             s_stateMapDstDepthTestBitGroup,      s_stateMapDstDepthWriteBitGroup,
                                             s_stateMapDstColorWriteBitGroup,     s_stateMapDstPolygonOffsetBitGroup,
                                             s_stateMapDstStencilBitGroup,        s_stateMapDstWireframeBitGroup};
    TechniqueCursor cursor = {(const char *)data, size, 0};
    unsigned int result[2] = {referenceBits[0], referenceBits[1]};
    bool seen[10] = {};
    bool valid = true;
    unsigned int sections = 0;
    char token[256];
    bool quoted;
    while (valid)
    {
        valid = TechniqueToken(&cursor, token, sizeof(token), &quoted);
        if (!valid || !token[0])
        {
            break;
        }
        unsigned int group = 0;
        while (group < 10 && strcmp(token, names[group]))
        {
            ++group;
        }
        if (quoted || group == 10 || group != sections || seen[group])
        {
            valid = false;
            break;
        }
        seen[group] = true;
        ++sections;
        valid = StateSection(&cursor, groups[group], referenceBits, result);
    }
    if (!valid || sections != 10)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or incomplete material state map near byte %zu", cursor.position);
        }
        return false;
    }
    if (!(toolFlags & 0x200) && (result[1] & 0x30) == 0x10)
    {
        result[1] &= ~0x30;
    }
    memcpy(stateBits, result, sizeof(result));
    return true;
}
