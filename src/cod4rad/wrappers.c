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

int rand_int(void)
{
    /* Retail calls the MSVC CRT rand() directly (for example 0x408243 and
     * 0x411A45).  The prior x64 port seeded a TLS xorshift from the Windows
     * thread id, making identical one-thread compiles vary between runs and
     * changing every transport ray. */
    return rand();
}
