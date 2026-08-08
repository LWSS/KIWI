#include <universal/q_shared.h>
#include "ui_shared.h"

int __cdecl compare_impact_files(const char **pe0, const char **pe1)
{
    return I_stricmp(*pe0, *pe1);
}

int __cdecl compare_hudelems(const void *pe0, const void *pe1)
{
    float delta; // [esp+0h] [ebp-Ch]

    delta = *(float *)(*(uint *)pe0 + 128) - *(float *)(*(uint *)pe1 + 128);
    if (delta >= 0.0)
        return delta > 0.0;
    else
        return -1;
}

void __cdecl compare_doesStringEqualString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_STRING), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = I_stricmp(leftSide->internals.string, rightSide->internals.string) == 0;
}

void __cdecl compare_doesStringNotEqualString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_STRING), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = I_stricmp(leftSide->internals.string, rightSide->internals.string) != 0;
}

void __cdecl compare_doesIntEqualInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.intVal == rightSide->internals.intVal;
}

void __cdecl compare_doesIntNotEqualInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.intVal != rightSide->internals.intVal;
}

void __cdecl compare_doesIntEqualFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal == (double)leftSide->internals.intVal;
}

void __cdecl compare_doesFloatEqualInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    compare_doesIntEqualFloat(rightSide, leftSide, result);
}

void __cdecl compare_doesFloatEqualFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal == leftSide->internals.floatVal;
}

void __cdecl compare_doesIntNotEqualFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal != (double)leftSide->internals.intVal;
}

void __cdecl compare_doesFloatNotEqualInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    compare_doesIntNotEqualFloat(rightSide, leftSide, result);
}

void __cdecl compare_doesFloatNotEqualFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal != leftSide->internals.floatVal;
}

void __cdecl compare_isIntLessThanInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.intVal < rightSide->internals.intVal;
}

void __cdecl compare_isIntLessThanFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal > (double)leftSide->internals.intVal;
}

void __cdecl compare_isFloatLessThanInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.floatVal < (double)rightSide->internals.intVal;
}

void __cdecl compare_isFloatLessThanFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal > (double)leftSide->internals.floatVal;
}

void __cdecl compare_isIntLessThanEqualToInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.intVal <= rightSide->internals.intVal;
}

void __cdecl compare_isFloatGreaterThanEqualToInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.floatVal >= (double)rightSide->internals.intVal;
}

void __cdecl compare_isIntLessThanEqualToFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    compare_isFloatGreaterThanEqualToInt(rightSide, leftSide, result);
}

void __cdecl compare_isFloatLessThanEqualToInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.floatVal <= (double)rightSide->internals.intVal;
}

void __cdecl compare_isFloatLessThanEqualToFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal >= (double)leftSide->internals.floatVal;
}

void __cdecl compare_isIntGreaterThanInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    compare_isIntLessThanInt(rightSide, leftSide, result);
}

void __cdecl compare_isIntGreaterThanFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    compare_isFloatLessThanInt(rightSide, leftSide, result);
}

void __cdecl compare_isFloatGreaterThanInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.floatVal > (double)rightSide->internals.intVal;
}

void __cdecl compare_isFloatGreaterThanFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal < (double)leftSide->internals.floatVal;
}

void __cdecl compare_isIntGreaterThanEqualToInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.intVal >= rightSide->internals.intVal;
}

void __cdecl compare_isIntGreaterThanEqualToFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal <= (double)leftSide->internals.intVal;
}

void __cdecl compare_isFloatGreaterThanEqualToFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.floatVal <= (double)leftSide->internals.floatVal;
}

void __cdecl add_IntWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.intVal + leftSide->internals.intVal;
}

void __cdecl add_IntWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    result->internals.floatVal = (double)leftSide->internals.intVal + rightSide->internals.floatVal;
}

void __cdecl add_FloatWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    add_IntWithFloat(rightSide, leftSide, result);
}

void __cdecl add_FloatWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    result->internals.floatVal = leftSide->internals.floatVal + rightSide->internals.floatVal;
}

char resultStr[256];
void __cdecl add_StringWithString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    char rightSideStr[256]; // [esp+0h] [ebp-208h] BYREF
    char leftSideStr[260]; // [esp+100h] [ebp-108h] BYREF

    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_STRING), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_STRING;
    I_strncpyz(leftSideStr, (char *)leftSide->internals.intVal, 256);
    I_strncpyz(rightSideStr, (char *)rightSide->internals.intVal, 256);
    Com_sprintf(resultStr, 0x100u, "%s%s", leftSideStr, rightSideStr);
    result->internals.intVal = (int)resultStr;
}

