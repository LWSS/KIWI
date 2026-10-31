#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "kiwi_test.h"
#include "kiwi_testprobe.h"     // probe verbs: geometry / camera / tool driving (remote suite)
#include "radiant_registry.h"   // Radiant_SetIniPath: isolated test profile
#include "kiwi_command.h"
#include "kiwi_modelbrowser.h"
#include "kiwi_particles.h"     // particle_place / particle_export verbs (test mode)
#include "kiwi_droptrace.h"
#include "kiwi_pick.h"
#include "kiwi_ux.h"
#include "radiant_frame.h"
#include "mainfrm.h"
#include "kiwi_refimage.h"     // refimage verbs + expectations (test mode)
#include "kiwi_terrain.h"      // terrain verb: tool / set / arm / stroke (test mode)
#include "kiwi_plastbridge.h"  // plasticity_export verb (test mode, no hand-off)
#include "kiwi_construct.h"    // construct verb: a headless line / polyline
#include "kiwi_extrude.h"
#include "kiwi_primitive.h"
#include "kiwi_numeric.h"
#include "kiwi_ported.h"
#include "kiwi_units.h"
#include "kiwi_conselect.h"    // ...selected for the verbs that act on construction
#include "kiwi_barbwire.h"     // barbwire verb (test mode, no dialog)
#include "kiwi_outliner.h"     // group verb + groups / grouped expectations
// KIWI GLASS
#include "kiwi_glass.h"        // glass_* verbs + expectations
#include "kiwi_selection.h"    // select face
// KIWI END
#include "kiwi_boxselect.h"    // click_at: the camera's real click path
#include "kiwi_region.h"       // expect regions_selected
#include "kiwi_boolean.h"      // curve_bool_probe: KiwiBool_TestPickTool
#include "kiwi_validity.h"     // curve_bool_probe: KiwiValid_Rebuild
#include "xywnd.h"             // expect xyview: Ed_ActiveXY / ED_VIEW_*

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Existing editor entry points.  Each declaration is kept beside the test-only
// call site so this translation unit does not expose another editor API surface.
extern bool KiwiSelect_TracePrefab( selbrush_t *, const float *, const float *, edTrace_t * );

extern void Map_New();                                                       // entity.cpp:1839
extern void RadiantClipboard_ForgetLocal();                                  // entity.cpp (test hook)
extern bool Map_SaveFile( const char *path, char region );                   // map.cpp
extern void Select_Invert();                                                 // select.cpp:1196
extern void Select_GetMid( float *mid );                                     // select.cpp:2213

extern void Undo_Undo();                                                     // undo.cpp:736
extern void Undo_Redo();                                                     // undo.cpp:1019

extern bool TexWnd_MakeMaterialCurrentByName( const char *name );             // texwnd.cpp:793
extern void Patch_Thicken( int amount, char seam );                           // pmesh.cpp:7667
extern int Radiant_ProfileGetInt( const char *, const char *, int );
extern bool Radiant_ProfileSetInt( const char *, const char *, int );

namespace
{
struct ScriptLine
{
    int number = 0;
    std::string raw;
    std::vector<std::string> words;
};

struct FailRule
{
    std::string needle;
    std::string tail;
    bool tripped = false;
};

struct TestState
{
    bool active = false;
    bool strict = false;
    bool keepOpen = false;
    bool finished = false;
    bool stopAtNextTick = false;
    int result = 0;
    size_t nextLine = 0;
    int waitFrames = 0;
    ULONGLONG sleepUntil = 0;
    HANDLE log = INVALID_HANDLE_VALUE;
    PVOID veh = nullptr;
    std::string scriptPath;
    std::string logPath;
    std::vector<ScriptLine> lines;
    std::string consoleText;
    std::string consolePending;
    std::vector<FailRule> failRules;

    // KIWI (2026-10-05): -kiwiremote.  Lines arrive over a named pipe and queue in `lines`;
    // each command's log lines (capture) go back once its frame / sleep waits are over,
    // then "@@END OK|FAIL".  `run <script>` inlines a script whose lines answer as one.
    bool remote = false;
    bool quitRequested = false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool pipeConnected = false;
    std::string pipeIn;
    int nextNumber = 0;
    bool replyOpen = false;
    bool cmdFailed = false;
    size_t batchEnd = 0;            // lines before this index belong to the open reply
    bool capturing = false;
    std::string capture;
};

TestState s_test;
volatile LONG s_inCrashHandler = 0;

static std::string Lower( const std::string &s )
{
    std::string out = s;
    std::transform( out.begin(), out.end(), out.begin(),
                    []( unsigned char c ) { return (char)std::tolower( c ); } );
    return out;
}

static std::string FullPath( const char *path )
{
    if ( !path || !path[0] )
        return std::string();
    char stackBuf[MAX_PATH];
    DWORD need = GetFullPathNameA( path, (DWORD)sizeof( stackBuf ), stackBuf, nullptr );
    if ( need && need < sizeof( stackBuf ) )
        return stackBuf;
    if ( need )
    {
        std::vector<char> buf( (size_t)need + 1 );
        if ( GetFullPathNameA( path, (DWORD)buf.size(), buf.data(), nullptr ) )
            return buf.data();
    }
    return path;
}

static std::string DirName( const std::string &path )
{
    const size_t slash = path.find_last_of( "\\/" );
    if ( slash == std::string::npos )
        return std::string();
    return path.substr( 0, slash );
}

static void RawWrite( const char *text, size_t length )
{
    if ( s_test.log == INVALID_HANDLE_VALUE || !text || !length )
        return;
    DWORD written = 0;
    WriteFile( s_test.log, text, (DWORD)length, &written, nullptr );
}

static void WriteOneLine( const char *channel, const char *text, size_t length )
{
    if ( s_test.capturing )                     // -kiwiremote: this command's reply
    {
        s_test.capture += '[';
        s_test.capture += channel ? channel : "TEST";
        s_test.capture += "] ";
        if ( text && length )
            s_test.capture.append( text, length );
        s_test.capture += '\n';
    }
    if ( s_test.log == INVALID_HANDLE_VALUE )
        return;
    SYSTEMTIME now;
    GetLocalTime( &now );
    char prefix[96];
    const int prefixLen = _snprintf( prefix, sizeof( prefix ),
                                     "[%02u:%02u:%02u.%03u] [%s] ",
                                     now.wHour, now.wMinute, now.wSecond,
                                     now.wMilliseconds, channel ? channel : "TEST" );
    if ( prefixLen > 0 )
        RawWrite( prefix, (size_t)prefixLen );
    if ( text && length )
        RawWrite( text, length );
    RawWrite( "\r\n", 2 );
    FlushFileBuffers( s_test.log );
}

static void WriteLines( const char *channel, const std::string &text )
{
    size_t start = 0;
    while ( start <= text.size() )
    {
        size_t end = text.find( '\n', start );
        const bool last = end == std::string::npos;
        if ( last )
            end = text.size();
        size_t length = end - start;
        if ( length && text[start + length - 1] == '\r' )
            --length;
        WriteOneLine( channel, text.data() + start, length );
        if ( last )
            break;
        start = end + 1;
        if ( start == text.size() )
            break; // A terminating newline completed the previous line; no phantom line.
    }
}

static void LogFormatV( const char *channel, const char *fmt, va_list args )
{
    char stackBuf[4096];
    va_list copy;
    va_copy( copy, args );
    int count = _vsnprintf( stackBuf, sizeof( stackBuf ), fmt, copy );
    va_end( copy );
    if ( count >= 0 && count < (int)sizeof( stackBuf ) )
    {
        WriteLines( channel, std::string( stackBuf, (size_t)count ) );
        return;
    }

    va_copy( copy, args );
    count = _vscprintf( fmt, copy );
    va_end( copy );
    if ( count < 0 )
    {
        WriteOneLine( channel, "<formatting failed>", 19 );
        return;
    }
    std::vector<char> buf( (size_t)count + 1 );
    va_copy( copy, args );
    vsnprintf( buf.data(), buf.size(), fmt, copy );
    va_end( copy );
    WriteLines( channel, std::string( buf.data(), (size_t)count ) );
}

static void LogFormat( const char *channel, const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    LogFormatV( channel, fmt, args );
    va_end( args );
}

static bool IsFatalException( DWORD code )
{
    // A VEH sees language/runtime notifications as well as faults.  These three are
    // continuable control-flow notifications, not crashes; all hardware/SEH faults
    // are terminal in unattended mode.
    return code != 0xE06D7363u && // MSVC C++ exception
           code != 0x406D1388u && // debugger thread-name notification
           code != 0x40010006u;   // OutputDebugString notification
}

static void FormatCrashAddress( const void *address, char *out, size_t outSize )
{
    HMODULE module = nullptr;
    if ( address && GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCSTR>( address ), &module ) )
    {
        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA( module, modulePath, (DWORD)sizeof( modulePath ) );
        const char *base = modulePath;
        for ( const char *p = modulePath; *p; ++p )
            if ( *p == '\\' || *p == '/' ) base = p + 1;
        const std::uintptr_t rva = reinterpret_cast<std::uintptr_t>( address ) -
                                   reinterpret_cast<std::uintptr_t>( module );
        _snprintf( out, outSize, "%s+0x%08lX (%p)", base,
                   (unsigned long)rva, address );
    }
    else
    {
        _snprintf( out, outSize, "%p", address );
    }
    out[outSize - 1] = '\0';
}

static LONG WINAPI TestVectoredExceptionHandler( EXCEPTION_POINTERS *ep )
{
    if ( !s_test.active || !ep || !ep->ExceptionRecord ||
         !IsFatalException( ep->ExceptionRecord->ExceptionCode ) )
        return EXCEPTION_CONTINUE_SEARCH;
    if ( InterlockedExchange( &s_inCrashHandler, 1 ) )
        ExitProcess( 3 );

    char where[2 * MAX_PATH];
    FormatCrashAddress( ep->ExceptionRecord->ExceptionAddress, where, sizeof( where ) );
    char message[2 * MAX_PATH + 96];
    const int length = _snprintf( message, sizeof( message ),
                                  "exception 0x%08lX at %s",
                                  (unsigned long)ep->ExceptionRecord->ExceptionCode, where );
    WriteOneLine( "CRASH", message, length > 0 ? (size_t)length : 0 );
    ExitProcess( 3 );
}

static bool ParseWords( const std::string &line, std::vector<std::string> &out,
                        std::string &error )
{
    std::string word;
    bool quoted = false;
    bool inWord = false;
    for ( size_t i = 0; i < line.size(); ++i )
    {
        const char c = line[i];
        if ( quoted )
        {
            if ( c == '"' )
            {
                quoted = false;
                inWord = true;
            }
            else if ( c == '\\' && i + 1 < line.size() &&
                      ( line[i + 1] == '\\' || line[i + 1] == '"' ) )
            {
                word.push_back( line[++i] );
                inWord = true;
            }
            else
            {
                word.push_back( c );
                inWord = true;
            }
            continue;
        }

        if ( c == '"' )
        {
            quoted = true;
            inWord = true;
            continue;
        }
        if ( c == '#' )
        {
            // `cmd #32927` is an id token; every other unquoted # starts a comment.
            const bool commandId = out.size() == 1 && Lower( out[0] ) == "cmd" &&
                                   !inWord && i + 1 < line.size() &&
                                   std::isdigit( (unsigned char)line[i + 1] );
            if ( !commandId )
                break;
        }
        if ( std::isspace( (unsigned char)c ) )
        {
            if ( inWord )
            {
                out.push_back( word );
                word.clear();
                inWord = false;
            }
            continue;
        }
        word.push_back( c );
        inWord = true;
    }
    if ( quoted )
    {
        error = "unterminated quoted string";
        return false;
    }
    if ( inWord )
        out.push_back( word );
    return true;
}

static bool ParseInt( const std::string &word, int &out )
{
    if ( word.empty() ) return false;
    errno = 0;
    char *end = nullptr;
    const long value = strtol( word.c_str(), &end, 10 );
    if ( errno || !end || *end || value < INT_MIN || value > INT_MAX )
        return false;
    out = (int)value;
    return true;
}

static bool ParseFloat( const std::string &word, float &out )
{
    if ( word.empty() ) return false;
    errno = 0;
    char *end = nullptr;
    const float value = strtof( word.c_str(), &end );
    if ( errno || !end || *end || !std::isfinite( value ) )
        return false;
    out = value;
    return true;
}

static bool FileExists( const std::string &path )
{
    const DWORD attrs = GetFileAttributesA( path.c_str() );
    return attrs != INVALID_FILE_ATTRIBUTES && !( attrs & FILE_ATTRIBUTE_DIRECTORY );
}

static void Finish( int requestedCode )
{
    if ( s_test.finished )
        return;
    if ( s_test.remote && !s_test.quitRequested )
    {
        // A remote session survives a failed command: the reply says FAIL, the editor stays.
        if ( requestedCode != 0 )
            s_test.cmdFailed = true;
        return;
    }
    if ( !s_test.consolePending.empty() )
    {
        WriteOneLine( "CONSOLE", s_test.consolePending.data(), s_test.consolePending.size() );
        s_test.consolePending.clear();
    }
    if ( requestedCode != 0 )
        s_test.result = requestedCode;
    s_test.finished = true;
    LogFormat( "RESULT", "finished with exit code %d%s", s_test.result,
               s_test.keepOpen ? " (-kiwitest-keep: editor left open)" : "" );
    if ( !s_test.keepOpen )
        PostQuitMessage( s_test.result );
}

static void ScriptError( const ScriptLine &line, const char *fmt, ... )
{
    char message[2048];
    va_list args;
    va_start( args, fmt );
    _vsnprintf( message, sizeof( message ), fmt, args );
    va_end( args );
    message[sizeof( message ) - 1] = '\0';
    LogFormat( "SCRIPT-ERROR", "line %d: %s", line.number, message );
    Finish( 2 );
}

static void ExpectResult( const ScriptLine &line, bool pass,
                          const std::string &actual, const std::string &expected )
{
    LogFormat( pass ? "EXPECT-PASS" : "EXPECT-FAIL",
               "line %d: actual=%s expected=%s", line.number,
               actual.c_str(), expected.c_str() );
    if ( pass )
        return;
    s_test.cmdFailed = true;
    if ( s_test.result == 0 )
        s_test.result = 1;
    if ( s_test.strict )
        Finish( 1 );
}

static void CollectList( selbrush_t *head, std::vector<selbrush_t *> &out )
{
    if ( !head ) return;
    size_t guard = 0;
    for ( selbrush_t *b = head->next; b && b != head && guard < 1000000;
          b = b->next, ++guard )
        out.push_back( b );
}

static std::vector<selbrush_t *> SelectableNodes()
{
    std::vector<selbrush_t *> result;
    CollectList( &active_brushes, result );
    CollectList( &filtered_brushes, result );
    return result;
}

static bool EntityIsSaved( entity_s *ent )
{
    if ( !ent || !ent->eclass || !ent->eclass->name )
        return false;
    const bool hasBrushes = (void *)ent->brushes.prev != (void *)&ent->def;
    return hasBrushes || strcmp( ent->eclass->name, "worldspawn" ) == 0;
}

