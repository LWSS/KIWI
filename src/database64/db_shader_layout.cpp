#include "db_shader_assets.h"
#include <string.h>

bool DB64_ValidateShaderProgram(const void *program, size_t words, bool pixelShader)
{
    if (!program || words < 2 || words > UINT16_MAX)
    {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)program;
    uint32_t token;
    memcpy(&token, bytes, sizeof(uint32_t));
    const unsigned int major = (token >> 8) & 255;
    if ((token >> 16) != (pixelShader ? 0xFFFF : 0xFFFE) || (major != 2 && major != 3))
    {
        return false;
    }
    // Shader models 2/3 encode each instruction's parameter-token count.
    // Validate the walk before handing the unbounded D3D9 API a bytecode pointer.
    for (size_t position = 1; position < words;)
    {
        memcpy(&token, bytes + position * sizeof(uint32_t), sizeof(uint32_t));
        if (token == 0x0000FFFF)
        {
            return position + 1 == words;
        }
        const unsigned int opcode = token & 0xFFFF;
        const size_t parameters = opcode == 0xFFFE ? (token >> 16) & 0x7FFF : (token >> 24) & 15;
        if (opcode == 0xFFFF || parameters > words - position - 1)
        {
            return false;
        }
        position += parameters + 1;
    }
    return false;
}
