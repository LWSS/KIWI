#pragma once
#include <ui/ui_shared.h>

// These limits match the raw menu parser and its fixed staging arrays.
inline bool DB64_ValidateMenuListHeader(const MenuList *list)
{
    return list && list->name && list->menuCount >= 0 && list->menuCount <= 512 &&
           (!list->menuCount || list->menus);
}

inline bool DB64_ValidateMenuHeader(const menuDef_t *menu)
{
    return menu && menu->window.name && menu->itemCount >= 0 && menu->itemCount <= 256 &&
           (!menu->itemCount || menu->items);
}

inline bool DB64_ValidateMenuStatement(const statement_s *statement)
{
    return statement && statement->numEntries >= 0 && statement->numEntries <= 200 &&
           (!statement->numEntries || statement->entries);
}

inline bool DB64_ValidateMenuExpression(const expressionEntry *entry)
{
    if (!entry)
    {
        return false;
    }
    if (entry->type == 0)
    {
        return entry->data.op >= OP_NOOP && entry->data.op < NUM_OPERATORS;
    }
    if (entry->type != 1)
    {
        return false;
    }
    const Operand *operand = &entry->data.operand;
    return operand->dataType == VAL_INT || operand->dataType == VAL_FLOAT ||
           (operand->dataType == VAL_STRING && operand->internals.string);
}

inline bool DB64_ValidateMenuListBox(const listBoxDef_s *list)
{
    return list && list->numColumns >= 0 && list->numColumns <= ARRAY_COUNT(list->columnInfo);
}

inline bool DB64_ValidateMenuMulti(const multiDef_s *multi)
{
    return multi && multi->count >= 0 && multi->count <= ARRAY_COUNT(multi->dvarList) &&
           (multi->strDef == 0 || multi->strDef == 1);
}