static std::vector<entity_s *> SavedEntities()
{
    std::vector<entity_s *> result;
    size_t guard = 0;
    for ( entity_s *ent = entities.next; ent && ent != &entities && guard < 1000000;
          ent = ent->next, ++guard )
        if ( EntityIsSaved( ent ) )
            result.push_back( ent );
    return result;
}

static entity_s *SavedEntityAt( int index )
{
    if ( index < 0 ) return nullptr;
    std::vector<entity_s *> defs = SavedEntities();
    return (size_t)index < defs.size() ? defs[(size_t)index] : nullptr;
}

static void SelectNodes( const std::vector<selbrush_t *> &nodes )
{
    for ( selbrush_t *node : nodes )
        Select_Brush( node, 0, 0, 0 );
    g_nUpdateBits = -1;
}

static bool SelectEntityDef( entity_s *def )
{
    Select_Deselect( 1 );
    std::vector<selbrush_t *> matches;
    for ( selbrush_t *node : SelectableNodes() )
        if ( node->owner && node->owner->def == def )
            matches.push_back( node );
    SelectNodes( matches );
    return true;
}

static bool SelectBrushOrdinal( int entityIndex, int brushIndex, bool add = false )
{
    entity_s *ent = SavedEntityAt( entityIndex );
    if ( !ent || brushIndex < 0 || !ent->eclass || ent->eclass->fixedsize )
        return false;
    brush_t *sentinel = (brush_t *)&ent->def;
    brush_t *wanted = (brush_t *)ent->brushes.prev;
    while ( wanted != sentinel && brushIndex > 0 )
    {
        wanted = wanted->onext;
        --brushIndex;
    }
    if ( wanted == sentinel || brushIndex != 0 )
        return false;

    if ( !add )                             // KIWI: `select brush+` keeps the selection
        Select_Deselect( 1 );
    for ( selbrush_t *node : SelectableNodes() )
    {
        if ( node->def == wanted )
        {
            Select_Brush( node, 0, 0, 0 );
            g_nUpdateBits = -1;
            return true;
        }
    }
    return false;
}

static const char *MaterialName( MaterialDef *def )
{
    if ( !def || ( ( def->lyrMtl != nullptr ) + ( def->radMtl != nullptr ) ) != 1 )
        return nullptr;
    return reinterpret_cast<const char *>( Materialdef_GetName( def ) );
}

static bool NodeUsesMaterial( selbrush_t *node, const char *name )
{
    if ( !node || !node->def || !name ) return false;
    int layer = g_qeglobals.current_edit_layer;
    if ( layer < 0 || layer > 2 ) layer = 0;
    if ( node->def->patch )
    {
        patchMesh_material *patchMaterial = &node->def->patch->texture + layer;
        MaterialDef material = {};
        material.lyrMtl = patchMaterial->lyrMtl;
        material.radMtl = patchMaterial->radMtl;
        const char *current = MaterialName( &material );
        return current && _stricmp( current, name ) == 0;
    }
    for ( int i = 0; i < node->def->faceCount; ++i )
    {
        const char *current = MaterialName( &node->def->faces[i].mtldef[layer] );
        if ( current && _stricmp( current, name ) == 0 )
            return true;
    }
    return false;
}

struct MapCounts
{
    int brushes = 0;
    int patches = 0;
    int selected = 0;
    int entities = 0;
};

static MapCounts CountMap()
{
    MapCounts counts;
    std::vector<selbrush_t *> all;
    CollectList( &active_brushes, all );
    CollectList( &selected_brushes, all );
    CollectList( &filtered_brushes, all );
    for ( selbrush_t *node : all )
    {
        if ( node->def && node->def->patch )
            ++counts.patches;
        else if ( node->owner && node->owner->def && node->owner->def->eclass &&
                  !node->owner->def->eclass->fixedsize )
            ++counts.brushes;
    }
    for ( selbrush_t *node = selected_brushes.next;
          node && node != &selected_brushes; node = node->next )
        ++counts.selected;
    counts.entities = (int)SavedEntities().size();
    return counts;
}

static bool HaveSelection()
{
    return selected_brushes.next != &selected_brushes;
}

