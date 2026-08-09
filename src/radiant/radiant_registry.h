#pragma once
// radiant_registry.h — editor settings persistence.  Formerly the HKCU registry profile
// (the non-MFC replacement for CWinApp::GetProfile*/WriteProfile*); ALL registry usage was
// removed on request — settings now live in kiwi_radiant.ini NEXT TO THE EXE, so the
// editor is fully self-contained/portable and nothing machine-global can redirect it.
// Same section/entry call-site semantics; std::string out-params keep the CString-era
// ergonomics.  (Existing registry-saved prefs are simply ignored — defaults apply once.)
#include <string>

// Full path of the settings file beside the exe: <exedir>\kiwi_radiant.ini.
const char *Radiant_IniPath();

int         Radiant_ProfileGetInt   ( const char *section, const char *entry, int defVal );
std::string Radiant_ProfileGetString( const char *section, const char *entry, const char *defVal = "" );
bool        Radiant_ProfileSetInt   ( const char *section, const char *entry, int value );
// value == nullptr DELETES the entry (MFC WriteProfileString(...,NULL) semantics).
bool        Radiant_ProfileSetString( const char *section, const char *entry, const char *value );
