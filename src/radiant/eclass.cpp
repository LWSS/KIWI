#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\eclass.cpp

#include "stdafx.h"
#include <io.h>                     // _findfirst64i32 / _findnext64i32 / _findclose
#include <universal/q_parse.h>       // ParseThreadInfo, Com_ParseExt, parseInfo_t
#include <universal/com_files.h>     // FS_ListFiles / FS_FreeFileList / FsListBehavior_e (weapon scan)
#include <gfx_d3d/r_model.h>         // R_RegisterModel
#include <string>                    // std::string locals (Prefab_Load/Init_CStr)
#include <vector>                    // Eclass_LoadModel: std::vector<std::string>
#include "kiwi_ported.h"             // KIWI: shared engine/editor externs

// ── external Radiant/engine function declarations ─────────────────────────────
// These are functions in other Radiant compilation units or the engine subset
// that are not yet declared in any header available to this TU.

// common.cpp (0x49A9E0)
void FatalError(int code, const char *fmt, ...);

// entity.cpp (0x483C80)
void Entity_Free_R(entity_s *e);

// map.cpp (0x486500) — load entities from file into linked list
int Map_LoadEntities(const char *filename, entity_s *entList, char skipLayerFlags);

// qe3.cpp (0x48B030)
char *AllocMaterialString(const char *src);

// FS helpers — declared with C++ linkage to match the kisak build.
// The names match C++ mangling in the kisak link map.
int  FS_ReadFile(const char *path, void **buf);
void Hunk_FreeTempMemory(char *buf);
extern int fs_loadStack;
// fs_searchpaths: forward-declare as void* for a null check only.
// The real definition is searchpath_s* in com_files.cpp (in the radiant build subset).
// Both are pointer-sized; the linker will match via name mangling.
struct searchpath_s;
extern searchpath_s *fs_searchpaths;

// Epair_SetRaw (0x483500) - raw epair-list set (defined below).
void Epair_SetRaw(epair_t **head, const char *key, const char *value);

// Prefab_Load (0x480AB0) — load a prefab .map into the model sub-node (defined after
// Eclass_RealizeModel); called by Eclass_LoadModel for classtype&0x10 (CLASS_PREFAB) eclasses.
char Prefab_Load(const char *path, modelnode_t **node);

// Eclass_GetToken (0x48CA40) — the editor's own token parser. Writes the next
// whitespace/quote/special-delimited token into g_eclassToken (0x2409CE0) and returns the
// advanced pointer; sets g_eclassTokenEof (0x23F1CDC) at end of input. Used by
// Eclass_InitFromText.
static char g_eclassToken[1024];
static int  g_eclassTokenEof = 0;
static char *Eclass_GetToken(char *text);

// ── globals ────────────────────────────────────────────────────────────────────
// Primary eclass linked list (alphabetized).  Corresponds to GtkRadiant 'eclass'.
eclass_t   *g_eclass     = nullptr;   // 0x025D5B20

// Fallback eclass returned for unknown entity names ("UNKNOWN_CLASS").
// Populated by Eclass_InitForSourceDirectory after scanning all .def files.
eclass_t   *eclass_bad   = nullptr;   // 0x025D5B24

// Per-map model eclass list (singly-linked via eclass_t::next cast to int).
// Each entry is a models_t* that has been allocated for a map entity model.
models_t   *g_models     = NULL;      // 0x025D5B28

// When non-zero, Init_ScanFiles stops after the first /*QUAKED block found.
int         g_eclassStopOnFirst = 0;  // 0x025D5B2C

// KIWI: the binary's build-list mode (g_bBuildList, 0x025D5B05: dump every QUAKED block to
// newdefs.def through MFC CString internals) is gone - nothing ever set it.

// Last eclass successfully parsed in Init_ScanFiles.
eclass_t   *g_lastParsedEclass  = NULL; // 0x023F1728
// Non-zero once the first /*QUAKED block has been found in the current scan.
int         g_eclassParsedAny   = 0;  // 0x023F172C
// Points to the name field of the eclass currently being built (for Assert msgs).
const char *g_eclassCurrentName = NULL; // 0x023F1730

// Resolved filepath set by Eclass_InitForSourceDirectory before each Init_ScanFiles call
// (the original passed it in a register; not an original global).
char        g_scanFilePath[1024];


// ── forward declarations ───────────────────────────────────────────────────────
static void Eclass_FreeEntityList(modelnode_t *model);
static void Eclass_FreeEpairs(models_t *model);
static void Eclass_FreeAll();


