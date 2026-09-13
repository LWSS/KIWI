#pragma once
#include <stddef.h>
struct MaterialVertexShader;
struct MaterialPixelShader;
// Input is a shader_bin file: 32-bit byte count followed by D3D bytecode. Caller frees the result.
bool Linker_ImportVertexShader(const void *data, size_t size, const char *name, MaterialVertexShader **shader,
                               char *error, size_t errorSize);
bool Linker_ImportPixelShader(const void *data, size_t size, const char *name, MaterialPixelShader **shader,
                              char *error, size_t errorSize);

// Returns register numbers for position, normal, color[0..1], texcoord[0..7]; unused entries are -1.
bool Linker_ReflectVertexInputs(const void *program, size_t words, int *resourceDest, char *error, size_t errorSize);

struct LinkerShaderConstant
{
    char name[256];
    unsigned int registerSet, registerIndex, registerCount;
    unsigned int parameterClass, parameterType, rows, columns, elements;
};
// Reflects non-struct constants and arrays. Caller frees the returned array.
bool Linker_ReflectShaderConstants(const void *program, size_t words, bool pixelShader,
                                   LinkerShaderConstant **constants, unsigned int *count, char *error,
                                   size_t errorSize);

// Resolves a shader source name through shader_bin/shader_names to a Shader Model 3 binary filename.
bool Linker_FindShaderBinary(const void *nameTable, size_t size, const char *shaderName, bool pixelShader,
                             char *filename, size_t filenameSize, char *error, size_t errorSize);
