#pragma once
enum XAssetType : int;
union XAssetHeader;
bool DB64_LoadExternalAsset(XAssetType type, XAssetHeader *header);
XAssetHeader DB64_FindLoadedAsset(XAssetType type, const char *name);