char resultStr_0[256];
void __cdecl add_StringWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    char leftSideStr[260]; // [esp+0h] [ebp-108h] BYREF

    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_STRING;
    I_strncpyz(leftSideStr, (char *)leftSide->internals.intVal, 256);
    Com_sprintf(resultStr_0, 0x100u, "%s%i", leftSideStr, rightSide->internals.intVal);
    result->internals.intVal = (int)resultStr_0;
}

char resultStr_1[256];
void __cdecl add_IntWithString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    char rightSideStr[260]; // [esp+0h] [ebp-108h] BYREF

    vassert((leftSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    vassert((rightSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    result->dataType = VAL_STRING;
    I_strncpyz(rightSideStr, (char *)rightSide->internals.intVal, 256);
    Com_sprintf(resultStr_1, 0x100u, "%i%s", leftSide->internals.intVal, rightSideStr);
    result->internals.intVal = (int)resultStr_1;
}

char resultStr_2[256];
void __cdecl add_FloatWithString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    char rightSideStr[260]; // [esp+Ch] [ebp-108h] BYREF

    vassert((leftSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    vassert((rightSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    result->dataType = VAL_STRING;
    I_strncpyz(rightSideStr, (char *)rightSide->internals.intVal, 256);
    Com_sprintf(resultStr_2, 0x100u, "%f%s", leftSide->internals.floatVal, rightSideStr);
    result->internals.intVal = (int)resultStr_2;
}

char resultStr_3[256];
void __cdecl add_StringWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    char leftSideStr[260]; // [esp+8h] [ebp-108h] BYREF

    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_STRING;
    I_strncpyz(leftSideStr, (char *)leftSide->internals.intVal, 256);
    Com_sprintf(resultStr_3, 0x100u, "%s%f", leftSideStr, rightSide->internals.floatVal);
    result->internals.intVal = (int)resultStr_3;
}

void __cdecl multiply_IntByInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = rightSide->internals.intVal * leftSide->internals.intVal;
}

void __cdecl multiply_IntByFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    result->internals.floatVal = (double)leftSide->internals.intVal * rightSide->internals.floatVal;
}

void __cdecl multiply_FloatByInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    multiply_IntByFloat(rightSide, leftSide, result);
}

void __cdecl multiply_FloatByFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    result->internals.floatVal = leftSide->internals.floatVal * rightSide->internals.floatVal;
}

void __cdecl divide_IntByInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    if (rightSide->internals.intVal)
        result->internals.floatVal = (double)leftSide->internals.intVal / (double)rightSide->internals.intVal;
    else
        result->internals.floatVal = 0.0;
}

void __cdecl divide_IntByFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    if (rightSide->internals.floatVal == 0.0)
        result->internals.floatVal = 0.0;
    else
        result->internals.floatVal = (double)leftSide->internals.intVal / rightSide->internals.floatVal;
}

void __cdecl divide_FloatByInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    if (rightSide->internals.intVal)
        result->internals.floatVal = leftSide->internals.floatVal / (double)rightSide->internals.intVal;
    else
        result->internals.floatVal = 0.0;
}

void __cdecl divide_FloatByFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    if (rightSide->internals.floatVal == 0.0)
        result->internals.floatVal = 0.0;
    else
        result->internals.floatVal = leftSide->internals.floatVal / rightSide->internals.floatVal;
}

void __cdecl mod_IntByInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    if (rightSide->internals.intVal)
        result->internals.intVal = leftSide->internals.intVal % rightSide->internals.intVal;
    else
        result->internals.intVal = leftSide->internals.intVal;
}

void __cdecl mod_FloatByInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    if (rightSide->internals.intVal)
        result->internals.intVal = SnapFloatToInt(leftSide->internals.floatVal) % rightSide->internals.intVal;
    else
        result->internals.intVal = leftSide->internals.intVal;
}

void __cdecl mod_IntByFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    int right; // [esp+10h] [ebp-4h]

    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    right = SnapFloatToInt(rightSide->internals.floatVal);
    if (right)
        result->internals.intVal = leftSide->internals.intVal % right;
    else
        result->internals.intVal = leftSide->internals.intVal;
}

