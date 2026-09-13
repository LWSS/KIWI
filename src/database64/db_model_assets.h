#pragma once
#include <stddef.h>
struct XModel;
union XAssetHeader;
bool DB64_ValidateModelHeader(const XModel *model, char *error, size_t errorSize);
void DB64_LoadModelAsset(XAssetHeader *header, bool atStreamStart);
