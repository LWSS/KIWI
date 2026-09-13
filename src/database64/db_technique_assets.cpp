#include "db_shader_argument_layout.h"
#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_state.h>
#include "database.h"
#include "db_shader_assets.h"
#include "db_technique_assets.h"

static void LoadPass(MaterialPass *pass)
{
    if ((uintptr_t)pass->vertexDecl == UINTPTR_MAX)
    {
        pass->vertexDecl = (MaterialVertexDeclaration *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)pass->vertexDecl, sizeof(MaterialVertexDeclaration));
        if (pass->vertexDecl->streamCount > ARRAY_COUNT(pass->vertexDecl->routing.data) ||
            *((uint8_t *)pass->vertexDecl + offsetof(MaterialVertexDeclaration, hasOptionalSource)) > 1 ||
            *((uint8_t *)pass->vertexDecl + offsetof(MaterialVertexDeclaration, isLoaded)) != 0)
        {
            Com_Error(ERR_DROP, "Invalid native vertex declaration");
        }
        for (uint i = 0; i < ARRAY_COUNT(pass->vertexDecl->routing.decl); ++i)
        {
            if (pass->vertexDecl->routing.decl[i])
            {
                Com_Error(ERR_DROP, "Native vertex declaration contains a runtime resource");
            }
        }
        for (uint i = 0; i < pass->vertexDecl->streamCount; ++i)
        {
            // Material_BuildVertexDecl indexes s_streamSourceInfo[9] and s_streamDestInfo[12].
            if (pass->vertexDecl->routing.data[i].source >= STREAM_SRC_COUNT ||
                pass->vertexDecl->routing.data[i].dest >= 12)
            {
                Com_Error(ERR_DROP, "Invalid native vertex stream routing");
            }
        }
        Load_BuildVertexDecl(&pass->vertexDecl);
    }
    else if (pass->vertexDecl)
    {
        DB_ConvertOffsetToPointer((uintptr_t *)&pass->vertexDecl);
    }
    DB64_LoadVertexShader(&pass->vertexShader, false);
    DB64_LoadPixelShader(&pass->pixelShader, false);
    const int count = pass->stableArgCount + pass->perObjArgCount + pass->perPrimArgCount;
    if (!!pass->args != (count != 0))
    {
        Com_Error(ERR_DROP, "Invalid native material argument count");
    }
    if (pass->args)
    {
        pass->args = (MaterialShaderArgument *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)pass->args, count * sizeof(MaterialShaderArgument));
        for (int i = 0; i < count; ++i)
        {
            MaterialShaderArgument *argument = &pass->args[i];
            if (argument->type > MTL_ARG_LITERAL_PIXEL_CONST)
            {
                Com_Error(ERR_DROP, "Invalid native shader argument type");
            }
            if (!DB64_ValidShaderArgument(argument))
            {
                Com_Error(ERR_DROP, "Invalid native shader argument range");
            }
            if (argument->type == MTL_ARG_LITERAL_VERTEX_CONST || argument->type == MTL_ARG_LITERAL_PIXEL_CONST)
            {
                if ((uintptr_t)argument->u.literalConst == UINTPTR_MAX)
                {
                    argument->u.literalConst = (const float *)DB_AllocStreamPos(15);
                    Load_Stream(true, (uint8_t *)argument->u.literalConst, sizeof(float[4]));
                }
                else if (argument->u.literalConst)
                {
                    DB_ConvertOffsetToPointer((uintptr_t *)&argument->u.literalConst);
                }
                else
                {
                    Com_Error(ERR_DROP, "Missing native shader literal constant");
                }
            }
        }
    }
}

static void LoadTechnique(MaterialTechnique **technique)
{
    if ((uintptr_t)*technique == UINTPTR_MAX)
    {
        *technique = (MaterialTechnique *)DB_AllocStreamPos(15);
        MaterialTechnique *value = *technique;
        Load_Stream(true, (uint8_t *)value, offsetof(MaterialTechnique, passArray));
        if (!value->name || !value->passCount)
        {
            Com_Error(ERR_DROP, "Invalid native material technique");
        }
        Load_Stream(true, (uint8_t *)value->passArray, value->passCount * sizeof(MaterialPass));
        for (uint i = 0; i < value->passCount; ++i)
        {
            LoadPass(&value->passArray[i]);
        }
        DB64_LoadAssetString(&value->name);
    }
    else if (*technique)
    {
        DB_ConvertOffsetToPointer((uintptr_t *)technique);
    }
}

void DB64_LoadTechniqueSet(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_TECHNIQUE_SET, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token)
    {
        if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
        {
            header->techniqueSet = (MaterialTechniqueSet *)DB_AllocStreamPos(15);
            const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
            MaterialTechniqueSet *set = header->techniqueSet;
            Load_Stream(true, (uint8_t *)set, sizeof(MaterialTechniqueSet));
            if (!set->name || set->remappedTechniqueSet ||
                *((uint8_t *)set + offsetof(MaterialTechniqueSet, hasBeenUploaded)) != 0)
            {
                Com_Error(ERR_DROP, "Invalid native technique set metadata");
            }
            DB_PushStreamPos(4);
            DB64_LoadAssetString(&set->name);
            for (uint i = 0; i < ARRAY_COUNT(set->techniques); ++i)
            {
                LoadTechnique(&set->techniques[i]);
            }
            DB_PopStreamPos();
            Load_MaterialTechniqueSetAsset(header);
            if (inserted)
            {
                *inserted = header->data;
            }
        }
        else
        {
            DB_ConvertOffsetToAlias((uintptr_t *)header);
        }
    }
    DB_PopStreamPos();
}