void __cdecl mod_FloatByFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    int right; // [esp+38h] [ebp-4h]

    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    right = SnapFloatToInt(rightSide->internals.floatVal);
    if (right)
        result->internals.intVal = SnapFloatToInt(leftSide->internals.floatVal) % right;
    else
        result->internals.intVal = SnapFloatToInt(leftSide->internals.floatVal);
}

void __cdecl and_IntWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = leftSide->internals.intVal && rightSide->internals.intVal;
    result->internals.intVal = v3;
}

void __cdecl and_FloatWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = leftSide->internals.floatVal != 0.0 && rightSide->internals.intVal;
    result->internals.intVal = v3;
}

void __cdecl and_IntWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    and_FloatWithInt(rightSide, leftSide, result);
}

void __cdecl and_StringWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = *(_BYTE *)leftSide->internals.intVal && rightSide->internals.intVal;
    result->internals.intVal = v3;
}

void __cdecl and_IntWithString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    and_StringWithInt(rightSide, leftSide, result);
}

void __cdecl and_StringWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = *(_BYTE *)leftSide->internals.intVal && rightSide->internals.floatVal != 0.0;
    result->internals.intVal = v3;
}

void __cdecl and_FloatWithString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    and_StringWithFloat(rightSide, leftSide, result);
}

void __cdecl and_FloatWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = leftSide->internals.floatVal != 0.0 && rightSide->internals.floatVal != 0.0;
    result->internals.intVal = v3;
}

void __cdecl or_IntWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = leftSide->internals.intVal || rightSide->internals.intVal;
    result->internals.intVal = v3;
}

void __cdecl or_FloatWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = leftSide->internals.floatVal != 0.0 || rightSide->internals.intVal;
    result->internals.intVal = v3;
}

void __cdecl or_IntWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    or_FloatWithInt(rightSide, leftSide, result);
}

void __cdecl or_StringWithInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = *(_BYTE *)leftSide->internals.intVal || rightSide->internals.intVal;
    result->internals.intVal = v3;
}

void __cdecl or_IntWithString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    or_StringWithInt(rightSide, leftSide, result);
}

void __cdecl or_StringWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_STRING), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = *(_BYTE *)leftSide->internals.intVal || rightSide->internals.floatVal != 0.0;
    result->internals.intVal = v3;
}

void __cdecl or_FloatWithString(Operand *leftSide, Operand *rightSide, Operand *result)
{
    or_StringWithFloat(rightSide, leftSide, result);
}

void __cdecl or_FloatWithFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    bool v3; // [esp+0h] [ebp-4h]

    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    v3 = leftSide->internals.floatVal != 0.0 || rightSide->internals.floatVal != 0.0;
    result->internals.intVal = v3;
}

void __cdecl bitwiseAnd(Operand *leftSide, Operand *rightSide, Operand *result)
{
    operandInternalDataUnion v3; // esi

    result->dataType = VAL_INT;
    v3.intVal = GetSourceInt(leftSide).intVal;
    result->internals.intVal = GetSourceInt(rightSide).intVal & v3.intVal;
}

void __cdecl bitwiseOr(Operand *leftSide, Operand *rightSide, Operand *result)
{
    operandInternalDataUnion v3; // esi

    result->dataType = VAL_INT;
    v3.intVal = GetSourceInt(leftSide).intVal;
    result->internals.intVal = GetSourceInt(rightSide).intVal | v3.intVal;
}


void __cdecl subtract_IntFromInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_INT;
    result->internals.intVal = leftSide->internals.intVal - rightSide->internals.intVal;
}

void __cdecl subtract_FloatFromInt(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_INT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    result->internals.floatVal = (double)leftSide->internals.intVal - rightSide->internals.floatVal;
}

void __cdecl subtract_IntFromFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_INT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    result->internals.floatVal = leftSide->internals.floatVal - (double)rightSide->internals.intVal;
}

void __cdecl subtract_FloatFromFloat(Operand *leftSide, Operand *rightSide, Operand *result)
{
    vassert((leftSide->dataType == VAL_FLOAT), "(leftSide->dataType) = %i", leftSide->dataType);
    vassert((rightSide->dataType == VAL_FLOAT), "(rightSide->dataType) = %i", rightSide->dataType);
    result->dataType = VAL_FLOAT;
    result->internals.floatVal = leftSide->internals.floatVal - rightSide->internals.floatVal;
}