static void MoveSelection( const float delta[3] )
{
    Undo_ClearRedo();
    Undo_GeneralStart( "kiwitest move" );
    Undo_AddBrushList( &selected_brushes );
    Select_Move( delta, 0 );
    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

static void RotateSelection( int axis, float degrees )
{
    Undo_ClearRedo();
    Undo_GeneralStart( "kiwitest rotate" );
    Undo_AddBrushList( &selected_brushes );
    float matrix[4][3];
    Select_GetMid( matrix[0] );
    Select_RotateAxis( axis, degrees, (float (*)[4][3])matrix );
    Select_ApplyMatrix_SelectedBrushes( 0, matrix[0], degrees, 0 );
    g_nUpdateBits = -1;
    UpdateSelection( -1, nullptr );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

static std::string QuoteCommandArg( const std::string &arg )
{
    std::string out = "\"";
    unsigned int slashes = 0;
    for ( char c : arg )
    {
        if ( c == '\\' )
        {
            ++slashes;
            continue;
        }
        if ( c == '"' )
        {
            out.append( slashes * 2 + 1, '\\' );
            out.push_back( '"' );
            slashes = 0;
            continue;
        }
        out.append( slashes, '\\' );
        slashes = 0;
        out.push_back( c );
    }
    out.append( slashes * 2, '\\' );
    out.push_back( '"' );
    return out;
}

enum class MapCompareResult { Equal, Different, Unavailable, Error };

static MapCompareResult CompareMaps( const std::string &left, const std::string &right,
                                     std::string &detail )
{
    // Test processes always run from bin\Debug.  Derive tooling from that fixed cwd
    // rather than the executable location so harness --exe builds outside the repo work.
    const std::string mapfile = FullPath( "..\\..\\tools\\kiwitest\\mapfile.py" );
    const std::string repo = DirName( DirName( DirName( mapfile ) ) );
    if ( !FileExists( mapfile ) )
    {
        detail = "tools\\kiwitest\\mapfile.py is not present";
        return MapCompareResult::Unavailable;
    }

    char python[MAX_PATH] = {};
    const DWORD pythonLength = SearchPathA( nullptr, "python.exe", nullptr,
                                            (DWORD)sizeof( python ), python, nullptr );
    if ( !pythonLength || pythonLength >= sizeof( python ) )
    {
        detail = "python.exe was not found on PATH";
        return MapCompareResult::Unavailable;
    }
    const std::string leftAbs = FullPath( left.c_str() );
    const std::string rightAbs = FullPath( right.c_str() );
    std::string command = QuoteCommandArg( python ) + " " + QuoteCommandArg( mapfile ) +
                          " diff " + QuoteCommandArg( leftAbs ) + " " + QuoteCommandArg( rightAbs );
    std::vector<char> mutableCommand( command.begin(), command.end() );
    mutableCommand.push_back( '\0' );
    STARTUPINFOA startup = {};
    startup.cb = sizeof( startup );
    SECURITY_ATTRIBUTES inherit = {};
    inherit.nLength = sizeof( inherit );
    inherit.bInheritHandle = TRUE;
    HANDLE nullStream = CreateFileA( "NUL", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    if ( nullStream != INVALID_HANDLE_VALUE )
    {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullStream;
        startup.hStdOutput = nullStream;
        startup.hStdError = nullStream;
    }
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessA( nullptr, mutableCommand.data(), nullptr, nullptr,
                                         nullStream != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW,
                                         nullptr, repo.c_str(), &startup, &process );
    if ( nullStream != INVALID_HANDLE_VALUE )
        CloseHandle( nullStream );
    if ( !created )
    {
        detail = "CreateProcess(python) failed with Win32 error " + std::to_string( GetLastError() );
        return MapCompareResult::Unavailable;
    }
    CloseHandle( process.hThread );
    const DWORD wait = WaitForSingleObject( process.hProcess, 60000 );
    if ( wait == WAIT_TIMEOUT )
    {
        TerminateProcess( process.hProcess, 4 );
        WaitForSingleObject( process.hProcess, 5000 );
        CloseHandle( process.hProcess );
        detail = "mapfile.py diff timed out after 60 seconds";
        return MapCompareResult::Error;
    }
    DWORD exitCode = 2;
    GetExitCodeProcess( process.hProcess, &exitCode );
    CloseHandle( process.hProcess );
    detail = "mapfile.py diff exit " + std::to_string( exitCode );
    if ( exitCode == 0 ) return MapCompareResult::Equal;
    if ( exitCode == 1 ) return MapCompareResult::Different;
    return MapCompareResult::Error;
}

static bool DispatchNamedCommand( const std::string &word, unsigned int &id )
{
    if ( word.size() > 1 && word[0] == '#' )
    {
        int parsed = 0;
        if ( !ParseInt( word.substr( 1 ), parsed ) || parsed < 0 )
            return false;
        id = (unsigned int)parsed;
        return true;
    }
    const RadiantCommand *commands = nullptr;
    const int count = Radiant_GetCommandTable( &commands );
    for ( int i = 0; commands && i < count; ++i )
    {
        if ( commands[i].name && _stricmp( commands[i].name, word.c_str() ) == 0 )
        {
            id = (unsigned int)commands[i].commandId;
            return true;
        }
    }
    return false;
}

static void ExecuteExpect( const ScriptLine &line )
{
    const std::vector<std::string> &w = line.words;
    if ( w.size() < 3 )
    {
        ScriptError( line, "expect requires a subject and value" );
        return;
    }
    const std::string subject = Lower( w[1] );
    if ( subject == "tool" )
    {
        const char *name = KiwiCmd_Active() ? KiwiCmd_Active()->Name() : "none";
        ExpectResult( line, w.size() == 3 && !_stricmp( name, w[2].c_str() ), name, w[2] );
        return;
    }
    MapCounts counts = CountMap();
    int expectedInt = 0;
    int actualInt = 0;
    if ( subject == "brushes" || subject == "patches" || subject == "entities" ||
         subject == "selected" || subject == "modified" )
    {
        if ( w.size() != 3 || !ParseInt( w[2], expectedInt ) )
        {
            ScriptError( line, "expect %s requires one integer", subject.c_str() );
            return;
        }
        if ( subject == "brushes" ) actualInt = counts.brushes;
        else if ( subject == "patches" ) actualInt = counts.patches;
        else if ( subject == "entities" ) actualInt = counts.entities;
        else if ( subject == "selected" ) actualInt = counts.selected;
        else
        {
            if ( expectedInt != 0 && expectedInt != 1 )
            {
                ScriptError( line, "expect modified accepts only 0 or 1" );
                return;
            }
            actualInt = modified ? 1 : 0;
        }
        ExpectResult( line, actualInt == expectedInt, std::to_string( actualInt ),
                      std::to_string( expectedInt ) );
        return;
    }
    if ( subject == "groups" || subject == "grouped" )
    {
        // expect groups <n>   - universal group nodes (nested + implied parents included)
        // expect grouped <n>  - brush instances (model boxes too) of grouped entities
        if ( w.size() != 3 || !ParseInt( w[2], expectedInt ) )
        {
            ScriptError( line, "expect %s requires one integer", subject.c_str() );
            return;
        }
        actualInt = subject == "groups" ? KiwiOutliner_TestGroupCount()
                                        : KiwiOutliner_TestGroupedBrushCount();
        ExpectResult( line, actualInt == expectedInt, std::to_string( actualInt ),
                      std::to_string( expectedInt ) );
        return;
    }
    if ( subject == "terrainz" )
    {
        // expect terrainz <x> <y> <z> [tolerance]  - the highest terrain surface under (x, y)
        float x = 0.0f, y = 0.0f, z = 0.0f, tol = 0.01f;
        if ( ( w.size() != 5 && w.size() != 6 ) || !ParseFloat( w[2], x ) || !ParseFloat( w[3], y )
          || !ParseFloat( w[4], z ) || ( w.size() == 6 && !ParseFloat( w[5], tol ) ) )
        {
            ScriptError( line, "expect terrainz <x> <y> <z> [tolerance]" );
            return;
        }
        float have = 0.0f;
        if ( !KiwiTerrain_TestHeightAt( x, y, &have ) )
        {
            ExpectResult( line, false, "no terrain", w[4] );
            return;
        }
        ExpectResult( line, fabsf( have - z ) <= tol, std::to_string( have ), w[4] );
        return;
    }
    if ( subject == "images" )
    {
        // expect images <n> | expect images selected <n>
        if ( w.size() == 4 && Lower( w[2] ) == "selected" && ParseInt( w[3], expectedInt ) )
        {
            actualInt = KiwiRefImage_SelectedCount();
            ExpectResult( line, actualInt == expectedInt, std::to_string( actualInt ),
                          std::to_string( expectedInt ) );
            return;
        }
        if ( w.size() != 3 || !ParseInt( w[2], expectedInt ) )
        {
            ScriptError( line, "expect images <n> | expect images selected <n>" );
            return;
        }
        actualInt = KiwiRefImage_Count();
        ExpectResult( line, actualInt == expectedInt, std::to_string( actualInt ),
                      std::to_string( expectedInt ) );
        return;
    }
    if ( subject == "image" )
    {
        // expect image <i> origin x y z | rotation <deg> | size w h | axis xy|xz|yz
        int index = -1;
        if ( w.size() < 5 || !ParseInt( w[2], index ) )
        {
            ScriptError( line, "expect image <index> origin x y z | rotation <deg> | tilt <deg> | size w h | axis xy|xz|yz" );
            return;
        }
        const krefImage_t *r = KiwiRefImage_At( index );
        if ( !r )
        {
            ExpectResult( line, false, "no such image", "image " + w[2] );
            return;
        }
        const std::string field = Lower( w[3] );
        if ( field == "axis" )
        {
            const std::string plane = Lower( w[4] );
            static const char *const names[3] = { "yz", "xz", "xy" };
            const char *have = ( r->axis >= 0 && r->axis < 3 ) ? names[r->axis] : "?";
            ExpectResult( line, w.size() == 5 && plane == have, have, plane );
            return;
        }
        float want[3] = { 0.0f, 0.0f, 0.0f }, have[3] = { 0.0f, 0.0f, 0.0f };
        int n = 0;
        if ( field == "origin" && w.size() == 7 )
        {
            n = 3;
            for ( int k = 0; k < 3; ++k ) have[k] = r->origin[k];
        }
        else if ( field == "rotation" && w.size() == 5 )
        {
            n = 1;
            have[0] = r->rotation;
        }
        else if ( field == "tilt" && w.size() == 5 )
        {
            n = 1;
            have[0] = KiwiRefImage_TiltDegrees( index );
        }
        else if ( field == "selected" && w.size() == 5 )
        {
            n = 1;
            have[0] = KiwiRefImage_IsSelected( index ) ? 1.0f : 0.0f;
        }
        else if ( field == "size" && w.size() == 6 )
        {
            n = 2;
            have[0] = r->width;
            have[1] = r->height;
        }
        else
        {
            ScriptError( line, "expect image: unknown field '%s' or wrong argument count", w[3].c_str() );
            return;
        }
        for ( int k = 0; k < n; ++k )
            if ( !ParseFloat( w[(size_t)k + 4], want[k] ) )
            {
                ScriptError( line, "expect image %s argument %d is not numeric", field.c_str(), k + 1 );
                return;
            }
        bool pass = true;
        for ( int k = 0; k < n; ++k )
            if ( fabsf( have[k] - want[k] ) > 0.05f )
                pass = false;
        char actual[96], expected[96];
        _snprintf( actual,   sizeof( actual ),   "%g %g %g", have[0], have[1], have[2] );
        _snprintf( expected, sizeof( expected ), "%g %g %g", want[0], want[1], want[2] );
        actual[sizeof( actual ) - 1] = expected[sizeof( expected ) - 1] = 0;
        ExpectResult( line, pass, actual, expected );
        return;
    }
    if ( subject == "xyview" )
    {
        if ( w.size() != 3 )
        {
            ScriptError( line, "expect xyview top|front|side" );
            return;
        }
        const std::string want = Lower( w[2] );
        const int vt = Ed_ActiveXY()->m_nViewType;
        const char *have = vt == ED_VIEW_XY ? "top" : vt == ED_VIEW_XZ ? "front" : "side";
        ExpectResult( line, want == have, have, want );
        return;
    }
    if ( subject == "console" )
    {
        if ( w.size() != 3 )
        {
            ScriptError( line, "expect console requires one quoted substring" );
            return;
        }
        const bool found = s_test.consoleText.find( w[2] ) != std::string::npos;
        ExpectResult( line, found, found ? "substring found" : "substring absent", w[2] );
        return;
    }
    if ( subject == "file" )
    {
        if ( w.size() != 4 || Lower( w[3] ) != "exists" )
        {
            ScriptError( line, "expect file syntax is: expect file <path> exists" );
            return;
        }
        const bool exists = FileExists( w[2] );
        ExpectResult( line, exists, exists ? "exists" : "missing", "exists" );
        return;
    }
    // KIWI GLASS
    if ( subject == "class" )
    {
        // expect class <classname> <count>   (saved entities, as select class counts them)
        if ( w.size() != 4 || !ParseInt( w[3], expectedInt ) )
        {
            ScriptError( line, "expect class syntax is: expect class <classname> <count>" );
            return;
        }
        for ( entity_s *ent : SavedEntities() )
            if ( ent->eclass && ent->eclass->name && _stricmp( ent->eclass->name, w[2].c_str() ) == 0 )
                ++actualInt;
        ExpectResult( line, actualInt == expectedInt, std::to_string( actualInt ), w[3] );
        return;
    }
    if ( subject == "key" )
    {
        // expect key <classname> <nth> <key> <value>   ("" = the key is absent)
        int nth = -1;
        if ( w.size() != 6 || !ParseInt( w[3], nth ) || nth < 0 )
        {
            ScriptError( line, "expect key syntax is: expect key <classname> <nth> <key> <value>" );
            return;
        }
        entity_s *match = nullptr;
        for ( entity_s *ent : SavedEntities() )
            if ( ent->eclass && ent->eclass->name && _stricmp( ent->eclass->name, w[2].c_str() ) == 0 && nth-- == 0 )
            {
                match = ent;
                break;
            }
        if ( !match )
        {
            ScriptError( line, "class '%s' has no match %s", w[2].c_str(), w[3].c_str() );
            return;
        }
        const char *v = ValueForKey2( match, w[4].c_str() );
        const std::string actual = v ? v : "";
        ExpectResult( line, actual == w[5], actual.empty() ? "(absent)" : actual, w[5] );
        return;
    }
    if ( subject == "glass_errors" || subject == "glass_warnings" || subject == "glass_panes" )
    {
        if ( w.size() != 3 || !ParseInt( w[2], expectedInt ) )
        {
            ScriptError( line, "expect %s requires a count", subject.c_str() );
            return;
        }
        int warnings = 0, panes = 0;
        const int errors = KiwiGlass_Validate( false, &warnings, &panes );
        actualInt = subject == "glass_errors" ? errors : subject == "glass_warnings" ? warnings : panes;
        ExpectResult( line, actualInt == expectedInt, std::to_string( actualInt ), w[2] );
        return;
    }
    if ( subject == "glass_can_make" )
    {
        if ( w.size() != 3 || ( w[2] != "0" && w[2] != "1" ) )
        {
            ScriptError( line, "expect glass_can_make requires 0 or 1" );
            return;
        }
        const bool can = KiwiGlass_CanMake();
        ExpectResult( line, ( w[2] == "1" ) == can, can ? "1" : "0", w[2] );
        return;
    }
    // KIWI END
    if ( subject == "regions_selected" )
    {
        // KIWI (2026-10-03): members of the region selection that still resolve.
        if ( w.size() != 3 || !ParseInt( w[2], expectedInt ) )
        { ScriptError( line, "expect regions_selected requires a count" ); return; }
        actualInt = KiwiRegion_SelectedCount();
        ExpectResult( line, actualInt == expectedInt, std::to_string( actualInt ), w[2] );
        return;
    }
    if ( subject == "mapequal" )
    {
        if ( w.size() != 4 )
        {
            ScriptError( line, "expect mapequal requires two map paths" );
            return;
        }
        std::string detail;
        const MapCompareResult result = CompareMaps( w[2], w[3], detail );
        if ( result == MapCompareResult::Unavailable )
        {
            LogFormat( "WARN", "line %d: mapequal skipped: %s", line.number, detail.c_str() );
            return;
        }
        ExpectResult( line, result == MapCompareResult::Equal, detail, "maps equal" );
        return;
    }
    ScriptError( line, "unknown expect subject '%s'", w[1].c_str() );
}

static void ExecuteSelect( const ScriptLine &line )
{
    const std::vector<std::string> &w = line.words;
    if ( w.size() < 2 )
    {
        ScriptError( line, "select requires a mode" );
        return;
    }
    const std::string mode = Lower( w[1] );
    if ( mode == "all" && w.size() == 2 )
    {
        Select_Deselect( 1 );
        Select_Invert();
        g_nUpdateBits = -1;
        return;
    }
    if ( mode == "none" && w.size() == 2 )
    {
        Select_Deselect( 1 );
        g_nUpdateBits = -1;
        return;
    }
    if ( mode == "invert" && w.size() == 2 )
    {
        Select_Invert();
        g_nUpdateBits = -1;
        return;
    }
    if ( mode == "brush" || mode == "brush+" )
    {
        // KIWI (2026-10-05): `select brush+ e b` adds to the selection (Shift-click).
        int entityIndex = 0, brushIndex = 0;
        if ( w.size() != 4 || !ParseInt( w[2], entityIndex ) || !ParseInt( w[3], brushIndex ) ||
             !SelectBrushOrdinal( entityIndex, brushIndex, mode == "brush+" ) )
            ScriptError( line, "no saved brush at entity %s ordinal %s",
                         w.size() > 2 ? w[2].c_str() : "?", w.size() > 3 ? w[3].c_str() : "?" );
        return;
    }
    // KIWI GLASS
    if ( mode == "face" )
    {
        // select face <entity-index> <ordinal> <face>   (one face of that brush, nothing else)
        int entityIndex = 0, brushIndex = 0, faceIndex = 0;
        if ( w.size() != 5 || !ParseInt( w[2], entityIndex ) || !ParseInt( w[3], brushIndex ) ||
             !ParseInt( w[4], faceIndex ) || !SelectBrushOrdinal( entityIndex, brushIndex ) )
        {
            ScriptError( line, "select face syntax is: select face <entity-index> <ordinal> <face>" );
            return;
        }
        selbrush_t *node = selected_brushes.next;
        if ( !node || node == &selected_brushes || faceIndex < 0 || faceIndex >= node->def->faceCount )
        {
            ScriptError( line, "brush has no face %d", faceIndex );
            return;
        }
        Sel_Clear( KiwiSel() );
        Sel_Add( KiwiSel(), Sel_MakeFace( node, faceIndex ) );
        Sel_SyncToLegacy();
        g_nUpdateBits = -1;
        return;
    }
    // KIWI END
    if ( mode == "vertex" )
    {
        // select vertex <entity-index> <ordinal> <x> <y> <z>   (that brush's corner within 0.01;
        // no pixel pick, so two corners on one screen pixel cannot be confused)
        int entityIndex = 0, brushIndex = 0;
        float p[3];
        if ( w.size() != 7 || !ParseInt( w[2], entityIndex ) || !ParseInt( w[3], brushIndex ) ||
             !ParseFloat( w[4], p[0] ) || !ParseFloat( w[5], p[1] ) || !ParseFloat( w[6], p[2] ) ||
             !SelectBrushOrdinal( entityIndex, brushIndex ) )
        {
            ScriptError( line, "select vertex syntax is: select vertex <entity-index> <ordinal> <x> <y> <z>" );
            return;
        }
        selbrush_t *node = selected_brushes.next;
        brush_t    *def  = ( node && node != &selected_brushes ) ? node->def : nullptr;
        // a patch's control point (the point-mode item: no face, index col * height + row)
        if ( def && def->patch )
        {
            const patchMesh_t *pm = def->patch;
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                {
                    const float *c = pm->ctrl[col][row].xyz;
                    if ( fabsf( c[0] - p[0] ) < 0.01f && fabsf( c[1] - p[1] ) < 0.01f && fabsf( c[2] - p[2] ) < 0.01f )
                    {
                        Sel_Clear( KiwiSel() );
                        Sel_Add( KiwiSel(), Sel_MakePatchPoint( node, col * pm->height + row ) );
                        Sel_SyncToLegacy();
                        g_nUpdateBits = -1;
                        return;
                    }
                }
        }
        for ( int fi = 0; def && fi < def->faceCount; ++fi )
        {
            const winding_t *wd = def->faces[fi].w;
            for ( int vi = 0; wd && vi < wd->numpoints; ++vi )
                if ( fabsf( wd->p[vi][0] - p[0] ) < 0.01f && fabsf( wd->p[vi][1] - p[1] ) < 0.01f &&
                     fabsf( wd->p[vi][2] - p[2] ) < 0.01f )
                {
                    Sel_Clear( KiwiSel() );
                    Sel_Add( KiwiSel(), Sel_MakeVertex( node, fi, vi ) );
                    Sel_SyncToLegacy();
                    g_nUpdateBits = -1;
                    return;
                }
        }
        ScriptError( line, "brush has no corner at %g %g %g", p[0], p[1], p[2] );
        return;
    }
    if ( mode == "entity" )
    {
        int entityIndex = 0;
        if ( w.size() != 3 || !ParseInt( w[2], entityIndex ) )
        {
            ScriptError( line, "select entity requires an integer index" );
            return;
        }
        entity_s *ent = SavedEntityAt( entityIndex );
        if ( !ent ) ScriptError( line, "no saved entity at index %d", entityIndex );
        else SelectEntityDef( ent );
        return;
    }
    if ( mode == "class" )
    {
        int nth = -1;
        if ( w.size() != 3 && w.size() != 4 )
        {
            ScriptError( line, "select class syntax is: select class <classname> [nth]" );
            return;
        }
        if ( w.size() == 4 && ( !ParseInt( w[3], nth ) || nth < 0 ) )
        {
            ScriptError( line, "select class nth must be a non-negative integer" );
            return;
        }
        std::vector<entity_s *> matches;
        for ( entity_s *ent : SavedEntities() )
            if ( ent->eclass && ent->eclass->name && _stricmp( ent->eclass->name, w[2].c_str() ) == 0 )
                matches.push_back( ent );
        if ( nth >= 0 && (size_t)nth >= matches.size() )
        {
            ScriptError( line, "class '%s' has no match %d", w[2].c_str(), nth );
            return;
        }
        Select_Deselect( 1 );
        std::vector<selbrush_t *> nodes;
        for ( selbrush_t *node : SelectableNodes() )
        {
            if ( !node->owner ) continue;
            if ( nth >= 0 )
            {
                if ( node->owner->def == matches[(size_t)nth] ) nodes.push_back( node );
            }
            else if ( std::find( matches.begin(), matches.end(), node->owner->def ) != matches.end() )
                nodes.push_back( node );
        }
        SelectNodes( nodes );
        return;
    }
    if ( mode == "material" )
    {
        if ( w.size() != 3 )
        {
            ScriptError( line, "select material requires one material name" );
            return;
        }
        Select_Deselect( 1 );
        std::vector<selbrush_t *> nodes;
        for ( selbrush_t *node : SelectableNodes() )
            if ( NodeUsesMaterial( node, w[2].c_str() ) ) nodes.push_back( node );
        SelectNodes( nodes );
        return;
    }
    if ( mode == "box" )
    {
        if ( w.size() != 8 )
        {
            ScriptError( line, "select box requires six coordinates" );
            return;
        }
        float p[6];
        for ( int i = 0; i < 6; ++i )
            if ( !ParseFloat( w[(size_t)i + 2], p[i] ) )
            {
                ScriptError( line, "select box coordinate %d is not numeric", i + 1 );
                return;
            }
        float mins[3], maxs[3];
        for ( int axis = 0; axis < 3; ++axis )
        {
            mins[axis] = p[axis] < p[axis + 3] ? p[axis] : p[axis + 3];
            maxs[axis] = p[axis] > p[axis + 3] ? p[axis] : p[axis + 3];
        }
        Select_Deselect( 1 );
        std::vector<selbrush_t *> nodes;
        for ( selbrush_t *node : SelectableNodes() )
        {
            if ( !node->def ) continue;
            // Fixed-size entity bounds are selectable display proxies, not map
            // brushes/patches; the DSL's box mode is deliberately geometry-only.
            if ( node->owner && node->owner->def && node->owner->def->eclass &&
                 node->owner->def->eclass->fixedsize )
                continue;
            bool intersects = true;
            for ( int axis = 0; axis < 3; ++axis )
                if ( node->def->maxs[axis] < mins[axis] || node->def->mins[axis] > maxs[axis] )
                    intersects = false;
            if ( intersects ) nodes.push_back( node );
        }
        SelectNodes( nodes );
        return;
    }
    ScriptError( line, "unknown select mode '%s'", w[1].c_str() );
}

// Reference-image verbs (kiwi_refimage.h).  The transform verbs go through the same
// MoveBegin / apply / MoveCommit arms the modal G / R / S tools use, so each one is
// exactly one reference-image record in the unified undo journal (editor_undo).
// `add` mirrors the sidecar loader: no undo record, file path map-relative.
// terrain tool <name> | set <key> <value> | arm 0|1 | stroke x y [seconds] [shift] [ctrl]
static void ExecuteTerrain( const ScriptLine &line )
{
    const std::vector<std::string> &w = line.words;
    if ( w.size() < 2 ) { ScriptError( line, "terrain requires a verb" ); return; }
    const std::string verb = Lower( w[1] );
    if ( verb == "tool" )
    {
        if ( w.size() != 3 ) { ScriptError( line, "terrain tool <raise|setheight|smooth|noise|texture|blend|grass|trim>" ); return; }
        if ( !KiwiTerrain_TestSetTool( w[2].c_str() ) )
        { ScriptError( line, "terrain tool: unknown tool '%s'", w[2].c_str() ); return; }
        g_nUpdateBits = -1;
        return;
    }
    if ( verb == "set" )
    {
        float value = 0.0f;
        if ( w.size() != 4 || !ParseFloat( w[3], value ) )
        { ScriptError( line, "terrain set <key> <numeric value>" ); return; }
        if ( !KiwiTerrain_TestSet( w[2].c_str(), value ) )
        { ScriptError( line, "terrain set: unknown key '%s'", w[2].c_str() ); return; }
        return;
    }
    if ( verb == "material" )
    {
        // Texture paint's brush material; "base" = erase to base.
        if ( w.size() != 3 ) { ScriptError( line, "terrain material <name|base>" ); return; }
        KiwiTerrain_TestSetPaintMaterial( w[2].c_str() );
        return;
    }
    if ( verb == "arm" )
    {
        int on = 0;
        if ( w.size() != 3 || !ParseInt( w[2], on ) ) { ScriptError( line, "terrain arm 0|1" ); return; }
        KiwiTerrain_TestArm( on != 0 );
        g_nUpdateBits = -1;
        return;
    }
    if ( verb == "stroke" )
    {
        // A whole press/hold/release at world (x, y): the cursor is a vertical ray
        // resolved like the camera's (patches, then surfaces / base plane when
        // creation is allowed).  Modifiers name the Shift/Ctrl grammars of the tool.
        float x = 0.0f, y = 0.0f, seconds = 0.5f;
        bool shift = false, ctrl = false;
        if ( w.size() < 4 || !ParseFloat( w[2], x ) || !ParseFloat( w[3], y ) )
        { ScriptError( line, "terrain stroke x y [seconds] [shift] [ctrl]" ); return; }
        size_t next = 4;
        if ( w.size() > next && ParseFloat( w[next], seconds ) )
            ++next;
        for ( ; next < w.size(); ++next )
        {
            const std::string m = Lower( w[next] );
            if      ( m == "shift" ) shift = true;
            else if ( m == "ctrl" )  ctrl = true;
            else { ScriptError( line, "terrain stroke: unknown modifier '%s'", w[next].c_str() ); return; }
        }
        if ( !KiwiTerrain_TestStroke( x, y, seconds, shift, ctrl ) )
        { ScriptError( line, "terrain stroke: refused (not armed, a live command, or nothing under %.0f %.0f)", x, y ); return; }
        g_nUpdateBits = -1;
        UpdateSelection( -1, nullptr );
        return;
    }
    if ( verb == "tessellate" || verb == "split" )
    {
        // The panel's Tessellate (absolute cell size) / Split (chunk size) over the
        // selection; the optional number sets the size first.
        float size = 0.0f;
        if ( w.size() > 3 || ( w.size() == 3 && !ParseFloat( w[2], size ) ) )
        { ScriptError( line, "terrain %s [size]", verb.c_str() ); return; }
        const bool did = verb == "tessellate" ? KiwiTerrain_TestTessellate( size )
                                              : KiwiTerrain_TestSplit( size );
        if ( !did )
        { ScriptError( line, "terrain %s: nothing changed (select flat terrain sheets first)", verb.c_str() ); return; }
        g_nUpdateBits = -1;
        UpdateSelection( -1, nullptr );
        return;
    }
    ScriptError( line, "terrain verb must be tool, set, arm, stroke, tessellate or split" );
}

static void ExecuteRefImage( const ScriptLine &line )
{
    const std::vector<std::string> &w = line.words;
    if ( w.size() < 2 ) { ScriptError( line, "refimage requires a verb" ); return; }
    const std::string verb = Lower( w[1] );
    if ( verb == "add" )
    {
        // refimage add <map-relative file> [xy|xz|yz] [x y z] [w h]
        if ( w.size() != 3 && w.size() != 4 && w.size() != 7 && w.size() != 9 )
        { ScriptError( line, "refimage add <file> [xy|xz|yz] [x y z] [w h]" ); return; }
        krefImage_t r;
        r.file = w[2];
        if ( w.size() >= 4 )
        {
            const std::string plane = Lower( w[3] );
            r.axis = plane == "xy" ? 2 : plane == "xz" ? 1 : plane == "yz" ? 0 : -1;
            if ( r.axis < 0 ) { ScriptError( line, "refimage add plane must be xy, xz, or yz" ); return; }
        }
        if ( w.size() >= 7 )
            for ( int k = 0; k < 3; ++k )
                if ( !ParseFloat( w[(size_t)k + 4], r.origin[k] ) )
                { ScriptError( line, "refimage add origin is not numeric" ); return; }
        if ( w.size() == 9 && ( !ParseFloat( w[7], r.width ) || !ParseFloat( w[8], r.height ) ) )
        { ScriptError( line, "refimage add size is not numeric" ); return; }
        const int index = KiwiRefImage_Add( r );
        LogFormat( "REFIMAGE", "added #%d %s", index, w[2].c_str() );
        g_nUpdateBits = -1;
        return;
    }
    if ( verb == "deleteselected" )
    {
        // Every selected picture as one record (the Delete key's route).
        if ( !KiwiRefImage_DeleteSelected() )
        { ScriptError( line, "refimage deleteselected: nothing selected" ); return; }
        g_nUpdateBits = -1;
        return;
    }
    if ( verb == "select" || verb == "select+" || verb == "deselect" || verb == "delete" )
    {
        // select = exclusive, select+ = add (and make primary), deselect = remove one.
        int index = -1;
        if ( w.size() != 3 || !ParseInt( w[2], index ) || index < 0 || index >= KiwiRefImage_Count() )
        { ScriptError( line, "refimage %s requires an existing image index", verb.c_str() ); return; }
        if      ( verb == "select" )   KiwiRefImage_Select( index );
        else if ( verb == "select+" )  KiwiRefImage_SelectAdd( index );
        else if ( verb == "deselect" ) KiwiRefImage_SelectRemove( index );
        else                           KiwiRefImage_DeleteAt( index );
        g_nUpdateBits = -1;
        return;
    }
    if ( verb == "move" || verb == "rotate" || verb == "scale" )
    {
        // Acts on the SELECTED image: move dx dy dz | rotate <deg about its plane normal>
        // | scale <uniform factor about its own origin>.
        // rotate takes an optional world axis (x|y|z); default = the plane normal.
        float v[3] = { 0.0f, 0.0f, 0.0f };
        int rotAxis = -1;
        if ( verb == "rotate" && w.size() == 4 )
        {
            const std::string a = Lower( w[3] );
            rotAxis = a == "x" ? 0 : a == "y" ? 1 : a == "z" ? 2 : -1;
            if ( rotAxis < 0 ) { ScriptError( line, "refimage rotate axis must be x, y, or z" ); return; }
        }
        const size_t need = verb == "move" ? 5 : ( rotAxis >= 0 ? 4 : 3 );
        const size_t nargs = verb == "move" ? 3 : 1;
        if ( w.size() != need )
        { ScriptError( line, "refimage move dx dy dz | rotate <deg> [x|y|z] | scale <factor>" ); return; }
        for ( size_t k = 0; k < nargs; ++k )
            if ( !ParseFloat( w[k + 2], v[k] ) )
            { ScriptError( line, "refimage %s argument %d is not numeric", verb.c_str(), (int)k + 1 ); return; }
        const krefImage_t *r = KiwiRefImage_At( KiwiRefImage_Selected() );
        if ( !r ) { ScriptError( line, "refimage %s requires a selected image", verb.c_str() ); return; }
        if ( !KiwiRefImage_CanMove() )
        { ScriptError( line, "refimage %s: the selected image is locked or hidden", verb.c_str() ); return; }
        const int axis = rotAxis >= 0 ? rotAxis : r->axis;
        float pivot[3];
        if ( !KiwiRefImage_MoveBegin( pivot ) )
        { ScriptError( line, "refimage %s: MoveBegin refused", verb.c_str() ); return; }
        if ( verb == "move" )
            KiwiRefImage_MoveApply( v );
        else if ( verb == "rotate" )
            KiwiRefImage_RotateApply( pivot, axis, v[0] );
        else
        {
            const float f[3] = { v[0], v[0], v[0] };
            KiwiRefImage_ScaleApply( pivot, f );
        }
        KiwiRefImage_MoveCommit();
        g_nUpdateBits = -1;
        return;
    }
    ScriptError( line, "unknown refimage verb '%s'", w[1].c_str() );
}

static bool LoadScriptLines( const std::string &path, std::vector<ScriptLine> &out, std::string &error );

static void ExecuteLine( const ScriptLine &line )
{
    const std::vector<std::string> &w = line.words;
    if ( w.empty() ) return;
    const std::string command = Lower( w[0] );
    // KIWI (2026-10-05): remote-session plumbing and the probe verbs (kiwi_testprobe.cpp).
    if ( command == "__noop" )
        return;
    if ( command == "__parse_error" )
    {
        ScriptError( line, "%s", w.size() > 1 ? w[1].c_str() : "unparsable line" );
        return;
    }
    if ( command == "run" )
    {
        // Inline a script after this line; in a remote session its lines answer as one.
        if ( w.size() != 2 ) { ScriptError( line, "usage: run <script.kt>" ); return; }
        std::vector<ScriptLine> more;
        std::string error;
        if ( !LoadScriptLines( FullPath( w[1].c_str() ), more, error ) )
        { ScriptError( line, "run: %s", error.c_str() ); return; }
        s_test.lines.insert( s_test.lines.begin() + (std::ptrdiff_t)s_test.nextLine, more.begin(), more.end() );
        if ( s_test.remote )
            s_test.batchEnd = s_test.nextLine + more.size();
        LogFormat( "RUN", "%u lines from %s", (unsigned)more.size(), w[1].c_str() );
        return;
    }
    {
        std::string error;
        if ( KiwiTestProbe_Execute( w, error ) )
        {
            if ( !error.empty() )
                ScriptError( line, "%s", error.c_str() );
            return;
        }
    }
    if ( command == "conpoint_move_probe" )
    {
        // KIWI (2026-10-31, user: "make it so i can move the points of a multi point spline
        // construction line and the spline will re-adjust to keep connection" / "i still want to
        // be able to move the entire thing"): a 4-point spline with a line joined to its last
        // point.  Moving that one control point moves it and the line's joined end only; moving
        // the spline as an object moves all of it and leaves the line.
        Map_New();
        KiwiCon_ClearAll();
        kconObject_t spline;
        spline.type = KCON_POLYLINE;
        const float sp[12] = { 0, 0, 0,  32, 0, 16,  64, 0, 0,  96, 0, 16 };
        spline.pts.assign( sp, sp + 12 );
        spline.smooth.assign( 4, 1 );
        kconObject_t joined;
        joined.type = KCON_LINE;
        const float jp[6] = { 96, 0, 16,  96, 0, 64 };
        joined.pts.assign( jp, jp + 6 );
        const int s = KiwiCon_Add( spline ), l = KiwiCon_Add( joined );
        if ( s < 0 || l < 0 ) { ScriptError( line, "could not add the spline and line" ); return; }

        auto movePt = [&]( const kconSelItem_t &it, const float delta[3] ) {
            KiwiConSel_Clear();
            KiwiConSel_ApplyClick( it, false, false );
            float ref[3];
            if ( !KiwiConSel_MoveBegin( ref ) )
                return false;
            KiwiConSel_MoveApply( delta );
            KiwiConSel_MoveCommit();
            return true;
        };
        auto near3 = []( const float *a, float x, float y, float z ) {
            return fabsf( a[0] - x ) < 0.01f && fabsf( a[1] - y ) < 0.01f && fabsf( a[2] - z ) < 0.01f;
        };

        kconSelItem_t pt;
        pt.object = s;  pt.kind = KCONSEL_POINT;  pt.index = 3;
        const float up[3] = { 0, 0, 8 };
        if ( !movePt( pt, up ) ) { ScriptError( line, "the point move did not start" ); return; }
        const kconObject_t *so = KiwiCon_At( s ), *lo = KiwiCon_At( l );
        if ( !near3( &so->pts[9], 96, 0, 24 ) || !near3( &so->pts[0], 0, 0, 0 ) || !near3( &so->pts[6], 64, 0, 0 ) )
        { ScriptError( line, "the spline's points are wrong after moving its last point" ); return; }
        if ( !near3( &lo->pts[0], 96, 0, 24 ) || !near3( &lo->pts[3], 96, 0, 64 ) )
        { ScriptError( line, "the joined line did not keep its connection (%g %g %g)", lo->pts[0], lo->pts[1], lo->pts[2] ); return; }
        if ( !KiwiCon_HasSmooth( *so ) || KiwiCon_VertCount( *so ) <= 4 )
        { ScriptError( line, "the spline lost its curve" ); return; }

        kconSelItem_t whole;
        whole.object = s;  whole.kind = KCONSEL_OBJECT;  whole.index = -1;
        const float side[3] = { 0, 32, 0 };
        if ( !movePt( whole, side ) ) { ScriptError( line, "the object move did not start" ); return; }
        so = KiwiCon_At( s );  lo = KiwiCon_At( l );
        if ( !near3( &so->pts[0], 0, 32, 0 ) || !near3( &so->pts[9], 96, 32, 24 ) )
        { ScriptError( line, "the whole spline did not move" ); return; }
        if ( !near3( &lo->pts[0], 96, 0, 24 ) )
        { ScriptError( line, "moving the whole spline dragged the line along" ); return; }
        KiwiConSel_Clear();
        LogFormat( "CONPOINT-MOVE", "spline point move keeps the joined line; whole-spline move moves all of it" );
        return;
    }
    if ( command == "curve_bool_probe" )
    {
        // KIWI (2026-10-31, user: "a brush can be boolean'd (diff) with a curve ... a square box
        // with a curve running perfectly through it, and caps that line up with the brush
        // exterior"): a 128 x 64 x 128 box and a radius-16 patch cylinder along +Y from y -32 to
        // 96.  After Q with the curve as the tool: the curve's ends sit on the box faces y 0 and
        // y 64, nothing solid is left on its axis, solid remains just outside it, and undo
        // brings back the whole box and the untrimmed curve.
        Map_New();
        kconPlane_t ground;                                  // z up through the origin
        kconPlane_t across;
        const float acrossOrigin[3] = { 64.0f, -32.0f, 64.0f }, acrossNormal[3] = { 0.0f, 1.0f, 0.0f };
        KiwiCon_MakePlane( acrossOrigin, acrossNormal, nullptr, &across );
        const float centre[2] = { 0.0f, 0.0f };
        brush_t *cyl = KiwiPrim_BuildPatchCylinder( across, centre, 16.0f, 0.0f, 128.0f, KiwiPrim_PatchDensity() );
        if ( !cyl || !cyl->patch ) { ScriptError( line, "could not build the patch cylinder" ); return; }
        selbrush_t *curve = KiwiExtrude_LandDef( cyl );
        Select_Deselect( 1 );
        const float square[8] = { 0, 0, 128, 0, 128, 64, 0, 64 };
        const char *why = "unknown";
        brush_t *box = KiwiExtrude_BuildPrismDef( ground, square, 4, 0.0f, 128.0f, &why );
        if ( !box ) { ScriptError( line, "could not build the box: %s", why ); return; }
        KiwiValid_Rebuild( box );
        KiwiExtrude_LandDef( box );

        if ( !KiwiCmd_Start( KIWI_CMD_BOOLEAN ) || !KiwiBool_TestPickTool( curve ) )
        { ScriptError( line, "boolean did not start with the box as its target" ); return; }
        KiwiCmd_Commit();

        const patchMesh_t *p = curve->def ? curve->def->patch : nullptr;
        if ( !p ) { ScriptError( line, "the curve tool was consumed" ); return; }
        for ( int col = 0; col < p->width; ++col )
        {
            const float y0 = p->ctrl[col][0].xyz[1], y1 = p->ctrl[col][p->height - 1].xyz[1];
            const float lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
            if ( fabsf( lo ) > 0.01f || fabsf( hi - 64.0f ) > 0.01f || fabsf( p->ctrl[col][1].xyz[1] - 32.0f ) > 0.01f )
            { ScriptError( line, "curve column %d ends at y %g / %g, not flush with the box (0 / 64)", col, y0, y1 ); return; }
        }

        // Solid at a point: inside every face plane of some brush.
        auto solidAt = []( const float pt[3] ) {
            for ( int list = 0; list < 2; ++list )
            {
                selbrush_t *head = list ? &selected_brushes : &active_brushes;
                for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                {
                    if ( !b->def || b->def->patch )
                        continue;
                    bool inside = true;
                    for ( int f = 0; f < b->def->faceCount && inside; ++f )
                        inside = Dot3( b->def->faces[f].plane.normal, pt ) - b->def->faces[f].plane.dist < -0.01f;
                    if ( inside )
                        return true;
                }
            }
            return false;
        };
        for ( int i = 1; i < 8; ++i )
        {
            const float onAxis[3] = { 64.0f, 8.0f * i, 64.0f };
            if ( solidAt( onAxis ) ) { ScriptError( line, "solid left inside the curve at y %g", onAxis[1] ); return; }
            const float outside[3] = { 64.0f + 30.0f, 8.0f * i, 64.0f };
            if ( !solidAt( outside ) ) { ScriptError( line, "no solid just outside the curve at y %g", outside[1] ); return; }
        }
        MapCounts counts = CountMap();
        if ( counts.patches != 1 || counts.brushes < 4 )
        { ScriptError( line, "expected the carved box pieces and one curve, got %d brushes %d patches", counts.brushes, counts.patches ); return; }

        Undo_Undo();
        counts = CountMap();
        bool restored = counts.brushes == 1 && counts.patches == 1;
        for ( int list = 0; list < 2 && restored; ++list )
        {
            selbrush_t *head = list ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
                if ( b->def && b->def->patch
                  && fabsf( b->def->patch->ctrl[0][0].xyz[1] + 32.0f ) > 0.01f
                  && fabsf( b->def->patch->ctrl[0][0].xyz[1] - 96.0f ) > 0.01f )
                    restored = false;
        }
        if ( !restored ) { ScriptError( line, "undo did not restore the box and the untrimmed curve (%d brushes %d patches)", counts.brushes, counts.patches ); return; }
        LogFormat( "CURVE-BOOL", "box carved around the curve, curve ends flush at y 0 / 64, undo restores both" );
        return;
    }
    if ( command == "small_circle_probe" )
    {
        // KIWI (2026-10-31, user: "if I make a small circle with the circle tool it doesn't make a
        // face"): the region weld used to fall back to the grid's quarter step for a circle whose
        // chords were all under KREG_JOIN_DIST, welding the loop away.  Every radius on a fine and
        // a coarse grid must make a face of about its own area.
        struct RestoreGrid
        {
            float  inches = KiwiUnits_GridSpacingInches();
            float  units  = KiwiUnits_PerInch();
            ~RestoreGrid()
            {
                KiwiUnits_SetGridSpacingInches( inches );
                KiwiUnits_SetPerInch( units );
            }
        } restore;
        KiwiUnits_SetPerInch( 1.0f );
        static const float s_grids[]  = { 8.0f, 64.0f };
        static const float s_radii[]  = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
        int cases = 0;
        for ( float grid : s_grids )
        for ( float radius : s_radii )
        {
            Map_New();
            KiwiUnits_SetGridSpacingInches( grid );
            kconObject_t circle;
            circle.type   = KCON_CIRCLE;
            circle.closed = true;
            circle.radius = radius;
            const int object = KiwiCon_AddWithUndo( circle );
            const auto &regions = KiwiRegion_All();
            int region = -1;
            for ( size_t i = 0; i < regions.size(); ++i )
                if ( regions[i].sourceObject == object ) region = (int)i;
            if ( region < 0 )
            { ScriptError( line, "radius %g circle on a %g grid made no face", radius, grid ); return; }
            const float area = KiwiRegion_SignedArea( regions[(size_t)region].pts );
            const float disc = KCON_PI * radius * radius;
            if ( area < disc * 0.6f || area > disc * 1.01f )
            { ScriptError( line, "radius %g circle on a %g grid: face area %g, disc %g", radius, grid, area, disc ); return; }
            ++cases;
        }
        LogFormat( "SMALL-CIRCLE", "%d cases passed: every radius 0.5-8 on an 8 and a 64 grid makes a face", cases );
        return;
    }
    if ( command == "circle_extrude_probe" )
    {
        // Exercise the actual modal command, source preservation, undo and patch
        // seam on horizontal, vertical and tilted construction planes.
        struct RestorePreferences
        {
            bool mode = KiwiPrim_PatchMode();
            int density = KiwiPrim_PatchDensity();
            int sides = Radiant_ProfileGetInt( "KiwiUX", "CircleExtrudeSides", 16 );
            float units = KiwiUnits_PerInch();
            bool backfaces = KiwiPrim_PatchBackfaces();
            float thickness = KiwiPrim_PatchThickness();
            ~RestorePreferences()
            {
                KiwiCmd_Cancel();
                KiwiPrim_SetPatchMode( mode );
                KiwiPrim_SetPatchDensity( density );
                Radiant_ProfileSetInt( "KiwiUX", "CircleExtrudeSides", sides );
                KiwiUnits_SetPerInch( units );
                KiwiPrim_SetPatchBackfaces( backfaces );
                KiwiPrim_SetPatchThickness( thickness );
            }
        } restore;
        KiwiUnits_SetPerInch( 1.0f );
        KiwiPrim_SetPatchBackfaces( false );
        KiwiPrim_SetPatchThickness( 0 );
        int cases = 0;
        for ( int axis = 0; axis < 3; ++axis )
        for ( int sign = -1; sign <= 1; sign += 2 )
        for ( int variant = 0; variant < 7; ++variant )
        {
            Map_New();
            kconObject_t circle;
            circle.type = KCON_CIRCLE;
            circle.closed = true;
            circle.radius = 128;
            circle.centre[0] = 35;
            circle.centre[1] = -27;
            circle.segs = 3; // old display preference must not determine output
            if ( axis )
            {
                const float angle = axis == 1 ? KCON_PI * 0.5f : KCON_PI * 0.25f;
                circle.plane.normal[0] = sinf( angle ); circle.plane.normal[2] = cosf( angle );
                circle.plane.u[0] = cosf( angle ); circle.plane.u[2] = -sinf( angle );
            }
            const int object = KiwiCon_AddWithUndo( circle );
            const auto &regions = KiwiRegion_All();
            int region = -1;
            for ( size_t i = 0; i < regions.size(); ++i )
                if ( regions[i].sourceObject == object ) region = (int)i;
            if ( region < 0 ) { ScriptError( line, "circle did not form a region" ); return; }
            KiwiRegion_Select( region );
            const bool patch = variant >= 3;
            const int count = patch ? variant - 2 : variant == 0 ? 3 : variant == 1 ? 5 : 64;
            KiwiPrim_SetPatchMode( !patch ); // switch through the real P key
            if ( !KiwiCmd_Start( KIWI_CMD_EXTRUDE_REGION ) )
            { ScriptError( line, "circle extrusion failed to start" ); return; }
            KiwiCmd_KeyDown( 'P', 0 );
            if ( KiwiNum_FieldCount() != (patch ? 3 : 2) || strcmp( KiwiNum_Field( 1 )->label, patch ? "density" : "sides" ) )
            { ScriptError( line, "wrong circle extrusion numeric field" ); return; }
            KiwiCmd_Active()->NumericFieldChanged( 1, true, Units_FromDisplay( (float)count ) );
            // Toggle twice: both choices keep their own count and the source stays analytic.
            KiwiCmd_KeyDown( 'P', 0 ); KiwiCmd_KeyDown( 'P', 0 );
            float reported = 0;
            KiwiCmd_Active()->NumericFieldValue( 1, &reported );
            if ( reported != count ) { ScriptError( line, "mode switch lost circle count" ); return; }
            if ( sign < 0 ) KiwiCmd_KeyDown( VK_OEM_MINUS, 0 );
            KiwiCmd_KeyDown( '6', 0 ); KiwiCmd_KeyDown( '4', 0 );
            KiwiCmd_Commit();
            selbrush_t *selected = selected_brushes.next;
            if ( selected == &selected_brushes || selected->next != &selected_brushes || !selected->def )
            { ScriptError( line, "circle extrusion must select exactly one result" ); return; }
            brush_t *def = selected->def;
            float axisEnds[2][3], centreWorld[3];
            KiwiCon_PlaneToWorld( circle.plane, circle.centre, centreWorld );
            if ( !KiwiPrim_CylinderAxis( def, axisEnds ) )
            { ScriptError( line, "circle output lost its cylinder centre anchors" ); return; }
            for ( int k = 0; k < 3; ++k )
                if ( fabsf( ( axisEnds[0][k]+axisEnds[1][k] ) * 0.5f -
                            ( centreWorld[k]+circle.plane.normal[k]*sign*32 ) ) > 0.02f )
                { ScriptError( line, "cylinder centre drifted from the analytic source" ); return; }
            if ( !!def->patch != patch || ( !patch && def->faceCount != count + 2 ) )
            { ScriptError( line, "wrong circle output type or brush side count" ); return; }
            if ( patch )
            {
                patchMesh_t *p = def->patch;
                if ( p->width != ( count + 3 ) * 2 + 1 || p->height != 3 || !p->curveDef || p->subDivType != ( 1 << ( 4 - count ) ) )
                { ScriptError( line, "wrong patch density or missing render mesh" ); return; }
                for ( int row = 0; row < 3; ++row )
                    if ( memcmp( p->ctrl[0][row].xyz, p->ctrl[p->width-1][row].xyz, sizeof( float ) * 3 ) )
                    { ScriptError( line, "circle patch seam is open" ); return; }
                float delta[3];
                for ( int k = 0; k < 3; ++k ) delta[k] = p->ctrl[0][2].xyz[k] - p->ctrl[0][0].xyz[k];
                float depth = 0;
                for ( int k = 0; k < 3; ++k ) depth += delta[k] * circle.plane.normal[k];
                if ( fabsf( depth - 64 ) > 0.01f )
                { ScriptError( line, "patch did not use the requested height/plane" ); return; }
            }
            const kconObject_t *after = KiwiCon_At( object );
            if ( !after || after->type != KCON_CIRCLE || after->radius != circle.radius ||
                 after->centre[0] != circle.centre[0] || after->centre[1] != circle.centre[1] || after->segs != 3 )
            { ScriptError( line, "extrusion changed the analytic source circle" ); return; }
            Undo_Undo();
            MapCounts counts = CountMap();
            if ( counts.brushes || counts.patches )
            { ScriptError( line, "undo left circle output behind" ); return; }
            Undo_Redo();
            counts = CountMap();
            if ( counts.brushes != ( patch ? 0 : 1 ) || counts.patches != ( patch ? 1 : 0 ) )
            { ScriptError( line, "redo lost circle output" ); return; }
            ++cases;
        }
        LogFormat( "CIRCLE-EXTRUDE", "%d cases passed: output modes/counts, planes, signed height, source, seam, undo/redo", cases );
        return;
    }
    if ( command == "open" )
    {
        if ( w.size() != 2 ) { ScriptError( line, "open requires one map path" ); return; }
        if ( !FileExists( w[1] ) ) { ScriptError( line, "map does not exist: %s", w[1].c_str() ); return; }
        Radiant_OpenMap( w[1].c_str() );
        return;
    }
    if ( command == "new" )
    {
        if ( w.size() != 1 ) { ScriptError( line, "new takes no arguments" ); return; }
        Map_New();
        return;
    }
    if ( command == "clipboard" )
    {
        // `clipboard forget`: drop this editor's in-app copy, as a second instance has
        // none - the next paste must come off the Windows clipboard (cross-instance paste).
        if ( w.size() != 2 || Lower( w[1] ) != "forget" ) { ScriptError( line, "clipboard forget" ); return; }
        RadiantClipboard_ForgetLocal();
        return;
    }
    if ( command == "save" || command == "saveas" )
    {
        if ( w.size() != 2 ) { ScriptError( line, "%s requires one map path", command.c_str() ); return; }
        Map_SaveFile( w[1].c_str(), 0 );
        return;
    }
    if ( command == "plasticity_export" )
    {
        // KIWI: the Ctrl+Shift+P export without its dialog.  Writes <map>_ref.step/.obj
        // beside the map and prints the paths; never drives Plasticity in test mode.
        bool construction = false;
        bool terrain = false;
        int  atOrigin = -1;
        for ( size_t i = 1; i < w.size(); ++i )
        {
            if ( Lower( w[i] ) == "construction" ) { construction = true; continue; }
            if ( Lower( w[i] ) == "terrain" )      { terrain = true; continue; }
            if ( Lower( w[i] ) == "origin" )       { atOrigin = 1; continue; }
            if ( Lower( w[i] ) == "world" )        { atOrigin = 0; continue; }
            ScriptError( line, "plasticity_export accepts only the words 'construction', 'terrain', 'origin' and 'world'" );
            return;
        }
        if ( !KiwiPlastBridge_ExportNow( construction, terrain, false, atOrigin ) )
            ScriptError( line, "plasticity_export wrote nothing (see console)" );
        return;
    }
    if ( command == "frames" )
    {
        int count = 0;
        if ( w.size() != 2 || !ParseInt( w[1], count ) || count < 0 )
        { ScriptError( line, "frames requires a non-negative integer" ); return; }
        s_test.waitFrames = count;
        return;
    }
    if ( command == "sleep_ms" )
    {
        int milliseconds = 0;
        if ( w.size() != 2 || !ParseInt( w[1], milliseconds ) || milliseconds < 0 )
        { ScriptError( line, "sleep_ms requires a non-negative integer" ); return; }
        s_test.sleepUntil = GetTickCount64() + (ULONGLONG)milliseconds;
        return;
    }
    if ( command == "camera" )
    {
        if ( w.size() != 7 ) { ScriptError( line, "camera requires x y z pitch yaw roll" ); return; }
        float value[6];
        for ( int i = 0; i < 6; ++i )
            if ( !ParseFloat( w[(size_t)i + 1], value[i] ) )
            { ScriptError( line, "camera argument %d is not numeric", i + 1 ); return; }
        camera_s *camera = Ed_Camera();
        for ( int i = 0; i < 3; ++i ) camera->origin[i] = value[i];
        for ( int i = 0; i < 3; ++i ) camera->angles[i] = value[i + 3];
        CamWnd_BuildMatrix();
        g_nUpdateBits = -1;
        return;
    }
    if ( command == "model_pick_bvh" )
    {
        int count=0;
        for ( selbrush_t *b=selected_brushes.next; b && b!=&selected_brushes; b=b->next )
        {
            if ( !KiwiDrop_IsModelEntity(b) || b->owner->prefab ) continue;
            if ( !KiwiDrop_ValidateModelTree(b) )
            { ScriptError(line,"cached model picking disagrees with exhaustive triangles");return; }
            ++count;
        }
        if ( !count ) ScriptError(line,"model_pick_bvh requires selected model instances");
        else LogFormat("PICK-BVH","%d models passed 243 rays each",count);
        return;
    }
    if ( command == "click_at" )
    {
        // KIWI (2026-10-03): click_at x y z [shift] [ctrl] - a plain camera click on the
        // pixel a world point projects to, through ClickSelect (construction, images,
        // regions, brushes in the real arbitration order).
        if ( w.size() < 4 || w.size() > 6 ) { ScriptError( line, "usage: click_at x y z [shift] [ctrl]" ); return; }
        float p[3];
        for ( int i = 0; i < 3; ++i )
            if ( !ParseFloat( w[(size_t)i + 1], p[i] ) ) { ScriptError( line, "click_at coordinate %d is not numeric", i + 1 ); return; }
        bool shift = false, ctrl = false;
        for ( size_t i = 4; i < w.size(); ++i )
        {
            if ( Lower( w[i] ) == "shift" ) shift = true;
            else if ( Lower( w[i] ) == "ctrl" ) ctrl = true;
            else { ScriptError( line, "click_at modifier must be shift or ctrl" ); return; }
        }
        float ix, iy;
        if ( !Pick_WorldToImage( p, &ix, &iy ) ) { ScriptError( line, "click_at point is off camera" ); return; }
        KiwiBox_ClickSelectAt( (int)floorf( ix + 0.5f ), (int)floorf( iy + 0.5f ), shift, ctrl );
        g_nUpdateBits = -1;
        return;
    }
    if ( command == "model_vert_probe" )
    {
        // KIWI (2026-10-03): xmodel vertex snapping.  Per selected model: the vertex nearest
        // its screen centre lies inside the model's world bounds, re-queries to itself at
        // 0 px, and the picker's own mesh trace finds nothing in front of it (a ray aimed
        // exactly at a vertex may slip between its triangles, so a miss passes).
        int count = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !KiwiDrop_IsModelEntity( b ) || b->owner->prefab ) continue;
            float mins[3], maxs[3], angles[3], scale, origin[3], rmin[3], rmax[3];
            if ( !KiwiDrop_GetModelInfo( b, mins, maxs, angles, &scale, origin )
              || !KiwiDrop_TransformBounds( mins, maxs, angles, scale, rmin, rmax ) )
            { ScriptError( line, "model_vert_probe: no model info" ); return; }
            float centre[3], sx, sy;
            for ( int k = 0; k < 3; ++k ) centre[k] = origin[k] + 0.5f * ( rmin[k] + rmax[k] );
            if ( !Pick_WorldToImage( centre, &sx, &sy ) )
            { ScriptError( line, "model_vert_probe: model centre is off camera" ); return; }
            kiwiModelVert_t cands[64], v, again;
            const int found = KiwiDrop_ModelVertsNear( sx, sy, 100000.0f, false, cands, 64 );
            int pickIndex = -1;
            for ( int i = 0; i < found && pickIndex < 0; ++i )
                if ( cands[i].node == b ) pickIndex = i;         // other models' vertices may be nearer
            if ( pickIndex < 0 )
            { ScriptError( line, "model_vert_probe: no vertex found on the model" ); return; }
            v = cands[pickIndex];
            for ( int k = 0; k < 3; ++k )
                if ( v.pos[k] < origin[k] + rmin[k] - 0.01f || v.pos[k] > origin[k] + rmax[k] + 0.01f )
                { ScriptError( line, "model_vert_probe: vertex outside the model bounds" ); return; }
            if ( KiwiDrop_ModelVertsNear( v.px, v.py, 0.5f, false, &again, 1 ) != 1
              || fabsf( again.pos[0] - v.pos[0] ) + fabsf( again.pos[1] - v.pos[1] ) + fabsf( again.pos[2] - v.pos[2] ) > 0.01f )
            { ScriptError( line, "model_vert_probe: the vertex does not re-query to itself" ); return; }
            const float *eye = Ed_Camera()->origin;
            float dir[3] = { v.pos[0] - eye[0], v.pos[1] - eye[1], v.pos[2] - eye[2] };
            const float len = sqrtf( dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] );
            for ( int k = 0; k < 3; ++k ) dir[k] /= len;
            float hit = 0.0f;
            if ( KiwiDrop_TraceModelCached( b, eye, dir, &hit, nullptr ) == 1 && hit < len - ( 0.05f + len * 1.0e-3f ) )
            { ScriptError( line, "model_vert_probe: vertex %.2f %.2f %.2f is behind the model's own surface (%.3f vs %.3f)",
                           v.pos[0], v.pos[1], v.pos[2], hit, len ); return; }
            LogFormat( "MODEL-VERT", "vertex %.3f %.3f %.3f at %.1f %.1f px", v.pos[0], v.pos[1], v.pos[2], v.px, v.py );
            ++count;
        }
        if ( !count ) ScriptError( line, "model_vert_probe requires selected model instances" );
        return;
    }
    if ( command == "facing_toggle_probe" )
    {
        // The camera's retained scene keys its kept arrows on this switch (Cam_SceneKey), so
        // a flip re-describes every entity; the probe checks the switch flips and restores.
        const bool before=KiwiUX_ShowFacingArrows();
        KiwiUX_SetShowFacingArrows(!before);
        const bool changed=KiwiUX_ShowFacingArrows()!=before;
        KiwiUX_SetShowFacingArrows(before);
        if ( !changed ) ScriptError(line,"facing toggle did not change the switch");
        else LogFormat("FACING","toggle passed");
        return;
    }
    if ( command == "prefab_insert" )
    {
        if ( w.size() != 2 || !KiwiModelBrowser_InsertPrefab( w[1].c_str() ) )
            ScriptError( line, "prefab_insert failed" );
        return;
    }
    if ( command == "model_swap" )
    {
        // KIWI: the Models tab's "Swap in place with new model" on the current selection.
        char err[256] = { 0 };
        if ( w.size() != 2 || !KiwiModelBrowser_SwapSelected( w[1].c_str(), err, sizeof( err ) ) )
            ScriptError( line, "model_swap failed: %s", err[0] ? err : "usage: model_swap <xmodel>" );
        return;
    }
    if ( command == "particle_place" )
    {
        // KIWI (2026-09-24): the Particles tab's drop, headless.
        // particle_place <fx> <x> <y> <z> [nx ny nz]   (fx = path under raw/fx, no .efx)
        if ( w.size() != 5 && w.size() != 8 )
        { ScriptError( line, "usage: particle_place <fx> x y z [nx ny nz]" ); return; }
        const float o[3] = { (float)atof( w[2].c_str() ), (float)atof( w[3].c_str() ), (float)atof( w[4].c_str() ) };
        float n[3] = { 0.0f, 0.0f, 1.0f };
        if ( w.size() == 8 )
            for ( int k = 0; k < 3; ++k ) n[k] = (float)atof( w[5 + k].c_str() );
        if ( !KiwiParticles_PlaceAt( w[1].c_str(), o, n ) )
            ScriptError( line, "particle_place failed" );
        return;
    }
    if ( command == "particle_export" )
    {
        // Write maps/createfx/<map>_fx.gsc + maps/mp/<map>_fx.gsc now (normally on save).
        KiwiParticles_ExportForMap( currentmap, false );
        return;
    }
    // KIWI GLASS
    if ( command == "glass_set" )
    {
        // glass_set <set>   (the active set, a file under raw/glass)
        if ( w.size() != 2 )
        { ScriptError( line, "usage: glass_set <set>" ); return; }
        if ( !KiwiGlass_SetActiveSet( w[1].c_str() ) )
            ScriptError( line, "glass set '%s' is not in raw/glass", w[1].c_str() );
        return;
    }
    if ( command == "glass_make" || command == "glass_unmake" || command == "glass_apply" )
    {
        const bool ok = command == "glass_make"   ? KiwiGlass_Make()
                      : command == "glass_unmake" ? KiwiGlass_Unmake()
                                                  : KiwiGlass_ApplySetToSelection();
        if ( !ok )
            ScriptError( line, "%s changed nothing", command.c_str() );
        return;
    }
    if ( command == "glass_validate" )
    {
        KiwiGlass_Validate( true, nullptr, nullptr );   // one console line per bad pane
        return;
    }
    // KIWI END
    if ( command == "toolkey" || command == "toolvalue" )
    {
        if ( !KiwiCmd_Active() || w.size() != 2 )
        { ScriptError( line, "toolkey/toolvalue requires an active tool and one argument" ); return; }
        if ( command == "toolkey" )
        {
            if ( w[1].size() != 1 ) { ScriptError( line, "toolkey requires one letter" ); return; }
            KiwiCmd_KeyDown( toupper( (unsigned char)w[1][0] ), 0 );
        }
        else
        {
            float value;
            if ( !ParseFloat( w[1], value ) ) { ScriptError( line, "toolvalue requires a number" ); return; }
            KiwiCmd_Active()->NumericChanged( true, value );
        }
        return;
    }
    if ( command == "toolcommit" ) { KiwiCmd_Commit(); return; }
    if ( command == "toolcancel" ) { KiwiCmd_Cancel(); return; }
    if ( command == "prefab_probe" )
    {
        // Compare the new model-mode route with native recursive prefab picking
        // across the placed bounds, then check selected exclusion for drag tracing.
        selbrush_t *node = selected_brushes.next;
        if ( !node || node == &selected_brushes || node->next != &selected_brushes
          || !node->owner || !node->owner->prefab || !node->def )
        { ScriptError( line, "prefab_probe requires exactly one loaded selected prefab" ); return; }
        float lo[3], hi[3], angles[3], scale, origin[3];
        if ( !KiwiDrop_GetModelInfo( node, lo, hi, angles, &scale, origin ) )
        { ScriptError( line, "prefab bounds unavailable" ); return; }
        int hits = 0;
        for ( int x = 0; x < 17; ++x ) for ( int y = 0; y < 17; ++y )
        {
            float start[3] = { origin[0] + lo[0] + ( hi[0] - lo[0] ) * ( x + 0.5f ) / 17,
                               origin[1] + lo[1] + ( hi[1] - lo[1] ) * ( y + 0.5f ) / 17,
                               origin[2] + hi[2] + 128 };
            const float dir[3] = { 0, 0, -1 };
            edTrace_t native = {};
            if ( !KiwiSelect_TracePrefab( node, start, dir, &native ) ) continue;
            ++hits;
            selbrush_t *picked = nullptr;
            float distance;
            if ( !KiwiDrop_PickModelMesh( start, dir, 262144, false, &picked, &distance, nullptr )
              || picked != node || fabsf( distance - native.dist ) > 0.01f )
            { ScriptError( line, "prefab model-mode pick did not return its outer instance" ); return; }
            if ( KiwiDrop_PickModelMesh( start, dir, 262144, true, &picked, &distance, nullptr ) && picked == node )
            { ScriptError( line, "prefab drag trace hit its own selection" ); return; }
            ray_t ray;
            for ( int i = 0; i < 3; ++i ) { ray.origin[i] = start[i]; ray.dir[i] = dir[i]; }
            const bool wasModelsOnly = KiwiSel_ModelsOnly();
            KiwiSel_SetModelsOnly( true );
            const pick_result_t result = Pick( ray, SEL_MASK_OBJECT );
            KiwiSel_SetModelsOnly( wasModelsOnly );
            if ( !result.valid || result.item.brush != node )
            { ScriptError( line, "unified models-only picker lost the prefab instance" ); return; }
        }
        if ( !hits ) ScriptError( line, "prefab probe found no child geometry" );
        else LogFormat( "PREFAB", "%d recursive/model-mode picks agree", hits );
        return;
    }
    if ( command == "select" ) { ExecuteSelect( line ); return; }
    if ( command == "cmd" )
    {
        if ( w.size() != 2 ) { ScriptError( line, "cmd requires a registered name or #id" ); return; }
        unsigned int id = 0;
        if ( !DispatchNamedCommand( w[1], id ) )
        { ScriptError( line, "unknown command '%s'", w[1].c_str() ); return; }
        LogFormat( "COMMAND", "%s -> #%u", w[1].c_str(), id );
        if ( !Radiant_DispatchCommandDirect( id ) )
            ScriptError( line, "command #%u is registered but not dispatchable", id );
        return;
    }
    if ( command == "layer" )
    {
        // layer hide|show <name>: the layer dialog's hidden bit (Layers_SetFlag / ClearFlag 1).
        if ( w.size() != 3 || ( Lower( w[1] ) != "hide" && Lower( w[1] ) != "show" ) )
        { ScriptError( line, "layer syntax is: layer hide|show <name>" ); return; }
        if ( !Layers_Exists( w[2].c_str() ) )
        { ScriptError( line, "no layer '%s'", w[2].c_str() ); return; }
        if ( Lower( w[1] ) == "hide" )
            Layers_SetFlag( w[2].c_str(), 1 );
        else
            Layers_ClearFlag( w[2].c_str(), 1 );
        g_nUpdateBits = -1;
        return;
    }
    if ( command == "undo" || command == "redo" )
    {
        int count = 1;
        if ( w.size() > 2 || ( w.size() == 2 && ( !ParseInt( w[1], count ) || count < 0 ) ) )
        { ScriptError( line, "%s accepts one optional non-negative count", command.c_str() ); return; }
        for ( int i = 0; i < count; ++i )
            if ( command == "undo" ) Undo_Undo(); else Undo_Redo();
        g_nUpdateBits = -1;
        UpdateSelection( -1, nullptr );
        return;
    }
    if ( command == "move" )
    {
        float delta[3];
        if ( w.size() != 4 || !ParseFloat( w[1], delta[0] ) || !ParseFloat( w[2], delta[1] ) ||
             !ParseFloat( w[3], delta[2] ) )
        { ScriptError( line, "move requires dx dy dz" ); return; }
        if ( !HaveSelection() ) { ScriptError( line, "move requires a selection" ); return; }
        MoveSelection( delta );
        return;
    }
    if ( command == "rotate" )
    {
        float degrees = 0.0f;
        if ( w.size() != 3 || !ParseFloat( w[2], degrees ) )
        { ScriptError( line, "rotate syntax is: rotate x|y|z <degrees>" ); return; }
        const std::string axisWord = Lower( w[1] );
        const int axis = axisWord == "x" ? 0 : axisWord == "y" ? 1 : axisWord == "z" ? 2 : -1;
        if ( axis < 0 ) { ScriptError( line, "rotate axis must be x, y, or z" ); return; }
        if ( !HaveSelection() ) { ScriptError( line, "rotate requires a selection" ); return; }
        RotateSelection( axis, degrees );
        return;
    }
    if ( command == "delete" || command == "clone" )
    {
        if ( w.size() != 1 ) { ScriptError( line, "%s takes no arguments", command.c_str() ); return; }
        if ( !HaveSelection() ) { ScriptError( line, "%s requires a selection", command.c_str() ); return; }
        const unsigned int id = command == "delete" ? 33003u : 33001u;
        // The ported Clone handler performs the classic clone core but has no undo
        // bracket of its own.  Test mode supplies the creation bracket used by
        // KiwiDuplicate so clone/move/rotate is exactly three undo records.
        bool haveCloneableSource = false;
        if ( command == "clone" )
        {
            // Clone_Selection skips patches and fixed-size entity display bounds.
            for ( selbrush_t *node = selected_brushes.next;
                  node && node != &selected_brushes; node = node->next )
            {
                entity_s_def *owner = node->owner ? (entity_s_def *)node->owner->def : nullptr;
                if ( node->def && !node->patch && owner && owner->eclass && !owner->eclass->fixedsize )
                {
                    haveCloneableSource = true;
                    break;
                }
            }
            if ( haveCloneableSource )
            {
                Undo_ClearRedo();
                Undo_GeneralStart( "kiwitest clone" );
            }
        }
        const bool dispatched = Radiant_DispatchCommandDirect( id );
        // Modern-input Clone tails into a paused Move gesture.  The DSL owns
        // transforms explicitly, so leave the cloned selection intact but do
        // not carry that idle command (and its baselines) into later undo work.
        if ( command == "clone" && KiwiCmd_Active() )
            KiwiCmd_Cancel();
        if ( haveCloneableSource )
        {
            Undo_EndBrushList( &selected_brushes );
            Undo_End();
        }
        if ( !dispatched )
            ScriptError( line, "%s command #%u was not dispatchable", command.c_str(), id );
        return;
    }
    if ( command == "setkey" )
    {
        if ( w.size() != 3 ) { ScriptError( line, "setkey requires a key and value" ); return; }
        std::vector<entity_s *> defs;
        for ( selbrush_t *node = selected_brushes.next;
              node && node != &selected_brushes; node = node->next )
        {
            entity_s *def = node->owner ? node->owner->def : nullptr;
            if ( def && std::find( defs.begin(), defs.end(), def ) == defs.end() ) defs.push_back( def );
        }
        if ( defs.empty() ) { ScriptError( line, "setkey requires at least one selected entity" ); return; }
        Undo_ClearRedo();
        Undo_GeneralStart( "kiwitest setkey" );
        for ( entity_s *def : defs ) Undo_AddEntity_W( def );
        for ( entity_s *def : defs )
        {
            entity_s_def *entityDef = (entity_s_def *)def;
            SetKeyValue( entityDef, w[1].c_str(), w[2].c_str() );
            // Match the entity inspector's special origin commit so its cached
            // origin/version agree with the epair immediately.
            if ( _stricmp( w[1].c_str(), "origin" ) == 0 )
            {
                Entity_GetVec3ForKey( entityDef, entityDef->origin, "origin" );
                ++entityDef->version;
            }
        }
        Undo_End();
        MarkMapModified();
        g_nUpdateBits = -1;
        UpdateSelection( -1, nullptr );
        return;
    }
    if ( command == "texture" )
    {
        if ( w.size() != 2 ) { ScriptError( line, "texture requires one material name" ); return; }
        if ( !HaveSelection() ) { ScriptError( line, "texture requires a selection" ); return; }
        if ( !TexWnd_MakeMaterialCurrentByName( w[1].c_str() ) )
            ScriptError( line, "material is not registered: %s", w[1].c_str() );
        return;
    }
    if ( command == "thicken" )
    {
        int amount = 0, seam = 0;
        if ( w.size() != 3 || !ParseInt( w[1], amount ) || !ParseInt( w[2], seam ) ||
             ( seam != 0 && seam != 1 ) )
        { ScriptError( line, "thicken syntax is: thicken <amount> <0|1 seam>" ); return; }
        selbrush_t *only = selected_brushes.next;
        if ( only == &selected_brushes || only->next != &selected_brushes ||
             !only->def || !only->def->patch )
        { ScriptError( line, "thicken requires exactly one selected patch" ); return; }
        if ( ( only->def->patch->type & PATCH_TERRAIN ) != 0 )
        { ScriptError( line, "thicken does not support terrain patches" ); return; }
        // The registered ThickenPatch command is a deferred ImGui panel toggle.
        // Patch_Thicken is its UI-independent core and owns its legacy undo record.
        Patch_Thicken( amount, (char)seam );
        g_nUpdateBits = -1;
        return;
    }
    if ( command == "construct" )
    {
        // KIWI (2026-09-16): construct x y z x y z [x y z ...] [closed] - a world-space
        // line (2 points) or polyline, added through the construction undo and left
        // SELECTED so barbwire / offset / extrude verbs can act on it.
        // KIWI (2026-10-27): ... [smooth] - every span a spline span (the Line tool's S mode).
        bool closed = false, smooth = false;
        std::vector<std::string> nums;
        for ( size_t i = 1; i < w.size(); ++i )
        {
            if ( Lower( w[i] ) == "closed" ) closed = true;
            else if ( Lower( w[i] ) == "smooth" ) smooth = true;
            else nums.push_back( w[i] );
        }
        if ( nums.size() < 6 || nums.size() % 3 )
        { ScriptError( line, "construct needs at least two x y z triples" ); return; }
        kconObject_t o;
        o.type   = nums.size() == 6 ? KCON_LINE : KCON_POLYLINE;
        o.closed = closed && nums.size() >= 9;
        for ( size_t i = 0; i < nums.size(); ++i )
        {
            float v = 0.0f;
            if ( !ParseFloat( nums[i], v ) ) { ScriptError( line, "construct coordinate %d is not numeric", (int)i + 1 ); return; }
            o.pts.push_back( v );
        }
        if ( smooth )
            o.smooth.assign( o.pts.size() / 3, 1 );
        const int index = KiwiCon_AddWithUndo( o );
        if ( index < 0 ) { ScriptError( line, "the construction store rejected the object" ); return; }
        KiwiConSel_SelectObjects( &index, 1 );
        LogFormat( "CONSTRUCT", "added #%d (%d points%s), selected", index, (int)nums.size() / 3,
                   o.closed ? ", closed" : "" );
        g_nUpdateBits = -1;
        return;
    }
    if ( command == "construct_circle" )
    {
        // KIWI (2026-10-31): construct_circle x y z nx ny nz radius [ang0 ang1] - a circle (or,
        // with the two angles in degrees, an arc) centred at x y z in the plane of normal
        // nx ny nz, added through the construction undo (Trim's true-arc cases).
        if ( w.size() != 8 && w.size() != 10 )
        { ScriptError( line, "construct_circle x y z nx ny nz radius [ang0 ang1]" ); return; }
        float v[9] = { 0.0f };
        for ( size_t i = 1; i < w.size(); ++i )
            if ( !ParseFloat( w[i], v[i - 1] ) )
            { ScriptError( line, "construct_circle argument %d is not numeric", (int)i ); return; }
        kconObject_t o;
        if ( !KiwiCon_MakePlane( &v[0], &v[3], nullptr, &o.plane ) )
        { ScriptError( line, "construct_circle: degenerate normal" ); return; }
        const bool arc = w.size() == 10;
        o.type      = arc ? KCON_ARC : KCON_CIRCLE;
        o.closed    = !arc;
        o.centre[0] = 0.0f;
        o.centre[1] = 0.0f;
        o.radius    = v[6];
        if ( arc )
        {
            o.ang0 = v[7];
            o.ang1 = v[8];
        }
        const int index = KiwiCon_AddWithUndo( o );
        if ( index < 0 ) { ScriptError( line, "the construction store rejected the object" ); return; }
        LogFormat( "CONSTRUCT", "added #%d (%s, radius %g)", index, arc ? "arc" : "circle", (double)o.radius );
        g_nUpdateBits = -1;
        return;
    }
    if ( command == "barbwire" )
    {
        // KIWI (2026-09-16): barbwire [zoffset] [source-index] [strands] - lay along the
        // selected construction objects with the persisted dialog options (no dialog).
        kiwiBarbwireOpts_t opts;
        KiwiBarbwire_GetOpts( &opts );
        if ( w.size() > 4 ) { ScriptError( line, "barbwire [zoffset] [source-index] [strands]" ); return; }
        if ( w.size() >= 2 && !ParseFloat( w[1], opts.zOffset ) )
        { ScriptError( line, "barbwire zoffset is not numeric" ); return; }
        if ( w.size() >= 3 && ( !ParseInt( w[2], opts.source ) || opts.source < 0
                                || opts.source >= KiwiBarbwire_SourceCount() ) )
        { ScriptError( line, "barbwire source index is out of range" ); return; }
        if ( w.size() == 4 && ( !ParseInt( w[3], opts.strands ) || opts.strands < 0 || opts.strands > 16 ) )
        { ScriptError( line, "barbwire strands must be 0..16" ); return; }
        char err[256] = { 0 };
        if ( !KiwiBarbwire_LayNow( opts, err, sizeof( err ) ) )
            ScriptError( line, "barbwire failed: %s", err );
        return;
    }
    if ( command == "group" )
    {
        // group          - Group Selection: a NEW universal group (nested where it belongs)
        // group add      - Add Selection to Group: join the one group the selection reaches into
        // group select   - Select Whole Group: grow the selection one group level
        // group ungroup  - dissolve the group(s) the selection is directly in
        const std::string mode = line.words.size() > 1 ? Lower( line.words[1] ) : "";
        bool ok = false;
        if ( mode.empty() )            ok = KiwiOutliner_GroupSelection();
        else if ( mode == "add" )      ok = KiwiOutliner_AddSelectionToGroup();
        else if ( mode == "select" )   ok = KiwiOutliner_SelectWholeGroup();
        else if ( mode == "ungroup" )  ok = KiwiOutliner_UngroupSelection();
        else { ScriptError( line, "group [add|select|ungroup]" ); return; }
        if ( !ok )
            ScriptError( line, "group %s did nothing", mode.empty() ? "(selection)" : mode.c_str() );
        return;
    }
    if ( command == "refimage" ) { ExecuteRefImage( line ); return; }
    if ( command == "terrain" )  { ExecuteTerrain( line ); return; }
    if ( command == "editor_undo" || command == "editor_redo" )
    {
        // The editor's own Ctrl+Z / Ctrl+Y route (ID_EDIT_UNDO / ID_EDIT_REDO): the
        // unified journal first — construction, visibility, reference images — with
        // the classic brush stack as its fallback.  `undo` above stays the raw classic
        // stack so the older brush scripts keep their exact semantics.
        int count = 1;
        if ( w.size() > 2 || ( w.size() == 2 && ( !ParseInt( w[1], count ) || count < 0 ) ) )
        { ScriptError( line, "%s accepts one optional non-negative count", command.c_str() ); return; }
        const unsigned int id = command == "editor_undo" ? 57643u : 57644u;
        for ( int i = 0; i < count; ++i )
            Radiant_DispatchCommandDirect( id );
        g_nUpdateBits = -1;
        UpdateSelection( -1, nullptr );
        return;
    }
    if ( command == "expect" ) { ExecuteExpect( line ); return; }
    if ( command == "console" )
    {
        if ( w.size() != 2 || Lower( w[1] ) != "clear" )
        { ScriptError( line, "only 'console clear' is supported" ); return; }
        if ( !s_test.consolePending.empty() )
        {
            WriteOneLine( "CONSOLE", s_test.consolePending.data(), s_test.consolePending.size() );
            s_test.consolePending.clear();
        }
        s_test.consoleText.clear();
        LogFormat( "CONSOLE", "capture cleared" );
        return;
    }
    if ( command == "fail_on_console" )
    {
        if ( w.size() != 2 || w[1].empty() )
        { ScriptError( line, "fail_on_console requires a non-empty substring" ); return; }
        FailRule rule;
        rule.needle = w[1];
        s_test.failRules.push_back( rule );
        return;
    }
    if ( command == "quit" )
    {
        int code = 0;
        if ( w.size() > 2 || ( w.size() == 2 && ( !ParseInt( w[1], code ) || code < 0 || code > 255 ) ) )
        { ScriptError( line, "quit accepts one optional code from 0 through 255" ); return; }
        s_test.quitRequested = true;            // a remote session ends only here
        Finish( code );
        return;
    }
    ScriptError( line, "unknown command '%s'", w[0].c_str() );
}
} // namespace

