/*
 * wrappers.c — thin forwards from cod2rad's *_wrap/*_fast names to CRT.
 * These exist because the binary had inlined/specialized CRT calls that
 * our decompilation abstracted under wrapper names.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void memcpy_fast(void *dst, const void *src, int size)
{
    memcpy(dst, src, (size_t)size);
}

void memset_fast(void *dst, int val, int size)
{
    memset(dst, val, (size_t)size);
}

void *fopen_wrap(const char *path, const char *mode)
{
    return fopen(path, mode);
}

void fseek_wrap(void *file, int offset, int whence)
{
    fseek((FILE *)file, offset, whence);
}

int ftell_wrap(void *file)
{
    return (int)ftell((FILE *)file);
}

long long fread_wrap(void *dst, int elemSize, long long count, void *file)
{
    return (long long)fread(dst, (size_t)elemSize, (size_t)count, (FILE *)file);
}

void fclose_wrap(void *file)
{
    fclose((FILE *)file);
}

double strtof_wrap(const char *str)
{
    return strtod(str, NULL);
}

int sscanf_wrap(const char *str, const char *fmt, ...)
{
    int result;
    va_list ap;
    va_start(ap, fmt);
    result = vsscanf(str, fmt, ap);
    va_end(ap);
    return result;
}

int sprintf_wrap(char *buf, const char *fmt, ...)
{
    int result;
    va_list ap;
    va_start(ap, fmt);
    result = vsprintf(buf, fmt, ap);
    va_end(ap);
    return result;
}

int atoi_wrap(const char *str)
{
    return atoi(str);
}

void qsort_wrapper(void *base, long long num, int size,
                   int (*cmp)(const void *, const void *))
{
    qsort(base, (size_t)num, (size_t)size, cmp);
}

float ceilf_wrapper(float x)
{
    return ceilf(x);
}

static __declspec(thread) unsigned int tls_rng_state;

int rand_int(void)
{
    unsigned int s = tls_rng_state;
    if (!s) s = GetCurrentThreadId() * 2654435761u + 1;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    tls_rng_state = s;
    return (int)(s & 0x7FFF);
}

void rand_seed_pixel(int lmapIdx, int s, int t)
{
    unsigned int h = (unsigned int)lmapIdx * 73856093u ^ (unsigned int)s * 19349663u ^ (unsigned int)t * 83492791u;
    if (!h) h = 1;
    h ^= h << 13;
    h ^= h >> 17;
    h ^= h << 5;
    tls_rng_state = h;
}
