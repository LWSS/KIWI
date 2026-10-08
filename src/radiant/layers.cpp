#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Layer map storage, visibility flags, and .map serialization.

#include "stdafx.h"
#include "qe3.h"
#include <map>
#include <set>
#include <vector>
#include <tuple>
#include <string>
#include <utility>
#include <cstdio>
#include <universal/q_parse.h>   // Com_Parse, Com_ParseOnLine, Com_UngetToken
#include "kiwi_ported.h"         // KIWI: shared engine/editor externs

extern char  g_activeLayer_string[];   // engine_stubs.cpp (256 bytes)
extern qeglobals_t g_qeglobals;        // qe3.cpp
extern "C" int _stricmp( const char *, const char * );

// ─── layerMap: layer name → flag bits ────────────────────────────────────────
// std::map is sorted by key (operator<), which reproduces the binary's RB-tree
// iteration order and therefore the on-disk layer-line order.
static std::map<std::string, int> layerMap;

// Layer flag bits:
//   hidden=1  prefab=2  expanded=4  frozen=8 ; 0x10 = internal prefab-dirty (not persisted)
static int LayerFlagFromName( const char *name )   // 0x418C10
{
    if ( !strcmp( name, "hidden"   ) ) return 1;
    if ( !strcmp( name, "prefab"   ) ) return 2;
    if ( !strcmp( name, "expanded" ) ) return 4;
    if ( !strcmp( name, "frozen"   ) ) return 8;
    return 0;
}

