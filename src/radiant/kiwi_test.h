#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Scripted, unattended Radiant regression mode.  Every function is deliberately
// cheap/no-op when -kiwitest is not present so the interactive editor keeps its
// existing startup, console, assert, and close behaviour.
bool KiwiTest_Init( const char *scriptPath, const char *logPath, bool keepOpen, bool strict );
bool KiwiTest_Active();

// KIWI (2026-10-05, user: "Make a remote connection suite that you can use (Without hogging
// the PC mouse/keyboard)"): -kiwiremote <pipe>.  The same verbs as a script, one per line
// over \\.\pipe\<pipe> (tools\kiwitest\kiwiremote.py); each command's log lines go back,
// then "@@END OK" or "@@END FAIL".  The editor stays up until `quit`.
bool KiwiTest_InitRemote( const char *pipeName, const char *logPath );

// Both test modes, before Prefs_Init: settings, dock layout and MRU live in
// <exedir>\kiwi_cache\kiwitest\ (seeded from the user's kiwi_radiant.ini, dock layout
// fresh), so the user's files are never written.
void KiwiTest_IsolateProfile();

// Both test modes, before the frame is shown: unless `show`, the frame is a tool window
// parked beyond every monitor that never activates (a CBT hook refuses activation and
// logs + cancels any dialog), so a test instance cannot take the user's focus.  Returns
// the ShowWindow command to use.
int KiwiTest_PrepareFrame( struct HWND__ *frame, int nCmdShow, bool show );
void KiwiTest_Tick();
void KiwiTest_ConsoleTap( const char *text );
void KiwiTest_Log( const char *fmt, ... );
void KiwiTest_Fail( const char *message );
// Probe verbs (kiwi_testprobe.cpp): a log / reply line on `channel`, and frames to wait
// before the next command (and before a remote reply) - e.g. so views render first.
void KiwiTest_LogChannel( const char *channel, const char *fmt, ... );
void KiwiTest_WaitFrames( int frames );
