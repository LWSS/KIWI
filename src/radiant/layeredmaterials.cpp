#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\layeredmaterials.cpp
// 0x416ac0 LayeredMaterialLibrary
// 0x417190 LayeredMaterials_texcoords
// 0x4D6C40 CheckLayeredMaterial_Modifications (real CRC32)
// 0x416a40 q_shared_texcoords (file-static helper, no external callsites)
// Assert: LayeredMaterials.cpp:65, :297
//
// Layered material library parser and texture utility.

#include "stdafx.h"
#include <cstdio>                 // FILE, fprintf, fclose
#include <cstdlib>                // qsort, free (LayerdMatWnd)
#include <cstring>                // memset, _stricmp (LayerdMatWnd)
#include "qe3.h"                  // g_qeglobals, entity_s, epair_t, qtexture_s
#include <universal/q_shared.h>   // I_stricmp, I_strnicmp
#include <universal/q_parse.h>    // Com_Parse, Com_ParseInt, Com_MatchToken
#include "kiwi_ported.h"          // KIWI: shared engine/editor externs

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
// Disk write (map.cpp) — opens the path "wb".
extern FILE *Map_OpenForSave( const char *path );                          // 0x48CC70

// LoadFile (kiwi_ported.h) does a plain fopen() relative to the editor CWD (no FS
// search paths) — see LayerdMatWnd's epair.

// ── Layered-material WINDOW globals/helpers (layeredmaterialwnd.cpp) ──────────
// AddEntries makes the newly-created entry the tool's active material; the caption and
// toolbar-state refresh the binary did here is per-frame work in imgui_panel_lyrmtl.cpp.
// (lyrMtlWndGlob extern comes from qe3.h)

// ─────────────────────────────────────────────────────────────────────────────
// lyrMtlGlob — global storage for the layered material library.
//
// Layout (0x416ac0 LayeredMaterialLibrary):
//   0x1814CF8  int      lyrMtlGlob.crcToken           CRC32 token
//   0x1814CFC  int      lyrMtlGlob.entryCount   # of loaded entries
//   0x1814D00  uint8[]  lyrMtlGlob.Layers        inline array, 512 × 84 bytes
//
// Entry (84 bytes) layout:
//   +0x00  char[64]  name         entry name
//   +0x40  int       nextId       next layer id to assign
//   +0x44  int       layerCount   number of layers in this entry (max 1)
//   +0x48  int       _pad
//   +0x4C  int       layer[n].id       layer ids (8 bytes per layer: id + handle)
//   +0x50  int       layer[n].handle   qtexture_s* handle (stored as int)
//
// lyrMtlGlob.Layers is an INLINE array (not a pointer).
// These globals are defined here.
// ─────────────────────────────────────────────────────────────────────────────

// Offsets within each 84-byte entry.
enum {
    LYR_ENTRY_SIZE    = sizeof(LyrEntry_t),
    LYR_NAME_SIZE     = 64,
    LYR_NEXTID_OFF    = 0x40,
    LYR_LAYERCNT_OFF  = 0x44,
    LYR_ID_OFF        = offsetof(LyrEntry_t, layers),
    LYR_HANDLE_OFF    = offsetof(LyrEntry_t, layers) + offsetof(LyrEntryLayer_t, handle),
    LYR_MAX_ENTRIES   = 512,
    LYR_MAX_LAYERS    = 1,
};

// Typed view of one 84-byte library entry (stride within lyrMtlGlob.Layers).
// Same head layout as materialdef.cpp's 120-byte LayerMaterialDef, but the library
// stride only carries LYR_MAX_LAYERS layer slots.
// Shared native entry in qe3.h.
static_assert( sizeof( LyrEntry_t ) == LYR_ENTRY_SIZE, "LyrEntry_t" );
static_assert( offsetof( LyrEntry_t, nextId ) == LYR_NEXTID_OFF
            && offsetof( LyrEntry_t, layers ) == LYR_ID_OFF, "LyrEntry_t offsets" );

LyrMtlGlob_t lyrMtlGlob = {};   // type in qe3.h
static_assert( sizeof( lyrMtlGlob.Layers ) == LYR_MAX_ENTRIES * LYR_ENTRY_SIZE,
               "LyrMtlGlob_t.Layers size" );

