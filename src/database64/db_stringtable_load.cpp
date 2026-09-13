#include <universal/q_shared.h>
#include "database.h"

void DB64_LoadScriptStrings(ScriptStringList *list, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)list, sizeof(ScriptStringList));
    if (list->count < 0 || list->count > 65536 || (!!list->strings != (list->count != 0)) ||
        (list->strings && (uintptr_t)list->strings != UINTPTR_MAX))
    {
        Com_Error(ERR_DROP, "Invalid native fastfile script-string table");
    }
    DB_PushStreamPos(4);
    if (list->count)
    {
        list->strings = (const char **)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)list->strings, DB_StreamArraySize(sizeof(const char *), list->count));
        if (list->strings[0])
        {
            Com_Error(ERR_DROP, "Native fastfile script-string zero must be null");
        }
        for (int i = 0; i < list->count; ++i)
        {
            DB64_LoadAssetString(&list->strings[i]);
        }
    }
    DB_PopStreamPos();
}

void __cdecl Load_ScriptStringCustom(uint16_t *var)
{
    if (!varXAssetList || !varXAssetList->stringList.strings || *var >= varXAssetList->stringList.count)
    {
        Com_Error(ERR_DROP, "Native fastfile script string index out of range");
    }
    const char *string = varXAssetList->stringList.strings[*var];
    *var = string ? SL_GetString(string, 4) : 0;
}

void __cdecl Mark_ScriptStringCustom(uint16_t *var)
{
    if (*var)
    {
        SL_AddUser(*var, 4);
    }
}
