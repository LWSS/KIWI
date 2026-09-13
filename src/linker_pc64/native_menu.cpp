// Keep parser filesystem calls separate from the engine/test filesystem services.
#define FS_FOpenFileRead Linker_MenuOpen
#define FS_Read Linker_MenuRead
#define FS_FCloseFile Linker_MenuClose
#define FS_FOpenFileByMode Linker_MenuOpenMode
#define Z_Malloc Linker_MenuAllocate
#define Z_Free Linker_MenuRelease
#define Com_PrintError Linker_MenuError
#include <ui/ui_shared_obj.cpp>
#include "native_menu.h"
#include "native_asset_files.h"
#include "native_fx_support.h"
#include <stdlib.h>

struct MenuAllocation
{
    void *data;
    MenuAllocation *next;
};
struct MenuFile
{
    unsigned char *data;
    size_t size;
    size_t position;
};
struct LinkerCompiledMenu
{
    MenuList list;
    MenuAllocation *allocations;
    MenuFile files[64];
    const char *root;
    LinkerMenuMaterial material;
    LinkerMenuSound sound;
    void *context;
    bool failed;
};
static thread_local LinkerCompiledMenu *s_menu;

void Linker_MenuError(int, const char *format, ...)
{
    if (s_menu)
    {
        s_menu->failed = true;
    }
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}
void Com_PrintWarning(int, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

void *Linker_MenuAllocate(int size, const char *, int)
{
    if (!s_menu || size < 0 || size > 64 * 1024 * 1024)
    {
        Com_Error(ERR_DROP, "Invalid menu compiler allocation");
    }
    MenuAllocation *allocation = (MenuAllocation *)malloc(sizeof(MenuAllocation));
    void *data = calloc(1, size ? size : 1);
    if (!allocation || !data)
    {
        free(allocation);
        free(data);
        Com_Error(ERR_DROP, "Out of memory compiling menu");
    }
    allocation->data = data;
    allocation->next = s_menu->allocations;
    s_menu->allocations = allocation;
    return data;
}
void Linker_MenuRelease(void *data, int)
{
    MenuAllocation **allocation = &s_menu->allocations;
    while (*allocation)
    {
        if ((*allocation)->data == data)
        {
            MenuAllocation *removed = *allocation;
            *allocation = removed->next;
            free(removed->data);
            free(removed);
            return;
        }
        allocation = &(*allocation)->next;
    }
}
uint Linker_MenuOpen(const char *name, int *handle)
{
    *handle = 0;
    for (int i = 1; i < ARRAY_COUNT(s_menu->files); ++i)
    {
        MenuFile *file = &s_menu->files[i];
        if (!file->data)
        {
            void *data = NULL;
            if (!Linker_ReadRawAssetFile(s_menu->root, name, &data, &file->size))
            {
                return -1;
            }
            file->data = (unsigned char *)data;
            file->position = 0;
            *handle = i;
            return (int)file->size;
        }
    }
    Com_Error(ERR_DROP, "Too many menu compiler files");
    return -1;
}
uint Linker_MenuOpenMode(char *name, int *handle, fsMode_t mode)
{
    if (mode != FS_READ)
    {
        Com_Error(ERR_DROP, "Menu compiler only supports reading files");
    }
    return Linker_MenuOpen(name, handle);
}
const rectDef_s *Item_GetTextRect(int localClientNum, const itemDef_s *item)
{
    iassert(localClientNum == 0);
    return &item->textRect[localClientNum];
}
uint Linker_MenuRead(uint8_t *buffer, uint size, int handle)
{
    if (handle <= 0 || handle >= ARRAY_COUNT(s_menu->files) || !s_menu->files[handle].data)
    {
        Com_Error(ERR_DROP, "Invalid menu compiler file handle");
    }
    MenuFile *file = &s_menu->files[handle];
    if (size > file->size - file->position)
    {
        size = (uint)(file->size - file->position);
    }
    memcpy(buffer, file->data + file->position, size);
    file->position += size;
    return size;
}
void Linker_MenuClose(int handle)
{
    if (handle > 0 && handle < ARRAY_COUNT(s_menu->files))
    {
        free(s_menu->files[handle].data);
        memset(&s_menu->files[handle], 0, sizeof(MenuFile));
    }
}

void Linker_FreeMenu(LinkerCompiledMenu *menu)
{
    if (!menu)
    {
        return;
    }
    while (menu->allocations)
    {
        MenuAllocation *allocation = menu->allocations;
        menu->allocations = allocation->next;
        free(allocation->data);
        free(allocation);
    }
    for (int i = 0; i < ARRAY_COUNT(menu->files); ++i)
    {
        free(menu->files[i].data);
    }
    free(menu);
}
MenuList *Linker_GetMenuList(LinkerCompiledMenu *menu)
{
    return menu ? &menu->list : NULL;
}
static Material *MenuMaterialService(const char *name, void *)
{
    Material *material = s_menu->material ? s_menu->material(name, s_menu->context) : NULL;
    if (!material)
    {
        Com_Error(ERR_DROP, "Cannot compile menu material %s", name);
    }
    return material;
}
static void *MenuAllocationService(size_t size, size_t alignment, void *)
{
    if (size > INT_MAX || alignment > 16)
    {
        Com_Error(ERR_DROP, "Invalid menu allocation size or alignment");
    }
    return Linker_MenuAllocate((int)size, "menu", 0);
}
void Menu_FreeMemory(menuDef_t *)
{
    // The compilation owner releases the entire graph on success or failure.
}
snd_alias_list_t *Com_FindSoundAlias(const char *name)
{
    snd_alias_list_t *sound = s_menu->sound ? s_menu->sound(name, s_menu->context) : NULL;
    if (!sound)
    {
        Com_Error(ERR_DROP, "Cannot compile menu sound %s", name);
    }
    return sound;
}
void I_strncat(char *buffer, int size, const char *text)
{
    if (size <= 0 || strlen(buffer) + strlen(text) >= (size_t)size)
    {
        Com_Error(ERR_DROP, "Menu compiler string exceeds capacity");
    }
    strcat(buffer, text);
}
char *I_strlwr(char *text)
{
    for (char *p = text; *p; ++p)
    {
        *p = (char)tolower((unsigned char)*p);
    }
    return text;
}

static bool ParseMenuInput(const char *path)
{
    void *bytes = NULL;
    size_t size = 0;
    if (!Linker_ReadRawAssetFile(s_menu->root, path, &bytes, &size))
    {
        return false;
    }
    char *text = (char *)Linker_MenuAllocate((int)size + 1, "menu source", 0);
    memcpy(text, bytes, size + 1);
    free(bytes);
    const char *cursor = text;
    bool valid = true;
    bool list = false;
    Com_BeginParseSession(path);
    try
    {
        const char *token = Com_Parse(&cursor)->token;
        const bool wrapped = !strcmp(token, "{");
        if (wrapped)
        {
            token = Com_Parse(&cursor)->token;
        }
        const size_t length = strlen(path);
        list = !_stricmp(token, "loadmenu") || (length >= 4 && !_stricmp(path + length - 4, ".txt"));
        if (list)
        {
            if (size >= 32768)
            {
                valid = false;
            }
            char loaded[512][64];
            unsigned int loadedCount = 0;
            while (valid && token[0] && strcmp(token, "}"))
            {
                if (_stricmp(token, "loadmenu") || strcmp(Com_Parse(&cursor)->token, "{"))
                {
                    valid = false;
                    break;
                }
                token = Com_Parse(&cursor)->token;
                while (valid && token[0] && strcmp(token, "}"))
                {
                    if (strlen(token) >= sizeof(loaded[0]) || loadedCount == ARRAY_COUNT(loaded))
                    {
                        valid = false;
                        break;
                    }
                    bool seen = false;
                    for (unsigned int i = 0; i < loadedCount; ++i)
                    {
                        seen = seen || !_stricmp(token, loaded[i]);
                    }
                    if (!seen)
                    {
                        strcpy(loaded[loadedCount], token);
                        valid = UI_ParseMenuInternal(loaded[loadedCount++], IMAGE_TRACK_UI) != 0 && !s_menu->failed;
                    }
                    token = Com_Parse(&cursor)->token;
                }
                valid = valid && !strcmp(token, "}");
                if (valid)
                {
                    token = Com_Parse(&cursor)->token;
                }
            }
            valid = valid && (wrapped ? !strcmp(token, "}") : !token[0]);
            if (valid && wrapped)
            {
                valid = !Com_Parse(&cursor)->token[0];
            }
        }
    }
    catch (...)
    {
        Com_EndParseSession();
        throw;
    }
    Com_EndParseSession();
    return list ? valid : UI_ParseMenuInternal((char *)path, IMAGE_TRACK_UI) != 0 && !s_menu->failed;
}

bool Linker_CompileMenu(const char *root, const char *path, LinkerMenuMaterial material, LinkerMenuSound sound,
                        void *context, LinkerCompiledMenu **result, char *error, size_t errorSize)
{
    if (!result || !root || !path || !path[0] || strlen(path) >= sizeof(script_s::filename) ||
        (!error && errorSize) || s_menu)
    {
        return false;
    }
    *result = NULL;
    LinkerCompiledMenu *menu = (LinkerCompiledMenu *)calloc(1, sizeof(LinkerCompiledMenu));
    if (!menu)
    {
        return false;
    }
    menu->root = root;
    menu->material = material;
    menu->sound = sound;
    menu->context = context;
    s_menu = menu;
    LinkerFxServices services = {};
    services.material = MenuMaterialService;
    services.allocate = MenuAllocationService;
    const LinkerFxServices previous = Linker_SetFxServices(services);
    bool valid = false;
    try
    {
        Item_SetupKeywordHash();
        Menu_SetupKeywordHash();
        extern stringDef_s *g_strHandle[2048];
        memset(g_strHandle, 0, sizeof(stringDef_s *[2048]));
        memset(&g_load_0, 0, sizeof(g_load_0));
        g_load_0.menuList.menus = g_load_0.menus;
        valid = ParseMenuInput(path);
        if (valid)
        {
            menu->list = g_load_0.menuList;
            menu->list.name = String_Alloc(path);
            menu->list.menus = (menuDef_t **)Linker_MenuAllocate(menu->list.menuCount * sizeof(menuDef_t *), "menus", 0);
            memcpy(menu->list.menus, g_load_0.menus, menu->list.menuCount * sizeof(menuDef_t *));
        }
    }
    catch (...)
    {
        valid = false;
    }
    Linker_SetFxServices(previous);
    memset(sourceFiles, 0, sizeof(sourceFiles));
    s_menu = NULL;
    if (!valid)
    {
        Linker_FreeMenu(menu);
        if (errorSize)
        {
            snprintf(error, errorSize, "Cannot parse native menu %s", path);
        }
        return false;
    }
    *result = menu;
    return true;
}