// ─────────────────────────────────────────────────────────────────────────────
// 0x4D6C40  CheckLayeredMaterial_Modifications
// CRC32 with Ethernet polynomial 0xEDB88320, Lsb-first, 8 bits per byte.
// a1=buffer, a2=size (bytes), a3=seed (used as ~a3 for the initial CRC value).
// Returns ~crc (standard final XOR).
// ─────────────────────────────────────────────────────────────────────────────
unsigned int CheckLayeredMaterial_Modifications( uint8_t *a1, int a2, int a3 )
{
    const uint8_t *v3  = a1;
    const uint8_t *end = a1 + a2;
    unsigned int   crc = ~(unsigned int)a3;

    while ( v3 != end )
    {
        // Lsb-first CRC32 bit-at-a-time, 8 rounds per byte.
        unsigned int x = (*v3 ^ crc);

        // Round 1:
        unsigned int r1 = (x >> 1) ^ (0xEDB88320u & -(x & 1u));
        // Round 2:
        unsigned int r2 = (r1 >> 1) ^ (0xEDB88320u & -(r1 & 1u));
        // Round 3:
        unsigned int r3 = (r2 >> 1) ^ (0xEDB88320u & -(r2 & 1u));
        // Round 4:
        unsigned int r4 = (r3 >> 1) ^ (0xEDB88320u & -(r3 & 1u));
        unsigned int r5 = (r4 >> 1) ^ (0xEDB88320u & -(r4 & 1u));
        unsigned int r6 = (r5 >> 1) ^ (0xEDB88320u & -(r5 & 1u));
        unsigned int r7 = (r6 >> 1) ^ (0xEDB88320u & -(r6 & 1u));
        unsigned int r8 = (r7 >> 1) ^ (0xEDB88320u & -(r7 & 1u));

        crc = r8;
        ++v3;
    }
    return ~crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x416a40  q_shared_texcoords — file-static
// Linear search for `name` in lyrMtlGlob.Layers[].
// Returns the index of the matching entry, or lyrMtlGlob.entryCount if not found
// (i.e. the "append here" slot).
// Assert strings from q_shared.cpp:640 / :641 — the function was inlined from
// q_shared.cpp's I_strnicmp wrapper.
// ─────────────────────────────────────────────────────────────────────────────
static int q_shared_texcoords( const char *name )
{
    int v1 = 0;
    if ( lyrMtlGlob.entryCount > 0 )
    {
        const uint8_t *v2 = lyrMtlGlob.Layers;
        do
        {
            // q_shared.cpp:640/641 (I_stricmp)
            if ( !I_stricmp( (const char *)v2, name ) )
                break;
            ++v1;
            v2 += LYR_ENTRY_SIZE;
        }
        while ( v1 < lyrMtlGlob.entryCount );
    }
    return v1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x4172f0  LayeredMaterials_GetMaterial
// Binary search over the SORTED lyrMtlGlob.Layers[] for an entry whose name
// (offset 0, 64-byte) case-insensitively equals `name`. Returns the entry pointer
// (cast by SetMaterial to LayerMaterialDef*), or NULL if not found / library empty.
// When the layered-material library has not been loaded (lyrMtlGlob.entryCount==0 —
// the case for the headless gate AND for stock maps with no layered materials),
// this returns NULL immediately, so SetMaterial falls through to Texture_GetHandle.
// NOTE (latent, P5.6): library entries are 84 bytes but a realised LayerMaterialDef
// is 120; reading past offset 84 only happens once the library is actually loaded
// (LayeredMaterialLibrary), which the editor does not do on the basic map-display
// path — flagged for the layered-material window work.
// ─────────────────────────────────────────────────────────────────────────────
void *LayeredMaterials_GetMaterial( const char *name )
{
    int hi = lyrMtlGlob.entryCount - 1;
    int lo = 0;
    if ( hi < 0 )
        return nullptr;                       // empty library

    int  mid;
    int  cmp;
    for ( ;; )
    {
        mid = (hi + lo) / 2;
        const uint8_t *entry = &lyrMtlGlob.Layers[LYR_ENTRY_SIZE * mid];
        // q_shared.cpp:640/641 (I_stricmp)
        cmp = I_stricmp( (const char *)entry, name );               // entry vs name
        if ( cmp >= 0 )
        {
            if ( cmp == 0 )
                return &lyrMtlGlob.Layers[LYR_ENTRY_SIZE * mid];    // exact match
            hi = mid - 1;                     // entry > name → search lower half
        }
        else
        {
            lo = mid + 1;                     // entry < name → search upper half
        }
        if ( lo > hi )
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x416ac0  LayeredMaterialLibrary
// Assert: LayeredMaterials.cpp:65
//
// Parses a "LayeredMaterialLibrary" block from a text token stream.
// Returns 1 on success/recoverable error, 0 on fatal parse error.
//
// Grammar:
//   "LayeredMaterialLibrary" <version:int>
//   { <name> <nextId:int> { (<id:int> <material> ";")* } }*
// ─────────────────────────────────────────────────────────────────────────────
char LayeredMaterialLibrary( const char **data_p )
{
    // v1 tracks the outer parse cursor (same as data_p).
    const char **v1 = data_p;

    if ( !Com_MatchToken( data_p, "LayeredMaterialLibrary", 1 ) )
        return 0;

    int v2 = Com_ParseInt( data_p );
    if ( v2 < 1 )
    {
        Sys_Printf( "Layered material library has bad version number %i\n", v2 );
        return 1;
    }
    if ( v2 > 1 )
    {
        Sys_Printf( "Can't read newer layered material library version "
                    "(code is version %i, file is version %i)\n", 1, v2 );
        return 1;
    }

    iassert( lyrMtlGlob.entryCount == 0 );   // LayeredMaterials.cpp:65

    // Read first entry name (or end of stream).
    // Com_Parse returns parseInfo_t*; use ->token for the string.
    const char *v4 = Com_Parse( data_p )->token;
    if ( !*data_p )
        return 1;

    // Outer loop: parse each named entry.
LABEL_9:
    {
        int v5  = q_shared_texcoords( v4 );
        int v11 = v5;

        if ( v5 == LYR_MAX_ENTRIES )
        {
            Sys_Printf( "ERROR: More than %i entries in layered material library.\n",
                        LYR_MAX_ENTRIES );
            return 0;
        }

        LyrEntry_t *v6 = (LyrEntry_t *)( lyrMtlGlob.Layers + LYR_ENTRY_SIZE * v5 );

        strcpy( v6->name, v4 );
        v6->nextId = Com_ParseInt( v1 );

        if ( Com_MatchToken( (const char **)v1, "{", 1 ) )
        {
            // Parse layers for this entry.
            while ( true )
            {
                const char *v7 = Com_Parse( (const char **)v1 )->token;
                if ( !*v1 )
                    return 0;   // end of stream inside block = failure

                if ( *v7 == '}' )
                {
                    // Close brace: commit entry if new.
                    if ( v11 == lyrMtlGlob.entryCount )
                        ++lyrMtlGlob.entryCount;

                    // Read next entry name.
                    v4 = Com_Parse( (const char **)v1 )->token;
                    if ( *v1 )
                        goto LABEL_9;
                    return 1;
                }

                // Layer count check: max LYR_MAX_LAYERS (1).
                int layerCount = v6->layerCount;
                if ( layerCount == LYR_MAX_LAYERS )
                {
                    Sys_Printf( "ERROR: more than %i layers in layered material %s\n",
                                LYR_MAX_LAYERS, v6->name );
                    return 0;
                }

                // Parse id.
                long v8 = atol( v7 );

                if ( v8 >= (long)v6->nextId )
                {
                    Sys_Printf( "ERROR: layer id %i >= next id %i in layered material %s\n",
                                (int)v8, v6->nextId, v6->name );
                    return 0;
                }

                int v9 = 0;
                const char *v13 = Com_Parse( (const char **)v1 )->token;

                // Walk existing layers to check for duplicates.
                if ( layerCount > 0 )
                {
                    while ( true )
                    {
                        if ( (long)v6->layers[v9].id == v8 )
                        {
                            Sys_Printf( "ERROR: Duplicate layer id %i in layered material %s\n",
                                        (int)v8, v6->name );
                            return 0;
                        }
                        if ( !I_stricmp( v13, v6->layers[v9].handle->name ) )
                        {
                            // Fall through to dup-material error below.
                            break;
                        }
                        ++v9;
                        if ( v9 >= layerCount )
                        {
                            // No duplicate found — proceed to LABEL_21 (write).
                            v1 = data_p;
                            goto LABEL_21;
                        }
                    }
                    // Broke out of loop = duplicate material.
                    Sys_Printf( "ERROR: Duplicate material %s in layered material %s\n",
                                v13, v6->name );
                    return 0;
                }

LABEL_21:
                // Write the new layer entry.
                v6->layers[layerCount].id     = (int)v8;
                v6->layers[layerCount].handle = Texture_GetHandle( v13 );
                ++v6->layerCount;

                if ( !Com_MatchToken( (const char **)v1, ";", 1 ) )
                    return 0;
            }
        }
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x416d30  LayeredMaterial_NameCompare — qsort comparator for the entry array.
// The entry name is the first field of each 84-byte record, so a bare I_stricmp on the
// two record pointers sorts case-insensitively by name.  (Its only use is this qsort
// callback.)
// ─────────────────────────────────────────────────────────────────────────────
static int __cdecl LayeredMaterial_NameCompare( const void *a, const void *b )
{
    return I_stricmp( (const char *)a, (const char *)b );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x416d40  LayerdMatWnd — load the layered-material library.
//
// Called from QE_LoadProject (0x48bab0), right after Load_Textures(), once
// the project entity has been parsed.  Resets the in-memory library, reads the file
// named by the project entity's "layeredmaterials" epair, parses it with
// LayeredMaterialLibrary (with spaceDelimited DISABLED so layer ids/names tokenise the
// way the parser expects), sorts the entries by name, and CRC-baselines the clean load
// (so LayeredMaterials_Save only writes when the operator has actually modified it).
// Returns the CRC token on the load-and-parse path,
// or the LoadFile result (-1) when the file is absent.
//
// FILE RESOLUTION (important): LoadFile() does a plain fopen() relative to the editor
// CWD — it does NOT consult FS search paths.  cod4.prj names the library
// "cod4_layered_material_library.txt"; that file ships in the CoD4 install's bin\ dir,
// so for the loader to find it the file must sit in the editor's working directory
// (bin\Debug).  When it is absent, LoadFile returns -1, this function leaves
// lyrMtlGlob.entryCount == 0, and the layered-material view is empty.
// ─────────────────────────────────────────────────────────────────────────────
signed int LayerdMatWnd()
{
    lyrMtlGlob.entryCount = 0;
    memset( lyrMtlGlob.Layers, 0, LYR_MAX_ENTRIES * LYR_ENTRY_SIZE );   // 0xA800 = 512*84

    // Resolve the library path from the project entity's "layeredmaterials" epair.
    // (Same epair walk as LayeredMaterials_Save.)
    const char *value = "";
    epair_t *ep = g_qeglobals.d_project_entity ? g_qeglobals.d_project_entity->epairs : nullptr;
    for ( ; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, "layeredmaterials" ) ) { value = ep->value; break; }
    }

    void *data = nullptr;
    signed int result = LoadFile( value, &data );
    if ( result >= 0 )
    {
        Com_BeginParseSession( value );
        // The parser relies on space-delimiting being off.
        Com_SetSpaceDelimited( 0 );

        const char *cursor = (const char *)data;
        if ( !LayeredMaterialLibrary( &cursor ) )
            Com_Error( ERR_DROP, "Layered material library is corrupt.\n" );

        Com_EndParseSession();
        free( data );

        qsort( lyrMtlGlob.Layers, lyrMtlGlob.entryCount, LYR_ENTRY_SIZE, LayeredMaterial_NameCompare );
        lyrMtlGlob.crcToken = (int)CheckLayeredMaterial_Modifications( lyrMtlGlob.Layers,
                                                                 LYR_ENTRY_SIZE * lyrMtlGlob.entryCount, 0 );
        result = lyrMtlGlob.crcToken;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x417000  LayeredMaterial_ValidateName (file-static)
// Returns an error string if `name` is invalid (empty / non-alnum-non-'_' / >= 64
// chars), or NULL if valid.  The >=64 message uses va().
// ─────────────────────────────────────────────────────────────────────────────
static const char *LayeredMaterial_ValidateName( const char *name )
{
    if ( !*name )
        return "Layered material names must be at least one character long.";
    const char *p = name;
    do
    {
        if ( !isalnum( (byte)*p ) && *p != '_' )
            return "Layered material names must contain only alphanumeric characters and '_'.";
        ++p;
    }
    while ( *p );
    if ( p - name < 64 )
        return nullptr;
    return va( "Layered material name length must not exceed %i characters.", 63 );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x416E50  LayeredMaterials_WriteFile (file-static)
// Writes the whole library to an open FILE*: a "LayeredMaterialLibrary 1" header
// then, per entry, "<name> <nextId>\n{ \t<id> "<material>";\n... }".  Returns 1.
// The material name for a layer is the radMtl handle's name (qtexture_s.name @+4),
// reached via the handle stored at the layer's +0x50 slot.
// ─────────────────────────────────────────────────────────────────────────────
static char LayeredMaterials_WriteFile( FILE *f )
{
    fprintf( f, "LayeredMaterialLibrary %i\n", 1 );
    for ( int i = 0; i < lyrMtlGlob.entryCount; ++i )
    {
        LyrEntry_t *entry = (LyrEntry_t *)&lyrMtlGlob.Layers[LYR_ENTRY_SIZE * i];
        fprintf( f, "\n%s %i\n", entry->name, entry->nextId );
        fprintf( f, "{\n" );
        for ( int j = 0; j < entry->layerCount; ++j )
            fprintf( f, "\t%i \"%s\";\n", entry->layers[j].id, entry->layers[j].handle->name );
        fprintf( f, "}\n" );
    }
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x417050  LayeredMaterials_AddEntries
// Validate the name, reject if the library is full (512) or the name already exists,
// otherwise insert a new ZEROED 84-byte entry KEEPING the array sorted by name
// (case-insensitive), make it the window's active material, refresh title/toolbar.
// PURE IN-MEMORY (the disk write is LayeredMaterials_Save, separately gated).
// ─────────────────────────────────────────────────────────────────────────────
void LayeredMaterials_AddEntries( char *name, HWND hWnd )
{
    const char *err = LayeredMaterial_ValidateName( name );
    if ( err )
    {
        MessageBoxA( hWnd, err, "Radiant", MB_ICONEXCLAMATION );
        return;
    }
    if ( lyrMtlGlob.entryCount == LYR_MAX_ENTRIES )
    {
        Sys_Printf( "Layered material library is currently limited to %i entries.\n", LYR_MAX_ENTRIES );
        return;
    }

    // Check before shifting: discovering a duplicate midway through the move
    // would otherwise leave the existing library entries overwritten.
    if ( LayeredMaterials_GetMaterial( name ) )
    {
        MessageBoxA( hWnd, "A layered material with that name already exists.",
                     "Radiant", MB_ICONEXCLAMATION );
        return;
    }

    // Find the sorted insertion slot, shifting larger entries up: walk from the LAST
    // entry downward, decrementing the slot index once per shift.  An equal name is a
    // duplicate error; the first entry sorting before `name` stops the scan.
    int slot = lyrMtlGlob.entryCount;
    if ( lyrMtlGlob.entryCount > 0 )
    {
        uint8_t *cursor = &lyrMtlGlob.Layers[LYR_ENTRY_SIZE * ( lyrMtlGlob.entryCount - 1 )];
        for ( ;; )
        {
            int cmp = I_stricmp( (const char *)cursor, name );
            if ( !cmp )
            {
                MessageBoxA( hWnd,
                             "Invalid name.\n\nA layered material with that name already exists.",
                             "Radiant", MB_ICONEXCLAMATION );
                return;
            }
            if ( cmp >= 0 )
            {
                // cursor entry sorts AFTER name → shift it up one slot, keep scanning down.
                memcpy( cursor + LYR_ENTRY_SIZE, cursor, LYR_ENTRY_SIZE );
                if ( --slot <= 0 )
                {
                    break;
                }
                cursor -= LYR_ENTRY_SIZE;
                continue;
            }
            break;   // cursor entry sorts BEFORE name → insert at `slot` (unchanged)
        }
    }

    LyrEntry_t *entry = (LyrEntry_t *)&lyrMtlGlob.Layers[LYR_ENTRY_SIZE * slot];
    memset( entry, 0, LYR_ENTRY_SIZE );
    entry->layerCount = 0;
    strcpy( entry->name, name );

    lyrMtlWndGlob.activeLyrMtl = entry;
    lyrMtlWndGlob.selectedLayerIndex  = 0;
    g_nUpdateBits |= 0x10u;   // W_TEXTURE
    ++lyrMtlGlob.entryCount;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x416f40  LayeredMaterials_Save
// **DISK-MUTATING** (Directive #4).  If the library's CRC differs from the clean-load
// token (i.e. the operator modified it), write it to the path named by the worldspawn
// "layeredmaterials" project epair, then re-baseline the token.  Returns 1 on success
// or when nothing was modified (the round-trip-gate path: empty library → CRC matches
// the 0 token → no write); 0 on a write failure (which aborts the whole map save).
//
// SAFETY: the CRC gate means this only writes when the operator has actually changed
// the library through the authoring window (which is operator-attended), so in a
// NORMAL (operator) build it writes.
// ─────────────────────────────────────────────────────────────────────────────
char LayeredMaterials_Save()
{
    // lyrMtlGlob.crcToken is the clean-library CRC token.  When the
    // current CRC matches it, nothing was modified → return 1 without writing.  The
    // headless round-trip gate has an empty library (entryCount==0 → CRC of 0 bytes ==
    // 0 == the initial token) so this is the path it always takes.
    if ( CheckLayeredMaterial_Modifications( lyrMtlGlob.Layers, LYR_ENTRY_SIZE * lyrMtlGlob.entryCount, 0 )
         != (unsigned)lyrMtlGlob.crcToken )
    {
        // Resolve the destination path from the project's "layeredmaterials" epair.
        const char *value = "";
        epair_t *ep = g_qeglobals.d_project_entity ? g_qeglobals.d_project_entity->epairs : nullptr;
        for ( ; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, "layeredmaterials" ) ) { value = ep->value; break; }
        }

        FILE *f = Map_OpenForSave( value );
        char ok = 0;
        if ( f )
        {
            ok = LayeredMaterials_WriteFile( f );
            fclose( f );
        }
        if ( !f || !ok )
        {
            Sys_Printf( "ERROR!!!! Failed to save layeredmaterials\n" );
            return 0;
        }
        // Re-baseline the clean-library token to the just-written contents.
        lyrMtlGlob.crcToken = (int)CheckLayeredMaterial_Modifications( lyrMtlGlob.Layers,
                                                                 LYR_ENTRY_SIZE * lyrMtlGlob.entryCount, 0 );
    }
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// LayeredMaterials_texcoords (0x417190) deletes an entry after replacing its uses
// with "$default", including referenced prefab maps.
// ─────────────────────────────────────────────────────────────────────────────
extern bool FindReplaceTextures( const char *find, const char *replace, char flags );  // findtexture.cpp 0x493160
extern void FindReplaceVisited_Reset();  // findtexture.cpp (Set_EraseTreeRec + reset sentinels)

void *LayeredMaterials_texcoords( char *a1 )
{
    // (1) reset the prefab-recursion visited set: the std::set<std::string> the flag&4
    //     prefab-recursion branch of FindReplaceTexture_Brush consults (findtexture.cpp).
    FindReplaceVisited_Reset();

    // (2) replace all uses of the deleted material with "$default" (flag&4 = recurse prefabs).
    FindReplaceTextures( a1, "$default", 4 );

    // (3) validate the entry index.
    int entryCount = lyrMtlGlob.entryCount;
    size_t entryIndex = (size_t)( a1 - (char *)lyrMtlGlob.Layers ) / sizeof(LyrEntry_t);
    bcassert( entryIndex, (unsigned int)lyrMtlGlob.entryCount );   // LayeredMaterials.cpp:297

    // (4) drop the entry: shrink count, shift the remaining entries down.
    lyrMtlGlob.entryCount = entryCount - 1;
    void *result = memmove( &lyrMtlGlob.Layers[sizeof(LyrEntry_t) * entryIndex],
                            &lyrMtlGlob.Layers[sizeof(LyrEntry_t) * ( entryIndex + 1 )],
                            sizeof(LyrEntry_t) * ( entryCount - 1 - entryIndex ) );

    // (5) request a redraw.
    g_nUpdateBits |= 0x10;   // W_TEXTURE
    return result;
}
