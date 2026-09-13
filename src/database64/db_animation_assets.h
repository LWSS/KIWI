#pragma once
#include <stddef.h>
struct XAnimParts;
union XAssetHeader;
bool DB64_ValidateAnimationHeader(const XAnimParts *parts, char *error, size_t errorSize);
void DB64_LoadAnimationAsset(XAssetHeader *header, bool atStreamStart);
