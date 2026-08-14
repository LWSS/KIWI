/*
 * cod2rad.c — Main entry point and error handling for cod2rad.
 *
 * All functions verified against cod2rad64 LST line by line.
 * Source: cod2rad.cpp
 */

#include "cod2rad64.h"
#include <windows.h>

extern void __security_check_cookie(unsigned __int64);

/*
================
Com_Error

Formats an error message using vsnprintf into a 64KB stack buffer,
then calls ErrorMsg("%s", buf). The first parameter (errorChannel) is
unused inside the body but is part of the signature because all callers
pass an int channel in rcx.
================
*/
void Com_Error(int errorChannel, const char *fmt, ...)
{
    char buf[0x10000];
    va_list args;

    (void)errorChannel;

    va_start(args, fmt);
    vsnprintf(buf, 0x10000, fmt, args);
    va_end(args);

    ErrorMsg("%s", buf);
}

/*
================
Sys_IsMainThread

Always returns 1 (true). cod2rad is single-process, so the
calling thread is always the main thread.
================
*/
char Sys_IsMainThread(void)
{
    return 1;
}

/*
================
main

Program entry point. Initializes print callback, parses command
line, loads BSP, runs radiosity compilation, writes output BSP.
Prints elapsed time as hours/minutes/seconds.
Returns nonzero when command-line parsing or map loading fails.
================
*/
int main(int argc, const char **argv)
{
    unsigned int startTime, endTime;
    unsigned int totalMs;
    int totalSec, hours, minutes, seconds;

    setvbuf(stdout, NULL, _IONBF, 0);
    startTime = timeGetTime();

    SetErrorHandler(ErrorMsgV);

    if (!ParseCommandLine(argc, argv))
        return 1;

    SL_InitOrShutdown();

    if (!Map_Read(g_mapName))
        return 1;

    Compile(g_numThreads);

    Map_Write(g_mapName);

    endTime = timeGetTime();
    totalMs = endTime - startTime + 500; /* add 500ms for rounding */

    /* convert ms to seconds */
    totalSec = totalMs / 1000;

    /* break into hours, minutes, seconds (binary uses %60 separate divide) */
    hours = totalSec / 3600;
    minutes = (totalSec - hours * 3600) / 60;
    seconds = totalSec % 60;

    printf("\nEntire light compile finished in ");
    if (hours)
        printf("%i hours ", hours);
    if (minutes)
        printf("%i minutes ", minutes);
    printf("%i seconds\n", seconds);

    return 0;
}

/*
================
Com_ErrorMsg

Variant of Com_Error that takes an error level/channel as the
first parameter. Formats the message into a 1024-byte stack
buffer, null-terminates, then calls Com_Error to handle it.
================
*/
void Com_ErrorMsg(int errorLevel, const char *fmt, ...)
{
    char buf[0x400];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, 0x3FF, fmt, args);
    va_end(args);
    buf[0x3FF] = '\0';

    Com_Error(errorLevel, "%s", buf);
}
