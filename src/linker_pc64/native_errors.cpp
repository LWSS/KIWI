#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <stdarg.h>
#include <stdio.h>

void Com_Error(errorParm_t code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    throw (int)code;
}
void MyAssertHandler(const char *file, int line, int, const char *format, ...)
{
    fprintf(stderr, "FX compiler assertion at %s:%d: ", file, line);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    throw (int)ERR_DROP;
}
