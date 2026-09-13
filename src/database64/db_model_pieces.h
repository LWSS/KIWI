#pragma once
struct XModelPieces;
union XAssetHeader;
void DB64_LoadModelPieces(XAssetHeader *header, bool atStreamStart);
void Load_XModelPiecesAsset(XAssetHeader *header);
void Mark_XModelPiecesAsset(XModelPieces *pieces);
