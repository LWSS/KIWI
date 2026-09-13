#pragma once
#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>

static inline bool DB64_ValidShaderArgument(const MaterialShaderArgument *argument)
{
    if (argument->type > MTL_ARG_LITERAL_PIXEL_CONST)
    {
        return false;
    }
    const bool sampler = argument->type == MTL_ARG_CODE_PIXEL_SAMPLER ||
                 argument->type == MTL_ARG_MATERIAL_PIXEL_SAMPLER;
    const bool vertex = argument->type == MTL_ARG_CODE_VERTEX_CONST ||
                argument->type == MTL_ARG_MATERIAL_VERTEX_CONST ||
                argument->type == MTL_ARG_LITERAL_VERTEX_CONST;
    const bool code = argument->type == MTL_ARG_CODE_VERTEX_CONST ||
              argument->type == MTL_ARG_CODE_PIXEL_CONST;
    const unsigned int limit = sampler ? 16 : vertex ? 256 : 224;
    const unsigned int rows = code ? argument->u.codeConst.rowCount : 1;
    if (!rows || argument->dest >= limit || rows > limit - argument->dest ||
        (code && (argument->u.codeConst.index >= 90 || argument->u.codeConst.firstRow + rows > 4 ||
          (argument->u.codeConst.index < 58 && rows != 1))) ||
        (argument->type == MTL_ARG_CODE_PIXEL_SAMPLER && (unsigned int)argument->u.codeSampler >= 27))
    {
        return false;
    }
    return true;
}
