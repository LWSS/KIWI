#pragma once
#include <cstdint>

struct constantConfigString // sizeof=0x10
{
    int configStringNum;
    const char *configString;
    int configStringHash;
    int lowercaseConfigStringHash;
};

uint __cdecl lowercaseHash(const char *str);
void __cdecl CCS_InitConstantConfigStrings();
int __cdecl CCS_GetConstConfigStringIndex(const char *configString);
int __cdecl CCS_GetConfigStringNumForConstIndex(uint index);
uint __cdecl CCS_IsConfigStringIndexConstant(int index);


extern constantConfigString constantConfigStrings[833];
