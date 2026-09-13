#include "db_external_assets.h"
#include <universal/q_shared.h>
#include "database.h"
#include "db_model_pieces.h"
#include "db_model_assets.h"
#include <limits.h>
#include <math.h>

void DB64_LoadModelPieces(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_XMODELPIECES, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->xmodelPieces = (XModelPieces *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        XModelPieces *pieces = header->xmodelPieces;
        Load_Stream(true, (uint8_t *)pieces, sizeof(XModelPieces));
        if (!pieces->name || pieces->numpieces < 0 || pieces->numpieces > INT_MAX / sizeof(XModelPiece) ||
            (!!pieces->pieces != (pieces->numpieces != 0)))
        {
            Com_Error(ERR_DROP, "Invalid native model-piece list");
        }
        DB_PushStreamPos(4);
        DB64_LoadAssetString(&pieces->name);
        if ((uintptr_t)pieces->pieces == UINTPTR_MAX)
        {
            pieces->pieces = (XModelPiece *)DB_AllocStreamPos(15);
            Load_Stream(true, (uint8_t *)pieces->pieces, pieces->numpieces * sizeof(XModelPiece));
            for (int i = 0; i < pieces->numpieces; ++i)
            {
                if (!pieces->pieces[i].model)
                {
                    Com_Error(ERR_DROP, "Native model piece has no model");
                }
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!isfinite(pieces->pieces[i].offset[axis]))
                    {
                        Com_Error(ERR_DROP, "Invalid native model piece offset");
                    }
                }
                DB64_LoadModelAsset((XAssetHeader *)&pieces->pieces[i].model, false);
            }
        }
        else if (pieces->pieces)
        {
            DB64_ConvertOffsetRange((uintptr_t *)&pieces->pieces, pieces->numpieces * sizeof(XModelPiece));
        }
        DB_PopStreamPos();
        Load_XModelPiecesAsset(header);
        if (inserted)
        {
            *inserted = header->data;
        }
    }
    else if (token)
    {
        DB_ConvertOffsetToAlias((uintptr_t *)header);
    }
    DB_PopStreamPos();
}