namespace
{
static bool LoadScriptLines( const std::string &path, std::vector<ScriptLine> &out, std::string &error )
{
    std::ifstream input( path, std::ios::binary );
    if ( !input )
    {
        error = "could not open script " + path;
        return false;
    }
    std::string textLine;
    int lineNumber = 0;
    while ( std::getline( input, textLine ) )
    {
        ++lineNumber;
        if ( !textLine.empty() && textLine.back() == '\r' ) textLine.pop_back();
        if ( lineNumber == 1 && textLine.size() >= 3 &&
             (unsigned char)textLine[0] == 0xEF && (unsigned char)textLine[1] == 0xBB &&
             (unsigned char)textLine[2] == 0xBF )
            textLine.erase( 0, 3 );
        ScriptLine parsed;
        parsed.number = lineNumber;
        parsed.raw = textLine;
        std::string why;
        if ( !ParseWords( textLine, parsed.words, why ) )
        {
            char buf[64];
            _snprintf( buf, sizeof( buf ), "line %d: ", lineNumber );
            buf[sizeof( buf ) - 1] = '\0';
            error = buf + why;
            return false;
        }
        if ( !parsed.words.empty() )
            out.push_back( parsed );
    }
    return true;
}

// ── -kiwiremote transport ────────────────────────────────────────────────────
// One client at a time on a byte-mode named pipe in PIPE_NOWAIT mode, polled once per
// pump tick: no thread, and nothing ever blocks the editor.

static void RemoteDisconnect( const char *why )
{
    if ( s_test.pipe == INVALID_HANDLE_VALUE || !s_test.pipeConnected )
        return;
    DisconnectNamedPipe( s_test.pipe );
    s_test.pipeConnected = false;
    s_test.pipeIn.clear();
    LogFormat( "REMOTE", "client disconnected (%s)", why );
}

static void RemoteSend( const std::string &text )
{
    if ( !s_test.pipeConnected )
        return;
    size_t done = 0;
    const ULONGLONG giveUp = GetTickCount64() + 10000;
    while ( done < text.size() )
    {
        DWORD wrote = 0;
        const DWORD want = (DWORD)( text.size() - done < 32768 ? text.size() - done : 32768 );
        if ( !WriteFile( s_test.pipe, text.data() + done, want, &wrote, nullptr ) )
        {
            RemoteDisconnect( "write failed" );
            return;
        }
        done += wrote;
        if ( !wrote )
        {
            if ( GetTickCount64() > giveUp )
            {
                RemoteDisconnect( "client stopped reading" );
                return;
            }
            Sleep( 1 );                         // the client drains the pipe buffer
        }
    }
}

// The open command's reply: its captured log lines, then the verdict.
static void RemoteReply()
{
    if ( !s_test.replyOpen )
        return;
    s_test.replyOpen = false;
    s_test.capturing = false;
    if ( !s_test.consolePending.empty() )       // a console line without its newline yet
    {
        s_test.capture += "[CONSOLE] " + s_test.consolePending + "\n";
        WriteOneLine( "CONSOLE", s_test.consolePending.data(), s_test.consolePending.size() );
        s_test.consolePending.clear();
    }
    std::string reply;
    reply.swap( s_test.capture );
    reply += s_test.cmdFailed ? "@@END FAIL\n" : "@@END OK\n";
    RemoteSend( reply );
}

// Accept a client, read what it sent, queue every complete line.
static void RemotePoll()
{
    if ( s_test.pipe == INVALID_HANDLE_VALUE )
        return;
    if ( !s_test.pipeConnected )
    {
        if ( ConnectNamedPipe( s_test.pipe, nullptr ) )
            s_test.pipeConnected = true;
        else
        {
            const DWORD err = GetLastError();
            if ( err == ERROR_PIPE_CONNECTED )
                s_test.pipeConnected = true;
            else if ( err == ERROR_NO_DATA )
                DisconnectNamedPipe( s_test.pipe );     // a client came and went
        }
        if ( !s_test.pipeConnected )
            return;
        LogFormat( "REMOTE", "client connected" );
    }
    for ( ;; )
    {
        DWORD avail = 0;
        if ( !PeekNamedPipe( s_test.pipe, nullptr, 0, nullptr, &avail, nullptr ) )
        {
            RemoteDisconnect( "pipe closed" );
            return;
        }
        if ( !avail )
            break;
        char buf[4096];
        DWORD got = 0;
        if ( !ReadFile( s_test.pipe, buf, avail < sizeof( buf ) ? avail : (DWORD)sizeof( buf ), &got, nullptr ) || !got )
            break;
        s_test.pipeIn.append( buf, got );
    }
    size_t newline;
    while ( ( newline = s_test.pipeIn.find( '\n' ) ) != std::string::npos )
    {
        std::string text = s_test.pipeIn.substr( 0, newline );
        s_test.pipeIn.erase( 0, newline + 1 );
        if ( !text.empty() && text.back() == '\r' )
            text.pop_back();
        ScriptLine parsed;
        parsed.number = ++s_test.nextNumber;
        parsed.raw = text;
        std::string error;
        if ( !ParseWords( text, parsed.words, error ) )
            parsed.words = { "__parse_error", error };
        else if ( parsed.words.empty() )
            parsed.words = { "__noop" };       // every line sent gets a reply
        s_test.lines.push_back( parsed );
    }
}

// The remote branch of KiwiTest_Tick: a command runs, and its reply goes once its waits
// (frames / sleep_ms / a run batch) are over.
static void RemoteTick()
{
    RemotePoll();
    if ( s_test.waitFrames > 0 )
    {
        --s_test.waitFrames;
        return;
    }
    if ( s_test.sleepUntil )
    {
        if ( GetTickCount64() < s_test.sleepUntil )
            return;
        s_test.sleepUntil = 0;
    }
    if ( s_test.replyOpen && s_test.nextLine >= s_test.batchEnd )
        RemoteReply();
    if ( s_test.nextLine >= s_test.lines.size() )
    {
        if ( s_test.nextLine > 4096 )           // keep the queue from growing for ever
        {
            s_test.lines.clear();
            s_test.nextLine = 0;
            s_test.batchEnd = 0;
        }
        return;
    }
    const ScriptLine line = s_test.lines[s_test.nextLine++];
    if ( !s_test.replyOpen )
    {
        s_test.replyOpen  = true;
        s_test.cmdFailed  = false;
        s_test.capturing  = true;
        s_test.capture.clear();
    }
    LogFormat( "SCRIPT", "%d: %s", line.number, line.raw.c_str() );
    ExecuteLine( line );
    if ( s_test.finished )                      // `quit`: answer before WM_QUIT ends the pump
    {
        s_test.batchEnd = 0;
        RemoteReply();
    }
    else if ( s_test.waitFrames == 0 && !s_test.sleepUntil && s_test.nextLine >= s_test.batchEnd )
        RemoteReply();                          // nothing to wait for: answer this tick
}

// Test instances must never take the user's focus.  Every activation in this thread is
// refused; a dialog (a message box) is logged and cancelled so it can neither wait for a
// click nor sit on the user's screen.
HHOOK s_cbtHook = nullptr;

BOOL CALLBACK CollectDialogText( HWND child, LPARAM out )
{
    char text[256] = {};
    if ( GetWindowTextA( child, text, sizeof( text ) ) > 0 )
    {
        std::string &s = *(std::string *)out;
        if ( !s.empty() )
            s += " | ";
        s += text;
    }
    return TRUE;
}

LRESULT CALLBACK TestCbtProc( int code, WPARAM wParam, LPARAM lParam )
{
    if ( code == HCBT_ACTIVATE )
    {
        HWND hwnd = (HWND)wParam;
        char cls[64] = {};
        GetClassNameA( hwnd, cls, sizeof( cls ) );
        if ( strcmp( cls, "#32770" ) == 0 )
        {
            char caption[256] = {};
            GetWindowTextA( hwnd, caption, sizeof( caption ) );
            std::string text;
            EnumChildWindows( hwnd, CollectDialogText, (LPARAM)&text );
            // A Yes/No box has no Cancel and ignores IDCANCEL (the instance then sat in its
            // modal loop): answer with a button it has - Cancel, else No, else Ignore (an
            // Abort/Retry/Ignore assert box), else OK.
            const int answer = GetDlgItem( hwnd, IDCANCEL ) ? IDCANCEL : GetDlgItem( hwnd, IDNO ) ? IDNO
                             : GetDlgItem( hwnd, IDIGNORE ) ? IDIGNORE : IDOK;
            LogFormat( "DIALOG", "answered %s to \"%s\": %s",
                       answer == IDCANCEL ? "Cancel" : answer == IDNO ? "No" : answer == IDIGNORE ? "Ignore" : "OK",
                       caption, text.c_str() );
            PostMessageA( hwnd, WM_COMMAND, answer, 0 );
        }
        return 1;
    }
    return CallNextHookEx( s_cbtHook, code, wParam, lParam );
}

static bool OpenLogAndHandlers( const char *logPath, const char *defaultName )
{
    if ( logPath && logPath[0] )
        s_test.logPath = FullPath( logPath );
    else
    {
        char temp[MAX_PATH] = {};
        DWORD length = GetTempPathA( (DWORD)sizeof( temp ), temp );
        if ( !length || length >= sizeof( temp ) )
            strcpy( temp, ".\\" );
        s_test.logPath = std::string( temp ) + defaultName;
    }
    s_test.log = CreateFileA( s_test.logPath.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr );
    if ( s_test.log == INVALID_HANDLE_VALUE )
    {
        s_test.active = false;
        return false;
    }
    s_test.veh = AddVectoredExceptionHandler( 1, TestVectoredExceptionHandler );
    if ( !s_test.veh )
    {
        LogFormat( "SCRIPT-ERROR", "AddVectoredExceptionHandler failed with Win32 error %lu",
                   (unsigned long)GetLastError() );
        return false;
    }
    return true;
}
} // namespace

