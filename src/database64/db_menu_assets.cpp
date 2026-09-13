#include "db_external_assets.h"
#include "db_menu_assets.h"
#include "db_menu_layout.h"
#include "db_material_assets.h"
#include "db_sound_aliases.h"

static void *InlineMenuData(const void *token, size_t size)
{
    if ((uintptr_t)token != UINTPTR_MAX)
    {
        Com_Error(ERR_DROP, "Invalid native menu inline pointer");
    }
    void *data = DB_AllocStreamPos(15);
    Load_Stream(true, (uint8_t *)data, size);
    return data;
}

static void MenuStatement(statement_s *statement)
{
    if (!DB64_ValidateMenuStatement(statement))
    {
        Com_Error(ERR_DROP, "Invalid native menu statement");
    }
    if (!statement->entries)
    {
        return;
    }
    statement->entries = (expressionEntry **)InlineMenuData(statement->entries,
        statement->numEntries * sizeof(expressionEntry *));
    for (int i = 0; i < statement->numEntries; ++i)
    {
        expressionEntry *entry = (expressionEntry *)InlineMenuData(statement->entries[i], sizeof(expressionEntry));
        statement->entries[i] = entry;
        if (!DB64_ValidateMenuExpression(entry))
        {
            Com_Error(ERR_DROP, "Invalid native menu expression");
        }
        if (entry->type == 1 && entry->data.operand.dataType == VAL_STRING)
        {
            DB64_LoadAssetString(&entry->data.operand.internals.string);
        }
    }
}

static void MenuKeys(ItemKeyHandler **keys)
{
    unsigned int count = 0;
    while (*keys)
    {
        if (++count > 256)
        {
            Com_Error(ERR_DROP, "Too many native menu key handlers");
        }
        *keys = (ItemKeyHandler *)InlineMenuData(*keys, sizeof(ItemKeyHandler));
        DB64_LoadAssetString(&(*keys)->action);
        keys = &(*keys)->next;
    }
}

static void MenuWindow(windowDef_t *window)
{
    DB64_LoadAssetString(&window->name);
    DB64_LoadAssetString(&window->group);
    DB64_LoadMaterialAsset((XAssetHeader *)&window->background, false);
}

static void MenuItemData(itemDef_s *item)
{
    if (!item->typeData.data)
    {
        return;
    }
    // ownerdraw changes the display type while retaining the allocated data type.
    switch (item->dataType)
    {
    case 6: {
        listBoxDef_s *list = (listBoxDef_s *)InlineMenuData(item->typeData.listBox, sizeof(listBoxDef_s));
        item->typeData.listBox = list;
        if (!DB64_ValidateMenuListBox(list))
        {
            Com_Error(ERR_DROP, "Invalid native menu list-box");
        }
        DB64_LoadAssetString(&list->doubleClick);
        DB64_LoadMaterialAsset((XAssetHeader *)&list->selectIcon, false);
        break;
    }
    case 0:
    case 4:
    case 9:
    case 10:
    case 11:
    case 14:
    case 16:
    case 17:
    case 18:
        item->typeData.editField = (editFieldDef_s *)InlineMenuData(item->typeData.editField, sizeof(editFieldDef_s));
        break;
    case 12: {
        multiDef_s *multi = (multiDef_s *)InlineMenuData(item->typeData.multi, sizeof(multiDef_s));
        item->typeData.multi = multi;
        if (!DB64_ValidateMenuMulti(multi))
        {
            Com_Error(ERR_DROP, "Invalid native menu multi-choice data");
        }
        for (int i = 0; i < ARRAY_COUNT(multi->dvarList); ++i)
        {
            DB64_LoadAssetString(&multi->dvarList[i]);
        }
        for (int i = 0; i < ARRAY_COUNT(multi->dvarStr); ++i)
        {
            DB64_LoadAssetString(&multi->dvarStr[i]);
        }
        break;
    }
    case 13:
        DB64_LoadAssetString(&item->typeData.enumDvarName);
        break;
    default:
        Com_Error(ERR_DROP, "Unexpected native menu item data for type %d", item->dataType);
        break;
    }
}

