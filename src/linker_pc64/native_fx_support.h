#pragma once
#include <stddef.h>
struct PhysPreset;
struct Material;
struct XModel;
struct FxEffectDef;
struct FxEditorEffectDef;
char FX_LoadEditorEffectFromBuffer(const char *buffer, const char *parseSessionName, FxEditorEffectDef *effect);
struct LinkerFxServices
{
    void *context;
    void *(*allocate)(size_t size, size_t alignment, void *context);
    PhysPreset *(*preset)(const char *name, void *context);
    Material *(*material)(const char *name, void *context);
    XModel *(*model)(const char *name, void *context);
    const FxEffectDef *(*effect)(const char *name, void *context);
};
// Services are scoped to the calling compiler thread. Returns the previous scope.
LinkerFxServices Linker_SetFxServices(LinkerFxServices services);
