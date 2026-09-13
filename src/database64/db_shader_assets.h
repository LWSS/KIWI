#pragma once
#include <stddef.h>
#include <stdint.h>

struct MaterialVertexShader;
struct MaterialPixelShader;
bool DB64_ValidateShaderProgram(const void *program, size_t words, bool pixelShader);
void DB64_LoadVertexShader(MaterialVertexShader **shader, bool atStreamStart);
void DB64_LoadPixelShader(MaterialPixelShader **shader, bool atStreamStart);
