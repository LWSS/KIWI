#pragma once
#include <stddef.h>
struct MaterialTechnique;
struct LinkerCompiledTechnique
{
    MaterialTechnique *technique;
    unsigned int stateBits[32][2];
};
bool Linker_CompileTechniqueFiles(const char *root, const char *name, const unsigned int *referenceBits,
                                  unsigned int toolFlags, LinkerCompiledTechnique **compiled, char *error,
                                  size_t errorSize);
void Linker_FreeCompiledTechnique(LinkerCompiledTechnique *compiled);

struct MaterialTechniqueSet;
struct LinkerCompiledTechniqueSet
{
    MaterialTechniqueSet *techniqueSet;
    // Slots sharing a technique also share its compiled state map results.
    LinkerCompiledTechnique *techniques[34];
};
// Compiles game technique slots; editor-only passes are omitted as in the game raw loader.
bool Linker_CompileTechniqueSetFiles(const char *root, const char *name, const unsigned int *referenceBits,
                                     unsigned int toolFlags, LinkerCompiledTechniqueSet **compiled, char *error,
                                     size_t errorSize);
void Linker_FreeCompiledTechniqueSet(LinkerCompiledTechniqueSet *compiled);
