#include <universal/q_shared.h>
#include "q_shared.h"
#include <qcommon/qcommon.h>
#include <database64/database.h>
#include "q_parse.h"
#include "com_files.h"

const char *__cdecl StringTable_GetColumnValueForRow(const StringTable *table, int row, int column)
{
    if (column < table->columnCount && row < table->rowCount && row >= 0 && column >= 0 && table->values[(size_t)table->columnCount * row + column])
    {
        return table->values[(size_t)table->columnCount * row + column];
    }
    else
    {
        return "";
    }
}

const char *__cdecl StringTable_Lookup(
    const StringTable *table,
    int comparisonColumn,
    const char *value,
    int valueColumn)
{
    int v5; // eax

    if (table)
    {
        v5 = StringTable_LookupRowNumForValue(table, comparisonColumn, value);
        return StringTable_GetColumnValueForRow(table, v5, valueColumn);
    }
    else
    {
        Com_PrintError(CON_CHANNEL_UI, "Unable to find the lookup table in the fastfile, aborting lookup\n");
        return "";
    }
}

int __cdecl StringTable_LookupRowNumForValue(const StringTable *table, int comparisonColumn, const char *value)
{
    size_t slot;
    int row; // [esp+4h] [ebp-4h]

    if (!table->columnCount)
    {
        return -1;
    }
    if (comparisonColumn < 0 || comparisonColumn >= table->columnCount)
    {
        Com_Error(
            ERR_DROP,
            "Unable to compare against column number %i - there are only %i columns",
            comparisonColumn,
            table->columnCount);
    }
    for (row = table->rowCount - 1; row >= 0; --row)
    {
        slot = comparisonColumn + (size_t)table->columnCount * row;
        if (table->values[slot] && !I_stricmp(value, table->values[slot]))
        {
            return row;
        }
    }
    return -1;
}

#define STRING_TABLE_CACHE_SIZE 32

static int g_numStringTables = 0;
static StringTable g_stringTableCache[STRING_TABLE_CACHE_SIZE];

void __cdecl StringTable_GetAsset(const char *filename, StringTable **tablePtr)
{
    if (!IsFastFileLoad())
    {
        // KISAKTODO: memory here is never freed (not big), string cmp slightly meh but probably fine for this amt. of data
        for (int i = 0; i < g_numStringTables; i++)
        {
            if (!I_strcmp(g_stringTableCache[i].name, filename))
            {
                *tablePtr = &g_stringTableCache[i];
                return;
            }
        }

        if (g_numStringTables >= STRING_TABLE_CACHE_SIZE)
        {
            Com_Error(ERR_DROP, "String table cache is full");
        }
        int f;
        int fileSize = FS_FOpenFileByMode((char *)filename, &f, FS_READ);

        // iassert(fileSize > 0);

        if (fileSize <= 0)
        {
            if (f)
            {
                FS_FCloseFile(f);
            }
            *tablePtr = NULL;
            return;
        }

        char *filebuf = (char *)Hunk_AllocateTempMemoryHigh(fileSize + 1, "Kisak StringTable FF hack");

        FS_Read((byte *)filebuf, fileSize, f);
        FS_FCloseFile(f);

        filebuf[fileSize] = 0;

        Com_BeginParseSession(filename);
        Com_SetCSV(1);

        const char *buf = filebuf;

        int newlines = 0;
        // count newlines (row count)
        for (int i = 0; i < fileSize; i++)
        {
            if (filebuf[i] == '\n')
            {
                newlines++;
            }
        }

        if (filebuf[fileSize - 1] != '\n')
        {
            ++newlines;
        }
        StringTable table;

        int totalcells = 0;

        char *ptrs[0x4000]{0};

        while (1)
        {
            parseInfo_t *token = Com_Parse(&buf);

            if (!buf)
            {
                break;
            }

            if (totalcells >= ARRAY_COUNT(ptrs))
            {
                Com_Error(ERR_DROP, "String table has too many cells: %s", filename);
            }
            if (token->token[0])
            {
                char *text = (char *)Z_Malloc(strlen(token->token) + 1, "Kisak stringtable->value (text)", 21);
                strcpy((char *)text, token->token);
                ptrs[totalcells] = text;
            }

            totalcells++;
        }

        table.values = (const char **)Z_Malloc(sizeof(char *) * totalcells, "Kisak stringtable->value (ptrs)", 21);
        memset(table.values, 0, sizeof(char *) * totalcells);
        for (int i = 0; i < totalcells; i++)
        {
            table.values[i] = ptrs[i];
        }

        table.rowCount = newlines;
        table.columnCount = (totalcells / table.rowCount);

        table.name = (const char *)Z_Malloc(strlen(filename) + 1, "Kisak stringtable->name", 21);
        strcpy((char *)table.name, filename);

        g_stringTableCache[g_numStringTables] = table;
        *tablePtr = &g_stringTableCache[g_numStringTables];

        Com_EndParseSession();
        Hunk_ClearTempMemoryHigh();

        g_numStringTables++;

        iassert(g_numStringTables <= STRING_TABLE_CACHE_SIZE);
    }
    else
    {
        *tablePtr = DB_FindXAssetHeader(ASSET_TYPE_STRINGTABLE, filename).stringTable;
    }
}

