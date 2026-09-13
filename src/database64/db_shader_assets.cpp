#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include "database.h"
#include "db_shader_assets.h"

static void *LoadProgram(uint16_t words, bool pixelShader)
{
    if (words < 2)
    {
        Com_Error(ERR_DROP, "Invalid native shader program length");
    }
    void *program = DB_AllocStreamPos(15);
    Load_Stream(true, (uint8_t *)program, (size_t)words * sizeof(uint32_t));
    if (!DB64_ValidateShaderProgram(program, words, pixelShader))
    {
        Com_Error(ERR_DROP, "Invalid or unterminated native shader bytecode");
    }
    return program;
}

void DB64_LoadVertexShader(MaterialVertexShader **shader, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)shader, sizeof(MaterialVertexShader *));
    if ((uintptr_t)*shader == UINTPTR_MAX)
    {
        *shader = (MaterialVertexShader *)DB_AllocStreamPos(15);
        MaterialVertexShader *value = *shader;
        Load_Stream(true, (uint8_t *)value, sizeof(MaterialVertexShader));
        if (!value->name || !value->prog.loadDef.program || value->prog.vs)
        {
            Com_Error(ERR_DROP, "Invalid native vertex shader metadata");
        }
        DB64_LoadAssetString(&value->name);
        value->prog.loadDef.program = LoadProgram(value->prog.loadDef.programSize, false);
        Load_CreateMaterialVertexShader(&value->prog.loadDef, value);
    }
    else if (*shader)
    {
        DB_ConvertOffsetToPointer((uintptr_t *)shader);
    }
}

void DB64_LoadPixelShader(MaterialPixelShader **shader, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)shader, sizeof(MaterialPixelShader *));
    if ((uintptr_t)*shader == UINTPTR_MAX)
    {
        *shader = (MaterialPixelShader *)DB_AllocStreamPos(15);
        MaterialPixelShader *value = *shader;
        Load_Stream(true, (uint8_t *)value, sizeof(MaterialPixelShader));
        if (!value->name || !value->prog.loadDef.program || value->prog.ps)
        {
            Com_Error(ERR_DROP, "Invalid native pixel shader metadata");
        }
        DB64_LoadAssetString(&value->name);
        value->prog.loadDef.program = LoadProgram(value->prog.loadDef.programSize, true);
        Load_CreateMaterialPixelShader(&value->prog.loadDef, value);
    }
    else if (*shader)
    {
        DB_ConvertOffsetToPointer((uintptr_t *)shader);
    }
}
