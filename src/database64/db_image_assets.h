#pragma once
#include <xanim/xanim.h>

struct GfxImageLoadDef;
bool DB64_ValidateImageLayout(const GfxImage *image, const GfxImageLoadDef *definition);
void DB64_LoadImageAsset(XAssetHeader *header, bool atStreamStart);
void DB64_LoadLightDefAsset(XAssetHeader *header, bool atStreamStart);