bool KiwiTest_InitRemote( const char *pipeName, const char *logPath )
{
    if ( s_test.active || !pipeName || !pipeName[0] )
        return false;
    s_test = TestState();
    s_test.active = true;
    s_test.remote = true;
    KiwiCmd_SetTestMods( 0 );               // the user's keyboard never reaches a test instance
    if ( !OpenLogAndHandlers( logPath, "kiwiremote.log" ) )
        return false;
    const std::string name = std::string( "\\\\.\\pipe\\" ) + pipeName;
    s_test.pipe = CreateNamedPipeA( name.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                                    1, 1u << 16, 1u << 16, 0, nullptr );
    if ( s_test.pipe == INVALID_HANDLE_VALUE )
    {
        LogFormat( "SCRIPT-ERROR", "CreateNamedPipe %s failed with Win32 error %lu", name.c_str(),
                   (unsigned long)GetLastError() );
        return false;
    }
    LogFormat( "START", "remote session on %s, log=%s", name.c_str(), s_test.logPath.c_str() );
    return true;
}

void KiwiTest_IsolateProfile()
{
    if ( !s_test.active )
        return;
    char exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA( nullptr, exe, sizeof( exe ) );
    if ( !n || n >= sizeof( exe ) )
        return;
    char *slash = strrchr( exe, '\\' );
    if ( !slash )
        return;
    slash[1] = '\0';
    const std::string dir = std::string( exe ) + "kiwi_cache";
    CreateDirectoryA( dir.c_str(), nullptr );
    const std::string sub = dir + "\\kiwitest";
    CreateDirectoryA( sub.c_str(), nullptr );
    const std::string ini = sub + "\\kiwi_radiant.ini";
    // The user's settings as they are now (grid, snapping, units); the dock layout starts
    // fresh because its file is not copied.
    CopyFileA( ( std::string( exe ) + "kiwi_radiant.ini" ).c_str(), ini.c_str(), FALSE );
    Radiant_SetIniPath( ini.c_str() );
    LogFormat( "START", "settings isolated in %s", ini.c_str() );
}

