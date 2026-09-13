#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <d3dx9shader.h>
#include <database64/db_shader_assets.h>
#include "native_shader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *ImportShader(const void *data, size_t size, const char *name, bool pixel, char *error, size_t errorSize)
{
    uint32_t bytes = 0;
    if (data && size >= sizeof(uint32_t))
    {
        memcpy(&bytes, data, sizeof(uint32_t));
    }
    if (!data || !name || !name[0] || size < 4 || bytes != size - 4 || bytes % 4 || bytes / 4 > 65535 ||
        !DB64_ValidateShaderProgram((const uint8_t *)data + 4, bytes / 4, pixel))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid precompiled shader file");
        }
        return NULL;
    }
    const size_t headerSize = pixel ? sizeof(MaterialPixelShader) : sizeof(MaterialVertexShader);
    const size_t nameSize = strlen(name) + 1;
    if (nameSize > 1024)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Shader name exceeds the compiler limit");
        }
        return NULL;
    }
    uint8_t *owned = (uint8_t *)calloc(1, headerSize + bytes + nameSize);
    if (!owned)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Cannot allocate native shader");
        }
        return NULL;
    }
    uint32_t *program = (uint32_t *)(owned + headerSize);
    char *ownedName = (char *)owned + headerSize + bytes;
    memcpy(program, (const uint8_t *)data + 4, bytes);
    memcpy(ownedName, name, nameSize);
    if (pixel)
    {
        MaterialPixelShader *shader = (MaterialPixelShader *)owned;
        shader->name = ownedName;
        shader->prog.loadDef.program = program;
        shader->prog.loadDef.loadForRenderer = ((program[0] >> 8) & 255) == 3 ? 1 : 0;
        shader->prog.loadDef.programSize = (uint16_t)(bytes / 4);
    }
    else
    {
        MaterialVertexShader *shader = (MaterialVertexShader *)owned;
        shader->name = ownedName;
        shader->prog.loadDef.program = program;
        shader->prog.loadDef.loadForRenderer = ((program[0] >> 8) & 255) == 3 ? 1 : 0;
        shader->prog.loadDef.programSize = (uint16_t)(bytes / 4);
    }
    return owned;
}

bool Linker_ImportVertexShader(const void *data, size_t size, const char *name, MaterialVertexShader **shader,
                               char *error, size_t errorSize)
{
    if (!shader || (!error && errorSize))
    {
        return false;
    }
    *shader = (MaterialVertexShader *)ImportShader(data, size, name, false, error, errorSize);
    return *shader != NULL;
}
bool Linker_ImportPixelShader(const void *data, size_t size, const char *name, MaterialPixelShader **shader,
                              char *error, size_t errorSize)
{
    if (!shader || (!error && errorSize))
    {
        return false;
    }
    *shader = (MaterialPixelShader *)ImportShader(data, size, name, true, error, errorSize);
    return *shader != NULL;
}

