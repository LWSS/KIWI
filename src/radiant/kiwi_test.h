#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Scripted, unattended Radiant regression mode.  Every function is deliberately
// cheap/no-op when -kiwitest is not present so the interactive editor keeps its
// existing startup, console, assert, and close behaviour.
bool KiwiTest_Init( const char *scriptPath, const char *logPath, bool keepOpen, bool strict );
bool KiwiTest_Active();
void KiwiTest_Tick();
void KiwiTest_ConsoleTap( const char *text );
void KiwiTest_Log( const char *fmt, ... );
void KiwiTest_Fail( const char *message );
