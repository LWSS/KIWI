#include <universal/q_shared.h>
#include "database.h"
#include "db_external_assets.h"
#include "fastfile_format.h"

bool DB64_LoadExternalAsset(XAssetType type, XAssetHeader *header)
{
    if ((uintptr_t)header->data != DB64_EXTERNAL_ASSET_TOKEN)
    {
        return false;
    }
    DB_PushStreamPos(4);
    DB64ExternalAssetRecord record;
    uint8_t *bytes = DB_GetStreamPos();
    Load_Stream(true, bytes, sizeof(DB64ExternalAssetRecord));
    memcpy(&record, bytes, sizeof(DB64ExternalAssetRecord));
    if (record.type != type || record.nameSize < 2 || record.nameSize > 1024)
    {
        Com_Error(ERR_DROP, "Invalid native external asset record");
    }
    const char *name = (const char *)DB_GetStreamPos();
    Load_Stream(true, (uint8_t *)name, record.nameSize);
    if (name[record.nameSize - 1] || memchr(name, 0, record.nameSize - 1) || name[0] == ',')
    {
        Com_Error(ERR_DROP, "Invalid native external asset name");
    }
    DB_PopStreamPos();
    *header = DB64_FindLoadedAsset(type, name);
    if (!header->data)
    {
        Com_Error(ERR_DROP, "Native fastfile requires a previously loaded asset: type %d, %s", type, name);
    }
    return true;
}
