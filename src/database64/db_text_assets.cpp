#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <ui/ui_shared.h>
#include "database.h"
#include "db_text_assets.h"
#include <limits.h>

static void LoadRaw(RawFile *file)
{
    Load_Stream(true, (uint8_t *)file, sizeof(RawFile));
    if (file->len < 0 || file->len == INT_MAX || !file->buffer || !file->name)
    {
        Com_Error(ERR_DROP, "Invalid native rawfile");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&file->name);
    file->buffer = (const char *)DB_GetStreamPos();
    Load_Stream(true, (uint8_t *)file->buffer, (size_t)file->len + 1);
    if (file->buffer[file->len])
    {
        Com_Error(ERR_DROP, "Unterminated native rawfile: %s", file->name);
    }
    DB_PopStreamPos();
}

static void LoadLocalize(LocalizeEntry *entry)
{
    Load_Stream(true, (uint8_t *)entry, sizeof(LocalizeEntry));
    if (!entry->name || !entry->value)
    {
        Com_Error(ERR_DROP, "Invalid native localization entry");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&entry->value);
    DB64_LoadAssetString(&entry->name);
    DB_PopStreamPos();
}

static void LoadTable(StringTable *table)
{
    Load_Stream(true, (uint8_t *)table, sizeof(StringTable));
    if (!table->name || table->rowCount < 0 || table->columnCount < 0 ||
        (table->rowCount && table->columnCount > INT_MAX / table->rowCount))
    {
        Com_Error(ERR_DROP, "Invalid native stringtable dimensions");
    }
    const size_t count = (size_t)table->rowCount * table->columnCount;
    if (count > INT_MAX / sizeof(const char *) || (count && !table->values))
    {
        Com_Error(ERR_DROP, "Invalid native stringtable values");
    }
    DB64_LoadAssetString(&table->name);
    if (table->values)
    {
        table->values = (const char **)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)table->values, count * sizeof(const char *));
        for (size_t i = 0; i < count; ++i)
        {
            DB64_LoadAssetString(&table->values[i]);
        }
    }
}

void DB64_LoadTextAsset(XAssetType type, XAssetHeader *header, bool atStreamStart)
{
    if (type != ASSET_TYPE_RAWFILE && type != ASSET_TYPE_LOCALIZE_ENTRY && type != ASSET_TYPE_STRINGTABLE)
    {
        Com_Error(ERR_DROP, "Invalid native text asset type");
    }
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(type, header))
    {
        return;
    }
    const bool temporary = type != ASSET_TYPE_STRINGTABLE;
    if (temporary)
    {
        DB_PushStreamPos(0);
    }
    const uintptr_t token = (uintptr_t)header->data;
    if (token)
    {
        if (token == UINTPTR_MAX || (temporary && token == UINTPTR_MAX - 1))
        {
            header->data = DB_AllocStreamPos(15);
            const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
            switch (type)
            {
            case ASSET_TYPE_RAWFILE:
                LoadRaw(header->rawfile);
                Load_RawFileAsset(header);
                break;
            case ASSET_TYPE_LOCALIZE_ENTRY:
                LoadLocalize(header->localize);
                Load_LocalizeEntryAsset(header);
                break;
            case ASSET_TYPE_STRINGTABLE:
                LoadTable(header->stringTable);
                Load_StringTableAsset(header);
                break;
            }
            if (inserted)
            {
                *inserted = header->data;
            }
        }
        else if (temporary)
        {
            DB_ConvertOffsetToAlias((uintptr_t *)header);
        }
        else
        {
            DB_ConvertOffsetToPointer((uintptr_t *)header);
        }
    }
    if (temporary)
    {
        DB_PopStreamPos();
    }
}
