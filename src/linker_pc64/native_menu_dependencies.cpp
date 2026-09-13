#include <universal/q_shared.h>
#include <universal/q_parse.h>
#include <database64/db_menu_layout.h>
#include "native_menu_dependencies.h"
#include <stdio.h>
#include <string.h>

static bool ScriptAssets(const char *script, LinkerMenuDependency add, void *context)
{
    if (!script)
    {
        return true;
    }
    if (strlen(script) >= 5120)
    {
        return false;
    }
    Com_BeginParseSession("native menu dependencies");
    bool valid = true, command = true;
    const char *cursor = script;
    try
    {
        while (valid)
        {
            const char *token = Com_Parse(&cursor)->token;
            if (!token[0])
            {
                break;
            }
            if (!strcmp(token, ";"))
            {
                command = true;
                continue;
            }
            if (!command)
            {
                continue;
            }
            command = false;
            const char *type = !_stricmp(token, "play") ? "sound" :
                               (!_stricmp(token, "setbackground") ? "material" : NULL);
            if (type)
            {
                token = Com_Parse(&cursor)->token;
                valid = token[0] && strcmp(token, ";") && add(type, token, context);
            }
        }
    }
    catch (...)
    {
        Com_EndParseSession();
        throw;
    }
    Com_EndParseSession();
    return valid;
}

static bool KeyAssets(const ItemKeyHandler *keys, LinkerMenuDependency add, void *context)
{
    unsigned int count = 0;
    while (keys)
    {
        if (++count > 256 || !ScriptAssets(keys->action, add, context))
        {
            return false;
        }
        keys = keys->next;
    }
    return true;
}

bool Linker_VisitMenuScriptAssets(const MenuList *list, LinkerMenuDependency add, void *context,
                                 char *error, size_t errorSize)
{
    if ((!error && errorSize) || !add)
    {
        return false;
    }
    bool valid = DB64_ValidateMenuListHeader(list);
    for (int m = 0; valid && m < list->menuCount; ++m)
    {
        const menuDef_t *menu = list->menus[m];
        valid = DB64_ValidateMenuHeader(menu);
        if (!valid)
        {
            break;
        }
        valid = ScriptAssets(menu->onOpen, add, context) && ScriptAssets(menu->onClose, add, context) &&
                ScriptAssets(menu->onESC, add, context) && KeyAssets(menu->onKey, add, context);
        for (int i = 0; valid && i < menu->itemCount; ++i)
        {
            const itemDef_s *item = menu->items[i];
            if (!item)
            {
                valid = false;
                break;
            }
            const char *scripts[] = {item->mouseEnterText, item->mouseExitText, item->mouseEnter, item->mouseExit,
                item->action, item->onAccept, item->onFocus, item->leaveFocus};
            for (int s = 0; valid && s < ARRAY_COUNT(scripts); ++s)
            {
                valid = ScriptAssets(scripts[s], add, context);
            }
            valid = valid && KeyAssets(item->onKey, add, context);
            if (valid && item->dataType == 6 && item->typeData.listBox)
            {
                valid = ScriptAssets(item->typeData.listBox->doubleClick, add, context);
            }
        }
    }
    if (!valid && errorSize)
    {
        snprintf(error, errorSize, "Cannot discover native menu script dependencies");
    }
    return valid;
}
