/*
 * q_shared.c — Shared utility functions
 * String handling, byte-swap, formatting, math transforms.
 */

#include "cod2rad64.h"

/*
================
ShortSwap

Byte-swap a 16-bit value.
================
*/
int ShortSwap(int l)
{
    return ((unsigned char)l << 8) + ((unsigned char)(l >> 8));
}

/*
================
ShortNoSwap

Identity function — returns input unchanged.
================
*/
int ShortNoSwap(int l)
{
    return l;
}

/*
================
LongSwap

Byte-swap a 32-bit value.
================
*/
int LongSwap(int l)
{
    return ((l >> 24) & 0xFF)
         | ((l >> 8) & 0xFF00)
         | ((l << 8) & 0xFF0000)
         | ((l << 24) & 0xFF000000u);
}

/*
================
LongNoSwap

Identity function — returns input unchanged.
================
*/
int LongNoSwap(int l)
{
    return l;
}

/*
================
Long64Swap

Byte-swap a 64-bit value.
================
*/
unsigned long long Long64Swap(long long ll)
{
    return ((unsigned long long)((unsigned char)(ll >> 56)) <<  0)
         | ((unsigned long long)((unsigned char)(ll >> 48)) <<  8)
         | ((unsigned long long)((unsigned char)(ll >> 40)) << 16)
         | ((unsigned long long)((unsigned char)(ll >> 32)) << 24)
         | ((unsigned long long)((unsigned char)(ll >> 24)) << 32)
         | ((unsigned long long)((unsigned char)(ll >> 16)) << 40)
         | ((unsigned long long)((unsigned char)(ll >>  8)) << 48)
         | ((unsigned long long)((unsigned char)(ll >>  0)) << 56);
}

/*
================
Long64NoSwap

Identity function — returns input unchanged.
================
*/
long long Long64NoSwap(long long ll)
{
    return ll;
}

/*
================
FloatReadSwap

Byte-swap int to float.
================
*/
float FloatReadSwap(int f)
{
    int swapped = LongSwap(f);
    float result;
    memcpy(&result, &swapped, sizeof(result));
    return result;
}

/*
================
FloatReadNoSwap

Reinterpret int as float.
================
*/
float FloatReadNoSwap(int f)
{
    float result;
    memcpy(&result, &f, sizeof(result));
    return result;
}

/*
================
FloatWriteSwap

Byte-swap float to int.
================
*/
int FloatWriteSwap(float f)
{
    int bits;
    memcpy(&bits, &f, sizeof(bits));
    return LongSwap(bits);
}

