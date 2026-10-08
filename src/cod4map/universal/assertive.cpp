/*
assertive.c — Assert handler

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char   g_assertMsgBuffer[MAXPRINTMSG];

/*
================
MyAssertHandler

KIWI: the engine's handler signature, behind iassert / vassert (cod4map.h).  Prints
ASSERTBEGIN / message / ASSERTEND, then the popup: OK exits (-1), Cancel breaks into the
debugger.  An assert while the popup is up exits.  type: 0 assert, 1 sanity, 2+ internal.
================
*/
static char g_assertModulePath[260];
static char g_assertReentryGuard;
static int g_assertType;

static const char *Assert_Caption(int type)
{
  if ( type == 0 )
    return "ASSERTION FAILURE... (this text is on the title bar of the popup)";
  if ( type == 1 )
    return "SANITY CHECK FAILURE... (this text is on the title bar of the popup)";
  return "INTERNAL ERROR";
}

void MyAssertHandler(const char *filename, int line, int type, const char *fmt, ...)
{
  char expr[1024];
  va_list va;

  va_start(va, fmt);
  _vsnprintf(expr, sizeof(expr), fmt, va);
  va_end(va);
  expr[sizeof(expr) - 1] = '\0';

  if ( g_assertReentryGuard )
  {
    /* recursive assert */
    MessageBoxA(NULL, g_assertMsgBuffer, Assert_Caption(g_assertType), MB_OKCANCEL | MB_ICONHAND | MB_TASKMODAL | MB_SETFOREGROUND);

    sprintf(g_assertMsgBuffer, "Expression:\n\t%s\n\nModule:\t%s\nFile:\t%s\nLine:\t%d\n\n", expr, g_assertModulePath, filename, line);
    Com_Printf("ASSERTBEGIN - ( Recursive assert )-----------------------------------------------\n");
    Com_Printf("%s", g_assertMsgBuffer);
    Com_Printf("ASSERTEND - ( Recursive assert ) ------------------------------------------------\n");
    exit(-1);
  }

  g_assertType = type;
  g_assertReentryGuard = 1;

  if ( !GetModuleFileNameA(NULL, g_assertModulePath, sizeof(g_assertModulePath)) )
    strcpy(g_assertModulePath, "<unknown application>");

  sprintf(g_assertMsgBuffer, "Expression:\n\t%s\n\nModule:\t%s\nFile:\t%s\nLine:\t%d\n\n", expr, g_assertModulePath, filename, line);

  Com_Printf("ASSERTBEGIN -----------------------------------------------------------------------\n");
  Com_Printf("%s", g_assertMsgBuffer);
  Com_Printf("ASSERTEND -------------------------------------------------------------------------\n");

  if ( MessageBoxA(NULL, g_assertMsgBuffer, Assert_Caption(type), MB_OKCANCEL | MB_ICONHAND | MB_TASKMODAL | MB_SETFOREGROUND) == IDOK )
    ExitProcess(-1);

  g_assertReentryGuard = 0;
  DebugBreak();
}
