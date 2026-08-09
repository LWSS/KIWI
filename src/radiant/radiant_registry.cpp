// radiant_registry.cpp — see radiant_registry.h.  The bodies were lifted verbatim from the
// retired kisak_mfc_shim.h CWinApp profile methods (HKCU registry); ALL registry usage was
// removed on request — the same section/entry API now reads/writes kiwi_radiant.ini beside
// the exe via the Win32 private-profile functions (which take a full path and handle
// create-on-first-write themselves).
#include "stdafx.h"
#include "radiant_registry.h"

const char *Radiant_IniPath()
{
    static char s_ini[MAX_PATH];
    if ( !s_ini[0] )
    {
        char exeDir[MAX_PATH] = { 0 };
        DWORD n = ::GetModuleFileNameA( NULL, exeDir, sizeof( exeDir ) );
        if ( n && n < sizeof( exeDir ) )
        {
            char *slash = strrchr( exeDir, '\\' );
            if ( slash ) slash[1] = 0;              // keep the trailing backslash
        }
        else
        {
            exeDir[0] = 0;                          // fall back to the CWD (".\\")
        }
        _snprintf( s_ini, sizeof( s_ini ), "%skiwi_radiant.ini", exeDir );
        s_ini[sizeof( s_ini ) - 1] = 0;
    }
    return s_ini;
}

int Radiant_ProfileGetInt( const char *section, const char *entry, int defVal )
{
    return (int)::GetPrivateProfileIntA( section, entry, defVal, Radiant_IniPath() );
}

std::string Radiant_ProfileGetString( const char *section, const char *entry, const char *defVal )
{
    char buf[1024];
    ::GetPrivateProfileStringA( section, entry, defVal ? defVal : "", buf, sizeof( buf ),
                                Radiant_IniPath() );
    return std::string( buf );
}

bool Radiant_ProfileSetInt( const char *section, const char *entry, int value )
{
    char num[32];
    _snprintf( num, sizeof( num ), "%d", value );
    num[sizeof( num ) - 1] = 0;
    return ::WritePrivateProfileStringA( section, entry, num, Radiant_IniPath() ) != 0;
}

bool Radiant_ProfileSetString( const char *section, const char *entry, const char *value )
{
    // value == nullptr deletes the entry — WritePrivateProfileString's native semantics.
    return ::WritePrivateProfileStringA( section, entry, value, Radiant_IniPath() ) != 0;
}
