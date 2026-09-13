#pragma once
#include <stddef.h>
struct DynEntityCreateParams;
struct DynEntityDef;
struct clipMap_t;
struct XModel;
struct XModelPieces;
struct FxEffectDef;
struct PhysPreset;
// Dependencies are borrowed and must already have been resolved by the compiler.
bool Linker_BuildDynamicEntity(const DynEntityCreateParams *params, const clipMap_t *map, XModel *model,
                                const FxEffectDef *effect, XModelPieces *pieces, PhysPreset *preset,
                                DynEntityDef *entity, char *error, size_t errorSize);
// Installs owned definition/runtime arrays into an empty clipmap. Asset pointers remain borrowed.
bool Linker_AttachDynamicEntities(clipMap_t *map, const DynEntityDef *entities, unsigned int count,
                                  char *error, size_t errorSize);
