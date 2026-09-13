#pragma once
#include <stddef.h>
#include <stdint.h>
struct XModel;
struct XSurface;
struct Material;
struct PhysPreset;
struct PhysGeomList;
struct LinkerModelSource;
struct LinkerCompiledMaterial;
struct LinkerCompiledModel
{
    XModel *model;
    LinkerModelSource *source;
    XSurface *surfaces;
    unsigned int surfaceCount;
    Material *materials[255];
    LinkerCompiledMaterial *ownedMaterials[255];
    unsigned int materialCount;
    PhysPreset *preset;
    PhysGeomList *physics;
};
// Intern must return a nonzero index into the zone's compiler-owned script string table.
typedef uint16_t (*LinkerInternModelString)(const char *text, void *context);
// Context is a ScriptStringList with 65536 allocated slots and slot zero reserved.
uint16_t Linker_InternModelString(const char *text, void *context);
bool Linker_CompileModelFiles(const char *root, const char *name, LinkerInternModelString intern, void *context,
                              LinkerCompiledModel **compiled, char *error, size_t errorSize);
void Linker_FreeCompiledModel(LinkerCompiledModel *compiled);
