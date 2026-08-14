/*
 * print.c — Console output, warnings, errors, and progress tracking.
 */

#include "cod2rad64.h"

#define PROGRESS_PRINT_THROTTLE_MS 500

static int g_progressCurrent;
static int g_progressTotal;
static DWORD g_progressStartTime;
static int g_progressNextPrintTime;
static int g_progressLastPercent;
/* g_warningLevel and g_extraVerbose — in cod2rad64.h */

static char s_assertDisable_UpdateProgressPrint;

/*
================
Com_Printf

Print a formatted message to stdout.
================
*/
void Com_Printf(const char *fmt, ...)
{
    va_list arglist;
    va_start(arglist, fmt);
    vprintf(fmt, arglist);
    va_end(arglist);
}

/*
================
WarningMsg

Print a warning if level <= warning level threshold.
================
*/
void WarningMsg(int level, const char *fmt, ...)
{
    va_list arglist;

    if (level <= g_warningLevel)
    {
        va_start(arglist, fmt);
        vprintf(fmt, arglist);
        va_end(arglist);
    }
}

/*
================
ErrorMsgV

Print error message and exit. Takes va_list.
================
*/
void __declspec(noreturn) ErrorMsgV(const char *fmt, va_list arglist)
{
    printf("\n");
    vprintf(fmt, arglist);
    if (fmt[strlen(fmt) - 1] != '\n')
        printf("\n");
    exit(-1);
}

/*
================
ErrorMsg

Print error message and exit. Variadic wrapper.
================
*/
void __declspec(noreturn) ErrorMsg(const char *fmt, ...)
{
    va_list arglist;
    va_start(arglist, fmt);
    ErrorMsgV(fmt, arglist);
}

/*
================
PrintMsecDuration

Print a duration in human-readable form.
================
*/
void PrintMsecDuration(int msec)
{
    int seconds = (msec + 500) / 1000;
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = (seconds % 3600) % 60;

    if (hours)
        Com_Printf("%i:%02i:%02i", hours, minutes, secs);
    else if (minutes)
        Com_Printf("%i:%02i", minutes, secs);
    else
        Com_Printf("%i seconds", secs);
}

/*
================
UpdateProgressPrint

Update progress bar display if enough time has passed.
================
*/
void UpdateProgressPrint(void)
{
    float percent;
    int pct;
    int elapsed;

    if (g_extraVerbose)
        return;

    Assert(g_progressTotal, s_assertDisable_UpdateProgressPrint);

    percent = (float)g_progressCurrent * 100.0f / (float)g_progressTotal;
    pct = (int)floorf(percent * 10.0f + 0.5f);  /* tenths-of-percent, rounded */
    if (pct != g_progressLastPercent)
    {
        g_progressLastPercent = pct;
        elapsed = timeGetTime() - g_progressStartTime;
        if (elapsed >= g_progressNextPrintTime)
        {
            g_progressNextPrintTime = elapsed + PROGRESS_PRINT_THROTTLE_MS;
            Com_Printf("%i.%i%% complete", pct / 10, pct % 10);
            Com_Printf(", ");
            PrintMsecDuration(elapsed);
            Com_Printf(" done, ");
            PrintMsecDuration((int)((float)elapsed / percent * (100.0f - percent)));
            Com_Printf(" remaining               ");
            Com_Printf("\r");
        }
    }
}

/*
================
BeginProgress

Start a progress operation with a label.
================
*/
void BeginProgress(const char *label)
{
    g_progressCurrent = 0;
    g_progressTotal = 1;
    g_progressStartTime = timeGetTime();
    g_progressNextPrintTime = 2000;
    g_progressLastPercent = 0;
    Com_Printf("----------------------------------------\n%s\n", label);
    UpdateProgressPrint();
}

/*
================
SetProgress

Set progress current and total values.
================
*/
void SetProgress(int current, int total)
{
    g_progressCurrent = current;
    g_progressTotal = total;
    if (total)
        UpdateProgressPrint();
}

/*
================
UpdateProgress

Increment progress by amount.
================
*/
void UpdateProgress(int amount)
{
    if (g_progressTotal)
    {
        InterlockedAdd((volatile long *)&g_progressCurrent, amount);
        UpdateProgressPrint();
    }
}

/*
================
EndProgress

Finish progress, print elapsed time.
================
*/
void EndProgress(void)
{
    int elapsed = timeGetTime() - g_progressStartTime;

    Com_Printf("Finished in ");
    PrintMsecDuration(elapsed);
    Com_Printf(".                                                 \n");
    g_progressCurrent = 0;
    g_progressTotal = 0;
}