static void MenuItem(itemDef_s **value)
{
    itemDef_s *item = (itemDef_s *)InlineMenuData(*value, sizeof(itemDef_s));
    *value = item;
    // Registration assigns the final pooled menu address to every item.
    if (item->parent)
    {
        Com_Error(ERR_DROP, "Native menu item contains a runtime parent pointer");
    }
    MenuWindow(&item->window);
    DB64_LoadAssetString(&item->text);
    DB64_LoadAssetString(&item->mouseEnterText);
    DB64_LoadAssetString(&item->mouseExitText);
    DB64_LoadAssetString(&item->mouseEnter);
    DB64_LoadAssetString(&item->mouseExit);
    DB64_LoadAssetString(&item->action);
    DB64_LoadAssetString(&item->onAccept);
    DB64_LoadAssetString(&item->onFocus);
    DB64_LoadAssetString(&item->leaveFocus);
    DB64_LoadAssetString(&item->dvar);
    DB64_LoadAssetString(&item->dvarTest);
    MenuKeys(&item->onKey);
    DB64_LoadAssetString(&item->enableDvar);
    DB64_LoadSoundAliases((XAssetHeader *)&item->focusSound, false);
    MenuItemData(item);
    MenuStatement(&item->visibleExp);
    MenuStatement(&item->textExp);
    MenuStatement(&item->materialExp);
    MenuStatement(&item->rectXExp);
    MenuStatement(&item->rectYExp);
    MenuStatement(&item->rectWExp);
    MenuStatement(&item->rectHExp);
    MenuStatement(&item->forecolorAExp);
}

static void MenuDefinition(menuDef_t *menu)
{
    if (!DB64_ValidateMenuHeader(menu))
    {
        Com_Error(ERR_DROP, "Invalid native menu header");
    }
    MenuWindow(&menu->window);
    DB64_LoadAssetString(&menu->font);
    DB64_LoadAssetString(&menu->onOpen);
    DB64_LoadAssetString(&menu->onClose);
    DB64_LoadAssetString(&menu->onESC);
    MenuKeys(&menu->onKey);
    MenuStatement(&menu->visibleExp);
    DB64_LoadAssetString(&menu->allowedBinding);
    DB64_LoadAssetString(&menu->soundName);
    MenuStatement(&menu->rectXExp);
    MenuStatement(&menu->rectYExp);
    if (menu->items)
    {
        menu->items = (itemDef_s **)InlineMenuData(menu->items, menu->itemCount * sizeof(itemDef_s *));
        for (int i = 0; i < menu->itemCount; ++i)
        {
            MenuItem(&menu->items[i]);
        }
    }
}

void DB64_LoadMenuAsset(XAssetType type, XAssetHeader *header, bool atStreamStart)
{
    if (type != ASSET_TYPE_MENU && type != ASSET_TYPE_MENULIST)
    {
        Com_Error(ERR_DROP, "Invalid native menu asset type");
    }
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(type, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->data = DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        Load_Stream(true, (uint8_t *)header->data, type == ASSET_TYPE_MENU ? sizeof(menuDef_t) : sizeof(MenuList));
        DB_PushStreamPos(4);
        if (type == ASSET_TYPE_MENU)
        {
            MenuDefinition(header->menu);
        }
        else
        {
            MenuList *list = header->menuList;
            if (!DB64_ValidateMenuListHeader(list))
            {
                Com_Error(ERR_DROP, "Invalid native menu-list header");
            }
            DB64_LoadAssetString(&list->name);
            if (list->menus)
            {
                list->menus = (menuDef_t **)InlineMenuData(list->menus, list->menuCount * sizeof(menuDef_t *));
                for (int i = 0; i < list->menuCount; ++i)
                {
                    if (!list->menus[i])
                    {
                        Com_Error(ERR_DROP, "Missing native menu in menu-list");
                    }
                    DB64_LoadMenuAsset(ASSET_TYPE_MENU, (XAssetHeader *)&list->menus[i], false);
                }
            }
        }
        DB_PopStreamPos();
        if (type == ASSET_TYPE_MENU)
        {
            Load_MenuAsset(header);
        }
        else
        {
            Load_MenuListAsset(header);
        }
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