bool Linker_ReflectVertexInputs(const void *program, size_t words, int *resourceDest, char *error, size_t errorSize)
{
    if (!resourceDest || (!error && errorSize))
    {
        return false;
    }
    int result[12];
    bool usedRegisters[16] = {};
    for (int i = 0; i < 12; ++i)
    {
        result[i] = resourceDest[i] = -1;
    }
    bool valid = DB64_ValidateShaderProgram(program, words, false);
    const uint8_t *bytes = (const uint8_t *)program;
    for (size_t position = 1; valid && position < words;)
    {
        uint32_t instruction;
        memcpy(&instruction, bytes + position * sizeof(uint32_t), sizeof(uint32_t));
        if (instruction == 0xFFFF)
        {
            break;
        }
        const unsigned int opcode = instruction & 0xFFFF;
        const size_t parameters = opcode == 0xFFFE ? (instruction >> 16) & 0x7FFF
                                                   : (instruction & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        if (opcode == D3DSIO_DCL)
        {
            if (parameters != 2)
            {
                valid = false;
                break;
            }
            uint32_t semantic, destination;
            memcpy(&semantic, bytes + (position + 1) * sizeof(uint32_t), sizeof(uint32_t));
            memcpy(&destination, bytes + (position + 2) * sizeof(uint32_t), sizeof(uint32_t));
            const unsigned int type = ((destination & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
                                      ((destination & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
            if (type == D3DSPR_INPUT)
            {
                const unsigned int usage = semantic & D3DSP_DCL_USAGE_MASK;
                const unsigned int index = (semantic & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
                const unsigned int reg = destination & D3DSP_REGNUM_MASK;
                int slot = -1;
                if (usage == D3DDECLUSAGE_POSITION && !index)
                {
                    slot = 0;
                }
                else if (usage == D3DDECLUSAGE_NORMAL && !index)
                {
                    slot = 1;
                }
                else if (usage == D3DDECLUSAGE_COLOR && index < 2)
                {
                    slot = 2 + index;
                }
                else if (usage == D3DDECLUSAGE_TEXCOORD && index < 8)
                {
                    slot = 4 + index;
                }
                if (slot < 0 || reg >= 16 || result[slot] != -1 || usedRegisters[reg])
                {
                    valid = false;
                    break;
                }
                result[slot] = (int)reg;
                usedRegisters[reg] = true;
            }
        }
        position += parameters + 1;
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unsupported vertex shader input declarations");
        }
        return false;
    }
    memcpy(resourceDest, result, sizeof(result));
    return true;
}

static bool ShaderTableRange(size_t size, size_t offset, size_t bytes)
{
    return offset <= size && bytes <= size - offset;
}
static bool ReflectConstantTable(const uint8_t *data, size_t size, LinkerShaderConstant **constants,
                                 unsigned int *count)
{
    D3DXSHADER_CONSTANTTABLE table;
    if (size < sizeof(D3DXSHADER_CONSTANTTABLE))
    {
        return false;
    }
    memcpy(&table, data, sizeof(D3DXSHADER_CONSTANTTABLE));
    if (table.Size != sizeof(D3DXSHADER_CONSTANTTABLE) || table.Constants > 4096 ||
        !ShaderTableRange(size, table.ConstantInfo, table.Constants * sizeof(D3DXSHADER_CONSTANTINFO)))
    {
        return false;
    }
    LinkerShaderConstant *result =
        table.Constants ? (LinkerShaderConstant *)calloc(table.Constants, sizeof(LinkerShaderConstant)) : NULL;
    if (table.Constants && !result)
    {
        return false;
    }
    bool valid = true;
    for (unsigned int i = 0; valid && i < table.Constants; ++i)
    {
        D3DXSHADER_CONSTANTINFO info;
        D3DXSHADER_TYPEINFO type;
        memcpy(&info, data + table.ConstantInfo + i * sizeof(D3DXSHADER_CONSTANTINFO), sizeof(D3DXSHADER_CONSTANTINFO));
        if (!ShaderTableRange(size, info.Name, 1) ||
            !ShaderTableRange(size, info.TypeInfo, sizeof(D3DXSHADER_TYPEINFO)))
        {
            valid = false;
            break;
        }
        const char *name = (const char *)data + info.Name;
        const char *end = (const char *)memchr(name, 0, size - info.Name);
        memcpy(&type, data + info.TypeInfo, sizeof(D3DXSHADER_TYPEINFO));
        if (!end || end == name || end - name >= sizeof(result[i].name) || info.RegisterSet > D3DXRS_SAMPLER ||
            !info.RegisterCount || (unsigned int)info.RegisterIndex + info.RegisterCount > 65536 ||
            type.StructMembers || type.Class == D3DXPC_STRUCT || !type.Elements || !type.Rows || !type.Columns ||
            type.Rows > 4 || type.Columns > 4)
        {
            valid = false;
            break;
        }
        memcpy(result[i].name, name, end - name + 1);
        result[i].registerSet = info.RegisterSet;
        result[i].registerIndex = info.RegisterIndex;
        result[i].registerCount = info.RegisterCount;
        result[i].parameterClass = type.Class;
        result[i].parameterType = type.Type;
        result[i].rows = type.Rows;
        result[i].columns = type.Columns;
        result[i].elements = type.Elements;
    }
    if (!valid)
    {
        free(result);
        return false;
    }
    *constants = result;
    *count = table.Constants;
    return true;
}

bool Linker_ReflectShaderConstants(const void *program, size_t words, bool pixelShader,
                                   LinkerShaderConstant **constants, unsigned int *count, char *error, size_t errorSize)
{
    if (!constants || !count || (!error && errorSize))
    {
        return false;
    }
    *constants = NULL;
    *count = 0;
    bool valid = DB64_ValidateShaderProgram(program, words, pixelShader);
    bool found = false;
    const uint8_t *bytes = (const uint8_t *)program;
    for (size_t position = 1; valid && position < words;)
    {
        uint32_t instruction;
        memcpy(&instruction, bytes + position * 4, 4);
        if (instruction == 0xFFFF)
        {
            break;
        }
        const unsigned int opcode = instruction & 0xFFFF;
        const size_t parameters = opcode == 0xFFFE ? (instruction >> 16) & 0x7FFF : (instruction >> 24) & 15;
        if (opcode == 0xFFFE && parameters)
        {
            uint32_t tag;
            memcpy(&tag, bytes + (position + 1) * 4, 4);
            if (tag == 0x42415443) // CTAB
            {
                valid =
                    !found && ReflectConstantTable(bytes + (position + 2) * 4, (parameters - 1) * 4, constants, count);
                found = true;
            }
        }
        position += parameters + 1;
    }
    if (!valid)
    {
        free(*constants);
        *constants = NULL;
        *count = 0;
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unsupported shader constant table");
        }
    }
    return valid;
}

bool Linker_FindShaderBinary(const void *nameTable, size_t size, const char *shaderName, bool pixelShader,
                             char *filename, size_t filenameSize, char *error, size_t errorSize)
{
    if (!filename || !filenameSize || (!error && errorSize))
    {
        return false;
    }
    filename[0] = 0;
    const uint8_t *bytes = (const uint8_t *)nameTable;
    bool valid = nameTable && shaderName && shaderName[0];
    uint32_t hash = 0;
    if (valid)
    {
        for (const char *p = shaderName; *p; ++p)
        {
            hash = (uint32_t)(int)*p ^ (33 * hash);
        }
    }
    size_t position = 0;
    bool found = false;
    uint32_t binary = 0;
    for (int stage = 0; valid && stage < 2; ++stage)
    {
        int32_t count;
        if (size - position < sizeof(int32_t))
        {
            valid = false;
            break;
        }
        memcpy(&count, bytes + position, sizeof(int32_t));
        position += sizeof(int32_t);
        if (count < 0 || (size_t)count > (size - position) / sizeof(uint32_t[2]))
        {
            valid = false;
            break;
        }
        uint32_t previous = 0;
        for (int32_t i = 0; valid && i < count; ++i)
        {
            uint32_t entry[2];
            memcpy(entry, bytes + position, sizeof(uint32_t[2]));
            position += sizeof(uint32_t[2]);
            if (i && entry[0] < previous)
            {
                valid = false;
                break;
            }
            previous = entry[0];
            if ((stage != 0) == pixelShader && entry[0] == hash)
            {
                if (found && binary != entry[1])
                {
                    valid = false;
                    break;
                }
                found = true;
                binary = entry[1];
            }
        }
    }
    valid = valid && position == size && found;
    if (valid)
    {
        const int length = snprintf(filename, filenameSize, "%s_3_0_%08x", pixelShader ? "ps" : "vs", binary);
        valid = length >= 0 && (size_t)length < filenameSize;
    }
    if (!valid)
    {
        filename[0] = 0;
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid shader name table or shader name not found");
        }
    }
    return valid;
}
