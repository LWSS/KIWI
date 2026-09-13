#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_material.h>
#include "native_fx_support.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static thread_local LinkerFxServices s_services;
LinkerFxServices Linker_SetFxServices(LinkerFxServices services)
{
    const LinkerFxServices previous = s_services;
    s_services = services;
    return previous;
}

uint8_t *Hunk_AllocAlign(uint size, int alignment, const char *, int)
{
    if (!s_services.allocate || alignment <= 0 || (alignment & (alignment - 1)))
    {
        Com_Error(ERR_DROP, "FX compiler allocation service is missing or invalid");
    }
    void *result = s_services.allocate(size, alignment, s_services.context);
    if (!result)
    {
        Com_Error(ERR_DROP, "FX compiler allocation failed");
    }
    return (uint8_t *)result;
}

PhysPreset *FX_RegisterPhysPreset(const char *name)
{
    return s_services.preset ? s_services.preset(name, s_services.context) : NULL;
}

Material *Material_RegisterHandle(const char *name, int)
{
    return s_services.material ? s_services.material(name, s_services.context) : NULL;
}
XModel *FX_RegisterModel(const char *name)
{
    return s_services.model ? s_services.model(name, s_services.context) : NULL;
}
const FxEffectDef *FX_Register(const char *name)
{
    return s_services.effect ? s_services.effect(name, s_services.context) : NULL;
}
void ReplaceString(const char **out, const char *text)
{
    const size_t size = strlen(text) + 1;
    char *copy = (char *)Hunk_AllocAlign((uint)size, 1, "FX string", 8);
    memcpy(copy, text, size);
    *out = copy;
}
bool Sys_IsMainThread() { return true; }
bool Sys_IsRenderThread() { return false; }
bool Sys_IsDatabaseThread() { return false; }
bool Sys_IsServerThread() { return false; }

void Com_PrintError(int, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

void Com_Printf(int, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}
int I_stricmp(const char *a, const char *b) { return _stricmp(a, b); }
int I_strcmp(const char *a, const char *b) { return strcmp(a, b); }
void I_strncpyz(char *out, const char *text, int size)
{
    if (!out || !text || size <= 0)
    {
        Com_Error(ERR_DROP, "Invalid FX compiler string copy");
    }
    const size_t length = strlen(text);
    const size_t copied = length < (size_t)size - 1 ? length : (size_t)size - 1;
    memcpy(out, text, copied);
    out[copied] = 0;
}
int Com_sprintf(char *out, uint size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(out, size, format, args);
    va_end(args);
    return count < 0 || (unsigned int)count >= size ? -1 : count;
}

char *va(const char *format, ...)
{
    static thread_local char buffers[4][4096];
    static thread_local unsigned int index;
    char *result = buffers[index++ % 4];
    va_list args;
    va_start(args, format);
    vsnprintf(result, 4096, format, args);
    va_end(args);
    return result;
}

float Q_fabs(float value)
{
    return fabsf(value);
}
float Vec2Distance(const float *a, const float *b)
{
    const float x = a[0] - b[0], y = a[1] - b[1];
    return sqrtf(x * x + y * y);
}
float Vec2Normalize(float *value)
{
    const float length = sqrtf(value[0] * value[0] + value[1] * value[1]);
    const float scale = length > 0 ? 1 / length : 1;
    value[0] *= scale;
    value[1] *= scale;
    return length;
}
void Vec3Add(const float *a, const float *b, float *out)
{
    for (int i = 0; i < 3; ++i)
    {
        out[i] = a[i] + b[i];
    }
}
void Vec3Sub(const float *a, const float *b, float *out)
{
    for (int i = 0; i < 3; ++i)
    {
        out[i] = a[i] - b[i];
    }
}
void Vec3Avg(const float *a, const float *b, float *out)
{
    for (int i = 0; i < 3; ++i)
    {
        out[i] = (a[i] + b[i]) * 0.5f;
    }
}
void Vec3Scale(const float *value, float scale, float *out)
{
    for (int i = 0; i < 3; ++i)
    {
        out[i] = value[i] * scale;
    }
}
float Vec3LengthSq(const float *value)
{
    return value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
}
void Vec3Lerp(const float *a, const float *b, float fraction, float *out)
{
    for (int i = 0; i < 3; ++i)
    {
        out[i] = a[i] + (b[i] - a[i]) * fraction;
    }
}
void Material_GetInfo(Material *material, MaterialInfo *info)
{
    *info = material->info;
}
void Byte4PackVertexColor(const float *from, uint8_t *to)
{
    const int channels[] = {2, 1, 0, 3};
    for (int i = 0; i < 4; ++i)
    {
        const float value = nearbyintf(from[i] * 255.0f);
        to[channels[i]] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
    }
}