int KiwiTest_PrepareFrame( HWND frame, int nCmdShow, bool show )
{
    if ( !s_test.active || !frame || show )
        return nCmdShow;
    // A tool window (no taskbar button, no Alt+Tab) that refuses activation, parked past
    // the right edge of the whole desktop at the size it was created with: still "visible",
    // so the editor's frame keeps rendering and laying out its viewports.
    LONG_PTR ex = GetWindowLongPtrA( frame, GWL_EXSTYLE );
    ex = ( ex & ~(LONG_PTR)WS_EX_APPWINDOW ) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    SetWindowLongPtrA( frame, GWL_EXSTYLE, ex );
    RECT r;
    GetWindowRect( frame, &r );
    const int x = GetSystemMetrics( SM_XVIRTUALSCREEN ) + GetSystemMetrics( SM_CXVIRTUALSCREEN ) + 64;
    const int y = GetSystemMetrics( SM_YVIRTUALSCREEN );
    SetWindowPos( frame, HWND_BOTTOM, x, y, r.right - r.left, r.bottom - r.top,
                  SWP_NOACTIVATE | SWP_FRAMECHANGED );
    if ( !s_cbtHook )
        s_cbtHook = SetWindowsHookExA( WH_CBT, TestCbtProc, nullptr, GetCurrentThreadId() );
    LogFormat( "START", "frame parked at %d,%d (not activatable)", x, y );
    return SW_SHOWNOACTIVATE;
}