/*
================
FloatWriteNoSwap

Reinterpret float as int.
================
*/
int FloatWriteNoSwap(float f)
{
    int bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/*
================
FloatSwap

Byte-swap a float value (float in, float out).
================
*/
float FloatSwap(float f)
{
    int bits;
    memcpy(&bits, &f, sizeof(bits));
    bits = LongSwap(bits);
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/*
================
LongSwap64

Byte-swap a 64-bit integer (alias for Long64Swap).
================
*/
unsigned long long LongSwap64(long long ll)
{
    return Long64Swap(ll);
}

/*
================
FloatSwap64

Byte-swap a 64-bit float (double).
================
*/
double FloatSwap64(double d)
{
    long long bits;
    memcpy(&bits, &d, sizeof(bits));
    bits = (long long)Long64Swap(bits);
    memcpy(&d, &bits, sizeof(d));
    return d;
}

#define VA_BUFFER_SIZE        31999    /* g_vaBuffer payload length (index of terminator) */
#define VA_CIRCULAR_BUF_SIZE  0x7D00   /* g_vaBuffer total length = VA_BUFFER_SIZE + 1 */

static char g_vaBuffer[VA_CIRCULAR_BUF_SIZE];
static char g_vaCircularBuf[VA_CIRCULAR_BUF_SIZE];
static int g_vaBufferIdx;

/*
================
va

Variadic sprintf into circular buffer.
================
*/
char *va(const char *format, ...)
{
    unsigned int len;
    int index;
    char *result;
    va_list arglist;

    va_start(arglist, format);
    len = _vsnprintf(g_vaBuffer, VA_CIRCULAR_BUF_SIZE, format, arglist);
    va_end(arglist);
    g_vaBuffer[VA_BUFFER_SIZE] = 0;
    if (len >= VA_CIRCULAR_BUF_SIZE)
        Com_Error(1, "Attempted to overrun string in call to va()\n");
    index = g_vaBufferIdx;
    if ((int)(g_vaBufferIdx + len) >= VA_BUFFER_SIZE)
        index = 0;
    result = g_vaCircularBuf + index;
    memmove(result, g_vaBuffer, len + 1);
    g_vaBufferIdx = index + len + 1;
    return result;
}

/*
================
CanKeepStringPointer

Returns 0 if pointer is on stack or in va() circular buffer.
================
*/
int CanKeepStringPointer(const char *ptr)
{
    char stackLocal;
    if ((uintptr_t)ptr >= (uintptr_t)&stackLocal &&
        (uintptr_t)ptr < (uintptr_t)&stackLocal + 0x2000)
        return 0;
    if ((uintptr_t)ptr >= (uintptr_t)g_vaCircularBuf &&
        (uintptr_t)ptr < (uintptr_t)g_vaCircularBuf + sizeof(g_vaCircularBuf))
        return 0;
    return 1;
}

static char s_assertDisable_Com_StripExtension_in;

/*
================
Com_StripExtension

Strips file extension, copies result to out buffer.
================
*/
void Com_StripExtension(const char *in, char *out)
{
    const char *dot = NULL;
    const char *p = in;

    Assert(in, s_assertDisable_Com_StripExtension_in);

    while (*p)
    {
        if (*p == '.')
            dot = p;
        else if (*p == '/' || *p == '\\')
            dot = NULL;
        p++;
    }
    if (!dot)
        dot = p;
    while (in != dot)
        *out++ = *in++;
    *out = 0;
}

static char s_assertDisable_I_strncpyz_src;
static char s_assertDisable_I_strncpyz_dest;
static char s_assertDisable_I_strncpyz_size;
static char s_assertDisable_Com_AssembleFilepath_folder;
static char s_assertDisable_Com_AssembleFilepath_name;
static char s_assertDisable_Com_AssembleFilepath_ext;
static char s_assertDisable_Com_AssembleFilepath_path;
static char s_assertDisable_Com_AssembleFilepath_count;
static char s_assertDisable_I_stristr_wild;
static char s_assertDisable_I_stristr_s;

/*
================
I_strncpyz

Safe string copy with guaranteed null termination.
================
*/
char *I_strncpyz(char *dest, const char *src, int destsize)
{
    Assert(src, s_assertDisable_I_strncpyz_src);
    Assert(dest, s_assertDisable_I_strncpyz_dest);
    Assert(destsize >= 1, s_assertDisable_I_strncpyz_size);
    strncpy(dest, src, destsize - 1);
    dest[destsize - 1] = 0;
    return dest;
}

/*
================
Com_sprintf

Safe sprintf into a fixed-size buffer.
================
*/
int Com_sprintf(char *dest, int size, const char *format, ...)
{
    int result;
    va_list va;

    va_start(va, format);
    result = _vsnprintf(dest, size, format, va);
    va_end(va);
    dest[size - 1] = 0;
    return result;
}

/*
================
I_islower

Returns 1 if character is lowercase letter.
================
*/
int I_islower(int c)
{
    return (unsigned int)(c - 'a') <= 25;
}

/*
================
I_strncmp

Compare at most n characters, returns -1/0/1.
================
*/
int I_strncmp(const char *s1, const char *s2, int n)
{
    int c1, c2;

    do
    {
        c1 = *s1++;
        c2 = *s2++;
        if (!n--)
            return 0;
        if (c1 != c2)
            return c1 < c2 ? -1 : 1;
    } while (c1);

    return 0;
}

/*
================
I_strcmp

Case-sensitive string compare.
================
*/
static char s_assertDisable_I_strcmp_s0;
static char s_assertDisable_I_strcmp_s1;

int I_strcmp(const char *s0, const char *s1)
{
    int c0, c1;
    int n = 0x7FFFFFFF;

    Assert(s0, s_assertDisable_I_strcmp_s0);
    Assert(s1, s_assertDisable_I_strcmp_s1);

    do
    {
        c0 = *s0++;
        c1 = *s1++;
        if (!n--)
            return 0;
        if (c0 != c1)
            return c0 < c1 ? -1 : 1;
    } while (c0);

    return 0;
}

/*
================
I_stricmp

Case-insensitive string compare.
================
*/
static char s_assertDisable_I_stricmp_s0;
static char s_assertDisable_I_stricmp_s1;

int I_stricmp(const char *s0, const char *s1)
{
    int c0, c1;
    int n = 0x7FFFFFFF;

    Assert(s0, s_assertDisable_I_stricmp_s0);
    Assert(s1, s_assertDisable_I_stricmp_s1);

    do
    {
        c0 = *s0++;
        c1 = *s1++;
        if (!n--)
            return 0;
        if (c0 != c1)
        {
            if ((unsigned int)(c0 - 'a') <= 25) c0 -= 32;
            if ((unsigned int)(c1 - 'a') <= 25) c1 -= 32;
            if (c0 != c1)
                return c0 < c1 ? -1 : 1;
        }
    } while (c0);

    return 0;
}

/*
================
I_strnicmp

Case-insensitive compare of at most n characters.
================
*/
int I_strnicmp(const char *s0, const char *s1, int n)
{
    int c0, c1;

    do
    {
        c0 = *s0++;
        c1 = *s1++;
        if (!n--)
            return 0;
        if (c0 != c1)
        {
            if ((unsigned int)(c0 - 'a') <= 25) c0 -= 32;
            if ((unsigned int)(c1 - 'a') <= 25) c1 -= 32;
            if (c0 != c1)
                return c0 < c1 ? -1 : 1;
        }
    } while (c0);

    return 0;
}

/*
================
I_strlwr

Convert string to lowercase in place.
================
*/
char *I_strlwr(char *s)
{
    char *p = s;
    while (*p)
    {
        if ((unsigned int)(*p - 'A') <= 25)
            *p += 32;
        p++;
    }
    return s;
}

/*
================
Com_AssembleFilepath

Concatenates folder + name + extension into path buffer.
================
*/
void Com_AssembleFilepath(const char *folder, const char *name, const char *extension, char *path, int maxCharCount)
{
    int folderLen, nameLen, extLen;

    Assert(folder, s_assertDisable_Com_AssembleFilepath_folder);
    Assert(name, s_assertDisable_Com_AssembleFilepath_name);
    Assert(extension, s_assertDisable_Com_AssembleFilepath_ext);
    Assert(path, s_assertDisable_Com_AssembleFilepath_path);
    Assert(maxCharCount > 0, s_assertDisable_Com_AssembleFilepath_count);

    folderLen = strlen(folder);
    nameLen = strlen(name);
    extLen = strlen(extension) + 1;
    if (folderLen + nameLen + extLen - 1 >= maxCharCount)
        Com_Error(1, "filepath '%s%s%s' is longer than %i characters", folder, name, extension, maxCharCount - 1);
    memmove(path, folder, folderLen);
    memmove(path + folderLen, name, nameLen);
    memmove(path + folderLen + nameLen, extension, extLen);
}

/*
================
I_stristr

Wildcard pattern match. Handles * and ? wildcards, case-insensitive.
Returns 0 on match, -1 or 1 on mismatch.
================
*/
int I_stristr(const char *wild, const char *s)
{
    char w, c;
    int diff;

    Assert(wild, s_assertDisable_I_stristr_wild);
    Assert(s, s_assertDisable_I_stristr_s);

    while (1)
    {
        while (1)
        {
            w = *wild++;
            if (w != '*')
                break;
            if (!*wild || (*s && !I_stristr(wild - 1, s + 1)))
                return 0;
        }
        c = *s++;
        if (w != c && w != '?')
        {
            diff = tolower((int)(signed char)w) - tolower((int)(signed char)c);
            if (diff)
                return diff < 0 ? -1 : 1;
        }
        if (!w)
            return 0;
    }
}

/*
================
MatrixTransformPoint

Transforms a point by orientation matrix (with translation).
================
*/
static char s_assertDisable_MatrixTransformPoint;

void MatrixTransformPoint(float *mat, float *pos, float *out)
{
    Assert(pos != out, s_assertDisable_MatrixTransformPoint);
    out[0] = mat[3] * pos[0] + mat[0] + mat[6] * pos[1] + mat[9] * pos[2];
    out[1] = mat[4] * pos[0] + mat[1] + mat[7] * pos[1] + mat[10] * pos[2];
    out[2] = mat[5] * pos[0] + mat[2] + mat[8] * pos[1] + mat[11] * pos[2];
}

/*
================
MatrixTransformDirection

Transforms a direction by orientation matrix (no translation).
================
*/
static char s_assertDisable_MatrixTransformDirection;

void MatrixTransformDirection(float *mat, float *dir, float *out)
{
    Assert(dir != out, s_assertDisable_MatrixTransformDirection);
    out[0] = mat[3] * dir[0] + mat[6] * dir[1] + mat[9] * dir[2];
    out[1] = mat[4] * dir[0] + mat[7] * dir[1] + mat[10] * dir[2];
    out[2] = mat[5] * dir[0] + mat[8] * dir[1] + mat[11] * dir[2];
}

/*
================
MatrixTransformVector3

Pure 3x3 matrix-vector multiply (no translation). Source: com_math_429890.
Asserts in1 != out (aliasing not allowed).
Computes out[i] = in1[0]*mat[i] + in1[1]*mat[i+3] + in1[2]*mat[i+6].
================
*/
static char s_assertDisable_MatrixTransformVector3;

void MatrixTransformVector3(float *in1, float *mat, float *out)
{
    Assert(in1 != (float *)out, s_assertDisable_MatrixTransformVector3);
    out[0] = in1[0] * mat[0] + in1[1] * mat[3] + in1[2] * mat[6];
    out[1] = in1[0] * mat[1] + in1[1] * mat[4] + in1[2] * mat[7];
    out[2] = in1[0] * mat[2] + in1[1] * mat[5] + in1[2] * mat[8];
}

/*
 * Byte swap function pointers — set by Swap_Init.
 * The _BigFloat slot holds a function that reinterprets int bits as
 * float (callee reads ECX, returns via XMM0). The _LittleFloat slot
 * holds the reverse (callee reads XMM0, returns via EAX).
 */
static int (*_BigShort)(int);
static int (*_BigLong)(int);
static float (*_BigFloat)(int);
static int (*_LittleShort)(int);
static int (*_LittleLong)(int);
static long long (*_LittleLong64)(long long);
static int (*_LittleFloat)(float);

/*
================
Swap_Init

Runtime endian probe that installs byte-swap pointers. On a
little-endian host, writes the LE set (Swap into the _Little*
slots, NoSwap into the _Big* slots); on a big-endian host, writes
the BE set (the inverse).
================
*/
void Swap_Init(void)
{
    _LittleShort  = ShortNoSwap;
    _BigShort     = ShortSwap;
    _LittleLong   = LongNoSwap;
    _BigLong      = LongSwap;
    _LittleLong64 = Long64NoSwap;
    _BigFloat     = FloatReadNoSwap;
    _LittleFloat  = FloatWriteNoSwap;
}

/*
================
BigShort

Indirect call through _BigShort function pointer.
================
*/
short BigShort(short value)
{
    return (short)_BigShort(value);
}

/*
================
BigLong

Indirect call through _BigLong function pointer.
================
*/
int BigLong(int value)
{
    return _BigLong(value);
}

/*
================
BigFloat

Indirect call through _BigFloat function pointer.
================
*/
float BigFloat(float value)
{
    int bits;
    memcpy(&bits, &value, sizeof(bits));
    return _BigFloat(bits);
}

/*
================
Swap_Init_BigEndian

Sets byte swap function pointers for big-endian data conversion.
On little-endian x64: BigShort/Long/Float swap bytes,
LittleShort/Long pass through unchanged.
================
*/
void Swap_Init_BigEndian(void)
{
    union { short s; char c[2]; } probe;
    probe.c[0] = 1;
    probe.c[1] = 0;
    if (probe.s == 1)
    {
        _LittleShort  = ShortSwap;
        _BigShort     = ShortNoSwap;
        _LittleLong   = LongSwap;
        _BigLong      = LongNoSwap;
        _LittleLong64 = (long long (*)(long long))LongSwap64;
        _BigFloat     = FloatReadSwap;
        _LittleFloat  = FloatWriteSwap;
    }
    else
    {
        _LittleShort  = ShortNoSwap;
        _BigShort     = ShortSwap;
        _LittleLong   = LongNoSwap;
        _BigLong      = LongSwap;
        _LittleLong64 = Long64NoSwap;
        _BigFloat     = FloatReadNoSwap;
        _LittleFloat  = FloatWriteNoSwap;
    }
}
