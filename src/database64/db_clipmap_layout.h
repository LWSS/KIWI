#pragma once
#include <stddef.h>
struct clipMap_t;
bool DB64_ValidateClipMapHeader(const clipMap_t *map, char *error, size_t errorSize);
size_t DB64_ClipMapWalkableBytes(int triangleCount);

struct DynEntityDef;
bool DB64_ValidateDynamicEntity(const DynEntityDef *entity, int group, unsigned int submodelCount);

bool DB64_ValidateCollisionTrees(const clipMap_t *map, char *error, size_t errorSize);

bool DB64_ValidateBspNodes(const clipMap_t *map, char *error, size_t errorSize);
