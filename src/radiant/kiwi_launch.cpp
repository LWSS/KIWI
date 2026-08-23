// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_launch.cpp — the Build dialog, child-process runner and BSP -> Light pipeline.
// ═════════════════════════════════════════════════════════════════════════════════════
// Read kiwi_launch.h first: it records why the compilers stay separate processes, why the
// target is always `raw\maps\mp\<name>.map`, the exact `-loadFrom` rule, why capture is
// frame-polled, where the `.errlog` / `.lin` handoff can disagree, and why cancellation
// warns about half-written lighting output.
// ═════════════════════════════════════════════════════════════════════════════════════
#include "stdafx.h"
#include "qe3.h"

#include <windows.h>
#include <imgui/imgui.h>

#include "kiwi_command.h"
#include "kiwi_launch.h"
#include "kiwi_matconvert.h"
#include "radiant_frame.h"      // Radiant_FileSave (:87) + Radiant_CurrentMapPath

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// ── ported / cross-file entry points (each verified against its definition) ─────────
extern int  Sys_Printf( const char *fmt, ... );          // win_qe3.cpp:118
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );  // mainfrm.cpp:1358
// The fs_basepath accessor every radiant file already uses (mainfrm.cpp:604, texwnd.cpp:1813,
// filters.cpp:1023), spelled exactly as they spell it.  Cited without a line number on purpose:
// TWO dvar.cpp exist in the tree (universal/ and cod4map/universal/), and a bare `dvar.cpp:NNN`
// resolves to neither.  Definition: universal/dvar.cpp, `Dvar_GetString` (declared in
// qcommon/qcommon.h beside Dvar_GetInt).
extern const char *Dvar_GetString( const char *dvarName );   // qcommon (fs_basepath)
// The loaded-map path global.  map.cpp line 44 -- `char currentmap[1024] = ""` (IDB 0x23F18D8);
// no line-number cite because nothing on that line is a checkable symbol.
extern char currentmap[];
extern void Pointfile_Errorfile_Public();                // errorfile.cpp:409
extern void Pointfile_Clear();                           // points.cpp:112  (frees the error-log entries)
extern void Pointfile_ResetPoints();                     // points.cpp:129  (s_num_points = 0)
extern int  Pointfile_GetNumPoints();                    // points.cpp:132
extern FILE *Pointfile_Check();                          // points.cpp:56   (reads <currentmap minus ext>.lin)
extern int  s_errLogCount;                               // points.cpp:50
extern int  g_nUpdateBits;                               // engine_stubs.cpp:773
// Radiant_FileSave()        radiant_frame.h:87 (mainfrm.cpp:1678) -- the Save funnel: with a path
//                           set it is Map_SaveFile(s_currentMapPath,0,0), else Save-As.
// Radiant_CurrentMapPath()  radiant_frame.h:92 (mainfrm.cpp:495) -- read-only view of
//                           s_currentMapPath (mainfrm.cpp:488), "" when untitled.

