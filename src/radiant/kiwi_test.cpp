#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "kiwi_test.h"
#include "kiwi_command.h"
#include "radiant_frame.h"
#include "mainfrm.h"
#include "kiwi_refimage.h"     // refimage verbs + expectations (test mode)
#include "kiwi_terrain.h"      // terrain verb: tool / set / arm / stroke (test mode)
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
extern entity_s entities;                                                   // entity.cpp:298
extern int      modified;                                                   // map.cpp:67
extern int      g_nUpdateBits;                                               // engine_stubs.cpp:773

extern void Map_New();                                                       // entity.cpp:1839
extern void Map_SaveFile( const char *path, char region, char autosave );     // map.cpp:744
extern void Select_Deselect( int deselectFaces );                            // select.cpp:1448
extern void Select_Invert();                                                 // select.cpp:1196
extern void Select_Brush( selbrush_t *b, char wholeEntity, char status,
                          char center );                                     // select.cpp:904
extern void Select_Move( const float *delta, char snap );                    // select.cpp:2156
extern void Select_GetMid( float *mid );                                     // select.cpp:2213
extern void Select_RotateAxis( int axis, float deg, float (*matrix)[4][3] ); // select.cpp:2367
extern void Select_ApplyMatrix_SelectedBrushes( int snap, float *matrix,
                                                 float deg, char swap );      // select.cpp:2245
extern int  UpdateSelection( int wParam, eclass_t *cls );                    // win_ent.cpp:664

extern void Undo_ClearRedo();                                                // undo.cpp:176
extern void Undo_GeneralStart( const char *operation );                      // undo.cpp:367
extern void Undo_AddBrushList( selbrush_t *list );                           // undo.cpp:551
extern void Undo_EndBrushList( selbrush_t *list );                           // undo.cpp:576
extern void Undo_AddEntity_W( entity_s *def );                               // undo.cpp:633
extern void Undo_End();                                                      // undo.cpp:686
extern void Undo_Undo();                                                     // undo.cpp:736
extern void Undo_Redo();                                                     // undo.cpp:1019

extern void SetKeyValue( entity_s_def *ent, const char *key, const char *value ); // entity.cpp:213
extern int  Entity_GetVec3ForKey( entity_s_def *ent, float *out, const char *key ); // entity.cpp:100
extern void MarkMapModified();                                               // win_qe3.cpp:198
extern bool TexWnd_MakeMaterialCurrentByName( const char *name );             // texwnd.cpp:793
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *def );             // materialdef.cpp:159
extern void Patch_Thicken( int amount, char seam );                           // pmesh.cpp:7667
extern camera_s *Ed_Camera();                                                // camwnd.cpp:165
extern void CamWnd_BuildMatrix();                                            // camwnd.cpp:214

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
    return EXCEPTION_EXECUTE_HANDLER;
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

static bool SelectBrushOrdinal( int entityIndex, int brushIndex )
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
    if ( mode == "brush" )
    {
        int entityIndex = 0, brushIndex = 0;
        if ( w.size() != 4 || !ParseInt( w[2], entityIndex ) || !ParseInt( w[3], brushIndex ) ||
             !SelectBrushOrdinal( entityIndex, brushIndex ) )
            ScriptError( line, "no saved brush at entity %s ordinal %s",
                         w.size() > 2 ? w[2].c_str() : "?", w.size() > 3 ? w[3].c_str() : "?" );
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
        if ( w.size() != 3 ) { ScriptError( line, "terrain tool <raise|setheight|smooth|noise|texture|colour|grass|trim>" ); return; }
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
    ScriptError( line, "terrain verb must be tool, set, arm, or stroke" );
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

static void ExecuteLine( const ScriptLine &line )
{
    const std::vector<std::string> &w = line.words;
    if ( w.empty() ) return;
    const std::string command = Lower( w[0] );
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
    if ( command == "save" || command == "saveas" )
    {
        if ( w.size() != 2 ) { ScriptError( line, "%s requires one map path", command.c_str() ); return; }
        Map_SaveFile( w[1].c_str(), 0, 0 );
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
        Finish( code );
        return;
    }
    ScriptError( line, "unknown command '%s'", w[0].c_str() );
}
} // namespace

bool KiwiTest_Init( const char *scriptPath, const char *logPath, bool keepOpen, bool strict )
{
    if ( s_test.active )
        return false;
    s_test = TestState();
    s_test.active = true;
    s_test.strict = strict;
    s_test.keepOpen = keepOpen;

    if ( logPath && logPath[0] )
        s_test.logPath = FullPath( logPath );
    else
    {
        char temp[MAX_PATH] = {};
        DWORD length = GetTempPathA( (DWORD)sizeof( temp ), temp );
        if ( !length || length >= sizeof( temp ) )
            strcpy( temp, ".\\" );
        s_test.logPath = std::string( temp ) + "kiwitest.log";
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
    s_test.scriptPath = FullPath( scriptPath );
    LogFormat( "START", "script=%s", s_test.scriptPath.empty() ? "<missing>" : s_test.scriptPath.c_str() );
    LogFormat( "START", "log=%s strict=%d keep=%d", s_test.logPath.c_str(), strict ? 1 : 0,
               keepOpen ? 1 : 0 );
    if ( s_test.scriptPath.empty() )
    {
        LogFormat( "SCRIPT-ERROR", "-kiwitest requires a script path" );
        return false;
    }

    std::ifstream input( s_test.scriptPath, std::ios::binary );
    if ( !input )
    {
        LogFormat( "SCRIPT-ERROR", "could not open script" );
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
        std::string error;
        if ( !ParseWords( textLine, parsed.words, error ) )
        {
            LogFormat( "SCRIPT-ERROR", "line %d: %s", lineNumber, error.c_str() );
            return false;
        }
        if ( !parsed.words.empty() )
            s_test.lines.push_back( parsed );
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

void KiwiTest_Fail( const char *message )
{
    if ( !s_test.active ) return;
    if ( s_test.result == 0 ) s_test.result = 1;
    LogFormat( "FAIL", "%s", message ? message : "unspecified failure" );
    if ( s_test.strict ) s_test.stopAtNextTick = true;
}
