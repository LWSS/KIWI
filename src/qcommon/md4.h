#pragma once
#include <cstdint>

struct MD4_CTX // sizeof=0x58
{                                       // ...
    uint state[4];
    uint count[2];
    uint8_t buffer[64];
};

void __cdecl Com_BlockChecksum128(uint8_t *buffer, uint length, int key, uint8_t *outChecksum);
void __cdecl Com_BlockChecksum128Cat(
    uint8_t *buffer0,
    uint length0,
    uint8_t *buffer1,
    uint length1,
    uint8_t *outChecksum);

void __cdecl MD4Init(MD4_CTX *context);
void __cdecl MD4Update(MD4_CTX *context, uint8_t *input, uint inputLen);
void __cdecl MD4Final(uint8_t *digest, MD4_CTX *context);
void __cdecl MD4Transform(uint *state, uint8_t *block);
