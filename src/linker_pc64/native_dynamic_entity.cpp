#include <universal/q_shared.h>
#include <DynEntity/DynEntity_client.h>
#include <xanim/xmodel.h>
#include <database64/db_clipmap_layout.h>
#include "native_dynamic_entity.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct DynamicOrder
{
    const DynEntityDef *entity;
    unsigned int source;
};

static int CompareDynamicOrder(const void *left, const void *right)
{
    const DynamicOrder *a = (const DynamicOrder *)left;
    const DynamicOrder *b = (const DynamicOrder *)right;
    const bool modelA = a->entity->xModel != NULL, modelB = b->entity->xModel != NULL;
    if (modelA != modelB)
    {
        return modelA ? -1 : 1;
    }
    const int result = modelA ? strcmp(a->entity->xModel->name, b->entity->xModel->name)
                              : (int)a->entity->brushModel - b->entity->brushModel;
    if (result)
    {
        return result;
    }
    return (a->source > b->source) - (a->source < b->source);
}

bool Linker_AttachDynamicEntities(clipMap_t *map, const DynEntityDef *entities, unsigned int count, char *error,
                                  size_t errorSize)
{
    if (!map || (count && !entities) || (!error && errorSize))
    {
        return false;
    }
    unsigned int counts[2] = {};
    bool valid = count <= 131070;
    for (unsigned int group = 0; group < 2; ++group)
    {
        valid = valid && !map->dynEntCount[group] && !map->dynEntDefList[group] && !map->dynEntPoseList[group] &&
                !map->dynEntClientList[group] && !map->dynEntCollList[group];
    }
    for (unsigned int i = 0; valid && i < count; ++i)
    {
        const unsigned int group = entities[i].xModel ? 0 : 1;
        valid = DB64_ValidateDynamicEntity(&entities[i], group, map->numSubModels) &&
                (!entities[i].xModel || entities[i].xModel->name) && ++counts[group] <= 65535;
    }
    DynamicOrder *order = valid && count ? (DynamicOrder *)malloc(count * sizeof(DynamicOrder)) : NULL;
    valid = valid && (!count || order);
    DynEntityDef *definitions[2] = {};
    DynEntityPose *poses[2] = {};
    DynEntityClient *clients[2] = {};
    DynEntityColl *collisions[2] = {};
    for (unsigned int group = 0; valid && group < 2; ++group)
    {
        if (counts[group])
        {
            definitions[group] = (DynEntityDef *)malloc(counts[group] * sizeof(DynEntityDef));
            poses[group] = (DynEntityPose *)calloc(counts[group], sizeof(DynEntityPose));
            clients[group] = (DynEntityClient *)calloc(counts[group], sizeof(DynEntityClient));
            collisions[group] = (DynEntityColl *)calloc(counts[group], sizeof(DynEntityColl));
            valid = definitions[group] && poses[group] && clients[group] && collisions[group];
        }
    }
    if (valid)
    {
        for (unsigned int i = 0; i < count; ++i)
        {
            order[i].entity = &entities[i];
            order[i].source = i;
        }
        if (count)
        {
            qsort(order, count, sizeof(DynamicOrder), CompareDynamicOrder);
        }
        unsigned int next[2] = {};
        for (unsigned int i = 0; i < count; ++i)
        {
            const DynEntityDef *entity = order[i].entity;
            const unsigned int group = entity->xModel ? 0 : 1;
            definitions[group][next[group]++] = *entity;
        }
        for (unsigned int group = 0; group < 2; ++group)
        {
            map->dynEntCount[group] = (uint16_t)counts[group];
            map->dynEntDefList[group] = definitions[group];
            map->dynEntPoseList[group] = poses[group];
            map->dynEntClientList[group] = clients[group];
            map->dynEntCollList[group] = collisions[group];
        }
    }
    else
    {
        for (unsigned int group = 0; group < 2; ++group)
        {
            free(definitions[group]);
            free(poses[group]);
            free(clients[group]);
            free(collisions[group]);
        }
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid dynamic entity list, occupied clipmap, or allocation failure");
        }
    }
    free(order);
    return valid;
}

