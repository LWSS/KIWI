/*
 * assertive.c — Assertion handler
 */

#include "cod2rad64.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static char g_assertMsgBuffer[0x400];
static char g_assertModulePath[MAX_PATH];
static char g_assertActive;
static char g_assertUnknownFlag;
static void (*g_assertHandler)(const char *msg);
static int g_assertSeverity;

/*
================
AssertFailed

Formats a message, copies it to the clipboard, shows a MessageBox.
IDOK exits the process, IDCANCEL returns 2 to disable this assert.
Reentrant calls exit immediately.
================
*/
int AssertFailed(const char *expr, const char *file, int line, int skip, int severity)
{
    char unknownBuf[16];
    const char *exprArg;
    const char *fileArg;
    const char *caption;

    if (g_assertActive)
    {
        /* reentrant path: already inside an assert */
        if (OpenClipboard(GetDesktopWindow()))
        {
            HANDLE hMem;
            SIZE_T len;
            char *dst;

            EmptyClipboard();
            len = strlen(g_assertMsgBuffer) + 1;
            hMem = GlobalAlloc(GMEM_MOVEABLE, len);
            if (hMem)
            {
                dst = GlobalLock(hMem);
                if (dst)
                {
                    const char *src = g_assertMsgBuffer;
                    while ((*dst++ = *src++) != '\0')
                        ;
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_TEXT, hMem);
                }
            }
            CloseClipboard();
        }

        if (g_assertHandler)
            g_assertHandler(g_assertMsgBuffer);

        if (g_assertSeverity == 0)
            caption = "ASSERTION FAILURE... (this text is on the clipboard)";
        else if (g_assertSeverity == 1)
            caption = "SANITY CHECK FAILURE... (this text is on the clipboard)";
        else
            caption = "INTERNAL ERROR";

        MessageBoxA(NULL, g_assertMsgBuffer, caption,
                    MB_OKCANCEL | MB_ICONHAND | MB_TASKMODAL | MB_SETFOREGROUND);

        strcpy(unknownBuf, "<unknown>");
        exprArg = expr ? expr : unknownBuf;
        fileArg = file ? file : unknownBuf;

        if (!GetModuleFileNameA(NULL, g_assertModulePath, MAX_PATH))
            strcpy(g_assertModulePath, "<unknown application>");

        sprintf(g_assertMsgBuffer,
                     "Expression:\n\t%s\n\nModule:\t%s\nFile:\t%s\nLine:\t%d\n\n",
                     exprArg, g_assertModulePath, fileArg, line);

        Com_Printf("ASSERTBEGIN - ( Recursive assert )--------------------------\n");
        Com_Printf(g_assertMsgBuffer);
        Com_Printf("ASSERTEND - ( Recursive assert ) --------------------------\n");
        exit(-1);
    }

    /* normal path: first assert */
    strcpy(unknownBuf, "<unknown>");
    fileArg = file ? file : unknownBuf;
    exprArg = expr ? expr : unknownBuf;

    g_assertSeverity = severity;
    g_assertActive = 1;

    if (!GetModuleFileNameA(NULL, g_assertModulePath, MAX_PATH))
        strcpy(g_assertModulePath, "<unknown application>");

    sprintf(g_assertMsgBuffer,
                 "Expression:\n\t%s\n\nModule:\t%s\nFile:\t%s\nLine:\t%d\n\n",
                 exprArg, g_assertModulePath, fileArg, line);

    Com_Printf("ASSERTBEGIN -----------------------------------------------\n");
    Com_Printf(g_assertMsgBuffer);
    Com_Printf("ASSERTEND -------------------------------------------------\n");

    g_assertUnknownFlag = 0;

    if (OpenClipboard(GetDesktopWindow()))
    {
        HANDLE hMem;
        SIZE_T len;
        char *dst;

        EmptyClipboard();
        len = strlen(g_assertMsgBuffer) + 1;
        hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hMem)
        {
            dst = (char *)GlobalLock(hMem);
            if (dst)
            {
                const char *src = g_assertMsgBuffer;
                while ((*dst++ = *src++) != '\0')
                    ;
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            }
        }
        CloseClipboard();
    }

    if (g_assertHandler)
        g_assertHandler(g_assertMsgBuffer);

    if (severity == 0)
        caption = "ASSERTION FAILURE... (this text is on the clipboard)";
    else if (severity == 1)
        caption = "SANITY CHECK FAILURE... (this text is on the clipboard)";
    else
        caption = "INTERNAL ERROR";

    if (MessageBoxA(NULL, g_assertMsgBuffer, caption,
                    MB_OKCANCEL | MB_ICONHAND | MB_TASKMODAL | MB_SETFOREGROUND) == IDOK)
    {
        ExitProcess((UINT)-1);
    }

    g_assertActive = 0;
    return 2;
}
