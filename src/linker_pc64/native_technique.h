#pragma once
#include <stddef.h>
struct LinkerTechniqueSetSource
{
    char techniques[34][256];
};
bool Linker_ReadTechniqueSet(const void *data, size_t size, LinkerTechniqueSetSource *source, char *error,
                             size_t errorSize);

struct LinkerTechniquePassSource
{
    char stateMap[256];
    char vertexShader[256];
    char pixelShader[256];
    int vertexVersion;
    int pixelVersion;
    size_t vertexBindingsBegin, vertexBindingsSize;
    size_t pixelBindingsBegin, pixelBindingsSize;
    size_t routingBegin, routingSize;
};
struct LinkerTechniqueSource
{
    char *text;
    size_t textSize;
    unsigned int passCount;
    LinkerTechniquePassSource passes[32];
};
// Binding/routing spans reference the owned text. Caller frees text and source.
bool Linker_ReadTechnique(const void *data, size_t size, LinkerTechniqueSource **source, char *error, size_t errorSize);

struct LinkerTechniqueBinding
{
    char destination[256];
    char expression[1024];
};
struct LinkerTechniqueBindings
{
    unsigned int count;
    LinkerTechniqueBinding entries[64];
};
bool Linker_ReadTechniqueBindings(const void *data, size_t size, LinkerTechniqueBindings *bindings, char *error,
                                  size_t errorSize);

struct MaterialVertexDeclaration;
// resourceDest maps the twelve engine input semantics to shader registers; -1 means unused.
bool Linker_BuildVertexRouting(const LinkerTechniqueBindings *bindings, const int *resourceDest,
                               MaterialVertexDeclaration *declaration, char *error, size_t errorSize);

struct LinkerShaderConstant;
struct MaterialShaderArgument;
// Resolves one material.* expression against one reflected scalar/vector or sampler parameter.
bool Linker_ResolveMaterialArgument(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                                    bool pixelShader, MaterialShaderArgument *argument, char *error, size_t errorSize);

// Resolves a code.* name, including nested names and individual array elements, to the engine constant index.
bool Linker_FindCodeConstant(const char *expression, unsigned int *index);

bool Linker_ResolveCodeArgument(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                                bool pixelShader, MaterialShaderArgument *argument, char *error, size_t errorSize);

bool Linker_ResolveCodeSampler(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                               MaterialShaderArgument *argument, char *error, size_t errorSize);

// literalStorage must remain alive while the returned argument is used.
bool Linker_ResolveLiteralArgument(const LinkerTechniqueBinding *binding, const LinkerShaderConstant *parameter,
                                   bool pixelShader, float *literalStorage, MaterialShaderArgument *argument,
                                   char *error, size_t errorSize);

struct MaterialPass;
// Renderer requirements inferred from an already compiled pass.
unsigned short Linker_TechniquePassFlags(const MaterialPass *pass);
// Installs an owned, sorted argument table and update counts. Caller frees pass->args (literals share its allocation).
bool Linker_BuildPassArguments(const MaterialShaderArgument *arguments, unsigned int count, MaterialPass *pass,
                               char *error, size_t errorSize);

// Resolves a shader stage's reflected parameters; returned arguments and literal data share one allocation.
bool Linker_BuildStageArguments(const LinkerTechniqueBindings *bindings, const LinkerShaderConstant *parameters,
                                unsigned int parameterCount, bool pixelShader, MaterialShaderArgument **arguments,
                                unsigned int *count, char *error, size_t errorSize);

struct MaterialVertexShader;
struct MaterialPixelShader;
// Shader pointers are borrowed. The resulting pass owns vertexDecl and args; state maps are compiled separately.
bool Linker_BuildTechniquePass(const LinkerTechniqueSource *source, unsigned int passIndex,
                               MaterialVertexShader *vertexShader, MaterialPixelShader *pixelShader, MaterialPass *pass,
                               char *error, size_t errorSize);

bool Linker_ApplyStateMap(const void *data, size_t size, const unsigned int *referenceBits, unsigned int toolFlags,
                          unsigned int *stateBits, char *error, size_t errorSize);