// ─────────────────────────────────────────────────────────────────────────────
// 0x4806D0  Eclass_FreeEntityList
// Free all entity_s nodes in the circular entity list embedded in a model sub-node.
// Sub-node layout (4-byte words):
//   [+0x00]  void *next_subnode   (next in models_t::x88 linked list)
//   [+0x08]  sentinel back-link   (the list is empty when [+0x0C] == &node[+0x08])
//   [+0x0C]  entity_s *entities_next  (first entity in circular list)
// For each entity its brush list is walked and each brush's refCount (brush_t +0x1C)
// decremented, then the entity's refCount (entity_s +0x88) is decremented and the entity freed.
// ─────────────────────────────────────────────────────────────────────────────
static void Eclass_FreeEntityList(modelnode_t *model)
{
    iassert( model->entities.next );   // eclass.cpp:39

    // Circular entity list: first at entities.next (+0xC), sentinel &x2 (+0x8) —
    // the same head/sentinel overlay as entity_s def(+0x08)/brushes(+0x0C).
    while ( model->entities.next != &model->entities )
    {
        entity_s *e = (entity_s *)model->entities.next;
        iassert( e->refCount == 1 );   // eclass.cpp:43

        // Walk the entity's brush circular list (sentinel = &e->def, same overlay).
        for ( brush_t *b = (brush_t *)e->brushes.prev; b != (brush_t *)&e->def; b = b->onext )
            --b->refCount;

        --e->refCount;
        Entity_Free_R( e );
    }

    model->entities.next = 0;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480760  Eclass_FreeEpairs
// Free the key/value epair linked list at model->x95 (+380).
// Node layout: [+0] void *next, [+4] char *key, [+8] char *value.
// ─────────────────────────────────────────────────────────────────────────────
static void Eclass_FreeEpairs(models_t *model)
{
    epair_t *node = model->xx8;
    while ( node )
    {
        epair_t *next = node->next;
        free(node->key);
        free(node->value);
        free(node);
        node = next;
    }
    model->xx8 = NULL;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x4807A0  Eclass_CheckEntities
// Completely free a models_t node:
//   1. Free every entity-group sub-node in model->x88.
//   2. Free material/string fields (x94, x89, x90-x93).
//   3. Free epairs (via Eclass_FreeEpairs at offset 380).
//   4. Free model->handle and model->x15.
//   5. Free the models_t node itself.
//
// models_t field index mapping (field name → word index → byte offset):
//   x88 = [88] → 0x160,  x94 = [94] → 0x178,  x89 = [89] → 0x164,
//   x90 = [90] → 0x168,  x91 = [91] → 0x16C,  x92 = [92] → 0x170,
//   x93 = [93] → 0x174,  handle = [1] → 0x04,  x15 = [15] → 0x3C
// ─────────────────────────────────────────────────────────────────────────────
void Eclass_CheckEntities(models_t *model)
{
    modelnode_t *sub = (modelnode_t *)model->xx1;
    while ( sub )
    {
        modelnode_t *next = (modelnode_t *)sub->next;
        if ( sub->entities.next )   // non-zero means the entity list is populated
        {
            Eclass_FreeEntityList( sub );
            {
                modelnode_t *model = sub;
                iassert( model->entities.next == NULL );   // eclass.cpp:88
            }
        }
        free( sub );
        sub = next;
    }

    free((void *)model->modelpath);
    free((void *)model->default_model_name);

    for ( int i = 1; i < 5; ++i )
    {
        free((void *)model->cycleModelName[i]);
    }

    Eclass_FreeEpairs( model );
    free((void *)model->name);   // heap NAME string, NOT an XModel* — BY3-audit note at EOF
    free((void *)model->comments);
    free(model);
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480920  Eclass_RealizeModel
// Load a .map/.ent file and attach its entities to the eclass node.
// Returns 1 on success, 0 on failure.
//   nodePtr = pointer-TO the sub-node pointer (Prefab_Load passes &node); *nodePtr is
//             the models_t/eclass sub-node, its entity-list sentinel at (*nodePtr)+8.
//   path    = file path.
//
// After loading, for each entity in the list: bump refCount to 1, rebuild brush
// windings (Brush_BuildWindings), and optionally refresh vertex selection.
// ─────────────────────────────────────────────────────────────────────────────
static char Eclass_RealizeModel(modelnode_t **nodePtr, const char *path)
{
    modelnode_t *node = *nodePtr;
    entity_s *list = &node->entities;
    entity_s *sentinel = list;
    list->next = list;
    list->prev = list;

    // Prefab-load material gate: the prefab's brushes/patches resolve their materials
    // through SetMaterial during the parse below; force the degenerate (wireframe)
    // material path for them so the live CoD4 material-asset loader (broken for CoD4
    // water materials in the CoD3-sized kisak engine — see materialdef.cpp) is not hit.
    // Counter so nested prefab realizes stay degenerate; restored after the parse.
    extern int g_radiantLoadingPrefab;    // materialdef.cpp
    ++g_radiantLoadingPrefab;
    int loadedOk = Map_LoadEntities(path, list, 1);
    --g_radiantLoadingPrefab;
    if ( loadedOk )
    {
        // KIWI: the p4 "known to the depot" mark (fileExists) is gone with Perforce.

        // Walk all loaded entities (circular list).
        entity_s *e = list->next;
        if ( e != list )
        {
            do
            {
                ++e->refCount;
                iassert( e->refCount == 1 );   // eclass.cpp:159

                // Walk the brush list: first = brushes.prev (entity+0x0C), sentinel &def.
                for ( brush_t *b = (brush_t *)e->brushes.prev;
                      b != (brush_t *)&e->def;
                      b = b->onext )
                {
                    ++b->refCount;
                    Brush_BuildWindings(b, 1);
                    // The prefab's brushes were parsed with DEGENERATE face materials
                    // (g_radiantLoadingPrefab forced the shim to dodge the CoD4-water-material
                    // AV).  Realize each face's material by name ONCE, here at load time
                    // (outside any draw), so the prefab-content walls render textured instead
                    // of as a white void.  Guarded against an unreadable CoD4 asset (ERR_DROP →
                    // that brush stays degenerate, no crash).  See brush.cpp
                    // Brush_RealizeFaceMaterials for why draw-time realize regressed.
                    extern void Brush_RealizeFaceMaterialsGuarded(brush_t *def);  // engine_stubs.cpp
                    Brush_RealizeFaceMaterialsGuarded(b);
                    if ( g_qeglobals.d_select_mode == sel_vertex ||
                         g_qeglobals.d_select_mode == sel_edge )
                    {
                        SetupVertexSelection();
                    }
                    ++b->version;
                }
                e = e->next;
            }
            while ( e != sentinel );
        }
        return 1;
    }
    else
    {
        list->next = NULL;
        list->prev = NULL;
        return 0;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480AB0  Prefab_Load  — load a prefab .map and realise its entities into the
// eclass sub-node.
//   name = prefab name (the misc_prefab "model" value, e.g. "prefabs/.../x.map")
//   node = the eclass sub-node pointer (Eclass_LoadModel passes &cur).
//
// The path is the project entity's "mapspath" value, ensured to end with a backslash, plus
// the prefab name; Eclass_RealizeModel then Map_LoadEntities's the file into the sub-node
// and bumps refCounts.  Returns Eclass_RealizeModel's result (1 success / 0 fail).
// ─────────────────────────────────────────────────────────────────────────────
char Prefab_Load(const char *name, modelnode_t **node)
{
    Sys_Printf( "Loading prefab %s...", name );

    // mapspath = the project entity's "mapspath" epair value (empty if absent).
    // The binary dereferenced d_project_entity->epairs unconditionally.  d_project_entity is
    // assigned only in mainfrm.cpp (CMainFrame project load), which the headless selftest gate
    // never runs, so it is NULL there; the NULL guard is headless-safety (same class as the
    // g_pParentWnd guards in map.cpp).
    std::string path;
    if ( g_qeglobals.d_project_entity )
        path = ValueForKey2( g_qeglobals.d_project_entity, "mapspath" );

    // Ensure a trailing backslash.
    if ( !path.empty() && path[path.length() - 1] != '\\' )
        path += '\\';

    path += ( name ? name : "" );

    const char ok = Eclass_RealizeModel( node, path.c_str() ) ? 1 : 0;


    if ( ok )
        Sys_Printf( " successful.\n" );
    else
        Sys_Printf( " failed.\n" );
    return ok;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480A00  Model_load
// Register an XModel by path into (**model)->handle.
// Strips a leading "xmodel/" or "xmodel\" prefix before registering.
//   modelPath = model path string
//   nodePtr   = pointer to the sub-node pointer (its +4 is the XModel handle)
// ─────────────────────────────────────────────────────────────────────────────
LRESULT Model_load(char *modelPath, modelnode_t **nodePtr)
{
    // R_RegisterModel below is not a lookup: on a miss it pulls the XModel off disk/iwd, then
    // every material it references, then every image those bind — synchronously.
    const char *path = modelPath;

    if ( !I_strnicmp(modelPath, "xmodel", 6) )
    {
        char sep = modelPath[6];
        if ( sep == '/' || sep == '\\' )
        {
            path += 7;
        }
    }

    Sys_Printf("Loading model %s...", path);

    // A single store into node->handle (a double dereference crashes: handle is 0 for a
    // fresh node).
    modelnode_t *node = *nodePtr;
    node->handle = R_RegisterModel((char *)path);

    {
        modelnode_t **model = nodePtr;
        iassert( (*model)->handle );   // eclass.cpp:179
    }

    XModel *xmodel = node->handle;
    // (the null-model check is XModelBad's own head iassert — xmodel.cpp:48 — which the
    // binary inlined into this caller)

    // CoD3 != CoD4: the CoD4 binary reads the bad flag at +193; kisak XModel.bad is +205 ->
    // use XModelBad() (the prefab path doesn't hit this — only xmodel models do).
    extern bool XModelBad( const XModel *model );
    if ( XModelBad( (const XModel *)xmodel ) )
    {
        return Sys_Printf(" failed.\n");
    }
    else
    {
        return Sys_Printf(" successful.\n");
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480CB0  Eclass_FreeModel
// Unlink a models_t from g_models and free it entirely.
// ─────────────────────────────────────────────────────────────────────────────
void Eclass_FreeModel(models_t *model)
{
    // Unlink from the g_models singly-linked list.
    models_t *ec = g_models;
    models_t **link = &g_models;
    for ( ; ec != model; ec = ec->next )
    {
        iassert(ec);
        link = &ec->next;
    }
    *link = ec->next;  // splice out

    // Free entity-group sub-nodes in model->x88; each sub-node's circular entity
    // list uses the def(+0x08)/entities.next(+0x0C) head overlay (sentinel &x2).
    modelnode_t *sub = (modelnode_t *)model->xx1;
    while ( sub )
    {
        modelnode_t *next = (modelnode_t *)sub->next;
        entity_s *e    = (entity_s *)sub->entities.next;
        if ( e )
        {
            for ( ; e != (entity_s *)&sub->entities; e = e->next )
            {
                iassert( e->refCount >= 2 );   // eclass.cpp:258
                --e->refCount;
                for ( brush_t *b = (brush_t *)e->brushes.prev; b != (brush_t *)&e->def; b = b->onext )
                {
                    iassert( b->refCount >= 2 );   // eclass.cpp:262
                    --b->refCount;
                }
            }
            sub->entities.next = 0;
        }
        free( sub );
        sub = next;
    }

    free( (void *)model->modelpath );
    free( (void *)model->default_model_name );
    for ( int i = 1; i < 5; ++i )
    {
        free( (void *)model->cycleModelName[i] );
    }

    free( (void *)model->name );   // heap NAME string, NOT an XModel* — BY3-audit note at EOF
    free( (void *)model->comments );
    free( model );
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480880  Eclass_FreeAll  (internal, called by Eclass_InitForSourceDirectory)
// Free every eclass in g_eclass, every model in g_models, then free eclass_bad.
// ─────────────────────────────────────────────────────────────────────────────
static void Eclass_FreeAll()
{
    eclass_t *ec = g_eclass;
    if ( g_eclass )
    {
        eclass_t *next;
        do
        {
            next = ec->next;
            Eclass_CheckEntities(ec);
            ec       = next;
            g_eclass = next;
        }
        while ( next );
    }

    models_t *model = g_models;
    g_eclass = nullptr;
    if ( g_models )
    {
        models_t *next;
        do
        {
            next = model->next;
            Eclass_CheckEntities(model);
            model    = next;
            g_models = next;
        }
        while ( next );
    }
    g_models = 0;

    if ( eclass_bad )
    {
        free(eclass_bad->name);
        free(eclass_bad->comments);
        free(eclass_bad);
        eclass_bad = nullptr;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480C50  Model_FreeMapModels
// Remove and free all g_eclass entries that carry map entity data (x88 != 0),
// then flush g_models entirely.
// ─────────────────────────────────────────────────────────────────────────────
models_t *Model_FreeMapModels()
{
    eclass_t *ec = g_eclass;
    eclass_t **link = &g_eclass;

    if ( g_eclass )
    {
        do
        {
            if ( ec->xx1 )
            {
                *link = ec->next;
                Eclass_CheckEntities(ec);
            }
            else
            {
                link = &ec->next;          // next is field 0
            }
            ec = *link;
        }
        while ( *link );
    }

    models_t *result = g_models;
    if ( g_models )
    {
        models_t *next;
        do
        {
            next     = result->next;
            Eclass_CheckEntities(result);
            result   = next;
            g_models = next;
        }
        while ( next );
    }
    g_models = 0;
    return result;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x480E30  Init_CStr
// Search the eclass comments (ec->comments, field +0x3C) for the attribute 'key'
// (e.g. "model="); if present, copy the quoted value that follows into *out via
// AllocMaterialString. The six call sites in Eclass_InitFromText pass the key
// explicitly (the binary passed it in a register).
// ─────────────────────────────────────────────────────────────────────────────
static void Init_CStr(eclass_t *ec, void **out, const char *key)
{
    if ( !key )
        return;

    std::string comments( ec->comments ? ec->comments : "" );
    // -1 when absent (std::string::find returns npos, mapped to -1).
    std::string::size_type foundAt = comments.find( key );
    int pos = ( foundAt == std::string::npos ) ? -1 : (int)foundAt;
    if ( pos < 0 )
        return;

    // Value text starts just past the key in the ORIGINAL comments string.
    const char *p = ec->comments + pos + strlen( key );
    if ( *p == '"' )            // skip an opening quote if present
        ++p;

    std::string value;
    for ( ; *p && *p != '"'; ++p )   // read until closing quote / end
        value += *p;

    if ( (int)value.length() > 0 )
        *out = AllocMaterialString( value.c_str() );
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x483500  Epair_SetRaw  (qe3.cpp:56)  — set key=value in an epair list with NO side
// effects (SetKeyValue is this plus the Checkkey_* hooks).  head is the ADDRESS of the
// list-head pointer; nodes are native epair_t objects (freed by Eclass_FreeEpairs).  A
// node with a matching key has its value replaced; otherwise a new node is prepended.
// ─────────────────────────────────────────────────────────────────────────────
void Epair_SetRaw(epair_t **head, const char *key, const char *value)
{
    for ( epair_t *node = *head; node; node = node->next )
    {
        if ( !strcmp( node->key, key ) )
        {
            // The caller may be reusing this value, or a substring of it.
            char *replacement = AllocMaterialString( value );
            free( node->value );
            node->value = replacement;
            return;
        }
    }

    // Not found → prepend a new {next, key, value} node.
    epair_t *node = (epair_t *)operator new( sizeof(epair_t) );
    node->next = *head;
    node->key = AllocMaterialString( key );
    node->value = AllocMaterialString( value );
    *head = node;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x480F90  Eclass_ParseDefaults
// Scan ec->comments (field +0x3C) for "default:" markers; for each, read the next
// two tokens (key, value) with Com_ParseExt and register the default into the
// eclass attribute list at ec+0x17C (via Epair_SetRaw).
// ─────────────────────────────────────────────────────────────────────────────
static void Eclass_ParseDefaults(eclass_t *ec)
{
    const char *p = strstr( ec->comments, "default:" );
    while ( p )
    {
        const char *text = p + 8;   // past "default:"

        parseInfo_t *kt = Com_ParseExt( &text, 0 );
        if ( !kt || !strlen( kt->token ) )
            Com_PrintMessage( "Invalid default key in .def file!" );
        char key[256];
        strcpy( key, kt ? kt->token : "" );

        parseInfo_t *vt = Com_ParseExt( &text, 0 );
        if ( !vt || !strlen( vt->token ) )
            Com_PrintMessage( "Invalid default value in .def file!" );
        char value[260];
        strcpy( value, vt ? vt->token : "" );

        // ec+0x17C (= &ec->xx8) is the eclass default/epair list head (freed by
        // Eclass_FreeEpairs at +380).
        Epair_SetRaw( &ec->xx8, key, value );

        p = strstr( text, "default:" );
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x48CA40  Eclass_GetToken  — the editor's own token parser
// Reads the next token from 'text' into g_eclassToken and returns the advanced
// pointer. Skips whitespace and "//" line comments; reads quoted strings (between
// "), single special chars ({ } ) ( ' :), or whitespace-delimited words. Sets
// g_eclassTokenEof at end of input (returns NULL).
// ─────────────────────────────────────────────────────────────────────────────
static bool IsTokenSpecial( char c )
{
    return c == '{' || c == '}' || c == ')' || c == '(' || c == '\'' || c == ':';
}

static char *Eclass_GetToken( char *text )
{
    int len = 0;
    g_eclassToken[0] = 0;
    if ( !text )
        return nullptr;

    char c = *text;
    bool singleSlash = false;
    while ( true )
    {
        if ( c <= 32 )                              // skip whitespace
        {
            while ( c ) { c = *++text; if ( c > 32 ) break; }
            if ( !c ) { g_eclassTokenEof = 1; return nullptr; }
        }
        if ( c != '/' )
            break;
        if ( text[1] != '/' ) { singleSlash = true; break; }   // lone '/' → word char
        while ( c && c != '\n' ) c = *++text;       // "//" comment → skip to EOL
        if ( !c ) { g_eclassTokenEof = 1; return nullptr; }
        // c == '\n' (whitespace) — loop re-skips it
    }

    if ( !singleSlash && c == '"' )                 // quoted string
    {
        // KIWI: bounded - an unterminated quote read past the text and the 1 KB token
        char q = text[1];
        text += 2;
        while ( q && q != '"' )
        {
            if ( len < (int)sizeof( g_eclassToken ) - 1 )
                g_eclassToken[len++] = q;
            q = *text++;
        }
        g_eclassToken[len] = 0;
        return q ? text : text - 1;   // unterminated: leave the cursor on the NUL
    }

    if ( !singleSlash && IsTokenSpecial( c ) )       // single special char
    {
        g_eclassToken[0] = c;
        g_eclassToken[1] = 0;
        return text + 1;
    }

    do                                               // word (incl. lone '/')
    {
        ++text;
        if ( len < (int)sizeof( g_eclassToken ) - 1 )   // KIWI: capped like the quoted form
            g_eclassToken[len++] = c;
        c = *text;
    }
    while ( !IsTokenSpecial( c ) && c > 32 );
    g_eclassToken[len] = 0;
    return text;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x481150  Eclass_InitFromText
// Parse one /*QUAKED <name> (r g b) [size] <flags...>\n<comments>*/ block into a
// fresh eclass_t. Uses Eclass_GetToken (above), sscanf for the color/bbox, the six
// Init_CStr attribute extractors (defaultmdl=/model=/previewmdl1-4=),
// Eclass_ParseDefaults, and the classtype switch.
// 'text' points at the "/*QUAKED" marker.
// ─────────────────────────────────────────────────────────────────────────────
eclass_t *Eclass_InitFromText( void *textPtr )
{
    char *text = (char *)textPtr;

    eclass_t *ec = (eclass_t *)operator new( sizeof(eclass_t) );
    memset( ec, 0, sizeof(eclass_t) );

    // Entity name = first token after "/*QUAKED " (text + 9).
    char *afterName = Eclass_GetToken( text + 9 );
    ec->name = (char *)operator new( strlen( g_eclassToken ) + 1 );
    strcpy( ec->name, g_eclassToken );

    ec->color[0] = ec->color[1] = ec->color[2] = 1.0f;
    ec->unk = 1.0f;
    g_eclassCurrentName = ec->name;

    // Color block " (r g b)".
    if ( sscanf( afterName, " (%g %g %g)", &ec->color[0], &ec->color[1], &ec->color[2] ) != 3 )
        return ec;

    SetMaterial( "$opaque", &ec->material );

    // Advance to the ')' that closes the color block.
    char *q = afterName;
    if ( *q != ')' )
    {
        char ch = *q;
        while ( ch ) { ch = *++q; if ( ch == ')' ) break; }
        if ( ch != ')' )
            return ec;
    }

    char *cur = Eclass_GetToken( q + 1 );

    // Optional fixed-size bbox: next token is '('.
    if ( g_eclassToken[0] == '(' )
    {
        *(int *)&ec->fixedsize = 1;
        if ( sscanf( cur, "%f %f %f) (%f %f %f)",
                     &ec->mins[0], &ec->mins[1], &ec->mins[2],
                     &ec->maxs[0], &ec->maxs[1], &ec->maxs[2] ) != 6 )
            return ec;   // malformed size block → name+color+fixedsize only

        // Skip past the two ')' that close the mins/maxs blocks.  Hitting a NUL before
        // finding both ')' returns the whole eclass (it does NOT fall through to the
        // flag/comment parse).  Unreachable in practice (the sscanf above returned 6, so
        // both ')' are present).
        int closed = 0;
        while ( 1 )
        {
            if ( *cur == ')' )
            {
                ++cur;
                if ( ++closed >= 2 )
                    break;
            }
            else if ( !*cur )
            {
                return ec;        // bail with name+color+fixedsize only
            }
            else
            {
                ++cur;
            }
        }
    }

    // ── Flags + comments ───────────────────────────────────────────
    // 1. Copy the remainder of the QUAKED line (the flag-name list) until '\n'.
    char descLine[260];
    {
        char *o = descLine;
        char ch = *cur;
        while ( *cur )
        {
            if ( ch == '\n' ) break;
            ++cur;
            *o++ = ch;
            ch = *cur;
        }
        *o = 0;
    }
    char *afterLine = cur + 1;   // first char past the '\n'

    // 2. Tokenize the flag line into up to 8 flag names (ec+0x40 + 32*k).
    {
        char *tp = descLine;
        char *flagNames[8] = { ec->flagname0, ec->flagname1, ec->flagname2, ec->flagname3,
                               ec->flagname4, ec->flagname5, ec->flagname6, ec->flagname7 };
        for ( int k = 0; k < 8; ++k )
        {
            tp = Eclass_GetToken( tp );
            if ( !tp )
                break;
            strncpy( flagNames[k], g_eclassToken, sizeof(ec->flagname0) - 1 );
            flagNames[k][sizeof(ec->flagname0) - 1] = '\0';
        }
    }

    // 3. Comments = text from afterLine up to "*/".
    {
        char *j = afterLine;
        while ( *j )
        {
            if ( *j == '*' && j[1] == '/' )
                break;
            ++j;
        }
        size_t clen = (size_t)( j - afterLine );
        ec->comments = (char *)operator new( clen + 1 );
        memcpy( ec->comments, afterLine, clen );
        ec->comments[clen] = 0;
    }

    // 4. Extract the model/preview attributes from the comments.
    Init_CStr( ec, (void **)&ec->default_model_name, "defaultmdl=" );
    Init_CStr( ec, (void **)&ec->modelpath,          "model=" );
    Init_CStr( ec, (void **)&ec->xx3,                "previewmdl1=" );
    Init_CStr( ec, (void **)&ec->xx4,                "previewmdl2=" );
    Init_CStr( ec, (void **)&ec->xx5,                "previewmdl3=" );
    Init_CStr( ec, (void **)&ec->xx6,                "previewmdl4=" );

    // 5. Register any "default: key value" attributes.
    Eclass_ParseDefaults( ec );

    // 6. classtype flags by name.
    ec->classtype = 0;
    if ( _stricmp( ec->name, "worldspawn" ) )
        ec->classtype |= 2;
    if ( !_stricmp( ec->name, "light" ) )            { ec->classtype |= 1;     return ec; }
    if ( !_stricmp( ec->name, "reflection_probe" ) ) { ec->classtype |= 0x100; return ec; }
    if ( !_stricmp( ec->name, "path" ) )             { ec->classtype |= 4;     return ec; }
    if ( !_stricmp( ec->name, "misc_model" )    || !_stricmp( ec->name, "script_model" ) ||
         !_stricmp( ec->name, "script_vehicle" )|| !_stricmp( ec->name, "dyn_model" ) )
                                                     { ec->classtype |= 8;     return ec; }
    if ( !_stricmp( ec->name, "misc_prefab" ) )      { ec->classtype |= 0x10;  return ec; }
    if ( !_stricmp( ec->name, "trigger_radius" ) )   { ec->classtype |= 0x40;  return ec; }
    if ( !_stricmp( ec->name, "trigger_disk" ) )     { ec->classtype |= 0x80;  return ec; }
    if ( !strncmp( ec->name, "node_", 5 ) )
        ec->classtype |= 0x20;

    return ec;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x4817E0  Eclass_InsertAlphabetized
// Insert `ec` into the name-ordered singly-linked list *list (g_eclass or g_models).
// (An empty list takes `ec` as-is: the binary does not touch ec->next there.)
// ─────────────────────────────────────────────────────────────────────────────
void Eclass_InsertAlphabetized(eclass_t **list, eclass_t *ec)
{
    eclass_t *head = *list;
    if ( !head )
    {
        *list = ec;
        return;
    }
    if ( _stricmp(ec->name, head->name) < 0 )
    {
        ec->next = head;
        *list = ec;
        return;
    }

    eclass_t *prev = head;
    for ( ; prev->next; prev = prev->next )
    {
        if ( _stricmp(ec->name, prev->next->name) < 0 )
            break;
    }
    ec->next   = prev->next;
    prev->next = ec;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x4815C0  Eclass_LoadModel
// Load the model(s)/prefab(s) named in ec->modelpath (semicolon-separated) into a
// linked list of 0x98-byte sub-model nodes headed by ec->xx1 (+0x160). Each name is
// loaded by Prefab_Load (classtype prefab bit 0x10) or Model_load, into the current
// node; a fresh empty node is appended after each. modelpath is freed/cleared on exit.
// ─────────────────────────────────────────────────────────────────────────────
void Eclass_LoadModel(eclass_t *ec)
{
    std::vector<std::string> list;

    // ec->xx1 (+0x160) heads the sub-model node list; create the first node if absent.
    if ( !ec->xx1 )
    {
        modelnode_t *node = (modelnode_t *)operator new( sizeof(modelnode_t) );
        memset( node, 0, sizeof(modelnode_t) );
        ec->xx1 = node;
    }

    // Split modelpath on ';' into the list (operate on a copy — strtok is destructive).
    char *copy = (char *)AllocMaterialString( (const char *)ec->modelpath );
    for ( char *tok = strtok( copy, ";" ); tok; tok = strtok( nullptr, ";" ) )
        list.push_back( tok );

    modelnode_t *tail = ec->xx1;
    modelnode_t *cur  = tail;     // the node the next name loads into

    // Forward head->tail walk.  Each `name` is a mutable per-iteration copy, so the
    // (char *) hand-off to Model_load still points at a writable buffer.
    for ( std::string name : list )
    {
        if ( ec->classtype & 0x10 )
        {
            Prefab_Load( name.c_str(), &cur );
        }
        else
        {
            Model_load( (char *)name.c_str(), &cur );
        }

        modelnode_t *newNode = (modelnode_t *)operator new( sizeof(modelnode_t) );
        tail->next = newNode;
        tail = newNode;
        cur  = newNode;
        memset( newNode, 0, sizeof(modelnode_t) );
    }

    free( copy );
    free( (void *)ec->modelpath );
    ec->modelpath = 0;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x481740  Eclass_hasModel
// Returns true if the eclass has a loadable XModel (handle valid, not error
// placeholder) or has sub-models (xx1+12 non-zero).
// ─────────────────────────────────────────────────────────────────────────────
bool Eclass_hasModel(eclass_t *e)
{
    iassert(e);

    // If modelpath is set, attempt to load it now (converts path → handle).
    if ( e->modelpath )
    {
        Eclass_LoadModel(e);
        iassert( e->modelpath == NULL );   // eclass.cpp:522
    }

    modelnode_t *xx1 = (modelnode_t *)e->xx1;
    if ( !xx1 )
        return false;

    // handle = XModel*, entities.next = sub-model list head.
    // CoD3 != CoD4 — the CoD4 binary reads the XModel "bad" flag at +193;
    // kisak's CoD3 XModel has `bad` at +205, so use XModelBad() (reads the right field).
    extern bool XModelBad( const XModel *model );
    XModel *xmodel = (XModel *)xx1->handle;
    return (xmodel && !XModelBad( xmodel )) || xx1->entities.next != 0;
}



// ─────────────────────────────────────────────────────────────────────────────
// 0x482190  Eclass_ForName
// Look up an eclass by name in g_eclass.  If not found, create a placeholder
// using Eclass_InitFromText with a synthetic QUAKED block.
//   has_brushes = 0: entity has fixed bounding box (-8 -8 -8) (8 8 8)
//   has_brushes = 1: entity is brush-based (no size block, just '?')
// ─────────────────────────────────────────────────────────────────────────────
eclass_t *Eclass_ForName(int has_brushes, const char *name)
{
    if ( !name || !*name )
        return eclass_bad;

    for ( eclass_t *ec = g_eclass; ec; ec = ec->next )
        if ( !_stricmp(name, ec->name) )
            return ec;

    // Not found: a placeholder class.
    char quaked[1028];
    if ( has_brushes )
    {
        sprintf(quaked,
            "/*QUAKED %s (0 0.5 0) ?\nNot found in source.\n",
            name);
    }
    else
    {
        sprintf(quaked,
            "/*QUAKED %s (0 0.5 0) (-8 -8 -8) (8 8 8)\nNot found in source.\n",
            name);
    }
    eclass_t *ec = Eclass_InitFromText(quaked);
    Eclass_InsertAlphabetized(&g_eclass, ec);
    return ec;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x482300  Eclass_ForModel
// The per-map model class (models_t, kept in g_models) for a model / prefab name: found, or
// allocated and loaded.  Sets ent->modelClass on success; the model-type flag (classtype
// & 0x10, prefab) must match the entity's eclass.
// ─────────────────────────────────────────────────────────────────────────────
models_t *Eclass_ForModel(const char *modelName, entity_s *ent)
{
    // The model/prefab CLASS cache is a LINEAR LIST walked with _stricmp per node, once per
    // model entity — O(entities x distinct models).  Eclass_InsertAlphabetized keeps g_models
    // sorted, so an early-exit or a hash index is available if a load profile ever wants one.

    if ( !modelName || !strlen(modelName) )
        return nullptr;

    // Search g_models for a node whose name matches AND whose model-type flag (bit 4 of
    // classtype) matches the entity's eclass flag.
    for ( models_t *e = g_models; e; e = e->next )
    {
        if ( _stricmp(modelName, e->name) ||
             (((byte)e->classtype ^ (byte)ent->eclass->classtype) & 0x10) != 0 )
            continue;
        iassert( Eclass_hasModel( e ) );   // eclass.cpp:860
        ent->modelClass = e;
        return e;
    }

    // Not found: allocate, load, and keep it if the load produced a model.  (The binary
    // bracketed this with its repaint-blocking refcount, DisableRendering++/--; KIWI's frame
    // loop never repaints inside a model load, so the pair is gone.)
    models_t *models = (models_t *)operator new(sizeof(models_t));
    memset(models, 0, sizeof(models_t));

    const size_t nameSize = strlen(modelName) + 1;
    models->name = (char *)operator new(nameSize);
    memcpy(models->name, modelName, nameSize);

    models->modelpath = (char *)operator new(nameSize);
    memcpy(models->modelpath, modelName, nameSize);

    // Copy the model-type flag bit from the entity's eclass.
    const int classFlag = ent->eclass->classtype;
    models->color[2] = 0.85000002f;
    models->color[0] = 0.85000002f;
    models->classtype |= classFlag & 0x10;

    // Seed the node's bounds from the eclass.
    eclass_t *srcEc = Eclass_ForName(0, modelName);
    memcpy( models->mins, srcEc->mins, sizeof(vec3_t) );
    memcpy( models->maxs, srcEc->maxs, sizeof(vec3_t) );

    const char *defaultModel = srcEc->default_model_name;   // +0x164
    if ( defaultModel )
    {
        models->default_model_name = AllocMaterialString(defaultModel);
    }

    if ( Eclass_hasModel(models) )
    {
        Eclass_InsertAlphabetized(&g_models, models);
        ent->modelClass = models;
        return models;
    }

    Eclass_CheckEntities(models);
    return nullptr;
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x481840  Init_ScanFiles
// Read a single .def file and parse all /*QUAKED blocks into g_eclass.
//   useFS = 0: use LoadFile (raw filesystem)
//   useFS = 1: use FS_ReadFile (VFS)
//
// The file path is passed through g_scanFilePath, which the caller writes before calling
// (the original took it in a register).
// ─────────────────────────────────────────────────────────────────────────────
void Init_ScanFiles(int useFS)
{
    const char *filePath = g_scanFilePath;

    // Normalise backslashes → forward slashes for display/strstr.
    char display[1024];
    {
        char *o = display;
        for ( const char *s = filePath; *s; ++s, ++o )
            *o = (*s == '\\') ? '/' : *s;
        *o = 0;
    }

    if ( strstr(display, "/.#") )
    {
        Sys_Printf("IngoringFile: %s\n", display);
        return;
    }

    Sys_Printf("ScanFile: %s\n", display);

    void  *buffer   = nullptr;
    int    fileSize;
    if ( useFS )
    {
        fileSize = (int)FS_ReadFile(filePath, &buffer);
    }
    else
    {
        fileSize = (int)LoadFile(filePath, &buffer);
    }

    g_eclassParsedAny = 0;

    for ( int pos = 0; pos < fileSize; ++pos )
    {
        if ( strncmp((const char *)buffer + pos, "/*QUAKED", 8u) )
            continue;

        eclass_t *ec = Eclass_InitFromText((char *)buffer + pos);
        if ( ec )
        {
            Eclass_InsertAlphabetized(&g_eclass, ec);
        }
        else
        {
            printf("Error parsing: %s in %s\n",
                   (const char *)g_eclassCurrentName, filePath);
        }

        g_lastParsedEclass = ec;
        g_eclassParsedAny  = 1;

        if ( g_eclassStopOnFirst )
            break;
    }

    if ( useFS )
    {
        // com_files.cpp:2840; kisak's FS_CheckFileSystemStarted asserts fs_searchpaths where
        // CoD3 FatalError'd.
        FS_FreeFile( (char *)buffer );
    }
    else
    {
        free(buffer);
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// 0x481B50  Eclass_InitForSourceDirectory
// Scan a glob pattern (e.g. "scripts/*.def") for /*QUAKED entity definitions.
// Handles up to two additional source directories derived from the path.
// At the end, creates the UNKNOWN_CLASS fallback eclass → eclass_bad.
//   path = the source directory glob path string.
// ─────────────────────────────────────────────────────────────────────────────

// One additional source directory's pass (Eclass_InitForSourceDirectory has two): print it,
// then scan every file the pattern matches, each as "<pattern's directory>\<name>".
static void Eclass_ScanAdditional( const char *pattern )
{
    char display[1024], baseDir[1024], filePath[1024];
    int k = 0;
    for ( ; pattern[k] && k < (int)sizeof( display ) - 1; ++k )
        display[k] = ( pattern[k] == '\\' ) ? '/' : pattern[k];
    display[k] = 0;
    Sys_Printf( "AdditionalSourceDirectory: %s\n", display );

    _snprintf( baseDir, sizeof( baseDir ), "%s", pattern );
    baseDir[sizeof( baseDir ) - 1] = 0;
    char *j = baseDir + strlen( baseDir );
    while ( j > baseDir && j[-1] != '/' && j[-1] != '\\' )
        --j;
    *( j > baseDir ? j - 1 : j ) = 0;                       // strip "/<name>"

    _finddata64i32_t findData;
    intptr_t hFind = _findfirst64i32( pattern, &findData );
    if ( hFind == -1 )
        return;
    do
    {
        _snprintf( filePath, sizeof( filePath ), "%s\\%s", baseDir, findData.name );
        filePath[sizeof( filePath ) - 1] = 0;
        strcpy( g_scanFilePath, filePath );
        Init_ScanFiles( 0 );
    }
    while ( _findnext64i32( hFind, &findData ) != -1 );
    _findclose( hFind );
}

eclass_t *Eclass_InitForSourceDirectory(char *path)
{
    char display[1024];      // normalised (forward-slash) copy of path
    char baseDir[1024];      // base directory (path with filename stripped)
    char filePath[1024];     // full path for each enumerated file
    _finddata64i32_t findData;           // _findfirst64i32 output
    char *foundName = findData.name;

    // Normalise backslashes → forward slashes.
    {
        char c = *path;
        const char *src = path;
        char *dst = display;
        for ( ; c; ++dst )
        {
            *dst = (c == '\\') ? '/' : c;
            c = dst[src - display + 1];
        }
        *dst = 0;
    }

    Sys_Printf("Eclass_InitForSourceDirectory: %s\n", display);
    strcpy(baseDir, path);

    // Strip the filename to get the base dir: null-terminate at the last path
    // separator so baseDir = the directory that sprintf("%s\\%s", baseDir, foundName)
    // rebuilds each enumerated file's full path from.
    {
        char *j = baseDir + (int)strlen(baseDir) - 1;
        while ( *j != '\\' )
        {
            if ( *j == '/' )     break;
            if ( j == baseDir )  break;
            --j;
        }
        *j = 0;
    }

    Eclass_FreeAll();
    g_eclass = nullptr;

    // ── First pass: scan the primary directory. ──────────────────────────────
    intptr_t hFind = _findfirst64i32(path, &findData);
    if ( hFind != -1 )
    {
        do
        {
            sprintf(filePath, "%s\\%s", baseDir, foundName);
            strcpy(g_scanFilePath, filePath);
            Init_ScanFiles(0);
        }
        while ( _findnext64i32(hFind, &findData) != -1 );
        _findclose(hFind);
    }

    // ── Additional source directories (only when path does not end in ".def") ─
    // KIWI: re-derived from 0x481B50.  The decompile read the binary's inlined strcats - their
    // scans start one byte before each buffer - as writes into the neighbouring stack arrays,
    // so the port appended to the wrong buffer.  For "<parent>/<dir>/<pattern>" the binary also
    // scans "<parent>/<dir>_mp/<pattern>" and "<parent>/b<dir>/<pattern>" (its literals are "_mp"
    // and "/b").  Dormant: the editor passes the cod4.def path.
    if ( !strstr(path, ".def") )
    {
        char alt[1024], tail[1024];
        _snprintf( alt, sizeof( alt ), "%s", path );
        alt[sizeof( alt ) - 1] = 0;
        int last = (int)strlen( alt ) - 1;                  // the separator before <pattern>
        while ( last > 0 && alt[last] != '/' && alt[last] != '\\' )
            --last;
        if ( last > 0 )
        {
            _snprintf( tail, sizeof( tail ), "%s", &alt[last] );          // "/<pattern>"
            tail[sizeof( tail ) - 1] = 0;
            alt[last] = 0;
            strncat( alt, "_mp", sizeof( alt ) - strlen( alt ) - 1 );
            strncat( alt, tail, sizeof( alt ) - strlen( alt ) - 1 );
            Eclass_ScanAdditional( alt );

            _snprintf( alt, sizeof( alt ), "%s", path );
            alt[sizeof( alt ) - 1] = 0;
            int prev = last - 1;                             // the separator before <dir>
            while ( prev > 0 && alt[prev] != '/' && alt[prev] != '\\' )
                --prev;
            if ( prev > 0 )
            {
                _snprintf( tail, sizeof( tail ), "%s", &alt[prev + 1] );  // "<dir>/<pattern>"
                tail[sizeof( tail ) - 1] = 0;
                alt[prev] = 0;
                strncat( alt, "/b", sizeof( alt ) - strlen( alt ) - 1 );
                strncat( alt, tail, sizeof( alt ) - strlen( alt ) - 1 );
                Eclass_ScanAdditional( alt );
            }
        }
    }

    eclass_t *result = Eclass_InitFromText((void *)"/*QUAKED UNKNOWN_CLASS (0 0.5 0) ?");
    eclass_bad = result;
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Weapon / AI def-folder eclass scanners.  Load_Defs / ScanWeapAiFiles synthesize weapon
//  (and AI-type) entity classes from the loose def files so they appear in the entity RMB
//  menu; Eclass_InitForSourceDirectory only scans .def files and would miss them.
// ═════════════════════════════════════════════════════════════════════════════
extern const char  *Info_ValueForKey( const char *s, const char *key );        // q_shared 0x4B98D0
extern int          Com_sprintf( char *dest, uint size, const char *fmt, ... );  // q_shared 0x4B9780

// 0x48B7B0  Init_ScanWeapons — read each weapon def file in `folder`, extract its
// displayName/worldModel, and synthesize a `weapon_<name>` /*QUAKED*/ eclass block.
void Init_ScanWeapons( const char *folder )
{
    int  weaponPrefixLen = (int)strlen( "WEAPONFILE" );
    if ( !fs_searchpaths )
        FatalError( 0, "Filesystem call made without initialization\n" );

    int          count = 0;
    const char **list  = folder ? FS_ListFiles( folder, "", FS_LIST_ALL, &count ) : nullptr;

    for ( int i = 0; i < count; ++i )
    {
        const char *fname = list[i];
        char qpath[1024];
        sprintf( qpath, "%s\\%s", folder, fname );
        Sys_Printf( "ScanWeapon: %s\n", qpath );

        char *buffer = nullptr;
        if ( (int)FS_ReadFile( qpath, (void **)&buffer ) > weaponPrefixLen )
        {
            if ( !strncmp( buffer, "WEAPONFILE", weaponPrefixLen ) )
            {
                char *info = &buffer[weaponPrefixLen];
                // the info string must be well-formed (no stray quotes / semicolons).
                if ( !strchr( info, '"' ) && !strchr( info, ';' ) )
                {
                    char className[1024];
                    Com_sprintf( className, 0x400, "weapon_%s", fname );
                    char displayName[1024];
                    char worldModel[68];
                    strcpy( displayName, Info_ValueForKey( info, "displayName" ) );
                    strcpy( worldModel,  Info_ValueForKey( info, "worldModel" ) );

                    char quaked[1024];
                    sprintf( quaked,
                             "/*QUAKED %s (.3 .3 1) (-16 -16 -16) (16 16 16) SUSPENDED SPIN - RESPAWN\n"
                             "defaultmdl=\"%s\"\n%s\nInternal(script) name:%s\n"
                             "\"count\" - Specifies a specific amount of ammo to include with the weapon. "
                             "Amount is capped to ammo max.",
                             className, worldModel, displayName, qpath );
                    eclass_t *ecls = Eclass_InitFromText( quaked );
                    if ( ecls )
                        Eclass_InsertAlphabetized( &g_eclass, ecls );
                }
            }
        }
        // com_files.cpp:2840
        FS_FreeFile( buffer );
    }
    if ( list )
        FS_FreeFileList( list );
}

// 0x48BA40  ScanWeapAiFiles — scan the single- and multi-player weapon folders.
void ScanWeapAiFiles()
{
    // (the binary also reads the project entity's "basepath" epair but never uses it;
    // the two weapon folders are fixed relative paths under the searchpaths.)
    Sys_Printf( "Scanning single player weapons: %s/*\n", "weapons/sp" );
    Init_ScanWeapons( "weapons/sp" );
    Sys_Printf( "Scanning multiplayer weapons: %s/*\n", "weapons/mp" );
    Init_ScanWeapons( "weapons/mp" );
}

// ── 0x48B6C0  Load_Defs — scan an AI-types def folder for /*QUAKED*/ eclass blocks ─────
// Lists every *.gsc file under `folder` in the searchpaths and, for each, builds
// "<folder>\<file>" in g_scanFilePath (where Init_ScanFiles reads its path) and calls
// Init_ScanFiles(1) (FS_ReadFile path).  Wired from QE_LoadProject with the AI-types +
// gametypes folders.  Uses FS_ListFiles(folder,"gsc",...) — kisak's equivalent of the
// older FS_ListFilteredFiles(folder,"gsc") (list files with extension under a searchpath dir).
void Load_Defs( const char *folder )
{
    if ( !fs_searchpaths )
        FatalError( 0, "Filesystem call made without initialization\n" );
    if ( !folder )
        return;

    int          count = 0;
    const char **list  = FS_ListFiles( folder, "gsc", FS_LIST_ALL, &count );
    Sys_Printf( "Scanning AI types folder: %s/*gsc\n", folder );

    for ( int i = 0; i < count; ++i )
    {
        sprintf( g_scanFilePath, "%s\\%s", folder, list[i] );
        Init_ScanFiles( 1 );
    }
    if ( list )
        FS_FreeFileList( list );
}


// TWO DIFFERENT `handle` fields live at struct offset +4 and only one of them is freeable:
//   * TOP-LEVEL nodes (0x184 B, the g_models/g_eclass lists) — handle is a HEAP NAME STRING
//     (operator new in Eclass_ForModel).  Every Eclass_CheckEntities caller passes one of these.
//   * SUB-nodes (0x98 B, the +0x160 list) — handle IS the XModel*, which is HUNK memory.
//     Both cleanup loops free the sub-NODE and must never free sub->handle.
