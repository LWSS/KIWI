// KIWI: the AllocConsole window next to the game. The engine prints Quake colour codes all over
// (^1 errors, ^3 warnings, ^6 load profiler ...); this draws them as colours instead of carets.
#include <universal/q_shared.h>
#include "win_local.h"

#include <Windows.h>

#include <stdio.h>
#include <string.h>

static bool s_consoleColors;

// ^0-^9 as 24-bit VT colours: g_color_table (cl_cgame.cpp) and the team colour defaults.
static const char *const s_consoleColorSequences[10] =
{
    "\x1b[38;2;128;128;128m", // ^0 black - grey here, black text vanishes on the black console
    "\x1b[38;2;255;92;92m",   // ^1 red
    "\x1b[38;2;0;255;0m",     // ^2 green
    "\x1b[38;2;255;255;0m",   // ^3 yellow
    "\x1b[38;2;0;0;255m",     // ^4 blue
    "\x1b[38;2;0;255;255m",   // ^5 cyan
    "\x1b[38;2;255;92;255m",  // ^6 magenta
    "\x1b[39m",               // ^7 white - back to the text colour, as RB_DrawText does
    "\x1b[38;2;153;163;176m", // ^8 viewer's team - g_TeamColor_Allies default, the console has no team
    "\x1b[38;2;166;145;105m", // ^9 other team - g_TeamColor_Axis default
};
#define CONSOLE_COLOR_RESET "\x1b[39m"

/*
==================
Sys_KiwiConsoleInit
==================
*/
void Sys_KiwiConsoleInit()
{
    AllocConsole();

    SetConsoleTitleA("KIWI");
    DeleteMenu(GetSystemMenu(GetConsoleWindow(), FALSE), SC_CLOSE, MF_BYCOMMAND);

    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(out,
        ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
        ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN |
        ENABLE_LVB_GRID_WORLDWIDE);

    SetConsoleCtrlHandler(nullptr, true);

    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);

    // colours need VT processing (Windows 10+); without it the codes are stripped instead
    DWORD mode = 0;
    s_consoleColors = GetConsoleMode(out, &mode) && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

struct ConsoleOut
{
    char text[4096];
    size_t length;
};

static void Sys_KiwiConsoleAppend(ConsoleOut *out, const char *text, size_t length)
{
    if (out->length + length > sizeof(out->text))
    {
        _fwrite_nolock(out->text, 1, out->length, stderr);
        out->length = 0;
        if (length > sizeof(out->text))
        {
            _fwrite_nolock(text, 1, length, stderr);
            return;
        }
    }
    memcpy(&out->text[out->length], text, length);
    out->length += length;
}

/*
==================
Sys_KiwiConsolePrint

Com_PrintMessage's echo. A colour holds to the end of the line, like the in-game console, so
a code printed on its own carries into the next print on that line.
==================
*/
void Sys_KiwiConsolePrint(const char *msg)
{
    ConsoleOut out;
    const char *run;
    const char *s;

    out.length = 0;
    _lock_file(stderr); // one message stays in one piece when several threads print
    run = msg;
    for (s = msg; *s; ++s)
    {
        if (Q_IsColorString(s))
        {
            Sys_KiwiConsoleAppend(&out, run, s - run);
            if (s_consoleColors)
            {
                const char *sequence = s_consoleColorSequences[s[1] - COLOR_BLACK];
                Sys_KiwiConsoleAppend(&out, sequence, strlen(sequence));
            }
            ++s;
            run = s + 1;
        }
        else if (*s == '\n' && s_consoleColors)
        {
            Sys_KiwiConsoleAppend(&out, run, s - run);
            Sys_KiwiConsoleAppend(&out, CONSOLE_COLOR_RESET, sizeof(CONSOLE_COLOR_RESET) - 1);
            run = s;
        }
    }
    Sys_KiwiConsoleAppend(&out, run, s - run);
    _fwrite_nolock(out.text, 1, out.length, stderr);
    _unlock_file(stderr);
}