static bool BrushIndex(const char *name, const clipMap_t *map, uint16_t *index)
{
    if (*name++ != '*' || !*name)
    {
        return false;
    }
    unsigned int value = 0;
    while (*name)
    {
        if (*name < '0' || *name > '9' || value > 6553)
        {
            return false;
        }
        value = value * 10 + *name++ - '0';
    }
    if (!value || value > 65535 || value >= map->numSubModels || !map->cmodels)
    {
        return false;
    }
    *index = (uint16_t)value;
    return true;
}

bool Linker_BuildDynamicEntity(const DynEntityCreateParams *params, const clipMap_t *map, XModel *model,
                               const FxEffectDef *effect, XModelPieces *pieces, PhysPreset *preset,
                               DynEntityDef *entity, char *error, size_t errorSize)
{
    if (!entity || !params || !map || (!error && errorSize))
    {
        return false;
    }
    *entity = {};
    DynEntityDef result = {};
    result.type = !params->typeName[0] || !_stricmp(params->typeName, "clutter") ? DYNENT_TYPE_CLUTTER
                  : !_stricmp(params->typeName, "destruct")                      ? DYNENT_TYPE_DESTRUCT
                                                                                 : DYNENT_TYPE_INVALID;
    bool valid = result.type != DYNENT_TYPE_INVALID && preset && params->modelName[0] &&
                 (!params->destroyFxFile[0] || effect) && (!params->destroyPiecesFile[0] || pieces);
    if (valid && params->modelName[0] == '*')
    {
        valid = !model && BrushIndex(params->modelName, map, &result.brushModel);
        if (valid)
        {
            const cLeaf_t *leaf = &map->cmodels[result.brushModel].leaf;
            result.contents = leaf->brushContents | leaf->terrainContents;
        }
    }
    else if (valid)
    {
        valid = model && !model->bad && model->collLod >= 0 && model->collLod < model->numLods;
        if (valid)
        {
            result.xModel = model;
            result.contents = model->contents;
        }
    }
    if (valid && params->physModelName[0] == '*')
    {
        valid = BrushIndex(params->physModelName, map, &result.physicsBrushModel);
    }
    for (unsigned int i = 0; valid && i < 3; ++i)
    {
        valid = isfinite(params->angles[i]) && isfinite(params->origin[i]) && isfinite(params->centerOfMass[i]) &&
                isfinite(params->momentsOfInertia[i]) && isfinite(params->productsOfInertia[i]);
        result.pose.origin[i] = params->origin[i];
        result.mass.centerOfMass[i] = params->centerOfMass[i];
        result.mass.momentsOfInertia[i] = params->momentsOfInertia[i];
        result.mass.productsOfInertia[i] = params->productsOfInertia[i];
    }
    if (valid)
    {
        const double radians = 3.14159265358979323846 / 360;
        const double p = params->angles[0] * radians, y = params->angles[1] * radians, r = params->angles[2] * radians;
        const double sp = sin(p), cp = cos(p), sy = sin(y), cy = cos(y), sr = sin(r), cr = cos(r);
        result.pose.quat[0] = (float)(sr * cp * cy - cr * sp * sy);
        result.pose.quat[1] = (float)(cr * sp * cy + sr * cp * sy);
        result.pose.quat[2] = (float)(cr * cp * sy - sr * sp * cy);
        result.pose.quat[3] = (float)(cr * cp * cy + sr * sp * sy);
        result.health = params->health;
        result.destroyFx = effect;
        result.destroyPieces = pieces;
        result.physPreset = preset;
        valid = DB64_ValidateDynamicEntity(&result, result.xModel ? 0 : 1, map->numSubModels);
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid dynamic entity or unresolved dependency: %s", params->modelName);
        }
        return false;
    }
    *entity = result;
    return true;
}