// ═════════════════════════════════════════════════════════════════════════════════════
//  STATE
// ═════════════════════════════════════════════════════════════════════════════════════
namespace
{

// ── the window ─────────────────────────────────────────────────────────────────────
bool s_open = false;

// ── options (persist for the session; nothing here goes in the ini) ────────────────
bool s_buildBsp    = true;
bool s_buildLight  = true;
bool s_saveFirst   = true;      // run the Save funnel before a BSP build
bool s_verbose     = true;      // cod4map -v
bool s_onlyEnts    = false;     // cod4map -onlyEnts
bool s_leakTest    = false;     // cod4map -leakTest

int  s_threads     = 0;         // cod4rad -Threads N  (0 = "not resolved yet")
int  s_quality     = 1;         // 0 = -Fast, 1 = (neither), 2 = -Extra
bool s_noRelight   = true;      // cod4rad -NoRelight

// ── the log ────────────────────────────────────────────────────────────────────────
// Bounded ring: append at the back, drop whole lines off the front past the cap.  256 KB
// is roughly 3000 lines of cod4map output, which is more than a full verbose compile.
const size_t kLogCap = 256u * 1024u;
std::string  s_log;
bool         s_logScrollPending = false;

// ── the running child ──────────────────────────────────────────────────────────────
enum launchStage_t
{
    LSTAGE_NONE = 0,
    LSTAGE_BSP,
    LSTAGE_LIGHT
};

// The chain's intent: either LIGHT follows a successful BSP, or the running process is the
// final selected stage.  Process creation and polling remain the same for both cases.
enum launchChain_t
{
    LCHAIN_OFF = 0,
    LCHAIN_LIGHT_AFTER_BSP,
    LCHAIN_FINAL_STAGE
};

launchStage_t s_stage = LSTAGE_NONE;
launchChain_t s_chain = LCHAIN_OFF;
HANDLE        s_proc  = nullptr;      // the child, while it runs
HANDLE        s_pipe  = nullptr;      // our READ end of its stdout+stderr
DWORD         s_pid   = 0;
bool          s_cancelRequest = false;  // the confirm popup is owed an OpenPopup
bool          s_cancelled = false;

enum buildStatus_t
{
    BSTATUS_IDLE = 0,
    BSTATUS_RUNNING,
    BSTATUS_SUCCEEDED,
    BSTATUS_FAILED
};

buildStatus_t s_buildStatus = BSTATUS_IDLE;
double        s_buildStartedAt = 0.0;
double        s_buildElapsed = 0.0;
char          s_buildOutputPath[MAX_PATH] = { 0 };
ULONGLONG     s_buildOutputBytes = 0;
launchStage_t s_failureStage = LSTAGE_NONE;
DWORD         s_failureExit = 0;
bool          s_failureHasExit = false;
std::string   s_failureDetail;

// Child output is already kept in the bounded visible log.  This small line accumulator is
// separate so the first ERROR:/assert line survives even if a very long build rolls the
// visible log's front off the ring.
std::string s_captureLine;
std::string s_firstCaptureError;

// The load source of the RUNNING (or last) BSP stage, kept so the post-run `.errlog` /
// `.lin` probe looks exactly where cod4map's BuildOutputPathFromLoadSource wrote them.
char s_bspLoadSource[MAX_PATH] = { 0 };

// The default thread count, resolved once and never zero at spawn time: the machine's
// processor count.  cod4rad clamps ITSELF to [1,4] when -Threads is absent
// (cmdline.c ParseCommandLine_Init), which is exactly why the dialog always passes the flag.
void EnsureThreadDefault()
{
    if ( s_threads > 0 )
        return;
    SYSTEM_INFO si;
    memset( &si, 0, sizeof( si ) );
    ::GetSystemInfo( &si );
    s_threads = (int)si.dwNumberOfProcessors;
    if ( s_threads < 1 )  s_threads = 1;
    if ( s_threads > 64 ) s_threads = 64;
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  LOG
// ═════════════════════════════════════════════════════════════════════════════════════
void LogAppend( const char *text, size_t len )
{
    if ( !text || !len )
        return;
    s_log.append( text, len );
    if ( s_log.size() > kLogCap )
    {
        // Drop from the front to the next line break so the top of the pane is never a
        // half line.  npos (one enormous line) degrades to a hard cut, which is correct.
        const size_t over = s_log.size() - kLogCap;
        const size_t nl   = s_log.find( '\n', over );
        s_log.erase( 0, nl == std::string::npos ? over : nl + 1 );
    }
    s_logScrollPending = true;
}

void LogLine( const char *fmt, ... )
{
    char buf[2048];
    va_list ap;
    va_start( ap, fmt );
    _vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );
    buf[sizeof( buf ) - 1] = '\0';
    LogAppend( buf, strlen( buf ) );
    LogAppend( "\n", 1 );
}

bool ContainsNoCase( const char *begin, const char *end, const char *needle )
{
    if ( !begin || !end || begin >= end || !needle || !needle[0] )
        return false;
    const size_t needleLen = strlen( needle );
    for ( const char *p = begin; p < end; ++p )
    {
        if ( (size_t)( end - p ) >= needleLen && _strnicmp( p, needle, needleLen ) == 0 )
            return true;
    }
    return false;
}

bool IsErrorText( const char *begin, const char *end )
{
    return ContainsNoCase( begin, end, "error:" ) ||
           ContainsNoCase( begin, end, "assert" ) ||
           ContainsNoCase( begin, end, "leak" );
}

bool IsWarningText( const char *begin, const char *end )
{
    return ContainsNoCase( begin, end, "warning" );
}

std::string TrimmedLine( const char *begin, const char *end )
{
    while ( begin < end && ( *begin == ' ' || *begin == '\t' || *begin == '\r' ) )
        ++begin;
    while ( end > begin && ( end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ) )
        --end;
    return std::string( begin, end );
}

void RememberCaptureLine()
{
    if ( s_firstCaptureError.empty() &&
         ( ContainsNoCase( s_captureLine.c_str(),
                           s_captureLine.c_str() + s_captureLine.size(), "error:" ) ||
           ContainsNoCase( s_captureLine.c_str(),
                           s_captureLine.c_str() + s_captureLine.size(), "assert" ) ) )
    {
        s_firstCaptureError = TrimmedLine( s_captureLine.c_str(),
                                           s_captureLine.c_str() + s_captureLine.size() );
    }
    s_captureLine.clear();
}

void CaptureAppend( const char *text, size_t len )
{
    for ( size_t i = 0; i < len; ++i )
    {
        const char c = text[i];
        if ( c == '\n' )
        {
            RememberCaptureLine();
            continue;
        }
        if ( c != '\r' && s_captureLine.size() < 16u * 1024u )
            s_captureLine.push_back( c );
    }
}

void FlushCaptureLine()
{
    if ( !s_captureLine.empty() )
        RememberCaptureLine();
}

void ResetStageCapture()
{
    s_captureLine.clear();
    s_firstCaptureError.clear();
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  PATHS  (D-BF-B / D-BF-C).  Resolved fresh on every draw — never cached across a map
//  change, because every one of these inputs moves when the user opens or saves a map.
// ═════════════════════════════════════════════════════════════════════════════════════
struct launchPaths_t
{
    char root[MAX_PATH];        // fs_basepath — the build root, and every child's CWD
    char binDir[MAX_PATH];      // the directory the running radiant exe sits in
    char mapPath[MAX_PATH];     // s_currentMapPath, "" when the map is untitled
    char name[128];             // basename of mapPath without the extension
    char targetRel[MAX_PATH];   // raw\maps\mp\<name>.map     (relative to root)
    char targetAbs[MAX_PATH];   // <root>\raw\maps\mp\<name>.map
    char radTargetRel[MAX_PATH];// raw\maps\mp\<name>          (cod4rad appends .d3dbsp)
    char bspAbs[MAX_PATH];      // <root>\raw\maps\mp\<name>.d3dbsp
    bool useLoadFrom;           // false only when mapPath IS targetAbs
    bool haveRoot;
    bool haveMap;
};

const char *ExeDir()
{
    static char s_dir[MAX_PATH];
    if ( !s_dir[0] )
    {
        if ( ::GetModuleFileNameA( nullptr, s_dir, sizeof( s_dir ) ) )
        {
            char *slash = strrchr( s_dir, '\\' );
            if ( slash )
                *slash = '\0';
        }
        else
            s_dir[0] = '\0';
    }
    return s_dir;
}

bool FileExists( const char *path )
{
    if ( !path || !path[0] )
        return false;
    const DWORD a = ::GetFileAttributesA( path );
    return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
}

void StripExt( char *path )
{
    char *dot   = strrchr( path, '.' );
    char *slash = strrchr( path, '\\' );
    char *fwd   = strrchr( path, '/' );
    if ( fwd > slash )
        slash = fwd;
    if ( dot && ( !slash || dot > slash ) )
        *dot = '\0';
}

// Full-path normalise + case-insensitive compare.  Used for the ONE decision `-loadFrom`
// turns on (D-BF-C) and for the `currentmap` agreement test (D-BF-E).
bool SamePath( const char *a, const char *b )
{
    if ( !a || !b || !a[0] || !b[0] )
        return false;
    char fa[MAX_PATH], fb[MAX_PATH];
    if ( !::GetFullPathNameA( a, sizeof( fa ), fa, nullptr ) )
        return false;
    if ( !::GetFullPathNameA( b, sizeof( fb ), fb, nullptr ) )
        return false;
    return _stricmp( fa, fb ) == 0;
}

void ResolvePaths( launchPaths_t &p )
{
    memset( &p, 0, sizeof( p ) );

    const char *base = Dvar_GetString( "fs_basepath" );
    if ( base && base[0] )
    {
        _snprintf( p.root, sizeof( p.root ), "%s", base );
        p.root[sizeof( p.root ) - 1] = '\0';
        size_t n = strlen( p.root );
        while ( n && ( p.root[n - 1] == '\\' || p.root[n - 1] == '/' ) )
            p.root[--n] = '\0';                 // no trailing separator: we append our own
        p.haveRoot = p.root[0] != '\0';
    }

    _snprintf( p.binDir, sizeof( p.binDir ), "%s", ExeDir() );
    p.binDir[sizeof( p.binDir ) - 1] = '\0';

    _snprintf( p.mapPath, sizeof( p.mapPath ), "%s", Radiant_CurrentMapPath() );
    p.mapPath[sizeof( p.mapPath ) - 1] = '\0';
    p.haveMap = p.mapPath[0] != '\0';
    if ( !p.haveMap )
        return;

    // NAME = basename minus extension.
    {
        const char *b = p.mapPath;
        for ( const char *c = p.mapPath; *c; ++c )
            if ( *c == '\\' || *c == '/' )
                b = c + 1;
        _snprintf( p.name, sizeof( p.name ), "%s", b );
        p.name[sizeof( p.name ) - 1] = '\0';
        StripExt( p.name );
    }

    _snprintf( p.targetRel, sizeof( p.targetRel ), "raw\\maps\\mp\\%s.map", p.name );
    _snprintf( p.radTargetRel, sizeof( p.radTargetRel ), "raw\\maps\\mp\\%s", p.name );
    if ( p.haveRoot )
    {
        _snprintf( p.targetAbs, sizeof( p.targetAbs ), "%s\\%s", p.root, p.targetRel );
        _snprintf( p.bspAbs, sizeof( p.bspAbs ), "%s\\raw\\maps\\mp\\%s.d3dbsp", p.root, p.name );
    }
    p.targetRel[sizeof( p.targetRel ) - 1]       = '\0';
    p.radTargetRel[sizeof( p.radTargetRel ) - 1] = '\0';
    p.targetAbs[sizeof( p.targetAbs ) - 1]       = '\0';
    p.bspAbs[sizeof( p.bspAbs ) - 1]             = '\0';

    // D-BF-C: bridge with -loadFrom unless the saved map IS the target file.
    p.useLoadFrom = !SamePath( p.mapPath, p.targetAbs );
}

// Both compiler exes sit beside the running radiant (one bin\<CONFIG> directory).
void ToolPath( const launchPaths_t &p, const char *exe, char *out, size_t outSz )
{
    _snprintf( out, outSz, "%s\\%s", p.binDir, exe );
    out[outSz - 1] = '\0';
}

// `<root>\raw\maps\mp` — created level by level.  ERROR_ALREADY_EXISTS is success.
bool EnsureTargetDir( const launchPaths_t &p )
{
    static const char *const kParts[] = { "raw", "raw\\maps", "raw\\maps\\mp" };
    for ( int i = 0; i < 3; ++i )
    {
        char dir[MAX_PATH];
        _snprintf( dir, sizeof( dir ), "%s\\%s", p.root, kParts[i] );
        dir[sizeof( dir ) - 1] = '\0';
        if ( ::CreateDirectoryA( dir, nullptr ) )
            continue;
        if ( ::GetLastError() == ERROR_ALREADY_EXISTS )
            continue;
        LogLine( "ERROR: could not create '%s' (GetLastError %lu)", dir, ::GetLastError() );
        return false;
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  THE PROCESS RUNNER (D-BF-D)
// ═════════════════════════════════════════════════════════════════════════════════════

// Drain whatever the pipe holds right now.  Never blocks: PeekNamedPipe gives the byte
// count first and ReadFile is asked for at most that many.  A broken pipe (the child exited
// and our write end is long closed) just ends the loop.
void ReadAvailable()
{
    if ( !s_pipe )
        return;
    for ( ;; )
    {
        DWORD avail = 0;
        if ( !::PeekNamedPipe( s_pipe, nullptr, 0, nullptr, &avail, nullptr ) )
            return;
        if ( !avail )
            return;
        char  buf[4096];
        DWORD want = avail > sizeof( buf ) ? (DWORD)sizeof( buf ) : avail;
        DWORD got  = 0;
        if ( !::ReadFile( s_pipe, buf, want, &got, nullptr ) || !got )
            return;
        CaptureAppend( buf, (size_t)got );
        LogAppend( buf, (size_t)got );
    }
}

void CloseChild()
{
    if ( s_pipe )
        ::CloseHandle( s_pipe );
    if ( s_proc )
        ::CloseHandle( s_proc );
    s_pipe = nullptr;
    s_proc = nullptr;
    s_pid  = 0;
}

const char *StageName( launchStage_t s )
{
    switch ( s )
    {
    case LSTAGE_BSP:   return "BSP";
    case LSTAGE_LIGHT: return "LIGHT";
    default:           return "BUILD";
    }
}

double CurrentBuildElapsed()
{
    if ( s_buildStatus == BSTATUS_RUNNING )
    {
        const double elapsed = ImGui::GetTime() - s_buildStartedAt;
        return elapsed > 0.0 ? elapsed : 0.0;
    }
    return s_buildElapsed;
}

void FormatElapsed( double elapsed, char *out, size_t outSz )
{
    const unsigned int totalSeconds = elapsed > 0.0 ? (unsigned int)elapsed : 0u;
    const unsigned int minutes = totalSeconds / 60u;
    const unsigned int seconds = totalSeconds % 60u;
    if ( minutes )
        _snprintf( out, outSz, "%um %02us", minutes, seconds );
    else
        _snprintf( out, outSz, "%us", seconds );
    out[outSz - 1] = '\0';
}

const char *FileNamePart( const char *path )
{
    const char *name = path ? path : "";
    if ( path )
    {
        for ( const char *p = path; *p; ++p )
            if ( *p == '\\' || *p == '/' )
                name = p + 1;
    }
    return name;
}

bool ReadOutputSize( const char *path, ULONGLONG &bytes )
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    memset( &data, 0, sizeof( data ) );
    if ( !path || !path[0] ||
         !::GetFileAttributesExA( path, GetFileExInfoStandard, &data ) ||
         ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) )
    {
        bytes = 0;
        return false;
    }
    bytes = ( (ULONGLONG)data.nFileSizeHigh << 32 ) | data.nFileSizeLow;
    return true;
}

void FormatFileSize( ULONGLONG bytes, char *out, size_t outSz )
{
    const ULONGLONG kb = 1024u;
    const ULONGLONG mb = kb * 1024u;
    const ULONGLONG gb = mb * 1024u;
    if ( bytes >= gb )
        _snprintf( out, outSz, "%.1f GB", (double)bytes / (double)gb );
    else if ( bytes >= mb )
        _snprintf( out, outSz, "%.1f MB", (double)bytes / (double)mb );
    else if ( bytes >= kb )
        _snprintf( out, outSz, "%.1f KB", (double)bytes / (double)kb );
    else
        _snprintf( out, outSz, "%I64u B", bytes );
    out[outSz - 1] = '\0';
}

void BeginBuildStatus( const launchPaths_t &p )
{
    s_buildStatus = BSTATUS_RUNNING;
    s_buildStartedAt = ImGui::GetTime();
    s_buildElapsed = 0.0;
    s_buildOutputBytes = 0;
    _snprintf( s_buildOutputPath, sizeof( s_buildOutputPath ), "%s", p.bspAbs );
    s_buildOutputPath[sizeof( s_buildOutputPath ) - 1] = '\0';
    s_failureStage = LSTAGE_NONE;
    s_failureExit = 0;
    s_failureHasExit = false;
    s_failureDetail.clear();
    ResetStageCapture();
    s_cancelled = false;
}

void FinishBuildFailure( launchStage_t stage, DWORD exitCode, bool haveExit,
                         const char *fallbackDetail )
{
    s_buildElapsed = CurrentBuildElapsed();
    s_buildStatus = BSTATUS_FAILED;
    s_failureStage = stage;
    s_failureExit = exitCode;
    s_failureHasExit = haveExit;
    s_chain = LCHAIN_OFF;

    if ( s_cancelled )
        s_failureDetail = "Cancelled by user.";
    else if ( !s_firstCaptureError.empty() )
        s_failureDetail = s_firstCaptureError;
    else if ( fallbackDetail && fallbackDetail[0] )
        s_failureDetail = fallbackDetail;
    else
        s_failureDetail = "No ERROR:/assert line was captured.";

    if ( s_failureDetail.size() > 240u )
    {
        s_failureDetail.resize( 237u );
        s_failureDetail += "...";
    }

    if ( haveExit )
    {
        LogLine( "ERROR: BUILD FAILED - %s exit %lu - %s",
                 StageName( stage ), exitCode, s_failureDetail.c_str() );
        Sys_Printf( "Build: FAILED - %s exit %lu - %s\n",
                    StageName( stage ), exitCode, s_failureDetail.c_str() );
    }
    else
    {
        LogLine( "ERROR: BUILD FAILED - %s - %s",
                 StageName( stage ), s_failureDetail.c_str() );
        Sys_Printf( "Build: FAILED - %s - %s\n",
                    StageName( stage ), s_failureDetail.c_str() );
    }
}

void FinishBuildSuccess( launchStage_t finalStage )
{
    ULONGLONG bytes = 0;
    if ( !ReadOutputSize( s_buildOutputPath, bytes ) )
    {
        char detail[MAX_PATH + 64];
        _snprintf( detail, sizeof( detail ), "Output file was not created: %s",
                   s_buildOutputPath );
        detail[sizeof( detail ) - 1] = '\0';
        FinishBuildFailure( finalStage, 0, true, detail );
        return;
    }
    if ( bytes == 0 )
    {
        char detail[MAX_PATH + 64];
        _snprintf( detail, sizeof( detail ), "Output file is empty: %s", s_buildOutputPath );
        detail[sizeof( detail ) - 1] = '\0';
        FinishBuildFailure( finalStage, 0, true, detail );
        return;
    }

    s_buildElapsed = CurrentBuildElapsed();
    s_buildOutputBytes = bytes;
    s_buildStatus = BSTATUS_SUCCEEDED;
    s_chain = LCHAIN_OFF;

    char elapsed[64], size[64];
    FormatElapsed( s_buildElapsed, elapsed, sizeof( elapsed ) );
    FormatFileSize( bytes, size, sizeof( size ) );
    LogLine( "BUILD SUCCEEDED - %s - %s (%s)", elapsed, s_buildOutputPath, size );
    Sys_Printf( "Build: SUCCEEDED in %s - %s (%s)\n", elapsed,
                s_buildOutputPath, size );
}

// Spawn a CAPTURED console child: stdout AND stderr into one anonymous pipe, no window,
// CWD = fs_basepath.  The write end is inherited and closed in the parent immediately —
// keeping it open here would mean the read end never sees EOF.  Both compilers run stdout
// unbuffered (cod4map bsp.cpp:1143 `setvbuf(stdout, NULL, _IONBF, 0)`), so the log streams
// live rather than arriving in one lump at exit.
bool SpawnCaptured( const char *cmdline, const char *cwd )
{
    SECURITY_ATTRIBUTES sa;
    memset( &sa, 0, sizeof( sa ) );
    sa.nLength              = sizeof( sa );
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle       = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if ( !::CreatePipe( &rd, &wr, &sa, 64 * 1024 ) )
    {
        LogLine( "ERROR: CreatePipe failed (GetLastError %lu)", ::GetLastError() );
        return false;
    }
    ::SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 );     // the READ end stays ours

    // A console child with STARTF_USESTDHANDLES and a NULL stdin gets an invalid handle;
    // neither compiler reads stdin, but NUL is the honest thing to hand it.
    HANDLE nul = ::CreateFileA( "NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, 0, nullptr );
    if ( nul == INVALID_HANDLE_VALUE )
        nul = nullptr;                      // never hand a child -1 as a std handle

    STARTUPINFOA si;
    memset( &si, 0, sizeof( si ) );
    si.cb         = sizeof( si );
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nul;
    si.hStdOutput = wr;
    si.hStdError  = wr;

    PROCESS_INFORMATION pi;
    memset( &pi, 0, sizeof( pi ) );

    std::string mutableCmd( cmdline );
    mutableCmd.push_back( '\0' );

    const BOOL ok = ::CreateProcessA( nullptr, &mutableCmd[0], nullptr, nullptr,
                                      TRUE, CREATE_NO_WINDOW, nullptr, cwd, &si, &pi );
    const DWORD err = ::GetLastError();

    ::CloseHandle( wr );                    // the parent's copy — see the note above
    if ( nul )
        ::CloseHandle( nul );

    if ( !ok )
    {
        ::CloseHandle( rd );
        LogLine( "ERROR: CreateProcess failed (GetLastError %lu)", err );
        return false;
    }
    ::CloseHandle( pi.hThread );

    s_proc = pi.hProcess;
    s_pipe = rd;
    s_pid  = pi.dwProcessId;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  THE .errlog / .lin HANDOFF (D-BF-E)
// ═════════════════════════════════════════════════════════════════════════════════════

// cod4map's own output base for the diagnostics: the -loadFrom path when one was passed,
// else the map argument (bsp.cpp:151-160 BuildOutputPathFromLoadSource).
void DiagBase( char *out, size_t outSz )
{
    _snprintf( out, outSz, "%s", s_bspLoadSource );
    out[outSz - 1] = '\0';
    StripExt( out );
}

// The two ported readers key off `currentmap`, not off the path we compiled (D-BF-E).
bool ReadersPointAtOurFiles()
{
    if ( !currentmap[0] || !s_bspLoadSource[0] )
        return false;
    return SamePath( currentmap, s_bspLoadSource );
}

// Delete the stale `.lin` and `.errlog` at BOTH candidate bases before a BSP run, so that
// "the file exists afterwards" means "THIS run produced it".  Two holes make this necessary
// and neither is ours to fix in the compiler:
//   * cod4map removes the stale `.lin` at the TARGET base (bsp.cpp:1216-1217) but WRITES the
//     new one at the LOAD-SOURCE base (leakfile.cpp:39/:154 via BuildOutputPathFromLoadSource).
//     With -loadFrom in play those are different files, so an old leak file next to the .map
//     survives forever and makes every later build look like it leaked;
//   * `Error_Init` truncates the `.errlog` (errors.cpp:11-15) but it is called late in main
//     (bsp.cpp:1229) — after the usage exit, after the platform gate and after FS_Startup —
//     so a run that dies before it leaves the PREVIOUS run's error log on disk.
void ClearStaleDiagFiles( const launchPaths_t &p )
{
    static const char *const kExts[] = { ".lin", ".errlog" };
    const char *const bases[2] = { p.mapPath, p.targetAbs };

    for ( int b = 0; b < 2; ++b )
    {
        if ( !bases[b] || !bases[b][0] )
            continue;
        char base[MAX_PATH];
        _snprintf( base, sizeof( base ), "%s", bases[b] );
        base[sizeof( base ) - 1] = '\0';
        StripExt( base );
        for ( int e = 0; e < 2; ++e )
        {
            char path[MAX_PATH];
            _snprintf( path, sizeof( path ), "%s%s", base, kExts[e] );
            path[sizeof( path ) - 1] = '\0';
            if ( FileExists( path ) && ::DeleteFileA( path ) )
                LogLine( "cleared stale %s", path );
        }
    }
}

// After a BSP stage: pull the leak path or the error log into the editor, exactly the way
// the File menu's own two handlers do it (mainfrm.cpp Cmd_OnPointfileOpen / Cmd_OnErrorFile).
//
// ONE LOADED AT A TIME.  Those ported handlers treat the pointfile and the error log as
// mutually exclusive displays — each clears the other before loading — and Misc->Next leak
// spot prefers the pointfile when both exist (mainfrm.cpp Cmd_OnMiscNextleakspot).  A leak
// is also the more actionable of the two, so a leak wins here.
//
// `Pointfile_Check` carries `iassert(s_num_points == 0)` (points.cpp:77), so the reset MUST
// come first — calling it over an already-loaded pointfile is an assert, not a reload.
void AfterBspDiagnostics( DWORD exitCode )
{
    char base[MAX_PATH];
    DiagBase( base, sizeof( base ) );
    if ( !base[0] )
        return;

    char lin[MAX_PATH], errlog[MAX_PATH];
    _snprintf( lin,    sizeof( lin ),    "%s.lin",    base );
    _snprintf( errlog, sizeof( errlog ), "%s.errlog", base );
    lin[sizeof( lin ) - 1]       = '\0';
    errlog[sizeof( errlog ) - 1] = '\0';

    const bool haveLin    = FileExists( lin );
    const bool haveErrlog = FileExists( errlog );

    if ( exitCode != 0 )
    {
        LogLine( "" );
        LogLine( "--- exit code legend ---" );
        LogLine( "  0            success" );
        LogLine( "  4294967295   (-1) usage / bad arguments -- see the USAGE block above" );
        LogLine( "  other        Com_Error; \"Not writing map due to errors\" means the .map" );
        LogLine( "               parsed but errors were logged (writebsp.cpp:807)" );
    }

    if ( !haveLin && !haveErrlog )
        return;

    if ( !ReadersPointAtOurFiles() )
    {
        // The editor's readers would look next to `currentmap`, which is not the file we
        // just compiled.  Say so with both paths rather than letting Pointfile_Errorfile
        // pop "Error log file was not found" at a path the user never named.
        LogLine( "" );
        if ( haveLin )
            LogLine( "LEAK -- pointfile written: %s", lin );
        if ( haveErrlog )
            LogLine( "errors written: %s", errlog );
        LogLine( "NOT loaded into the editor: the built-in reader keys off `currentmap`" );
        LogLine( "  currentmap      = %s", currentmap[0] ? currentmap : "(empty)" );
        LogLine( "  compiled source = %s", s_bspLoadSource );
        LogLine( "  (they diverge after Save-As and during prefab editing -- File>Open the" );
        LogLine( "   saved map to resync, then File>Pointfile / File>Error file)" );
        return;
    }

    if ( haveLin )
    {
        Pointfile_Clear();                  // drop any loaded error log (the ported pairing)
        Pointfile_ResetPoints();            // points.cpp:77 asserts this is zero
        Pointfile_Check();
        LogLine( "" );
        LogLine( "LEAK -- pointfile loaded (%d points) from %s",
                 Pointfile_GetNumPoints(), lin );
        g_nUpdateBits = W_ALL;              // repaint the views with the leak line on them
        Sys_Printf( "Build: LEAK -- pointfile loaded from %s\n", lin );
        return;
    }

    if ( haveErrlog )
    {
        Pointfile_ResetPoints();            // the ported Cmd_OnErrorFile head
        Pointfile_Clear();                  // reload rather than toggle
        Pointfile_Errorfile_Public();
        LogLine( "" );
        LogLine( "error log loaded (%d entries) from %s", s_errLogCount, errlog );
        g_nUpdateBits = W_ALL;
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  STAGES
// ═════════════════════════════════════════════════════════════════════════════════════
bool Busy() { return s_stage != LSTAGE_NONE; }

bool StartBsp( const launchPaths_t &p )
{
    ResetStageCapture();
    char exe[MAX_PATH];
    ToolPath( p, "KIWI-cod4map.exe", exe, sizeof( exe ) );
    if ( !FileExists( exe ) )
    {
        LogLine( "ERROR: %s is missing -- build the cod4map target.", exe );
        return false;
    }
    if ( !EnsureTargetDir( p ) )
        return false;

    if ( s_saveFirst )
    {
        // The same funnel Radiant_FileSave uses; with a path set (guaranteed here, the
        // button is disabled otherwise) that is Map_SaveFile(s_currentMapPath, 0, 0).
        Radiant_FileSave();
        LogLine( "saved %s", p.mapPath );
    }

    // The load source (D-BF-C) doubles as the diagnostics base (D-BF-E).
    _snprintf( s_bspLoadSource, sizeof( s_bspLoadSource ), "%s",
               p.useLoadFrom ? p.mapPath : p.targetAbs );
    s_bspLoadSource[sizeof( s_bspLoadSource ) - 1] = '\0';
    ClearStaleDiagFiles( p );

    std::string cmd;
    cmd += "\"";  cmd += exe;  cmd += "\"";
    cmd += " -platform pc";
    if ( s_verbose )  cmd += " -v";
    if ( s_onlyEnts ) cmd += " -onlyEnts";
    if ( s_leakTest ) cmd += " -leakTest";
    if ( p.useLoadFrom )
    {
        cmd += " -loadFrom \"";
        cmd += p.mapPath;
        cmd += "\"";
    }
    // The MAP NAME MUST BE LAST (bsp.cpp:1195 parses options over argv[1..argc-2],
    // :1210 takes argv[argc-1] as the output base).
    cmd += " \"";
    cmd += p.targetRel;
    cmd += "\"";

    LogLine( "" );
    LogLine( "==== BSP ====" );
    LogLine( "cwd: %s", p.root );
    LogLine( "%s", cmd.c_str() );
    LogLine( "" );

    if ( !SpawnCaptured( cmd.c_str(), p.root ) )
        return false;
    s_stage = LSTAGE_BSP;
    return true;
}

bool StartLight( const launchPaths_t &p )
{
    ResetStageCapture();
    EnsureThreadDefault();
    char exe[MAX_PATH];
    ToolPath( p, "KIWI-cod4rad.exe", exe, sizeof( exe ) );
    if ( !FileExists( exe ) )
    {
        LogLine( "ERROR: %s is missing -- build the cod4rad target (x64).", exe );
        return false;
    }
    if ( !FileExists( p.bspAbs ) )
    {
        LogLine( "ERROR: %s does not exist -- run Build BSP first.", p.bspAbs );
        return false;
    }

    std::string cmd;
    cmd += "\"";  cmd += exe;  cmd += "\"";
    cmd += " -Platform pc";
    {
        char t[64];
        _snprintf( t, sizeof( t ), " -Threads %d", s_threads );
        t[sizeof( t ) - 1] = '\0';
        cmd += t;
    }
    if ( s_quality == 0 ) cmd += " -Fast";
    if ( s_quality == 2 ) cmd += " -Extra";
    if ( s_noRelight )    cmd += " -NoRelight";
    // Map name LAST, extension-less: cod4rad strips any extension and appends .d3dbsp
    // itself (cmdline.c:755 SetBspFileExtensions("d3d"), :794-814).
    cmd += " \"";
    cmd += p.radTargetRel;
    cmd += "\"";

    LogLine( "" );
    LogLine( "==== LIGHT ====" );
    LogLine( "cwd: %s", p.root );
    LogLine( "%s", cmd.c_str() );
    LogLine( "" );

    if ( !SpawnCaptured( cmd.c_str(), p.root ) )
        return false;
    s_stage = LSTAGE_LIGHT;
    return true;
}

// Called from the poll the frame a stage's process is seen to have exited.
void OnStageFinished( DWORD exitCode )
{
    const launchStage_t finished = s_stage;
    s_stage = LSTAGE_NONE;

    LogLine( "" );
    LogLine( "---- %s exited with code %lu (0x%08lX) ----",
             StageName( finished ), exitCode, exitCode );

    if ( finished == LSTAGE_BSP )
        AfterBspDiagnostics( exitCode );

    if ( s_cancelled )
    {
        FinishBuildFailure( finished, exitCode, true, "Cancelled by user." );
        return;
    }

    if ( s_chain == LCHAIN_OFF )
        return;

    if ( exitCode != 0 )
    {
        LogLine( "build STOPPED: %s failed.", StageName( finished ) );
        FinishBuildFailure( finished, exitCode, true, nullptr );
        return;
    }

    if ( s_chain == LCHAIN_FINAL_STAGE )
    {
        FinishBuildSuccess( finished );
        return;
    }

    launchPaths_t p;
    ResolvePaths( p );
    if ( !p.haveRoot || !p.haveMap || !SamePath( p.bspAbs, s_buildOutputPath ) )
    {
        LogLine( "ERROR: build stopped because the map path changed during BSP." );
        FinishBuildFailure( LSTAGE_LIGHT, 0, false,
                            "The map path changed before LIGHT could start." );
        return;
    }

    if ( s_chain == LCHAIN_LIGHT_AFTER_BSP )
    {
        if ( StartLight( p ) )
            s_chain = LCHAIN_FINAL_STAGE;
        else
        {
            LogLine( "build STOPPED: could not start LIGHT." );
            FinishBuildFailure( LSTAGE_LIGHT, 0, false,
                                "The LIGHT stage could not be started." );
        }
        return;
    }
}

// ONE poll per frame.  STILL_ACTIVE is 259; neither compiler returns it (cod4map exits 0 or
// -1 or through Com_Error, cod4rad returns 0/1), so the classic ambiguity does not bite.
void PollChild()
{
    if ( s_stage == LSTAGE_NONE || !s_proc )
        return;

    ReadAvailable();

    DWORD code = STILL_ACTIVE;
    if ( !::GetExitCodeProcess( s_proc, &code ) )
    {
        const DWORD err = ::GetLastError();
        LogLine( "ERROR: GetExitCodeProcess failed (GetLastError %lu) -- dropping the child.",
                 err );
        const launchStage_t failed = s_stage;
        FlushCaptureLine();
        CloseChild();
        s_stage = LSTAGE_NONE;
        char detail[128];
        _snprintf( detail, sizeof( detail ),
                   "GetExitCodeProcess failed (GetLastError %lu).", err );
        detail[sizeof( detail ) - 1] = '\0';
        FinishBuildFailure( failed, 0, false, detail );
        return;
    }
    if ( code == STILL_ACTIVE )
        return;

    ReadAvailable();          // whatever it wrote between the last drain and its exit
    FlushCaptureLine();       // preserve a final ERROR/assert line without a trailing newline
    CloseChild();
    OnStageFinished( code );
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  THE WINDOW
// ═════════════════════════════════════════════════════════════════════════════════════
void HelpMarker( const char *text )
{
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos( ImGui::GetFontSize() * 32.0f );
        ImGui::TextUnformatted( text );
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DrawSpinner( float radius, float thickness, ImU32 color )
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 centre( pos.x + radius, pos.y + radius );
    ImGui::Dummy( ImVec2( radius * 2.0f, radius * 2.0f ) );

    const float start = (float)ImGui::GetTime() * 5.0f;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->PathClear();
    draw->PathArcTo( centre, radius, start, start + 4.8f, 24 );
    draw->PathStroke( color, thickness, ImDrawFlags_None );
}

void DrawStatusBanner()
{
    ImVec4 background( 0.12f, 0.13f, 0.15f, 1.0f );
    ImVec4 border( 0.35f, 0.37f, 0.40f, 1.0f );
    ImVec4 primary( 0.78f, 0.80f, 0.84f, 1.0f );
    ImVec4 secondary( 0.66f, 0.68f, 0.72f, 1.0f );
    char headline[1024];
    char detail[1024];
    headline[0] = '\0';
    detail[0] = '\0';

    char elapsed[64];
    FormatElapsed( CurrentBuildElapsed(), elapsed, sizeof( elapsed ) );

    switch ( s_buildStatus )
    {
    case BSTATUS_RUNNING:
        background = ImVec4( 0.30f, 0.23f, 0.04f, 1.0f );
        border = ImVec4( 0.95f, 0.72f, 0.16f, 1.0f );
        primary = ImVec4( 1.0f, 0.82f, 0.28f, 1.0f );
        secondary = ImVec4( 0.95f, 0.84f, 0.52f, 1.0f );
        _snprintf( headline, sizeof( headline ), "Running %s\xE2\x80\xA6  %s",
                   StageName( s_stage ), elapsed );
        _snprintf( detail, sizeof( detail ), "Output: %s", s_buildOutputPath );
        break;

    case BSTATUS_SUCCEEDED:
    {
        background = ImVec4( 0.06f, 0.25f, 0.13f, 1.0f );
        border = ImVec4( 0.20f, 0.82f, 0.43f, 1.0f );
        primary = ImVec4( 0.43f, 1.0f, 0.62f, 1.0f );
        secondary = ImVec4( 0.66f, 0.92f, 0.74f, 1.0f );
        char size[64];
        FormatFileSize( s_buildOutputBytes, size, sizeof( size ) );
        _snprintf( headline, sizeof( headline ),
                   "BUILD SUCCEEDED \xC2\xB7 %s \xC2\xB7 %s %s", elapsed,
                   FileNamePart( s_buildOutputPath ), size );
        _snprintf( detail, sizeof( detail ), "%s", s_buildOutputPath );
        break;
    }

    case BSTATUS_FAILED:
        background = ImVec4( 0.31f, 0.07f, 0.08f, 1.0f );
        border = ImVec4( 0.94f, 0.28f, 0.30f, 1.0f );
        primary = ImVec4( 1.0f, 0.48f, 0.48f, 1.0f );
        secondary = ImVec4( 1.0f, 0.68f, 0.52f, 1.0f );
        if ( s_failureHasExit )
            _snprintf( headline, sizeof( headline ),
                       "BUILD FAILED \xE2\x80\x94 %s exit %lu",
                       StageName( s_failureStage ), s_failureExit );
        else
            _snprintf( headline, sizeof( headline ),
                       "BUILD FAILED \xE2\x80\x94 %s could not start",
                       StageName( s_failureStage ) );
        _snprintf( detail, sizeof( detail ), "%s", s_failureDetail.c_str() );
        break;

    default:
        _snprintf( headline, sizeof( headline ), "Ready to build" );
        _snprintf( detail, sizeof( detail ), "Select BSP and/or LIGHT, then press Build." );
        break;
    }
    headline[sizeof( headline ) - 1] = '\0';
    detail[sizeof( detail ) - 1] = '\0';

    ImGui::PushStyleColor( ImGuiCol_ChildBg, background );
    ImGui::PushStyleColor( ImGuiCol_Border, border );
    if ( ImGui::BeginChild( "##buildstatus", ImVec2( 0.0f, 82.0f ),
                            ImGuiChildFlags_Borders,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse ) )
    {
        if ( s_buildStatus == BSTATUS_RUNNING )
        {
            DrawSpinner( 9.0f, 2.5f, ImGui::ColorConvertFloat4ToU32( primary ) );
            ImGui::SameLine( 0.0f, 10.0f );
        }

        float headingSize = ImGui::GetFont()->LegacySize;
        if ( headingSize <= 0.0f )
            headingSize = ImGui::GetStyle().FontSizeBase;
        if ( headingSize <= 0.0f )
            headingSize = 13.0f;
        ImGui::PushFont( nullptr, headingSize * 1.22f );
        ImGui::TextColored( primary, "%s", headline );
        ImGui::PopFont();
        ImGui::PushStyleColor( ImGuiCol_Text, secondary );
        ImGui::TextWrapped( "%s", detail );
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor( 2 );
}

void DrawStageRows( bool busy )
{
    static const char *const kQuality[] = { "Fast", "Normal", "Extra" };

    ImGui::BeginDisabled( busy );

    ImGui::PushID( "bsp_stage" );
    ImGui::Checkbox( "##enabled", &s_buildBsp );
    ImGui::SameLine();
    ImGui::TextUnformatted( "BSP" );
    ImGui::SameLine( 105.0f );
    ImGui::BeginDisabled( !s_buildBsp );
    ImGui::Checkbox( "Verbose (-v)", &s_verbose );
    ImGui::SameLine();
    ImGui::Checkbox( "Entities only (-onlyEnts)", &s_onlyEnts );
    ImGui::SameLine();
    ImGui::Checkbox( "Leak test (-leakTest)", &s_leakTest );
    ImGui::EndDisabled();
    ImGui::PopID();

    ImGui::PushID( "light_stage" );
    ImGui::Checkbox( "##enabled", &s_buildLight );
    ImGui::SameLine();
    ImGui::TextUnformatted( "LIGHT" );
    ImGui::SameLine( 105.0f );
    ImGui::BeginDisabled( !s_buildLight );
    ImGui::TextUnformatted( "Threads" );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 74.0f );
    if ( ImGui::InputInt( "##threads", &s_threads, 1, 4 ) )
    {
        if ( s_threads < 1 )  s_threads = 1;
        if ( s_threads > 64 ) s_threads = 64;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted( "Quality" );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 92.0f );
    ImGui::Combo( "##quality", &s_quality, kQuality, IM_ARRAYSIZE( kQuality ) );
    ImGui::SameLine();
    ImGui::Checkbox( "No relight (-NoRelight)", &s_noRelight );
    ImGui::EndDisabled();
    ImGui::PopID();

    ImGui::BeginDisabled( !s_buildBsp );
    ImGui::Checkbox( "Save map before BSP", &s_saveFirst );
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void StartSelectedBuild( const launchPaths_t &p )
{
    BeginBuildStatus( p );
    s_chain = LCHAIN_OFF;

    LogLine( "" );
    LogLine( "================ BUILD ================" );

    if ( s_buildBsp )
    {
        if ( StartBsp( p ) )
            s_chain = s_buildLight ? LCHAIN_LIGHT_AFTER_BSP : LCHAIN_FINAL_STAGE;
        else
            FinishBuildFailure( LSTAGE_BSP, 0, false,
                                "The BSP stage could not be started." );
        return;
    }

    if ( StartLight( p ) )
        s_chain = LCHAIN_FINAL_STAGE;
    else
        FinishBuildFailure( LSTAGE_LIGHT, 0, false,
                            "The LIGHT stage could not be started." );
}

void DrawCancelPopup()
{
    if ( s_cancelRequest )
    {
        ImGui::OpenPopup( "Cancel build?" );
        s_cancelRequest = false;
    }
    if ( !ImGui::BeginPopupModal( "Cancel build?", nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize ) )
        return;

    ImGui::TextUnformatted( "Terminate the running compiler?" );
    ImGui::Separator();
    ImGui::TextWrapped(
        "A terminated BSP may leave a partial .d3dbsp. LIGHT rewrites the same file in "
        "place, so cancelling it can leave the output half-lit; rebuild BSP before using it." );
    ImGui::Separator();
    if ( ImGui::Button( "Cancel build", ImVec2( 120.0f, 0.0f ) ) )
    {
        if ( s_proc )
        {
            LogLine( "" );
            LogLine( "---- cancel requested: TerminateProcess on %s (pid %lu) ----",
                     StageName( s_stage ), s_pid );
            if ( ::TerminateProcess( s_proc, 1 ) )
            {
                s_cancelled = true;                 // the poll records the exit next frame
                s_chain = LCHAIN_OFF;               // never start another stage after cancel
            }
            else
                LogLine( "ERROR: TerminateProcess failed (GetLastError %lu)",
                         ::GetLastError() );
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if ( ImGui::Button( "Keep building", ImVec2( 120.0f, 0.0f ) ) )
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawLog()
{
    if ( ImGui::Button( "Copy log" ) )
        ImGui::SetClipboardText( s_log.c_str() );
    ImGui::SameLine();
    if ( ImGui::Button( "Clear" ) )
    {
        s_log.clear();
        s_logScrollPending = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "%d KB", (int)( s_log.size() / 1024u ) );

    if ( ImGui::BeginChild( "##buildlog", ImVec2( 0.0f, 0.0f ), ImGuiChildFlags_Borders,
                            ImGuiWindowFlags_HorizontalScrollbar ) )
    {
        const bool followTail = s_logScrollPending &&
                                ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;

        std::vector<size_t> lineStarts;
        if ( !s_log.empty() )
        {
            lineStarts.push_back( 0 );
            for ( size_t i = 0; i + 1 < s_log.size(); ++i )
                if ( s_log[i] == '\n' )
                    lineStarts.push_back( i + 1 );
        }

        // The app uses ImGui's embedded Proggy font as Fonts[0], so selecting it explicitly
        // keeps compiler columns monospaced even if a future panel temporarily changes fonts.
        ImFont *mono = ImGui::GetIO().Fonts->Fonts.Size > 0
                     ? ImGui::GetIO().Fonts->Fonts[0] : nullptr;
        if ( mono )
            ImGui::PushFont( mono, 0.0f );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 4.0f, 1.0f ) );

        ImGuiListClipper clipper;
        clipper.Begin( (int)lineStarts.size() );
        while ( clipper.Step() )
        {
            for ( int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line )
            {
                const size_t start = lineStarts[(size_t)line];
                size_t end = (size_t)line + 1u < lineStarts.size()
                           ? lineStarts[(size_t)line + 1u] - 1u : s_log.size();
                if ( end > start && s_log[end - 1] == '\r' )
                    --end;

                const char *beginText = s_log.c_str() + start;
                const char *endText = s_log.c_str() + end;
                if ( IsErrorText( beginText, endText ) )
                    ImGui::PushStyleColor( ImGuiCol_Text,
                                           ImVec4( 1.0f, 0.38f, 0.38f, 1.0f ) );
                else if ( IsWarningText( beginText, endText ) )
                    ImGui::PushStyleColor( ImGuiCol_Text,
                                           ImVec4( 1.0f, 0.78f, 0.24f, 1.0f ) );
                else
                    ImGui::PushStyleColor( ImGuiCol_Text,
                                           ImVec4( 0.86f, 0.87f, 0.89f, 1.0f ) );
                ImGui::TextUnformatted( beginText, endText );
                ImGui::PopStyleColor();
            }
        }

        ImGui::PopStyleVar();
        if ( mono )
            ImGui::PopFont();

        if ( followTail )
            ImGui::SetScrollHereY( 1.0f );
        s_logScrollPending = false;
    }
    ImGui::EndChild();
}

void DrawWindow()
{
    EnsureThreadDefault();
    ImGui::SetNextWindowSize( ImVec2( 780.0f, 620.0f ), ImGuiCond_FirstUseEver );
    if ( !ImGui::Begin( "Build", &s_open, ImGuiWindowFlags_NoDocking ) )
    {
        ImGui::End();
        return;
    }

    launchPaths_t p;
    ResolvePaths( p );

    DrawStatusBanner();

    if ( !p.haveRoot )
        ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ),
                            "fs_basepath is empty -- the build root could not be resolved." );

    if ( !p.haveMap )
        ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.25f, 1.0f ),
                            "Map is untitled -- save it first (File > Save As)." );
    else
    {
        ImGui::Text( "Map: %s", p.mapPath );
        ImGui::TextDisabled( "Output: %s", p.bspAbs );
        if ( p.useLoadFrom )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "(-loadFrom)" );
            HelpMarker( "cod4map reads the saved map through -loadFrom while writing the "
                        "compiled output under raw\\maps\\mp. The flag is omitted only when "
                        "the saved map already is that exact target path." );
        }
    }

    KiwiMatConvert_DrawMapHealth();

    const bool canBuild = p.haveRoot && p.haveMap;
    const bool busy     = Busy();

    ImGui::Separator();
    DrawStageRows( busy );

    char bspExe[MAX_PATH], lightExe[MAX_PATH];
    ToolPath( p, "KIWI-cod4map.exe", bspExe, sizeof( bspExe ) );
    ToolPath( p, "KIWI-cod4rad.exe", lightExe, sizeof( lightExe ) );
    const bool haveBspExe = FileExists( bspExe );
    const bool haveLightExe = FileExists( lightExe );
    const char *blockedReason = nullptr;
    if ( !canBuild )
        blockedReason = "A saved map and fs_basepath are required.";
    else if ( !s_buildBsp && !s_buildLight )
        blockedReason = "Select at least one build stage.";
    else if ( s_buildBsp && !haveBspExe )
        blockedReason = "KIWI-cod4map.exe was not found beside Radiant.";
    else if ( s_buildLight && !haveLightExe )
        blockedReason = "KIWI-cod4rad.exe was not found beside Radiant.";
    else if ( !s_buildBsp && s_buildLight && !FileExists( p.bspAbs ) )
        blockedReason = "LIGHT needs an existing .d3dbsp; enable BSP first.";

    if ( busy )
    {
        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.55f, 0.12f, 0.13f, 1.0f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.72f, 0.18f, 0.20f, 1.0f ) );
        if ( ImGui::Button( "Cancel", ImVec2( -1.0f, 36.0f ) ) )
            s_cancelRequest = true;
        ImGui::PopStyleColor( 2 );
    }
    else
    {
        ImGui::BeginDisabled( blockedReason != nullptr );
        if ( ImGui::Button( "Build", ImVec2( -1.0f, 36.0f ) ) )
            StartSelectedBuild( p );
        ImGui::EndDisabled();
    }

