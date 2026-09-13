#pragma once
#include <stddef.h>
#include <stdint.h>
struct Material;
struct LinkerCompiledTechniqueSet;
// Installs owned stateBitsTable plus entry indices, state flags and camera region on a fresh material.
// The technique set remains borrowed. Leaves the material unchanged on failure.
bool Linker_BuildMaterialState(const LinkerCompiledTechniqueSet *compiled, Material *material, char *error,
                               size_t errorSize);
struct MaterialRaw;
struct MaterialTextureDef;
// Dependencies are borrowed, in raw texture order; only their image/water union is consumed.
// Caller releases the returned material with Linker_FreeMaterial (dependencies remain owned by the caller).
bool Linker_BuildMaterial(const void *data, size_t size, unsigned int materialType,
                          const LinkerCompiledTechniqueSet *techniques, const MaterialTextureDef *dependencies,
                          unsigned int dependencyCount, Material **material, char *error, size_t errorSize);
void Linker_FreeMaterial(Material *material);
struct LinkerMaterialLayer
{
    unsigned int materialIndex;
    bool normalMap;
};
bool Linker_ParseMaterialLayers(const char *name, unsigned int materialCount, LinkerMaterialLayer layers[5],
                                unsigned int *layerCount, char *error, size_t errorSize);
bool Linker_BuildLayeredTechniqueName(const Material *const *layers, unsigned int layerCount, char *name,
                                      size_t nameSize, uint8_t *worldFormat, char *error, size_t errorSize);
bool Linker_BuildLayeredMaterial(const char *name, const Material *const *layers, unsigned int layerCount,
                                 const LinkerCompiledTechniqueSet *techniques, Material **material, char *error,
                                 size_t errorSize);
// Owns a validated copy of an offset-based raw material file. All offsets remain file-relative.
bool Linker_ReadMaterialSource(const void *data, size_t size, MaterialRaw **material, char *error, size_t errorSize);
struct MaterialConstantDef;
// Caller releases the native constant table with free().
bool Linker_ImportMaterialConstants(const void *data, size_t size, MaterialConstantDef **constants, unsigned int *count,
                                    char *error, size_t errorSize);

struct LinkerMaterialTexture
{
    char name[256];
    char image[1024];
    uint32_t nameHash;
    uint8_t samplerState;
    uint8_t semantic;
    int32_t waterWidth;
    float waterParameters[6]; // world lengths, amplitude, wind speed, wind direction xy
};
// Water entries carry procedural parameters instead of an image filename. Caller frees the array.
bool Linker_ImportMaterialTextures(const void *data, size_t size, LinkerMaterialTexture **textures, unsigned int *count,
                                   char *error, size_t errorSize);