bool KiwiTest_Init( const char *scriptPath, const char *logPath, bool keepOpen, bool strict )
{
    if ( s_test.active )
        return false;
    s_test = TestState();
    s_test.active = true;
    s_test.strict = strict;
    s_test.keepOpen = keepOpen;
    KiwiCmd_SetTestMods( 0 );               // the user's keyboard never reaches a test instance

    if ( !OpenLogAndHandlers( logPath, "kiwitest.log" ) )
        return false;
    s_test.scriptPath = FullPath( scriptPath );
    LogFormat( "START", "script=%s", s_test.scriptPath.empty() ? "<missing>" : s_test.scriptPath.c_str() );
    LogFormat( "START", "log=%s strict=%d keep=%d", s_test.logPath.c_str(), strict ? 1 : 0,
               keepOpen ? 1 : 0 );
    if ( s_test.scriptPath.empty() )
    {
        LogFormat( "SCRIPT-ERROR", "-kiwitest requires a script path" );
        return false;
    }

    std::string error;
    if ( !LoadScriptLines( s_test.scriptPath, s_test.lines, error ) )
    {
        LogFormat( "SCRIPT-ERROR", "%s", error.c_str() );
        return false;
    }
    LogFormat( "START", "loaded %u executable script lines", (unsigned int)s_test.lines.size() );
    return true;
}