    if ( !busy && blockedReason )
        ImGui::TextColored( ImVec4( 1.0f, 0.62f, 0.30f, 1.0f ), "%s", blockedReason );

    DrawCancelPopup();

    ImGui::Separator();
    DrawLog();

    ImGui::End();
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════════════
//  COMMANDS + THE PER-FRAME ENTRY POINT
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiLaunch_RegisterCommands()
{
    // Keep the established command name and id so existing radiant.ini bindings continue
    // to resolve. F9 remains the compiled-in default in both keymap profiles.
    Radiant_RegisterCommand( "KiwiBuildAndRun", 0x78, 0, KIWI_CMD_BUILD_RUN );
}

// The native menu bar is this shell's top-bar command surface. Appending preserves every
// index-based popup consumer, and the command system owns any accelerator annotation.
void KiwiLaunch_BuildMenu( void *frameMenu )
{
    HMENU hMenu = (HMENU)frameMenu;
    if ( !hMenu )
        return;
    ::AppendMenuA( hMenu, MF_STRING, (UINT_PTR)KIWI_CMD_BUILD_RUN, "&Build" );
    ::DrawMenuBar( g_qeglobals.d_hwndMain );
}

bool KiwiLaunch_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_BUILD_RUN )
        return false;
    s_open = !s_open;
    if ( s_open )
        EnsureThreadDefault();
    return true;
}

void KiwiLaunch_Draw()
{
    // POLL FIRST, ALWAYS (D-BF-D).  This runs whether or not the window is open, so closing
    // it mid-build neither stalls the pipe nor freezes the chain between stages.
    PollChild();

    if ( !s_open )
        return;
    DrawWindow();
}
