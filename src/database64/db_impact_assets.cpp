#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <gfx_d3d/fxprimitives.h>
#include "database.h"
#include "db_impact_assets.h"
#include "db_effect_assets.h"

void DB64_LoadImpactAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_IMPACT_FX, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->impactFx = (FxImpactTable *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        FxImpactTable *table = header->impactFx;
        Load_Stream(true, (uint8_t *)table, sizeof(FxImpactTable));
        if (!table->name || (uintptr_t)table->table != UINTPTR_MAX)
        {
            Com_Error(ERR_DROP, "Invalid native impact-effect table");
        }
        DB_PushStreamPos(4);
        DB64_LoadAssetString(&table->name);
        // The client looks up the current map's impact table under the empty name.
        if (table->name[0])
        {
            Com_Error(ERR_DROP, "Native impact-effect table must have the empty asset name");
        }
        table->table = (FxImpactEntry *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)table->table, 12 * sizeof(FxImpactEntry));
        for (int entry = 0; entry < 12; ++entry)
        {
            for (int i = 0; i < 29; ++i)
            {
                DB64_LoadEffectAsset((XAssetHeader *)&table->table[entry].nonflesh[i], false);
            }
            for (int i = 0; i < 4; ++i)
            {
                DB64_LoadEffectAsset((XAssetHeader *)&table->table[entry].flesh[i], false);
            }
        }
        DB_PopStreamPos();
        Load_FxImpactTableAsset(header);
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