bool KiwiTest_Active()
{
    return s_test.active;
}

void KiwiTest_Tick()
{
    if ( !s_test.active || s_test.finished )
        return;
    if ( s_test.remote )
    {
        RemoteTick();
        return;
    }
    if ( s_test.stopAtNextTick )
    {
        Finish( s_test.result ? s_test.result : 1 );
        return;
    }
    if ( s_test.waitFrames > 0 )
    {
        --s_test.waitFrames;
        return;
    }
    if ( s_test.sleepUntil )
    {
        if ( GetTickCount64() < s_test.sleepUntil )
            return;
        s_test.sleepUntil = 0;
    }
    if ( s_test.nextLine >= s_test.lines.size() )
    {
        Finish( s_test.result );
        return;
    }
    const ScriptLine &line = s_test.lines[s_test.nextLine++];
    LogFormat( "SCRIPT", "line %d: %s", line.number, line.raw.c_str() );
    ExecuteLine( line );
}

void KiwiTest_ConsoleTap( const char *text )
{
    if ( !s_test.active || !text || !text[0] )
        return;
    const std::string chunk( text );
    s_test.consoleText += chunk;
    const size_t maxCapture = 8u * 1024u * 1024u;
    if ( s_test.consoleText.size() > maxCapture )
        s_test.consoleText.erase( 0, s_test.consoleText.size() - maxCapture );

    s_test.consolePending += chunk;
    size_t newline = std::string::npos;
    while ( ( newline = s_test.consolePending.find( '\n' ) ) != std::string::npos )
    {
        std::string complete = s_test.consolePending.substr( 0, newline );
        if ( !complete.empty() && complete.back() == '\r' ) complete.pop_back();
        WriteOneLine( "CONSOLE", complete.data(), complete.size() );
        s_test.consolePending.erase( 0, newline + 1 );
    }

    for ( FailRule &rule : s_test.failRules )
    {
        if ( rule.tripped ) continue;
        const std::string scan = rule.tail + chunk;
        if ( scan.find( rule.needle ) != std::string::npos )
        {
            rule.tripped = true;
            s_test.cmdFailed = true;
            if ( s_test.result == 0 ) s_test.result = 1;
            LogFormat( "EXPECT-FAIL", "console matched fail_on_console substring: %s",
                       rule.needle.c_str() );
            if ( s_test.strict ) s_test.stopAtNextTick = true;
        }
        const size_t keep = rule.needle.size() > 1 ? rule.needle.size() - 1 : 0;
        rule.tail = scan.size() > keep ? scan.substr( scan.size() - keep ) : scan;
    }
}

void KiwiTest_Log( const char *fmt, ... )
{
    if ( !s_test.active || !fmt ) return;
    va_list args;
    va_start( args, fmt );
    LogFormatV( "TEST", fmt, args );
    va_end( args );
}

void KiwiTest_LogChannel( const char *channel, const char *fmt, ... )
{
    if ( !s_test.active || !fmt ) return;
    va_list args;
    va_start( args, fmt );
    LogFormatV( channel, fmt, args );
    va_end( args );
}

void KiwiTest_WaitFrames( int frames )
{
    if ( s_test.active && frames > s_test.waitFrames )
        s_test.waitFrames = frames;
}

void KiwiTest_Fail( const char *message )
{
    if ( !s_test.active ) return;
    s_test.cmdFailed = true;
    if ( s_test.result == 0 ) s_test.result = 1;
    LogFormat( "FAIL", "%s", message ? message : "unspecified failure" );
    if ( s_test.strict ) s_test.stopAtNextTick = true;
}
