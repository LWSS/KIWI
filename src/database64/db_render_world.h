#pragma once
struct GfxWorld;
union XAssetHeader;
void DB64_LoadRenderWorld(GfxWorld *world, bool atStreamStart);
void DB64_LoadRenderWorldAsset(XAssetHeader *header, bool atStreamStart);