static void LayerFlagsToString( char *out, int flags )   // 0x418C80
{
    out[0] = '\0';
    if ( flags & 1 ) { strcat( out, "hidden"   ); strcat( out, " " ); }
    if ( flags & 2 ) { strcat( out, "prefab"   ); strcat( out, " " ); }
    if ( flags & 4 ) { strcat( out, "expanded" ); strcat( out, " " ); }
    if ( flags & 8 ) { strcat( out, "frozen"   ); strcat( out, " " ); }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x419630  Layers_AddLayerPath — register a hierarchical layer PATH.
// The path is split on '/' and EVERY cumulative prefix is looked up: a missing prefix
// is inserted with `flags & 2`, and every visited node has the internal 0x10 "dirty /
// unreferenced" bit CLEARED.  The LEAF finally ORs in `flags & ~0x10`.
// ─────────────────────────────────────────────────────────────────────────────
void Layers_AddLayerPath( const char *name, int flags )
{
    if ( !name ) return;

    char work[1028];
    strncpy( work, name, sizeof( work ) - 1 );
    work[sizeof( work ) - 1] = '\0';

    char acc[1024];
    acc[0] = '\0';

    std::map<std::string, int>::iterator node = layerMap.end();

    for ( char *tok = strtok( work, "/" ); tok; tok = strtok( nullptr, "/" ) )
    {
        strcat( acc, tok );
        auto it = layerMap.find( std::string( acc ) );
        if ( it == layerMap.end() )
            it = layerMap.insert( std::make_pair( std::string( acc ), flags & 2 ) ).first;
        it->second &= ~0x10;
        node = it;
        strcat( acc, "/" );
    }
    // The LEAF carries the caller's flags (0x10 is never persisted).  An empty path does
    // nothing (the binary hit _invalid_parameter_noinfo).
    if ( node != layerMap.end() )
        node->second |= ( flags & ~0x10 );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x419310  Layers_TagListLayers — walk ONE brush list, register each brush's layer
// path, and RECURSE into every placed prefab's content list.
//   path      = prefix ++ brush->def->parent_layer_string   -> Layers_AddLayerPath
//               with flags 2 at a prefab level, 0 at the top level.
//   childPath = prefix ++ strlwr(<"model" epair of the owner's def>)
// The recursion runs when the prefab's own layer key is ABSENT from the map or still
// carries the 0x10 dirty bit; the child prefix then gets a trailing '/' and the
// prefab's content list is walked at level 1.
// ─────────────────────────────────────────────────────────────────────────────
static void Layers_TagListLayers( selbrush_t *listHead, const char *prefix, char isPrefabLevel )
{
    for ( selbrush_t *b = listHead->next; b && b != listHead; b = b->next )
    {
        brush_t *def = b->def;
        if ( !def )
            continue;

        char path[1024];
        strncpy( path, prefix, sizeof( path ) - 1 );
        path[sizeof( path ) - 1] = '\0';
        if ( def->parent_layer_string )
            strncat( path, def->parent_layer_string, sizeof( path ) - strlen( path ) - 1 );
        Layers_AddLayerPath( path, isPrefabLevel ? 2 : 0 );

        entity_s *owner = b->owner;
        if ( !owner || !owner->prefab )
            continue;

        char childPath[1024];
        strncpy( childPath, prefix, sizeof( childPath ) - 1 );
        childPath[sizeof( childPath ) - 1] = '\0';

        // the "model" epair off the owner's DEF; "" when absent
        const char *model = "";
        entity_s *eDef = owner->def;
        if ( eDef )
        {
            for ( epair_t *ep = eDef->epairs; ep; ep = ep->next )
                if ( !_stricmp( ep->key, "model" ) ) { model = ep->value ? ep->value : ""; break; }
        }
        char modelLc[1024];
        strncpy( modelLc, model, sizeof( modelLc ) - 1 );
        modelLc[sizeof( modelLc ) - 1] = '\0';
        _strlwr( modelLc );
        strncat( childPath, modelLc, sizeof( childPath ) - strlen( childPath ) - 1 );

        // recurse when the prefab's layer key is missing or still dirty
        auto it = layerMap.find( std::string( childPath ) );
        const bool needsWalk = ( it == layerMap.end() ) || ( it->second & 0x10 ) != 0;
        if ( !needsWalk )
            continue;

        strncat( childPath, "/", sizeof( childPath ) - strlen( childPath ) - 1 );
        Layers_TagListLayers( &((prefab_s *)owner->prefab)->brushes, childPath, 1 );   // the content list
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x4187A0  Map_InitlLayers — reset active layer + clear the layer map.
// (The original also pokes a layers-dialog window via SendMessageA; UI-only.)
// ─────────────────────────────────────────────────────────────────────────────
void Map_InitlLayers()
{
    strcpy( g_activeLayer_string, "000_Global" );
    layerMap.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x418DD0  Layers_SetMapLayers — the full layer RESYNC.  Three phases:
//   1. ensure the two built-in layers exist, flags 0 (std::map::operator[] inserts 0
//      only if the key is absent, so a layer already parsed from the file keeps its
//      flags);
//   2. walk the three brush display lists through Layers_TagListLayers, which
//      registers every referenced layer path — including the recursion into placed
//      prefabs' content lists — and CLEARS the internal 0x10 "dirty" bit on everything
//      it touches;
//   3. ERASE every layer still carrying 0x10 — i.e. the layers the load-time
//      Layers_TagModelPath tagging marked that no live brush or prefab actually
//      references any more.
// Phases 2+3 are a pair: 3 without 2 would delete live layers.  The 0x10 bit is set
// by Map_ParseLayers (a `flags prefab` layer) and by Layers_TagModelPath (per loaded model
// path) — e.g. blackout.map's "blackout_geo.map" / "prefabs" layers, whose bit is
// cleared again by the phase-2 prefab recursion walking "blackout_geo.map/<layer>".
// ─────────────────────────────────────────────────────────────────────────────
void Layers_SetMapLayers()
{
    layerMap[ "The Map"    ];   // insert with 0 if absent
    layerMap[ "000_Global" ];

    Layers_TagListLayers( &active_brushes,   "", 0 );
    Layers_TagListLayers( &selected_brushes, "", 0 );
    Layers_TagListLayers( &filtered_brushes, "", 0 );

    // drop every still-dirty layer
    for ( auto it = layerMap.begin(); it != layerMap.end(); )
    {
        if ( ( it->second & 0x10 ) != 0 )
            it = layerMap.erase( it );
        else
            ++it;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Prefab-edit-in-place layer-state stack.  The binary deep-copies the layer map into
// the prefab-stack slot on ENTER and back on LEAVE; here that is std::map assignment.
// g_activeLayer_string (the layer *name*) is saved in the prefab slot by map.cpp, so
// only the map<name,flags> is stacked here.  Indexed by the post-push / pre-pop prefab
// level (max 16, like the binary's stack).
// ─────────────────────────────────────────────────────────────────────────────
static std::map<std::string, int> g_prefabLayerStack[16];

void Layers_SavePrefabLayers( int level )      // ENTER: snapshot before swapping to sub-map
{
    if ( level < 0 || level >= 16 )
        return;
    g_prefabLayerStack[ level ] = layerMap;
}

void Layers_RestorePrefabLayers( int level )   // LEAVE: restore parent map's layers
{
    if ( level < 0 || level >= 16 )
        return;
    layerMap = g_prefabLayerStack[ level ];
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x41A0E0  Layers_RefreshDialog — refresh the layers list from layerMap (the binary
// rebuilt its tree view); a no-op when it was never opened.
// ─────────────────────────────────────────────────────────────────────────────
extern void LayersDlg_RefreshIfOpen();          // layersdlg.cpp
void Layers_RefreshDialog()
{
    LayersDlg_RefreshIfOpen();
}

// 0x41C9C0  Layers_HighlightActive(name) — mark the active layer in the list.  The marker
// is drawn from g_activeLayer_string, so this is a refresh.  (Caller: map.cpp after the
// map-load layer parse.)
void Layers_HighlightActive( const char * /*name*/ )
{
    LayersDlg_RefreshIfOpen();
}

// ─────────────────────────────────────────────────────────────────────────────
// Layer block writer — extracted from Map_SaveFile (0x486C00).
// For each layer (sorted), unless flags == 2 (a transient prefab-only layer),
// emit:  "<name>" flags <flagstr> [ active]\n
// The flagstr keeps its trailing space, as the binary wrote it.
// ─────────────────────────────────────────────────────────────────────────────
void Layers_WriteToFile( FILE *f )
{
    for ( const auto &kv : layerMap )
    {
        int flags = kv.second & ~0x10;   // 0x10 is internal
        if ( kv.second == 2 )            // transient prefab-only layer — skip
            continue;

        char flagstr[64];
        fprintf( f, "\"%s\" flags", kv.first.c_str() );
        LayerFlagsToString( flagstr, flags );
        fprintf( f, " %s", flagstr );
        if ( !strcmp( g_activeLayer_string, kv.first.c_str() ) )
            fprintf( f, " active" );
        fprintf( f, "\n" );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Map-file header parsers (live in map.cpp in the original; grouped here next to
//  the layer logic they feed). All take the live text pointer and drive the
//  shared q_parse session (Com_Parse advances both *text and g_parse state).
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 0x4861F0  Map_ReadVersion — read the leading `iwmap <n>`.
// Returns the version number, 0 if no version token, -1 on empty file.
// ─────────────────────────────────────────────────────────────────────────────
int Map_ReadVersion( const void **text )
{
    parseInfo_t *tok = Com_Parse( (const char **)text );
    if ( !tok->token[0] )
    {
        Sys_Printf( "WARNING: empty map\n" );
        return -1;
    }
    if ( !strcmp( tok->token, "iwmap" ) )
    {
        parseInfo_t *ver = Com_ParseOnLine( (const char **)text );
        return atoi( ver->token );
    }
    Com_UngetToken();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x486280  Map_ParseLayers — parse the layer-declaration block.
// Each line: "<name>" flags [flagnames...] [active]   until the first '{'.
// `skipFlags` suppresses flag processing (used for prefab re-import); the normal
// map-load path passes 0.
// ─────────────────────────────────────────────────────────────────────────────
void Map_ParseLayers( const void **text, int skipFlags )
{
    const char **t = (const char **)text;

    for ( ;; )
    {
        parseInfo_t *nameTok = Com_Parse( t );          // crossline: the layer name (or '{')
        if ( !*t || nameTok->token[0] == '{' )
            break;

        char layerName[1028];
        strncpy( layerName, nameTok->token, sizeof( layerName ) - 1 );
        layerName[sizeof( layerName ) - 1] = '\0';

        parseInfo_t *kw = Com_ParseOnLine( t );         // "flags"
        if ( !skipFlags && !strstr( kw->token, ";" ) && !strcmp( kw->token, "flags" ) )
        {
            int  flags  = 0;
            bool active = false;
            for ( parseInfo_t *f = Com_ParseOnLine( t ); f->token[0]; f = Com_ParseOnLine( t ) )
            {
                if ( !strcmp( f->token, "active" ) )
                    active = true;
                else
                    flags |= LayerFlagFromName( f->token );
            }

            Layers_AddLayerPath( layerName, flags );
            if ( flags & 2 )
                Layers_SetFlag( layerName, 16 );   // internal prefab-dirty bit
            if ( active )
                strcpy( g_activeLayer_string, layerName );
        }
    }

    // Unget the '{' so ParseEntity reads the first entity's opening brace.
    Com_UngetToken();
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x42F8A0  Map_ParseEntityLayerKey — peek for an optional leading
// `<key> "<value>"` line inside an entity block; write the value (or `def`) to
// `out`. Used by ParseEntity to read the per-entity `layer "..."` line.
// ─────────────────────────────────────────────────────────────────────────────
void Map_ParseEntityLayerKey( const char **text, const char *def, char *out, const char *key )
{
    parseInfo_t *tok = Com_Parse( text );
    if ( !strcmp( tok->token, key ) )
    {
        parseInfo_t *val = Com_ParseOnLine( text );
        if ( out )
        {
            if ( val->token[0] )
                strcpy( out, val->token );
            else
                strcpy( out, def );
        }
    }
    else
    {
        Com_UngetToken();
        if ( out )
            strcpy( out, def );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  LAYERS DIALOG BACKEND (layersdlg.cpp drives these — the unit's core).
//  All operate on the same layerMap + g_activeLayer_string the writer/parser use,
//  so every dialog op round-trips through the .map.
// ═════════════════════════════════════════════════════════════════════════════

// ── enumeration: copy the (sorted) layer names + flags out for the dialog list ──
// std::map iteration is already in operator< order == the on-disk + tree order.
void Layers_Enumerate( std::vector<std::pair<std::string,int>> &out )
{
    out.clear();
    for ( const auto &kv : layerMap )
        out.push_back( kv );
}
bool        Layers_Exists( const char *name )
{
    return layerMap.find( std::string( name ) ) != layerMap.end();
}
const char *Layers_GetActive() { return g_activeLayer_string; }

// ── 0x4190F0 layers_01 / 0x419200 layers_02 — generic set/clear-flag-by-name ──
// Both bump g_layerCount_maybe when the flag touches bit 0 or 3 (hidden / frozen).
void Layers_SetFlag( const char *name, int flag )      // layers_01 0x4190F0
{
    auto mapIter = layerMap.find( std::string( name ) );
    if ( mapIter == layerMap.end() )
    {
        iassert( mapIter != layerMap.end() );   // layers.cpp:203
        return;
    }
    mapIter->second |= flag;
    if ( flag & 9 )
        ++g_qeglobals.g_layerCount_maybe;
}
void Layers_ClearFlag( const char *name, int flag )    // layers_02 0x419200
{
    auto mapIter = layerMap.find( std::string( name ) );
    if ( mapIter == layerMap.end() )
    {
        iassert( mapIter != layerMap.end() );   // layers.cpp:218
        return;
    }
    mapIter->second &= ~flag;
    if ( flag & 9 )
        ++g_qeglobals.g_layerCount_maybe;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x418A50  Layers_TagModelPath — OR the internal 0x10 "model/prefab-managed" bit into
// every layer whose NAME starts with `path` (strncmp over strlen( path ) - an empty path,
// from an entity without a "model" key, tags every layer).  It does not bump
// g_layerCount_maybe.  Callers: Map_LoadFromFile and the prefab PrevLevel restore
// (map.cpp), once per model entity's "model" path - they tag the layers a loaded
// model/prefab owns (the inverse of Layers_AddLayerPath, which clears 0x10 on the paths it
// touches).
// ─────────────────────────────────────────────────────────────────────────────
void Layers_TagModelPath( const char *path )
{
    size_t plen = strlen( path );
    for ( auto &kv : layerMap )
    {
        if ( strncmp( kv.first.c_str(), path, plen ) == 0 )
            kv.second |= 0x10;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x4188A0 / 0x418B20 — layerMap exact-name lookups: bit 0 ("hidden") / bit 3
// ("frozen") of the layer's flags, 0 on a miss.  Used by Brush_UpdateLayerVis (the
// binary's sole caller) and Layers_RebuildVisibilityFilters.
// ─────────────────────────────────────────────────────────────────────────────
static int Layers_KeyIsHidden( const char *key )     // 0x4188A0
{
    auto mapIter = layerMap.find( std::string( key ) );
    return ( mapIter == layerMap.end() ) ? 0 : ( mapIter->second & 1 );
}
static bool Layers_KeyIsFrozen( const char *key )    // 0x418B20
{
    auto mapIter = layerMap.find( std::string( key ) );
    return mapIter != layerMap.end() && ( mapIter->second & 8 ) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x418990  Brush_UpdateLayerVis — refresh a brush INSTANCE's layer-derived
// brushFlags from the layer map: bit 0x2 (layer-hidden — DrawBrush skips the
// brush) from the layer's "hidden" flag (bit 0), and bit 0x20 (layer-frozen —
// the pick/select paths skip the brush) from the layer's "frozen" flag (bit 3).
// Lazy per-draw refresh cached against g_layerCount_maybe (the layer-change
// generation counter bumped by Layers_SetFlag/ClearFlag on flag bits 1|8).
// The lookup key is layerPrefix ++ def->parent_layer_string — layerPrefix is
// "" for world brushes, and the accumulated "<model path lc>/" chain for brushes
// drawn inside placed prefabs (built by DrawBrush_PrefabContents).  Called at the
// head of DrawBrush.
// ─────────────────────────────────────────────────────────────────────────────
void Brush_UpdateLayerVis( selbrush_t *b, const char *layerPrefix )
{
    if ( b->xx7 == g_qeglobals.g_layerCount_maybe )
        return;

    // KIWI: a std::string - strcpy / strcat into char[1028] overran on deep prefab prefixes
    std::string key = layerPrefix;
    if ( b->def->parent_layer_string )
        key += b->def->parent_layer_string;

    if ( Layers_KeyIsHidden( key.c_str() ) )
        b->brushFlags |= 2;
    else
        b->brushFlags &= ~2u;
    if ( Layers_KeyIsFrozen( key.c_str() ) )
        b->brushFlags |= 0x20;
    else
        b->brushFlags &= ~0x20u;

    b->xx7 = g_qeglobals.g_layerCount_maybe;
}

// ── brush re-layer worker (0x41A010) ───────────────────────────────────────────
// Walk one DISPLAY brush list; for each instance whose def's parent_layer_string
// equals `oldName`, set it to `newName` (Brush_SetInstanceLayerString also resets the
// instance's cached layer state, xx7).
extern void Brush_SetInstanceLayerString( selbrush_t *inst, const char *str ); // brush.cpp 0x4758E0
static void Layers_RelayerList( const char *newName, selbrush_t *sentinel, const char *oldName )
{
    for ( selbrush_t *b = sentinel->next; b != sentinel; b = b->next )
    {
        if ( b->def && b->def->parent_layer_string
             && !strcmp( b->def->parent_layer_string, oldName ) )
            Brush_SetInstanceLayerString( b, newName );
    }
}

// ── 0x419940 Layers_Update_01 — DELETE a layer (+ its sub-layers) ─────────────
// For every layer whose name has `victim` as a path prefix (next char '/' or end):
// move its brushes to the active layer (000_Global if the active layer was the
// victim), erase the node.  Returns the number of layers removed.
int Layers_DeleteLayer( const char *victim )
{
    size_t vlen = strlen( victim );
    std::vector<std::string> toErase;

    for ( const auto &kv : layerMap )
    {
        const std::string &nm = kv.first;
        if ( nm.compare( 0, vlen, victim ) == 0 &&
             ( nm.size() == vlen || nm[vlen] == '/' ) )
            toErase.push_back( nm );
    }

    for ( const std::string &nm : toErase )
    {
        // If the active layer is being deleted, fall back to 000_Global first.
        if ( !strcmp( g_activeLayer_string, nm.c_str() ) )
            strcpy( g_activeLayer_string, "000_Global" );

        // Re-tag every brush off the victim layer onto the (new) active layer.
        Layers_RelayerList( g_activeLayer_string, &active_brushes,   nm.c_str() );
        Layers_RelayerList( g_activeLayer_string, &selected_brushes, nm.c_str() );
        Layers_RelayerList( g_activeLayer_string, &filtered_brushes, nm.c_str() );

        layerMap.erase( nm );
    }

    Sys_Printf( "Updating layers...\n" );
    Layers_SetMapLayers();   // re-ensure The Map / 000_Global exist
    Layers_RefreshDialog();
    return (int)toErase.size();
}

// ── 0x419BE0 Layers_Update_02 — RENAME a layer's path prefix ──────────────────
// Every layer under the old full name (the name itself, or followed by '/') moves under
// the new full name ("<parentPath>/<newLeaf>", or "<newLeaf>" at the root); the caller
// builds both from the picked layer and the typed name.
int Layers_RenameLayer( const char *oldFull, const char *newFull )
{
    if ( !strcmp( oldFull, newFull ) || !strcmp( oldFull, "000_Global" ) )
        return 0;

    size_t olen = strlen( oldFull );
    // Collect (oldName, newName, flags) for every affected layer (prefix match).
    std::vector<std::tuple<std::string,std::string,int>> moves;
    for ( const auto &kv : layerMap )
    {
        const std::string &nm = kv.first;
        if ( nm.compare( 0, olen, oldFull ) == 0 &&
             ( nm.size() == olen || nm[olen] == '/' ) )
        {
            std::string nn = std::string( newFull ) + nm.substr( olen ); // keep the suffix
            moves.emplace_back( nm, nn, kv.second );
        }
    }

    for ( auto &mv : moves )
    {
        const std::string &on = std::get<0>( mv );
        const std::string &nn = std::get<1>( mv );
        int flags = std::get<2>( mv );

        Layers_RelayerList( nn.c_str(), &active_brushes,   on.c_str() );
        Layers_RelayerList( nn.c_str(), &selected_brushes, on.c_str() );
        Layers_RelayerList( nn.c_str(), &filtered_brushes, on.c_str() );

        if ( !strcmp( g_activeLayer_string, on.c_str() ) )
            strncpy( g_activeLayer_string, nn.c_str(), 255 );

        layerMap.erase( on );
        layerMap[ nn ] |= ( flags & ~0x10 );
    }

    Sys_Printf( "Updating layers...\n" );
    Layers_SetMapLayers();
    Layers_RefreshDialog();
    return (int)moves.size();
}

// ── set the active layer (CLayerDlg::SetActiveLayer 0x41C610 core) ────────────
void Layers_SetActive( const char *name )
{
    strncpy( g_activeLayer_string, name, 255 );
    g_activeLayer_string[255] = '\0';
}

// ── collect the unique entity script_layer values (0x41C200's dedup core) ─────
// The visibility filters come from the set of `script_layer` epair values carried
// by the live brushes' owning entities (NOT the layerMap — the layerMap drives the
// layer list; the filters drive FilterBrush).  This walks the active + selected
// display lists, gathers the unique values, and (re)builds one script_layer filter
// per value via Layers_BuildFilter (filters.cpp).
extern void        Layers_FreeVisibilityFilters();          // filters.cpp
extern const char *Layers_BuildFilter( const char *name );  // filters.cpp 0x411950

static void Layers_CollectScriptLayers( selbrush_t *sentinel, std::set<std::string> &seen )
{
    for ( selbrush_t *b = sentinel->next; b != sentinel; b = b->next )
    {
        if ( !b || !b->owner ) continue;
        entity_s *ent = (entity_s *)b->owner->def;
        if ( !ent ) continue;
        const char *val = nullptr;
        for ( epair_t *ep = ent->epairs; ep; ep = ep->next )
            if ( !_stricmp( ep->key, "script_layer" ) ) { val = ep->value; break; }
        if ( val && val[0] )
            seen.insert( std::string( val ) );
    }
}

void Layers_RebuildVisibilityFilters()   // 0x41C200 backend
{
    Layers_FreeVisibilityFilters();

    std::set<std::string> seen;
    Layers_CollectScriptLayers( &active_brushes,   seen );
    Layers_CollectScriptLayers( &selected_brushes, seen );

    for ( const std::string &nm : seen )
    {
        Layers_BuildFilter( nm.c_str() );
        // KIWI: seed from the layer flag; a rebuild re-showed every other hidden layer
        if ( Layers_KeyIsHidden( nm.c_str() ) )
            g_qeglobals.d_filterGlobals_layerFilters->isShown = false;   // BuildFilter prepends
    }
}
