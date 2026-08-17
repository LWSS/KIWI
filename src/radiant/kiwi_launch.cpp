// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_launch.cpp — KIWI-UX (ROUND BF): the Build & Run dialog, the child-process
//  runner and the BSP -> Light -> Run pipeline.
// ═════════════════════════════════════════════════════════════════════════════════════
// Read kiwi_launch.h first — D-BF-A..G carry the whole derivation: why the compilers stay
// separate processes, why the target is always `raw\maps\mp\<name>.map`, the exact rule for
// `-loadFrom`, why there is no thread, where the `.errlog` / `.lin` handoff can disagree,
// and what the game needs on its command line.  Every contract cited there was read out of
// src/cod4map/, src/cod4rad/ and the game sources; NONE of those files is touched by this
// round.
// ═════════════════════════════════════════════════════════════════════════════════════
#include "stdafx.h"
#include "qe3.h"

#include <windows.h>
#include <imgui/imgui.h>

#include "kiwi_command.h"
#include "kiwi_launch.h"
#include "radiant_frame.h"      // Radiant_FileSave (:87) + Radiant_CurrentMapPath (ROUND BF)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

// ── ported / cross-file entry points (each verified against its definition) ─────────
extern int  Sys_Printf( const char *fmt, ... );          // win_qe3.cpp:112
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );  // mainfrm.cpp:1340
// The fs_basepath accessor every radiant file already uses (mainfrm.cpp:604, texwnd.cpp:1783,
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
// Radiant_CurrentMapPath()  radiant_frame.h:92 (mainfrm.cpp:495, ROUND BF) -- read-only view of
//                           s_currentMapPath (mainfrm.cpp:488), "" when untitled.

// ═════════════════════════════════════════════════════════════════════════════════════
//  STATE
// ═════════════════════════════════════════════════════════════════════════════════════
namespace
{

// ── the window ─────────────────────────────────────────────────────────────────────
bool s_open = false;

// ── options (persist for the session; nothing here goes in the ini) ────────────────
bool s_saveFirst   = true;      // run the Save funnel before a BSP build
bool s_verbose     = true;      // cod4map -v
bool s_onlyEnts    = false;     // cod4map -onlyEnts
bool s_leakTest    = false;     // cod4map -leakTest
char s_bspExtra[256] = { 0 };

int  s_threads     = 0;         // cod4rad -Threads N  (0 = "not resolved yet")
int  s_quality     = 1;         // 0 = -Fast, 1 = (neither), 2 = -Extra
bool s_noRelight   = true;      // cod4rad -NoRelight
char s_radExtra[256] = { 0 };

bool s_developer   = true;      // game +set developer 1
bool s_cheats      = true;      // game +set sv_cheats 1
bool s_devmap      = true;      // devmap vs map
bool s_skipLight   = false;     // the chain's "skip lighting" tick

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

// The chain's INTENT: what should start when the current stage exits 0.  LCHAIN_OFF means
// the stage was started on its own and nothing follows it.
enum launchChain_t
{
    LCHAIN_OFF = 0,
    LCHAIN_AFTER_BSP,       // BSP is running as part of Build & Run
    LCHAIN_AFTER_LIGHT      // Light is running as part of Build & Run
};

launchStage_t s_stage = LSTAGE_NONE;
launchChain_t s_chain = LCHAIN_OFF;
HANDLE        s_proc  = nullptr;      // the child, while it runs
HANDLE        s_pipe  = nullptr;      // our READ end of its stdout+stderr
DWORD         s_pid   = 0;
DWORD         s_lastExit = 0;
bool          s_haveLastExit = false;
launchStage_t s_lastStage = LSTAGE_NONE;
bool          s_killRequest = false;  // the confirm popup is owed an OpenPopup

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

// ═════════════════════════════════════════════════════════════════════════════════════
//  PATHS  (D-BF-B / D-BF-C).  Resolved fresh on every draw — never cached across a map
//  change, because every one of these inputs moves when the user opens or saves a map.
// ═════════════════════════════════════════════════════════════════════════════════════
struct launchPaths_t
{
    char root[MAX_PATH];        // fs_basepath — the game root, and every child's CWD
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

// The three tool exes, all beside the running radiant (SURVEY §A/B/D: one bin\<CONFIG>).
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
    case LSTAGE_BSP:   return "cod4map";
    case LSTAGE_LIGHT: return "cod4rad";
    default:           return "(idle)";
    }
}

// Spawn a CAPTURED console child: stdout AND stderr into one anonymous pipe, no window,
// CWD = the game root.  The write end is inherited and closed in the parent immediately —
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

// The game: a GUI process with a window of its own, so NO pipe, NO CREATE_NO_WINDOW, and
// both handles are closed at once — nothing about it is polled (D-BF-F: Run Map is exempt
// from the one-at-a-time rule).
bool SpawnDetached( const char *cmdline, const char *cwd )
{
    STARTUPINFOA si;
    memset( &si, 0, sizeof( si ) );
    si.cb = sizeof( si );
    PROCESS_INFORMATION pi;
    memset( &pi, 0, sizeof( pi ) );

    std::string mutableCmd( cmdline );
    mutableCmd.push_back( '\0' );

    if ( !::CreateProcessA( nullptr, &mutableCmd[0], nullptr, nullptr,
                            FALSE, 0, nullptr, cwd, &si, &pi ) )
    {
        LogLine( "ERROR: CreateProcess failed (GetLastError %lu)", ::GetLastError() );
        return false;
    }
    ::CloseHandle( pi.hThread );
    ::CloseHandle( pi.hProcess );
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
    if ( s_bspExtra[0] ) { cmd += " "; cmd += s_bspExtra; }
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
    if ( s_radExtra[0] ) { cmd += " "; cmd += s_radExtra; }
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

bool StartGame( const launchPaths_t &p )
{
    char exe[MAX_PATH];
    ToolPath( p, "KIWI-mp.exe", exe, sizeof( exe ) );
    if ( !FileExists( exe ) )
    {
        LogLine( "ERROR: %s is missing -- build the mp target.", exe );
        return false;
    }

    std::string cmd;
    cmd += "\"";  cmd += exe;  cmd += "\"";
    if ( s_developer ) cmd += " +set developer 1";
    if ( s_cheats )    cmd += " +set sv_cheats 1";
    cmd += s_devmap ? " +devmap " : " +map ";
    cmd += p.name;

    LogLine( "" );
    LogLine( "==== RUN ====" );
    LogLine( "cwd: %s", p.root );
    LogLine( "%s", cmd.c_str() );
    if ( !FileExists( p.bspAbs ) )
        LogLine( "WARNING: %s does not exist -- the game will not find the map.", p.bspAbs );

    if ( !SpawnDetached( cmd.c_str(), p.root ) )
        return false;
    LogLine( "launched (detached -- this window does not follow the game)" );
    return true;
}

// Called from the poll the frame a stage's process is seen to have exited.
void OnStageFinished( DWORD exitCode )
{
    const launchStage_t finished = s_stage;
    s_stage        = LSTAGE_NONE;
    s_lastStage    = finished;
    s_lastExit     = exitCode;
    s_haveLastExit = true;

    LogLine( "" );
    LogLine( "---- %s exited with code %lu (0x%08lX) ----",
             StageName( finished ), exitCode, exitCode );
    Sys_Printf( "Build: %s exited with code %lu\n", StageName( finished ), exitCode );

    if ( finished == LSTAGE_BSP )
        AfterBspDiagnostics( exitCode );

    if ( s_chain == LCHAIN_OFF )
        return;

    if ( exitCode != 0 )
    {
        LogLine( "chain STOPPED: %s failed.", StageName( finished ) );
        s_chain = LCHAIN_OFF;
        return;
    }

    launchPaths_t p;
    ResolvePaths( p );
    if ( !p.haveRoot || !p.haveMap )
    {
        LogLine( "chain STOPPED: the map path changed while the build ran." );
        s_chain = LCHAIN_OFF;
        return;
    }

    if ( s_chain == LCHAIN_AFTER_BSP && !s_skipLight )
    {
        if ( StartLight( p ) )
            s_chain = LCHAIN_AFTER_LIGHT;
        else
        {
            LogLine( "chain STOPPED: could not start the lighting stage." );
            s_chain = LCHAIN_OFF;
        }
        return;
    }

    // Either the light stage just finished, or lighting was skipped: run the game.
    s_chain = LCHAIN_OFF;
    StartGame( p );
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
        LogLine( "ERROR: GetExitCodeProcess failed (GetLastError %lu) -- dropping the child.",
                 ::GetLastError() );
        CloseChild();
        s_stage = LSTAGE_NONE;
        s_chain = LCHAIN_OFF;
        return;
    }
    if ( code == STILL_ACTIVE )
        return;

    ReadAvailable();          // whatever it wrote between the last drain and its exit
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

void DrawWindow()
{
    EnsureThreadDefault();
    ImGui::SetNextWindowSize( ImVec2( 760.0f, 620.0f ), ImGuiCond_FirstUseEver );
    if ( !ImGui::Begin( "Build & Run", &s_open, ImGuiWindowFlags_NoDocking ) )
    {
        ImGui::End();
        return;
    }

    launchPaths_t p;
    ResolvePaths( p );

    // ── what we are about to compile ───────────────────────────────────────────────
    if ( !p.haveRoot )
        ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ),
                            "fs_basepath is empty -- the game root could not be resolved." );
    else
        ImGui::Text( "root:   %s", p.root );

    if ( !p.haveMap )
        ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.25f, 1.0f ),
                            "map:    (untitled) -- save the map first (File > Save As)." );
    else
    {
        ImGui::Text( "map:    %s", p.mapPath );
        ImGui::Text( "target: %s", p.targetRel );
        HelpMarker( "Both compilers derive their base path by walking the map argument back "
                    "for a folder named 'maps' and require two folders below it, and the MP "
                    "game reads maps/mp/<name>.d3dbsp off the search path. raw\\maps\\mp is "
                    "the only directory that satisfies both, so every build targets it." );
        if ( p.useLoadFrom )
        {
            ImGui::TextDisabled( "        (compiled with -loadFrom: the saved .map is read "
                                 "in place, output lands on the target)" );
        }
    }

    const bool canBuild = p.haveRoot && p.haveMap;
    const bool busy     = Busy();

    ImGui::Separator();
    ImGui::Checkbox( "Save the map before building", &s_saveFirst );

    // ── BSP ────────────────────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextUnformatted( "BSP  (KIWI-cod4map.exe)" );
    ImGui::Checkbox( "verbose (-v)", &s_verbose );
    ImGui::SameLine();
    ImGui::Checkbox( "entities only (-onlyEnts)", &s_onlyEnts );
    ImGui::SameLine();
    ImGui::Checkbox( "leak test (-leakTest)", &s_leakTest );
    HelpMarker( "-onlyEnts recompiles the entity lump only and leaves geometry and lighting "
                "alone. -leakTest quits as soon as the map is found to leak." );
    ImGui::SetNextItemWidth( 420.0f );
    ImGui::InputText( "extra cod4map args", s_bspExtra, sizeof( s_bspExtra ) );

    {
        char exe[MAX_PATH];
        ToolPath( p, "KIWI-cod4map.exe", exe, sizeof( exe ) );
        const bool haveExe = FileExists( exe );
        ImGui::BeginDisabled( busy || !canBuild || !haveExe );
        if ( ImGui::Button( "Build BSP", ImVec2( 140.0f, 0.0f ) ) )
        {
            s_chain = LCHAIN_OFF;
            StartBsp( p );
        }
        ImGui::EndDisabled();
        if ( !haveExe )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "KIWI-cod4map.exe not found" );
        }
        else if ( !canBuild )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "save the map first" );
        }
    }

    // ── LIGHT ──────────────────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextUnformatted( "Lighting  (KIWI-cod4rad.exe)" );
    ImGui::SetNextItemWidth( 120.0f );
    if ( ImGui::InputInt( "threads (-Threads)", &s_threads ) )
    {
        if ( s_threads < 1 )  s_threads = 1;
        if ( s_threads > 64 ) s_threads = 64;
    }
    HelpMarker( "Without -Threads the tool clamps itself to at most 4 threads regardless of "
                "the machine (cmdline.c ParseCommandLine_Init). The default here is the "
                "processor count GetSystemInfo reports." );
    ImGui::RadioButton( "Fast (-Fast)", &s_quality, 0 );
    ImGui::SameLine();
    ImGui::RadioButton( "Normal", &s_quality, 1 );
    ImGui::SameLine();
    ImGui::RadioButton( "Extra (-Extra)", &s_quality, 2 );
    ImGui::Checkbox( "no relight cache (-NoRelight)", &s_noRelight );
    HelpMarker( "cod4rad opens its relight cache as the bare relative name 'radtrans.bin' "
                "(compile.c), so it lands in the working directory -- the game root -- and "
                "is shared by every map compiled from here. Leave this on unless you are "
                "re-lighting the SAME map repeatedly and want the cache." );
    ImGui::SetNextItemWidth( 420.0f );
    ImGui::InputText( "extra cod4rad args", s_radExtra, sizeof( s_radExtra ) );

    {
        char exe[MAX_PATH];
        ToolPath( p, "KIWI-cod4rad.exe", exe, sizeof( exe ) );
        const bool haveExe = FileExists( exe );
        ImGui::BeginDisabled( busy || !canBuild || !haveExe );
        if ( ImGui::Button( "Build Light", ImVec2( 140.0f, 0.0f ) ) )
        {
            s_chain = LCHAIN_OFF;
            StartLight( p );
        }
        ImGui::EndDisabled();
        if ( !haveExe )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "KIWI-cod4rad.exe not found" );
        }
        else if ( canBuild && !FileExists( p.bspAbs ) )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "no .d3dbsp yet -- build the BSP first" );
        }
    }

    // ── RUN ────────────────────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextUnformatted( "Run  (KIWI-mp.exe)" );
    ImGui::Checkbox( "developer 1", &s_developer );
    ImGui::SameLine();
    ImGui::Checkbox( "sv_cheats 1", &s_cheats );
    HelpMarker( "SV_Map_f sets sv_cheats itself from the command name -- devmap turns it on, "
                "map turns it off -- so this only survives as a pre-spawn value." );
    // The bool-taking RadioButton overload, NOT the (int*, int) one: s_devmap is a `bool`
    // and handing its address to the int* form would write four bytes into a one-byte
    // object and stamp on whatever the linker put next to it.
    if ( ImGui::RadioButton( "+devmap", s_devmap ) )
        s_devmap = true;
    ImGui::SameLine();
    if ( ImGui::RadioButton( "+map", !s_devmap ) )
        s_devmap = false;

    {
        char exe[MAX_PATH];
        ToolPath( p, "KIWI-mp.exe", exe, sizeof( exe ) );
        const bool haveExe = FileExists( exe );
        ImGui::BeginDisabled( !canBuild || !haveExe );
        if ( ImGui::Button( "Run Map", ImVec2( 140.0f, 0.0f ) ) )
            StartGame( p );
        ImGui::EndDisabled();
        if ( !haveExe )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "KIWI-mp.exe not found" );
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "(+map / +devmap need a player profile -- see players\\)" );
    }

    // ── THE CHAIN ──────────────────────────────────────────────────────────────────
    ImGui::Separator();
    {
        ImGui::BeginDisabled( busy || !canBuild );
        if ( ImGui::Button( "Build & Run", ImVec2( 140.0f, 0.0f ) ) )
        {
            if ( StartBsp( p ) )
                s_chain = LCHAIN_AFTER_BSP;
            else
                s_chain = LCHAIN_OFF;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox( "skip lighting", &s_skipLight );
        HelpMarker( "BSP, then lighting, then the game. Each stage starts only when the "
                    "previous one exits 0." );
    }

    // ── STATUS + KILL ──────────────────────────────────────────────────────────────
    ImGui::Separator();
    if ( busy )
    {
        ImGui::TextColored( ImVec4( 0.4f, 0.85f, 1.0f, 1.0f ),
                            "running: %s (pid %lu)%s", StageName( s_stage ), s_pid,
                            s_chain != LCHAIN_OFF ? "  [Build & Run]" : "" );
        ImGui::SameLine();
        if ( ImGui::Button( "Kill" ) )
            s_killRequest = true;
    }
    else if ( s_haveLastExit )
    {
        const ImVec4 col = s_lastExit == 0 ? ImVec4( 0.45f, 0.9f, 0.45f, 1.0f )
                                           : ImVec4( 1.0f, 0.45f, 0.45f, 1.0f );
        ImGui::TextColored( col, "%s finished with exit code %lu",
                            StageName( s_lastStage ), s_lastExit );
    }
    else
        ImGui::TextDisabled( "idle" );

    if ( s_killRequest )
    {
        ImGui::OpenPopup( "Kill the running build?" );
        s_killRequest = false;
    }
    if ( ImGui::BeginPopupModal( "Kill the running build?", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::TextUnformatted( "Terminate the running compiler?" );
        ImGui::Separator();
        ImGui::TextWrapped(
            "A killed cod4map leaves whatever it had written of the .d3dbsp; a killed "
            "cod4rad is worse -- it reads and REWRITES the same .d3dbsp in place, so the "
            "map on disk is left HALF LIT and has to be rebuilt from the BSP stage." );
        ImGui::Separator();
        if ( ImGui::Button( "Terminate", ImVec2( 120.0f, 0.0f ) ) )
        {
            if ( s_proc )
            {
                LogLine( "" );
                LogLine( "---- kill requested: TerminateProcess on %s (pid %lu) ----",
                         StageName( s_stage ), s_pid );
                ::TerminateProcess( s_proc, 1 );      // the poll sees the exit next frame
            }
            s_chain = LCHAIN_OFF;                     // never chain past a kill
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Keep building", ImVec2( 120.0f, 0.0f ) ) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── LOG ────────────────────────────────────────────────────────────────────────
    ImGui::Separator();
    if ( ImGui::Button( "Copy" ) )
        ImGui::SetClipboardText( s_log.c_str() );
    ImGui::SameLine();
    if ( ImGui::Button( "Clear" ) )
    {
        s_log.clear();
        s_haveLastExit = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "%d KB", (int)( s_log.size() / 1024 ) );

    ImGui::BeginChild( "##buildlog", ImVec2( 0.0f, 0.0f ), ImGuiChildFlags_Borders,
                       ImGuiWindowFlags_HorizontalScrollbar );
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 4.0f, 1.0f ) );
    // TextUnformatted has a built-in clipper for long text inside a scroll region — the
    // same shape the console tab uses (imgui_shell.cpp ImGuiShell_DrawConsoleTab).
    ImGui::TextUnformatted( s_log.c_str(), s_log.c_str() + s_log.size() );
    ImGui::PopStyleVar();
    // Follow the tail only while the view IS at the tail: the moment the user scrolls up to
    // read an error, new output must not yank them back down.
    if ( s_logScrollPending && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f )
        ImGui::SetScrollHereY( 1.0f );
    s_logScrollPending = false;
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════════════
//  COMMANDS + THE PER-FRAME ENTRY POINT
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiLaunch_RegisterCommands()
{
    // ── KIWI-UX (ROUND BH, ITEM 5): F9 — AND IT IS THE COMPILED-IN DEFAULT ──────
    // USER DIRECTIVE, verbatim: *"Also bind it to like F9 if it's not taken,
    // otherwise something like shift-F9."*
    //
    // F9 (vk 0x78) IS FREE, and this is the full audit rather than a spot check.
    // Every row of the stock table (mainfrm.cpp:1082-1270) carrying an F-key:
    //     0x70 F1  mods 2  GetDistance 33178          (mainfrm.cpp:1241)
    //     0x71 F2  mods 2  AutoEdgeTurn 33179         (:1242)
    //     0x73 F4  mods 0  ToggleLayeredMaterialWnd   (:1268)
    //     0x74 F5  mods 0  RefreshTextures 33204      (:1265)
    //     0x75 F6  mods 0  SetViewToEntity 33210      (:1179)
    //     0x77 F8  mods 0/1/2/3/4/5/6  the seven LightPreview* rows (:1257-1263)
    // 0x78 appears NOWHERE — the only 0x78 in mainfrm.cpp is the key-NAME table
    // row `{ "F9", 0x78 }` at :1436, which is the radiant.ini parser's spelling
    // table, not a binding.  The MODERN profile's 81 Bind rows (kiwi_keymap.cpp
    // ApplyModern) contain no 0x78 either, and no KIWI command or alias row claims
    // it: every Radiant_RegisterCommand / Radiant_RegisterCommandAlias call in the
    // tree registers UNBOUND except this one.  res/radiant.rc's IDR_MAIN_ACCEL
    // carries no F-key, so TranslateAccelerator — which runs BEFORE the hotkey
    // table — cannot swallow it.  So NO Shift+F9 fallback is needed.
    //
    // BOUND AT REGISTRATION, not in kiwi_keymap.cpp's modern profile, and that is
    // deliberate: registering with a (vk, mods) makes this row's COMPILED-IN
    // DEFAULT F9 (Radiant_RegisterCommand copies it into
    // g_radiantCommandsKiwiDefault, mainfrm.cpp:1369), which is what
    // Radiant_ResetCommandBindings restores — so F9 is live in the CLASSIC profile
    // too.  A build key is profile-neutral: it is not a modelling verb competing
    // for a prime letter, and a mapper who prefers the stock bindings still wants
    // one key to compile.  radiant.ini's [Commands] section still overrides it,
    // exactly as it overrides any other row.
    Radiant_RegisterCommand( "KiwiBuildAndRun", 0x78, 0, KIWI_CMD_BUILD_RUN );
}

// ── KIWI-UX (ROUND BH, ITEM 5): THE TOP-BAR BUTTON ─────────────────────────────
// USER DIRECTIVE, verbatim: *"Build and run needs to be in the win32 toolbar
// somewhere."*
//
// THERE IS NO WIN32 TOOLBAR IN THIS SHELL, and that is a fact about the port
// rather than an omission of this round: the icon toolbar was a CToolBar and went
// out with the MFC rip (radiant_main.cpp step 1b: *"SKIPPED — the icon toolbar
// (mainfrm.cpp:1473, OnCreate 0x420ac6 CreateEx + LoadToolBar(152)) is a CToolBar,
// and every TB_CHECKBUTTON seed with it"*), and res/radiant.rc:12-13 records that
// IDR_TOOLBAR152's BITMAP and TOOLBAR resources were DELETED with it, leaving
// res/toolbar.bmp on disk with no consumer.  Re-creating a real TOOLBARCLASSNAME
// child is not a small change either: the frame's ENTIRE client area is the ImGui
// dockspace surface (radiant_main.cpp hands the frame to
// ImGuiShell_SetPrimarySurface), so a native toolbar child would have to be carved
// out of a D3D swap-chain window and the whole layout re-inset for it.
//
// THE NATIVE MENU BAR IS THE WIN32 TOP-BAR CHROME THIS SHELL ACTUALLY HAS, and it
// is already the shell's stated command source (radiant_main.cpp: *"The menu bar
// IS the command source in this shell"*).  So Build & Run becomes a TOP-LEVEL
// MENU-BAR ITEM WITH NO POPUP: one click on the bar sends WM_COMMAND 34130 to the
// frame WndProc, which routes it through Radiant_ExecCommand and the 34000..34199
// KIWI arm exactly like the "Windows" popup's items do.  That satisfies "a
// text-capable style, no new bitmap art" in the only vocabulary available.
//
// APPENDED AT THE RIGHT END, which is also the only safe place: the index-based
// menu consumers in this tree read POPUP indices (radiant_main.cpp uses index 0
// for the File popup's MRU, kiwi_windows.cpp uses index 2 for the View popup,
// texwnd.cpp uses index 5 for the Textures popup), and appending after the last
// popup cannot move any of them.  Same argument kiwi_windows.h writes out for the
// "Windows" popup, which this sits to the right of.
//
// THE CAPTION CARRIES NO ACCELERATOR TEXT OF ITS OWN, deliberately.  This runs
// AFTER Radiant_ShowMenuItemKeyBindings at boot (radiant_main.cpp annotates before
// the KIWI menu builders, so their captions stay clean), but that annotator
// searches BY COMMAND ID through the whole menu tree — a menu-bar item included —
// and it runs again on every keymap-profile switch
// (Radiant_RefreshMenuKeyBindings <- kiwi_keymap.cpp Rebuild).  So it WILL find
// this row now that the command is bound and rewrite the caption to
// "Build & Run\tF9".  Writing "(F9)" here as well would leave the key named twice
// after the first profile switch; leaving it out means the caption is stable and
// the annotator's own convention is the only one in play.  "&&" is Win32's escape
// for a literal ampersand.
void KiwiLaunch_BuildMenu( void *frameMenu )
{
    HMENU hMenu = (HMENU)frameMenu;
    if ( !hMenu )
        return;
    ::AppendMenuA( hMenu, MF_STRING, (UINT_PTR)KIWI_CMD_BUILD_RUN, "&Build && Run" );
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
