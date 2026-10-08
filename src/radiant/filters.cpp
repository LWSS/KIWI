#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// CoD4Radiant filter system parsed from RadiantFilters.txt; each filter entry
// is a linked list node (filter_entry_s) in one of four category lists hanging off
// g_qeglobals.d_filterGlobals_*.

#include "stdafx.h"
#include <universal/surfaceflags.h>
#include "qe3.h"
#include "kiwi_matconvert.h"     // KIWI-UX: non-lit surfaces remain visible in Shift+L
#include "kiwi_ported.h"         // KIWI: shared engine/editor externs
#include <universal/q_parse.h>   // Com_Parse / Com_ParseOnLine — the filter file grammar
#include <map>                    // faceTexMap
#include <string>

// ── forward declarations of helpers from materialdef.cpp / engine_stubs.cpp ──
struct qtexture_s;
extern int  MaterialDef_05(qtexture_s *mtl);                  // 0x431840 — tex_num_or_localefilter (case 6 decal)
extern int  MaterialDef_07(qtexture_s *mtl);                  // 0x431910 — callback for MaterialDef_02
extern int  MaterialDef_09(qtexture_s *mtl);                  // 0x431a00 — color_or_surfacetype_filter (case 7)
struct LayerMaterialDef;
extern bool Materialdef_Realize(MaterialDef *m);
extern int  TexWnd_GetDecalLocaleBit();                       // texwnd.cpp — texWndGlob.unk_8 (case 6 decal seed)
extern char *AllocMaterialString( const char *s );           // 0x48B030 (string heap-copy)
extern "C" int _stricmp( const char *, const char * );
extern "C" char *_strlwr( char * );
extern "C" void free( void * );
// (strcmp/strncpy/strlen come from <cstring> via stdafx.h)

// prefs.cpp — for the user filter path (g_PrefsDlg->m_strUserFilterPath).
#include "prefs.h"
#include "radiant_registry.h"   // Radiant_ProfileGet/SetInt — the non-MFC CWinApp profile replacement

// ═════════════════════════════════════════════════════════════════════════════
//  CONDITION-TREE PARSER (DynamicFilter_ParseCondition + leaf parsers + Filter_ParseMaterialList)
//
//  The filter file (raw\..\RadiantFilters.txt) defines, per filter entry, a boolean
//  condition over leaf predicates:
//      Texture [All] [*]<name>     contents node {name, flags: bit0=`*`, bit1=All}      case 3
//      Contents [All] <name>       {contentBits, flags: bit1=All}                       case 4
//      Misc <world|terrain|curve|decal>  {name (lowercased)}                            case 6
//      KeyValue <key> <value>[*]   {key, value, flags: bit0=`*` wildcard}               case 5
//      Surface [All] <name>        {surfaceBits, flags: bit1=All}                        case 7
//      EClassFlag <name>           {eclassBits}                                          case 8
//  combined with `&&` (surfaceFlags=1, AND), `||` (=2, OR), `!` (negate flag) and
//  parentheses.  The evaluator is FilterCondition_Eval below; the case
//  numbers here are EXACTLY the surfaceFlags values it switches on.
//
// Nodes carry an explicit native sibling link. Payloads keep string pointers
// separate from scalar flag values; the original x86 sizes are not allocation sizes.
//
//  Tokens are read with Com_Parse / Com_ParseOnLine directly — there is no `g_parse`
//  global in the engine (it uses Com_GetParseThreadInfo).
// ═════════════════════════════════════════════════════════════════════════════

// The condition-type case values (filter_info_s.surfaceFlags) FilterCondition_Eval switches on.
enum {
    FILT_AND       = 1,   // children all-match
    FILT_OR        = 2,   // children any-match
    FILT_TEXTURE   = 3,   // material-name match           (Filter_ParseTexture / FilterCond_Material)
    FILT_CONTENTS  = 4,   // brush/face contents bits      (Filter_ParseContents / FilterCond_Contents)
    FILT_KEYVALUE  = 5,   // entity epair key/value        (Filter_ParseKeyValue / FilterCond_KeyValue)
    FILT_MISC      = 6,   // world/terrain/curve/decal     (Filter_ParseMisc / FilterCond_Misc)
    FILT_SURFACE   = 7,   // surface flag bits             (Filter_ParseSurface / FilterCond_SurfaceFlag)
    FILT_ECLASS    = 8,   // eclass classtype flag bits    (Filter_ParseEClassFlag / FilterCond_EClassFlag)
};

// 0x4B83E0 _GetToken — read the next ON-LINE token (Com_ParseOnLine).
static inline parseInfo_t *Filter_GetToken( const char **text ) { return Com_ParseOnLine( text ); }

struct FilterNameCondition { char *name; int flags; };
struct FilterBitsCondition { int bits; int flags; };

// Allocate the complete condition node, including its native sibling link.
static filter_info_s *Filter_NewInfo( int type )
{
    filter_info_s *n = (filter_info_s *)operator new( sizeof(filter_info_s) );
    n->surfaceFlags    = type;        // condition type
    n->z_pad_0x0004[0] = 0;           // negate flag (set by caller)
    n->contents_ptr    = nullptr;     // contents/children
    n->next           = nullptr;     // next-sibling link (leaf → 0)
    return n;
}

// ── Texture [All] [*]<name>  (0x411500) → case 3 ──────────────────
// contents node {char* name, int flags (bit0 = `*` leading wildcard,
// bit1 = "All" — match any face)}.
static filter_info_s *Filter_ParseTexture( const char **text )
{
    filter_info_s *n = Filter_NewInfo( FILT_TEXTURE );
    FilterNameCondition *c = (FilterNameCondition *)operator new( sizeof(FilterNameCondition) );
    n->contents_ptr = (filter_contents_s *)c;
    c->name = 0; c->flags = 0;
    parseInfo_t *t = Filter_GetToken( text );
    if ( !_stricmp( t->token, "all" ) )
    {
        c->flags |= 2;
        t = Filter_GetToken( text );
    }
    if ( t->token[0] == '*' )
    {
        c->flags |= 1;
        t = Filter_GetToken( text );
    }
    c->name = AllocMaterialString( t->token );
    return n;
}

// ── name→bit tables ──
// Each is a {const char* name; int bits} pair table.
struct FilterFlagName { const char *name; int bits; };
static const FilterFlagName s_contentsTbl[] = {   // 0x73C388 — 34 pairs
    {"solid", CONTENTS_SOLID}, {"foliage", CONTENTS_FOLIAGE}, {"ai_avoid", CONTENTS_AI_AVOID},
    {"vehicletrigger", CONTENTS_VEHICLETRIGGER}, {"glass", CONTENTS_GLASS}, {"water", CONTENTS_WATER},
    {"canshootclip", CONTENTS_CANSHOOTCLIP}, {"missileclip", CONTENTS_MISSILECLIP}, {"item", CONTENTS_ITEM},
    {"vehicleclip", CONTENTS_VEHICLECLIP}, {"itemclip", CONTENTS_ITEMCLIP}, {"sky", CONTENTS_SKY},
    {"ai_nosight", CONTENTS_AI_NOSIGHT}, {"clipshot", CONTENTS_CLIPSHOT}, {"actor", CONTENTS_ACTOR},
    {"fake_actor", CONTENTS_FAKE_ACTOR}, {"playerclip", CONTENTS_PLAYERCLIP}, {"monsterclip", CONTENTS_MONSTERCLIP},
    {"axistrigger", CONTENTS_AXISTRIGGER}, {"alliestrigger", CONTENTS_ALLIESTRIGGER},
    {"neutraltrigger", CONTENTS_NEUTRALTRIGGER}, {"use", CONTENTS_USE},
    {"nonsentienttrigger", CONTENTS_NONSENTIENTTRIGGER}, {"vehicle", CONTENTS_VEHICLE},
    {"mantle", CONTENTS_MANTLE}, {"player", CONTENTS_PLAYER}, {"corpse", CONTENTS_CORPSE},
    {"detail", CONTENTS_DETAIL}, {"structural", CONTENTS_STRUCTURAL}, {"lookat", CONTENTS_LOOKAT},
    {"translucent", CONTENTS_TRANSLUCENT}, {"playertrigger", CONTENTS_PLAYERTRIGGER},
    {"nodrop", static_cast<int>(CONTENTS_NODROP)}, {"noncolliding", CONTENTS_NONCOLLIDING},
};
static const FilterFlagName s_surfaceTbl[] = {    // 0x73C498 — 17 pairs
    {"nodamage", SURF_NODAMAGE}, {"slick", SURF_SLICK}, {"sky", SURF_SKY}, {"ladder", SURF_LADDER},
    {"noimpact", SURF_NOIMPACT}, {"nomarks", SURF_NOMARKS}, {"nopenetrate", SURF_NOPENETRATE},
    {"hdrportal", SURF_HDRPORTAL}, {"nodraw", SURF_NODRAW}, {"nolightmap", SURF_NOLIGHTMAP},
    {"nosteps", SURF_NOSTEPS}, {"nonsolid", SURF_NONSOLID}, {"nodlight", SURF_NODLIGHT},
    {"nocastshadow", SURF_NOCASTSHADOW}, {"mantleon", SURF_MANTLEON}, {"mantleover", SURF_MANTLEOVER},
    {"portal", static_cast<int>(SURF_PORTAL)},
};
static const FilterFlagName s_eclassTbl[] = {     // 0x73C520 — 9 pairs
    {"light",0x1},{"angle",0x2},{"path",0x4},{"miscmodel",0x8},{"prefab",0x10},
    {"node",0x20},{"triggerradius",0x40},{"triggerdisk",0x80},{"reflection_probe",0x100},
};
static int Filter_LookupBits( const FilterFlagName *tbl, int n, const char *name )
{
    int bits = 0;   // 0 if no name matches
    for ( int i = 0; i < n; ++i )
        if ( !_stricmp( name, tbl[i].name ) )
            bits = tbl[i].bits;
    return bits;
}

// ── Contents [All] <name>  (0x411580) → case 4 ────────────────────
// contents node {int contentBits, int flags (bit1=All)}; <name> mapped
// to a bit via the 34-entry table.
static filter_info_s *Filter_ParseContents( const char **text )
{
    filter_info_s *n = Filter_NewInfo( FILT_CONTENTS );
    FilterBitsCondition *c = (FilterBitsCondition *)operator new( sizeof(FilterBitsCondition) );
    n->contents_ptr = (filter_contents_s *)c;
    c->bits = 0; c->flags = 0;
    parseInfo_t *t = Filter_GetToken( text );
    if ( !_stricmp( t->token, "all" ) )
    {
        c->flags |= 2;
        t = Filter_GetToken( text );
    }
    char *nm = (char *)AllocMaterialString( t->token );
    _strlwr( nm );
    c->bits = Filter_LookupBits( s_contentsTbl, 34, nm );
    free( nm );
    return n;
}

// ── EClassFlag <name>  (0x411630) → case 8 ────────────────────────
// contents node {int eclassBits}; <name> mapped via the 9-entry table.
static filter_info_s *Filter_ParseEClassFlag( const char **text )
{
    filter_info_s *n = Filter_NewInfo( FILT_ECLASS );
    FilterBitsCondition *c = (FilterBitsCondition *)operator new( sizeof(FilterBitsCondition) );
    n->contents_ptr = (filter_contents_s *)c;
    c->bits = 0; c->flags = 0;
    parseInfo_t *t = Filter_GetToken( text );
    char *nm = (char *)AllocMaterialString( t->token );
    _strlwr( nm );
    c->bits = Filter_LookupBits( s_eclassTbl, 9, nm );
    free( nm );
    return n;
}

// ── Surface [All] <name>  (0x4116B0) → case 7 ─────────────────────
// contents node {int surfaceBits, int flags (bit1=All)}.
static filter_info_s *Filter_ParseSurface( const char **text )
{
    filter_info_s *n = Filter_NewInfo( FILT_SURFACE );
    FilterBitsCondition *c = (FilterBitsCondition *)operator new( sizeof(FilterBitsCondition) );
    n->contents_ptr = (filter_contents_s *)c;
    c->bits = 0; c->flags = 0;
    parseInfo_t *t = Filter_GetToken( text );
    if ( !_stricmp( t->token, "all" ) )
    {
        c->flags |= 2;
        t = Filter_GetToken( text );
    }
    char *nm = (char *)AllocMaterialString( t->token );
    _strlwr( nm );
    c->bits = Filter_LookupBits( s_surfaceTbl, 17, nm );
    free( nm );
    return n;
}

// ── KeyValue <key> <value>[*]  (0x411450) → case 5 ────────────────
// contents node {char* key, char* value, int flags (bit0 = `*`
// trailing-wildcard prefix match)}.  Read key + value on the line; if the next token
// is a bare `*`, set the wildcard bit, else unget it.
static filter_info_s *Filter_ParseKeyValue( const char **text )
{
    filter_info_s *n = Filter_NewInfo( FILT_KEYVALUE );
    filter_contents_s *c = (filter_contents_s *)operator new( sizeof(filter_contents_s) );
    n->contents_ptr = (filter_contents_s *)c;
    c->flags = 0;
    parseInfo_t *t = Filter_GetToken( text );
    c->key = AllocMaterialString( t->token );
    t = Filter_GetToken( text );
    c->value = AllocMaterialString( t->token );
    if ( Filter_GetToken( text )->token[0] == '*' )
    {
        c->flags |= 1;
    }
    else
    {
        Com_UngetToken();
    }                         // value had no trailing `*`
    return n;
}

// ── Misc <world|terrain|curve|decal>  (0x4113D0) → case 6 ─────────
// contents node {char* name (lowercased)}.
static filter_info_s *Filter_ParseMisc( const char **text )
{
    filter_info_s *n = Filter_NewInfo( FILT_MISC );
    FilterNameCondition *c = (FilterNameCondition *)operator new( sizeof(FilterNameCondition) );
    n->contents_ptr = (filter_contents_s *)c;
    c->flags = 0;
    parseInfo_t *t = Filter_GetToken( text );
    if ( !_stricmp( t->token, "all" ) )
    {
        c->flags |= 2;
        t = Filter_GetToken( text );
    }
    c->name = AllocMaterialString( t->token );
    _strlwr( (char *)c->name );
    return n;
}

// ── DynamicFilter_ParseCondition (0x411760) — recursive-descent over the grammar.
// A leading `!` sets the negate flag.  A `(` opens a list:
// parse children, joining with `&&` (type 1) / `||` (type 2); the list node's type is
// set to whichever operator separates the children (the LAST
// `&&`/`||` seen wins).  Otherwise dispatch on the leaf keyword.
filter_info_s *DynamicFilter_ParseCondition( const char **text )
{
    parseInfo_t *t = Com_Parse( text );
    bool negate = false;
    if ( t->token[0] == '!' ) { negate = true; t = Com_Parse( text ); }

    if ( t->token[0] == '(' )
    {
        filter_info_s *list = Filter_NewInfo( 0 );  // type set by the last operator
        list->z_pad_0x0004[0] = (char)negate;
        while ( true )
        {
            filter_info_s *child = DynamicFilter_ParseCondition( text );
            // Prepend the child to list->contents_ptr.
            child->next = (filter_info_s *)list->contents_ptr;
            list->contents_ptr    = (filter_contents_s *)child;
            t = Com_Parse( text );
            if ( !strcmp( t->token, "&&" ) ) { list->surfaceFlags = FILT_AND; continue; }
            if ( !strcmp( t->token, "||" ) ) { list->surfaceFlags = FILT_OR;  continue; }
            break;   // expect `)`
        }
        return list;
    }

    filter_info_s *leaf = nullptr;
    if      ( !_stricmp( t->token, "texture" )    ) leaf = Filter_ParseTexture( text );
    else if ( !_stricmp( t->token, "contents" )   ) leaf = Filter_ParseContents( text );
    else if ( !_stricmp( t->token, "eclassflag" ) ) leaf = Filter_ParseEClassFlag( text );
    else if ( !_stricmp( t->token, "surface" )    ) leaf = Filter_ParseSurface( text );
    else if ( !_stricmp( t->token, "keyvalue" )   ) leaf = Filter_ParseKeyValue( text );
    else if ( !_stricmp( t->token, "misc" )       ) leaf = Filter_ParseMisc( text );

    if ( !leaf )
        Com_Error( ERR_FATAL, "DynamicFilter_ParseCondition: failed" );
    leaf->z_pad_0x0004[0] = (char)negate;
    return leaf;
}

// ── Filter_ParseMaterialList (0x411280) — parse a whitespace-separated material-name
// LIST for a `face`-type filter entry (the body between { } when filter_type_enum & 4).
// Each node is {char* name, void* next}, prepended.  Reads the first token
// on-line (after the `{`, space-delimited off), then subsequent tokens until empty.
// Returns the list head (or null if the list is empty).
struct FaceTexNode { void *name; FaceTexNode *next; };
FaceTexNode *Filter_ParseMaterialList( const char **text )
{
    FaceTexNode *head = nullptr;
    parseInfo_t *t = Com_Parse( text );
    if ( !strlen( t->token ) )
        return nullptr;
    while ( true )
    {
        FaceTexNode *node = (FaceTexNode *)operator new( sizeof(FaceTexNode) );
        node->name = AllocMaterialString( t->token );
        node->next = head;
        head = node;
        t = Filter_GetToken( text );
        if ( !strlen( t->token ) )
            return head;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FilterCondition_Eval (0x412170)
// Recursively evaluates a filter condition tree against a brush definition.
// AND (1) and OR (2) recurse; cases 3..8 evaluate the six leaf forms.
// ─────────────────────────────────────────────────────────────────────────────
static bool FilterCondition_Eval(brush_t *def, filter_info_s *info);

// Cases 3..8 are Texture, Contents, KeyValue, Misc, Surface, and EClassFlag.
static bool FilterCond_Material  (brush_t *def, filter_info_s *info);
static bool FilterCond_Contents  (brush_t *def, filter_info_s *info);
static bool FilterCond_KeyValue  (brush_t *def, filter_info_s *info);
static bool FilterCond_Misc      (brush_t *def, filter_info_s *info);
static bool FilterCond_SurfaceFlag(brush_t *def, filter_info_s *info);
static bool FilterCond_EClassFlag(brush_t *def, filter_info_s *info);

static bool FilterCondition_Eval(brush_t *def, filter_info_s *info)
{
    // switch on info->surfaceFlags (the condition type)
    switch ( info->surfaceFlags )
    {
    case 1: // AND — all children must match
    {
        filter_contents_s *child = info->contents_ptr;
        if ( !child )
            return info->z_pad_0x0004[0] == 0;
        do {
            if ( !FilterCondition_Eval(def, (filter_info_s *)child) )
                return (bool)info->z_pad_0x0004[0];
            child = (filter_contents_s *)((filter_info_s *)child)->next;
        } while ( child );
        return info->z_pad_0x0004[0] == 0;
    }
    case 2: // OR — any child must match
    {
        filter_contents_s *child = info->contents_ptr;
        if ( !child )
            return (bool)info->z_pad_0x0004[0];
        do {
            if ( FilterCondition_Eval(def, (filter_info_s *)child) )
                return info->z_pad_0x0004[0] == 0;
            child = (filter_contents_s *)((filter_info_s *)child)->next;
        } while ( child );
        return (bool)info->z_pad_0x0004[0];
    }
    case 3: // Texture / material-name match
        return FilterCond_Material(def, info);
    case 4: // Contents flag bits
        return FilterCond_Contents(def, info);
    case 5: // KeyValue epair match
        return FilterCond_KeyValue(def, info);
    case 6: // Misc world/terrain/curve/decal
        return FilterCond_Misc(def, info);
    case 7: // Surface flag bits
        return FilterCond_SurfaceFlag(def, info);
    case 8: // EClassFlag bits
        return FilterCond_EClassFlag(def, info);
    default:
        return false;
    }
}

// case 3 — Texture/material-name match (0x412630).
// Materialdef_GetName returns the radiant material name for ordinary world
// materials and the layered-material name for layered ones:
//   • NOT "All" (node.flags&2==0) and NOT a patch → per-face: compare every base-layer face's
//     material name against node.name (node.flags&1 → substring/strstr, else exact/strcmp); the
//     brush matches if ANY face matches.
//   • "All" set, or a patch brush → use the brush's representative material (patch->texture for
//     a patch; else brush.xx1, the first-face mtldef cached when all faces share contents — 0 if
//     heterogeneous) and compare once (same wildcard/exact rule).
// Match → !negate; no match → negate.
static bool FilterCond_Material(brush_t *def, filter_info_s *info)
{
    const int   flags  = ((FilterNameCondition *)info->contents_ptr)->flags;               // node.flags (bit0=`*`, bit1=All)
    const char *target = ((FilterNameCondition *)info->contents_ptr)->name; // node.name
    bool        negate = (info->z_pad_0x0004[0] != 0);

    if ( ( flags & 2 ) == 0 && !def->patch )
    {
        if ( !def->faceCount )
            return negate;
        for ( int i = 0; i < def->faceCount; ++i )
        {
            MaterialDef *md = &def->faces[i].mtldef[0];
            // the binary inlines Materialdef_GetName here (MaterialDef.cpp:85 lives in it)
            const char *name = (const char *)Materialdef_GetName( md );
            bool hit = ( flags & 1 ) ? ( strstr( name, target ) != nullptr )
                                     : ( strcmp( name, target ) == 0 );
            if ( hit )
                return !negate;
        }
        return negate;
    }

    // "All" or patch: one representative material for the whole brush.
    MaterialDef *md = def->patch ? (MaterialDef *)&def->patch->texture
                                 : def->xx1;
    if ( !md )
        return negate;
    const char *name = (const char *)Materialdef_GetName( md );
    bool hit = ( flags & 1 ) ? ( strstr( name, target ) != nullptr )
                             : ( strcmp( name, target ) == 0 );
    return hit ? !negate : negate;
}

// ── case 4 — Contents flag bits (0x412470) ────────────────────────
// Matches when EVERY face's combined contents (brush.contents | face.contents)
// intersects the filter's content bits; one face without them → no match.
// Loop while ((brushC|faceC)&bits)!=0.
static bool FilterCond_Contents(brush_t *def, filter_info_s *info)
{
    int *cond      = (int *)info->contents_ptr;   // {contentBits@0, flags@4}
    int  bits      = cond[0];
    bool negate    = (info->z_pad_0x0004[0] != 0);
    unsigned int faceCount = (unsigned int)def->faceCount;
    if ( !faceCount )
        return !negate;                            // no faces → "matches"
    int   brushContents = def->contents;
    face_t *face = def->faces;
    for ( unsigned int i = 0; ; ++i, ++face )
    {
        int faceContents = face->contents;
        if ( ( ( brushContents | faceContents ) & bits ) == 0 )
            return negate;                         // a face NOT excluded by these bits
        if ( i + 1 >= faceCount )
            return !negate;                        // every face is excluded
    }
}
// ── case 5: key/value condition (0x412290) ───────────────────────
// This is what the LAYER filter (Layers_BuildFilter below) uses:
// a filter_contents_s with key="script_layer" and value=<layer name>, flags=0.
// flags bit0 set → walk the entity's epairs by hand and strncmp the value (literal
// prefix compare path); flags clear (our layer case) → Entity_HasEpairMatch exact.
// `def` is the brush DEFINITION; `def->owner` is the brush's owning entity (whose
// epairs carry the script_layer key).  z_pad_0x0004[0] is the negation flag: when
// it matches, the result is `negate==0` (true unless negated), else `negate`.
static bool FilterCond_KeyValue(brush_t *def, filter_info_s *info)
{
    filter_contents_s *cond = info->contents_ptr;
    const char       *key   = cond->key;
    bool              negate = (info->z_pad_0x0004[0] != 0);

    if ( (cond->flags & 1) != 0 )
    {
        // Literal path: walk the owning entity's epairs, find `key`, prefix-compare.
        const char *val = "";
        epair_t *ep = def->owner ? def->owner->epairs : nullptr;
        for ( ; ep; ep = ep->next )
            if ( _stricmp( ep->key, key ) == 0 ) { val = ep->value; break; }
        if ( !strncmp( val, cond->value, strlen( cond->value ) ) )
            return !negate;
        return negate;
    }

    // Epair-exact path (the layer-filter case): entity must have key==value.
    if ( Entity_HasEpairMatch( def->owner, key, cond->value ) )
        return !negate;
    return negate;
}
// case 6 — Misc world/terrain/curve/decal (0x412320):
//   world   — brush owned by the worldspawn (owner == world_entity->def).
//   decal   — any base-layer face whose locale bits intersect the "decal" locale bit.
//   terrain — patch brush whose patch->type == 64 (the terrain discriminant).
//   curve   — patch brush whose patch->type != 64.
// world/terrain/curve are pure struct reads.  decal walks faces through the
// MaterialDef_02/MaterialDef_05 realize path (qtexture.tex_num_or_localefilter); the SEED is
// texWndGlob.unk_8 (1<<localeIndex("decal"), set by FillTextureMenu).
static bool FilterCond_Misc(brush_t *def, filter_info_s *info)
{
    const FilterNameCondition *cond = (const FilterNameCondition *)info->contents_ptr;
    const char *node = cond->name;
    const int   flags = cond->flags;
    bool        negate = (info->z_pad_0x0004[0] != 0);

    if ( !strcmp( node, "world" )
         && (void *)def->owner == (world_entity ? world_entity->def : nullptr) )
        return !negate;

    if ( !strcmp( node, "decal" ) )
    {
        int matchCount = 0;
        for ( int i = 0; i < def->faceCount; ++i )
        {
            MaterialDef *md = &def->faces[i].mtldef[0];
            if ( !Materialdef_Realize( md ) )
                continue;
            dword_181F51C = TexWnd_GetDecalLocaleBit();         // texWndGlob.unk_8 seed
            MaterialDef_02( md, MaterialDef_05 );
            if ( dword_181F51C )
                ++matchCount;
        }
        if ( matchCount && ( matchCount == def->faceCount || ( flags & 2 ) == 0 ) )
            return !negate;
        // fall through to terrain/curve tests — a brush is not both.
    }

    if ( !strcmp( node, "terrain" ) && def->patch && (int)def->patch->type == 64 )
        return !negate;
    if ( !strcmp( node, "curve" )   && def->patch && (int)def->patch->type != 64 )
        return !negate;
    return negate;
}

// case 7 — Surface flag bits (0x412500). Editor_AddRadiantMaterial
// populates qtexture_s.color_or_surfacetype_filter from the registered material's
// MaterialInfo.surfaceTypeBits — so the realize callback (MaterialDef_09 ANDs that field
// into dword_181F51C) reads surface-type bits:
//   patch brush      → seed dword_181F51C=surfaceBits, realize patch->texture[0]; matched
//                      iff bits survive the AND.
//   plain brush      → realize each base-layer face; count how many keep their bits.  The
//                      brush matches when EVERY face matched, OR (when "All" is NOT set,
//                      node.flags&2==0) when ANY face matched.
//   no faces         → no match (return negate).
// Match result: when matched, return !negate; else negate.
static bool FilterCond_SurfaceFlag(brush_t *def, filter_info_s *info)
{
    int  *node       = (int *)info->contents_ptr;   // {surfaceBits@0, flags@4 (bit1=All)}
    int   surfaceBits = node[0];
    bool  negate     = (info->z_pad_0x0004[0] != 0);

    if ( def->patch )
    {
        MaterialDef *md = (MaterialDef *)&def->patch->texture;
        if ( !Materialdef_Realize( md ) )
            return negate;
        dword_181F51C = surfaceBits;                // seed; MaterialDef_09 ANDs surfaceTypeBits
        MaterialDef_02( md, MaterialDef_09 );
        return dword_181F51C ? !negate : negate;
    }
    if ( !def->faceCount )
        return negate;

    int matchCount = 0;
    for ( int i = 0; i < def->faceCount; ++i )
    {
        MaterialDef *md = &def->faces[i].mtldef[0];
        if ( !Materialdef_Realize( md ) )
            continue;
        dword_181F51C = surfaceBits;
        MaterialDef_02( md, MaterialDef_09 );
        if ( dword_181F51C )
            ++matchCount;
    }
    // matched if all faces matched, or (when "All" not set) any face matched.
    if ( matchCount && ( matchCount == def->faceCount || ( node[1] & 2 ) == 0 ) )
        return !negate;
    return negate;
}

// ── case 8 — EClassFlag bits (0x4124D0) ───────────────────────────
// Reads the owning entity's eclass classtype flags.
// Match when (brush->owner->eclass->classtype & filterEclassBits) != 0.
static bool FilterCond_EClassFlag(brush_t *def, filter_info_s *info)
{
    int *cond   = (int *)info->contents_ptr;        // {eclassBits@0}
    bool negate = (info->z_pad_0x0004[0] != 0);
    int classtype = def->owner->eclass->classtype;
    if ( ( classtype & cond[0] ) != 0 )
        return !negate;
    return negate;
}

// ─────────────────────────────────────────────────────────────────────────────
// FilterBrush_CheckFilterListHide (0x412050)
// Walks a filter_entry_s list and returns true if any non-face, non-fdshow entry
// matches the brush def (i.e., the brush would be HIDDEN by that filter).
// ─────────────────────────────────────────────────────────────────────────────
static bool FilterBrush_CheckFilterListHide(filter_entry_s *list, brush_t *def)
{
    // Stop and return 1 (hidden) only when: !isShown && !face_type && condition matches
    filter_entry_s *f = (filter_entry_s *)list;
    if ( !f )
        return false;
    while ( f->isShown                                  // isShown → skip
         || ( f->filter_type_enum & 4 ) != 0            // face-type filter → skip
         || !FilterCondition_Eval( def, f->info ) )     // condition not met → skip
    {
        f = f->next_filter;
        if ( !f )
            return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FilterBrush_IsFilteredByAnyList (0x412120)
// Returns true if ANY filter list (geometry/entity/trigger/other/layer) hides
// this brush def.
// ─────────────────────────────────────────────────────────────────────────────
static bool FilterBrush_IsFilteredByAnyList(brush_t *def)
{
    return FilterBrush_CheckFilterListHide(g_qeglobals.d_filterGlobals_geometryFilters, def)
        || FilterBrush_CheckFilterListHide(g_qeglobals.d_filterGlobals_entityFilters,   def)
        || FilterBrush_CheckFilterListHide(g_qeglobals.d_filterGlobals_triggerFilters,  def)
        || FilterBrush_CheckFilterListHide(g_qeglobals.d_filterGlobals_otherFilters,    def)
        || FilterBrush_CheckFilterListHide(g_qeglobals.d_filterGlobals_layerFilters,    def);
}

// ─────────────────────────────────────────────────────────────────────────────
// FilterBrush_ApplyFilterList (0x412090)
// Accumulates fdhide (bit 3) / fdshow (bit 4) flags from a filter list into
// the returned int bitmask.  Called once per category list from FilterBrush.
// ─────────────────────────────────────────────────────────────────────────────
static int FilterBrush_ApplyFilterList(filter_entry_s *list, brush_t *def)
{
    int flags = 0;
    for ( filter_entry_s *v = list; v; v = v->next_filter )
    {
        if ( (v->filter_type_enum & 3) != 0 && (v->filter_type_enum & 4) == 0 )
        {
            if ( FilterCondition_Eval(def, v->info) )
            {
                if ( v->filter_type_enum & 2 )
                    flags |= 0x10; // fdshow matched
                else
                    flags |= 0x08; // fdhide matched
            }
        }
    }
    return flags;
}

// ─────────────────────────────────────────────────────────────────────────────
// FilterBrush_CheckLayerFaces (0x46A190)
// When current_edit_layer == 1, iterates all faces on a brush definition and
// calls MaterialDef_02 to check if any face passes the material realize.
// ─────────────────────────────────────────────────────────────────────────────
static bool FilterBrush_CheckLayerFaces(brush_t *def)
{
    if ( KiwiMatConvert_ShouldShowBrushInLightmap( def ) ) return true; // KIWI-UX: techset health gates the checker visibility
    // Use the host layout: x64 face_t is 296 bytes and mtldef starts at 40,
    // rather than the original executable's 232-byte stride / 36-byte offset.
    for ( int i = 0; i < def->faceCount; ++i )
    {
        MaterialDef *md = &def->faces[i].mtldef[0];
        dword_181F51C = 2;
        MaterialDef_02(md, MaterialDef_07);
        if ( dword_181F51C )
            return true;   // face passes layer material test
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// FilterBrush (0x46A1F0)
// Main brush visibility/filter check.  Returns non-zero if the brush should be
// hidden / excluded from selection operations.
//
// The result is CACHED in b->brushFlags (bit 0 = hidden by geometry/entity/etc)
// and invalidated by incrementing g_qeglobals.g_filtersUpdated (which RadiantFilters_ToggleEntry
// and Load_RadiantFilters do on filter state change).  b->xx6 stores the last
// seen filtersUpdated value; a mismatch forces recomputation.
//
// full = 0: simple filtered check; non-zero: full hidden+fdshow+fdhide check.
// Returns non-zero if the brush is hidden/filtered.
// ─────────────────────────────────────────────────────────────────────────────
char FilterBrush(selbrush_t *b, int full)
{
    brush_t *def = b->def;
    if ( g_qeglobals.current_edit_layer == 1 )
    {
        // Fixed-size entities are NOT layer-filtered
        if ( !def->owner->eclass->fixedsize && !FilterBrush_CheckLayerFaces(def) )
            return 1;
    }

    if ( b->xx6 != g_qeglobals.g_filtersUpdated )
    {
        // Recompute the cached brushFlags filter bits
        bool filtered = FilterBrush_IsFilteredByAnyList(def);

        if ( (g_qeglobals.d_savedinfo.d_xyShowFlags & 0x40) != 0 )
            filtered = !filtered;  // reverse-filter mode

        if ( filtered )
            b->brushFlags |=  1u;
        else
            b->brushFlags &= ~1u;

        // Accumulate fdhide/fdshow flags from geometry/entity/trigger/other lists.
        // Only 4 lists here (not layerFilters).
        int fd  = FilterBrush_ApplyFilterList(g_qeglobals.d_filterGlobals_geometryFilters, def);
        fd     |= FilterBrush_ApplyFilterList(g_qeglobals.d_filterGlobals_entityFilters,   def);
        fd     |= FilterBrush_ApplyFilterList(g_qeglobals.d_filterGlobals_triggerFilters,  def);
        fd     |= FilterBrush_ApplyFilterList(g_qeglobals.d_filterGlobals_otherFilters,    def);
        // KIWI: replace, not OR - the binary never cleared 0x08 / 0x10, so a brush that stopped
        // matching an fdhide / fdshow filter (classname or material changed) kept the stale bit
        b->brushFlags = ( b->brushFlags & ~0x18u ) | fd;

        b->xx6 = g_qeglobals.g_filtersUpdated;
    }

    if ( !full )
        return (b->brushFlags & 5) != 0;

    int brushFlags = b->brushFlags;
    if ( (brushFlags & 8) != 0 )
        return 1;   // fdhide
    if ( (brushFlags & 0x10) != 0 )
        return (brushFlags & 6) != 0;  // fdshow: also check bit 1/2

    return (b->brushFlags & 5) != 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  faceTexMap — the "filter faces by material name" container: a
//  std::map<std::string,int> per-material-name REFCOUNT.  A face-type filter
//  entry owns a LIST of material-name substrings (filter_entry_s.material_ptr,
//  built by Filter_ParseMaterialList: {char* name; node* next}); when that entry is HIDDEN
//  (isShown→0) every name in its list is added (++refcount); when it is SHOWN
//  (isShown→1) every name is removed (--refcount, erase at 0).  The consumer
//  MtlDef_IsFaceFiltered (mayaexport.cpp) tests a face's material name
//  against every key by SUBSTRING (strstr) — if any filtered substring occurs in
//  the face material's name, the face is filtered out of the view.
//
//  The map lives in this TU; nothing else references it directly — readers go
//  through FaceTexMap_HasSubstringOf / Filter_IsFaceMaterialHidden below.
// ═════════════════════════════════════════════════════════════════════════════
static std::map<std::string, int> s_faceTexMap;   // 0x26656A0

// material-name list node built by Filter_ParseMaterialList (filter_entry_s.material_ptr
// chain): {char* name; FaceTexNode* next}.  The chain is typed "MaterialInfo*"
// (a misnomer — it is the FaceTexNode list the parser allocates).
struct FaceTexEntry { const char *name; FaceTexEntry *next; };

// ─────────────────────────────────────────────────────────────────────────────
// Filter_AddFaceMaterialRefs (0x411be0) — INCREMENT each
//   material-name refcount in faceTexMap.  Called on HIDE (Filter_Hide) and at load
//   for a face entry that starts unchecked (RadiantFilters).  Walks
//   filter->material_ptr and for each name does `++faceTexMap[name]` (insert 1 if
//   absent).  Returns the last list node walked.
// ─────────────────────────────────────────────────────────────────────────────
MaterialInfo *Filter_AddFaceMaterialRefs(filter_entry_s *filter)
{
    FaceTexEntry *node = (FaceTexEntry *)filter->material_ptr;
    FaceTexEntry *last = node;
    for ( ; node; last = node, node = node->next )
        ++s_faceTexMap[node->name];
    return (MaterialInfo *)last;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter_ReleaseFaceMaterialRefs (0x411e30) — DECREMENT each
//   material-name refcount in faceTexMap, erasing the entry when it hits 0.  Called on
//   SHOW (Filter_Show).  For each name: find (asserts the key is present —
//   "mapIter != ...faceTexMap.end()"), `--*value`, and erase() when it reaches 0.
//   Returns the last list node walked.
// ─────────────────────────────────────────────────────────────────────────────
MaterialInfo *Filter_ReleaseFaceMaterialRefs(filter_entry_s *filter)
{
    FaceTexEntry *node = (FaceTexEntry *)filter->material_ptr;
    FaceTexEntry *last = node;
    for ( ; node; last = node, node = node->next )
    {
        std::map<std::string, int>::iterator it = s_faceTexMap.find( node->name );
        iassert( it != s_faceTexMap.end() );
        if ( it == s_faceTexMap.end() )
            continue;
        if ( --it->second == 0 )
            s_faceTexMap.erase( it );
    }
    return (MaterialInfo *)last;
}

// Reader hook for MtlDef_IsFaceFiltered (0x46FBE0, mayaexport.cpp): true iff ANY
// faceTexMap key is a SUBSTRING of `materialName`.  Empty map ⇒ false (the default/headless case).
bool FaceTexMap_HasSubstringOf( const char *materialName )
{
    if ( !materialName )
        return false;
    for ( std::map<std::string, int>::const_iterator it = s_faceTexMap.begin();
          it != s_faceTexMap.end(); ++it )
        if ( strstr( materialName, it->first.c_str() ) )
            return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter_IsFaceMaterialHidden (0x46FCF0) — the picker's face-texture-filter test.  Walk every key in
// faceTexMap and register its render material via Texture_GetHandle; return 1 iff any
// key's registered Material* (qtexture_s.next, +0) equals the picked face's render
// material.  The picker (Test_Ray cycle-pick, BrushList_Trace) calls this on the hit
// face's material so a face hidden by a Texture filter is not pickable.  Empty map ⇒ 0
// (the default/headless case — no face filter active, so every face is pickable).
// ─────────────────────────────────────────────────────────────────────────────

char Filter_IsFaceMaterialHidden( Material *faceMtl )
{
    for ( std::map<std::string, int>::const_iterator it = s_faceTexMap.begin();
          it != s_faceTexMap.end(); ++it )
    {
        // a >=64-char key makes Texture_GetHandle print its "too long" message.
        if ( Texture_GetHandle( it->first.c_str() )->next == faceMtl )   // ->next == registered Material*
            return 1;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter_Show (0x411b60) — set a filter entry's isShown = 1.
// For a face-type filter (bit 2), the material names it owns are no longer hidden,
// so DECREMENT their faceTexMap refcounts.
// ─────────────────────────────────────────────────────────────────────────────
void Filter_Show(filter_entry_s *filter)
{
    iassert( !filter->isShown );
    filter->isShown = 1;
    if ( filter->filter_type_enum & 4 )
        Filter_ReleaseFaceMaterialRefs(filter);
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter_Hide (0x411ba0) — set a filter entry's isShown = 0.
// For a face-type filter (bit 2), the material names it owns are now hidden, so
// INCREMENT their faceTexMap refcounts.
// ─────────────────────────────────────────────────────────────────────────────
void Filter_Hide(filter_entry_s *filter)
{
    iassert( filter->isShown );
    filter->isShown = 0;
    if ( filter->filter_type_enum & 4 )
        Filter_AddFaceMaterialRefs(filter);
}

// Forward-decl: the recursive condition-tree free (defined with the layer code below).
static void Filters_FreeCondTree( filter_info_s *info );

// ─────────────────────────────────────────────────────────────────────────────
// RadiantFilters (0x410c50) — parse a RadiantFilters.txt file and build
// the filter_entry_s linked lists.
//
// Grammar (per block):  <Category> <Name> [fdhide|fdshow|face] { <condition body> }
//   Category ∈ {geometry, trigger, entity, other} → the matching d_filterGlobals_* list.
//   Modifiers: fdhide → type|=1, fdshow → type|=2, face → type|=4.
//   Body: face → Filter_ParseMaterialList; else → DynamicFilter_ParseCondition.
//   Then the registry default ("Filters" <name>, default 1) is read and isShown=0 set
//   if the user had unchecked it (CFilterWnd_GetSavedCheck).
//
// Parsing: Com_BeginParseSession + Com_Parse (space-delimited token reads) /
// Com_ParseOnLine (the on-line modifier reads) / Com_UngetToken.  spaceDelimited
// is toggled around the { } body.
// ─────────────────────────────────────────────────────────────────────────────
extern int CFilterWnd_GetSavedCheck( const char *name );            // registry "Filters\<name>"
static int LoadFile_Resolved( const char *name, void **buf );       // cwd → <fs_basepath>\bin\<name>

static void Filter_SetSpaceDelimited( int on )
{
    ParseThreadInfo *pi = Com_GetParseThreadInfo();
    pi->parseInfo[pi->parseInfoNum].spaceDelimited = (char)on;
}

void RadiantFilters( const char *filterFile )
{
    void *buf = nullptr;
    if ( LoadFile_Resolved( filterFile, &buf ) < 0 || !buf )
        return;                                  // file missing → no filters from it

    const char *text = (const char *)buf;
    Com_BeginParseSession( filterFile );
    Filter_SetSpaceDelimited( 1 );

    while ( true )
    {
        parseInfo_t *cat = Com_Parse( &text );   // category keyword (space-delimited)
        if ( !text )
            break;                               // EOF

        filter_entry_s **listHead;
        if      ( !_stricmp( cat->token, "geometry" ) ) listHead = &g_qeglobals.d_filterGlobals_geometryFilters;
        else if ( !_stricmp( cat->token, "trigger"  ) ) listHead = &g_qeglobals.d_filterGlobals_triggerFilters;
        else if ( !_stricmp( cat->token, "entity"   ) ) listHead = &g_qeglobals.d_filterGlobals_entityFilters;
        else if ( !_stricmp( cat->token, "other"    ) ) listHead = &g_qeglobals.d_filterGlobals_otherFilters;
        else
        {
            Sys_Printf( "unknown filter categegory type '%s' in RadiantFilters.txt\n", cat->token );
            Com_SkipRestOfLine( &text );
            continue;
        }

        // Filter NAME (read on-line after the category).
        parseInfo_t *nm = Filter_GetToken( &text );
        char nameBuf[256];
        strncpy( nameBuf, nm->token, sizeof( nameBuf ) - 1 );
        nameBuf[sizeof( nameBuf ) - 1] = '\0';

        // Optional modifier tokens (fdhide/fdshow/face), still on the same line, until
        // the line runs out (an empty on-line token), then `{` is expected.
        int typeFlags = 0;
        while ( true )
        {
            parseInfo_t *mod = Filter_GetToken( &text );
            if ( !mod->token[0] )
                break;                           // end of line → modifiers done
            if      ( !_stricmp( mod->token, "fdhide" ) ) typeFlags |= 1;
            else if ( !_stricmp( mod->token, "fdshow" ) ) typeFlags |= 2;
            else if ( !_stricmp( mod->token, "face"   ) ) typeFlags |= 4;
            // (any other on-line token is ignored)
        }

        // Expect `{` (space-delimited), then parse the body with spaceDelimited=0.
        parseInfo_t *open = Com_Parse( &text );
        if ( strcmp( open->token, "{" ) )
            Com_ScriptErrorDrop( "MatchToken: got '%s', expected '%s'\n", open->token, "{" );
        Filter_SetSpaceDelimited( 0 );

        MaterialInfo  *faceMtl   = nullptr;
        filter_info_s *condTree  = nullptr;
        bool           isFace    = (typeFlags & 4) != 0;
        if ( isFace )
            faceMtl = (MaterialInfo *)Filter_ParseMaterialList( &text );
        else
            condTree = DynamicFilter_ParseCondition( &text ); // condition tree

        // Expect `}` (back to space-delimited).
        Filter_SetSpaceDelimited( 1 );
        parseInfo_t *close = Com_Parse( &text );
        if ( strcmp( close->token, "}" ) )
            Com_ScriptErrorDrop( "MatchToken: got '%s', expected '%s'\n", close->token, "}" );

        // Build the entry and prepend it to the category list (*listHead is the head).
        filter_entry_s *e = (filter_entry_s *)operator new( sizeof( filter_entry_s ) );
        e->filter_type_enum = typeFlags;
        e->isShown          = true;              // shown by default
        e->material_ptr     = faceMtl;           // face: material-name list; else null
        e->name             = AllocMaterialString( nameBuf );
        e->info             = condTree;
        e->next_filter      = *listHead;
        *listHead           = e;

        // Honour the saved checkbox state (registry "Filters\<name>", default 1=shown).
        // For a FACE entry that starts hidden, Filter_AddFaceMaterialRefs(e) seeds the
        // faceTexMap refcounts.
        if ( !CFilterWnd_GetSavedCheck( e->name ) )
        {
            e->isShown = false;
            if ( isFace )
                Filter_AddFaceMaterialRefs( e );
        }
    }

    // End the parse session (Com_EndParseSession — pop the parseInfo frame).
    Com_EndParseSession();
    free( buf );
}

// ─────────────────────────────────────────────────────────────────────────────
// Load_RadiantFilters (0x411190) — load RadiantFilters.txt + the user
// filter file into the (empty-at-startup) filter lists.  Called at startup
// (radiant_main.cpp).  The binary just calls RadiantFilters("RadiantFilters.txt"),
// relying on the editor's cwd being the install bin/ dir (where the .exe + the .txt
// sit).  This port can be launched from anywhere (Start-Process sets cwd to the caller),
// so we RESOLVE the path: bare name (cwd, the original case) → <fs_basepath>\bin\<name>
// (the known install location — fs_basepath itself is the install root, com_files.cpp).
// ─────────────────────────────────────────────────────────────────────────────
const char *Dvar_GetString( const char *dvarName );              // qcommon (C++ linkage; fs_basepath)

static int LoadFile_Resolved( const char *name, void **buf )
{
    // 1) bare name (cwd) — works when the editor runs from the install bin/.
    int n = (int)LoadFile( name, buf );
    if ( n >= 0 && *buf )
        return n;
    // 2) <fs_basepath>\bin\<name> — the install location of RadiantFilters.txt.
    const char *base = Dvar_GetString( "fs_basepath" );
    if ( base && base[0] )
    {
        char path[1024];
        _snprintf( path, sizeof( path ) - 1, "%s\\bin\\%s", base, name );
        path[sizeof( path ) - 1] = '\0';
        n = (int)LoadFile( path, buf );
        if ( n >= 0 && *buf )
            return n;
    }
    return -1;                            // not found in either location
}

void Load_RadiantFilters()        // g_PrefsDlg / prefData_t from prefs.h (included above)
{
    g_qeglobals.g_filtersUpdated = 1;
    RadiantFilters( "RadiantFilters.txt" );

    // The user's extra filter file, if one is configured in preferences.
    if ( g_PrefsDlg && (int)g_PrefsDlg->m_strUserFilterPath.length() > 0 )
        RadiantFilters( g_PrefsDlg->m_strUserFilterPath.c_str() );
}

// ─────────────────────────────────────────────────────────────────────────────
// RadiantFilters07 (0x4149c0) — toggle ONE filter entry's isShown (the CFilterWnd
// checklist click).  The binary reads the new check state from the CCheckListBox and
// asserts it differs from the entry's current isShown.  This port exposes the CORE
// (no MFC control coupling) so the hand-built listbox UI can drive it directly:
//   show=true  → Filter_Show; show=false → Filter_Hide (face entries touch faceTexMap)
// then ++g_filtersUpdated (invalidate the per-brush FilterBrush cache) + repaint.
// ─────────────────────────────────────────────────────────────────────────────
void RadiantFilters_ToggleEntry( filter_entry_s *cb, bool show )
{
    // KIWI: a same-state call is a no-op (it would double-count the face refcounts).
    if ( !cb || cb->isShown == show )
        return;
    if ( show ) Filter_Show( cb );
    else        Filter_Hide( cb );
    // KIWI: remember it - the startup load reads [Filters] <name>, but nothing ever wrote it
    // (the binary's Filters_Free 0x411AD0 did, at exit), so every toggle reset on relaunch.
    if ( cb->name )
        CFilterWnd_SaveCheck( cb->name, show );
    ++g_qeglobals.g_filtersUpdated;                  // invalidate the FilterBrush cache
    g_nUpdateBits = -1;                              // repaint all views
}

// ═════════════════════════════════════════════════════════════════════════════
//  CFilterWnd PUBLIC API — what the hand-built Filters inspector pane (win_ent.cpp)
//  drives.  Two control groups, exactly the binary's CFilterWnd:
//    (1) the four category CHECKLISTS (geometry/trigger/entity/other), each a list of
//        the loaded filter_entry_s nodes the user checks/unchecks → RadiantFilters_-
//        ToggleEntry (= RadiantFilters07).  Enumerated via CFilterWnd_GetCategoryHead.
//    (2) the six SIMPLE checkboxes (Angles/Connections/Names/Blocks/Coordinates/Reverse
//        Filter), each a d_xyShowFlags bit — exactly CFilterWnd::GetSettings
//        pushes into them, and the same bits the View→Show menu commands flip.  The
//        get/set here mirror that 1:1 (a checked box = the feature SHOWN = bit CLEAR,
//        except ReverseFilter where checked = bit SET — the inverted polarity GetSettings
//        encodes as `(d_xyShowFlags >> 6) & 1`).
// ═════════════════════════════════════════════════════════════════════════════

// Registry "Filters\<name>" saved isShown default (1 = shown).  Radiant_ProfileGetInt
// reads HKCU\...\<app>\Filters; defaults to 1 if absent — matching the binary's
// GetProfileIntA(., "Filters", name, 1).  Headless-safe: the free-function
// profile helper needs no CWinApp and just returns the default when the key is absent.
int CFilterWnd_GetSavedCheck( const char *name )
{
    return Radiant_ProfileGetInt( "Filters", name, 1 );
}

// Persist a filter's checkbox state (the binary wrote it at app close, Filters_Free; KIWI
// writes on each toggle - RadiantFilters_ToggleEntry - so it survives a restart or a crash).
void CFilterWnd_SaveCheck( const char *name, bool shown )
{
    Radiant_ProfileSetInt( "Filters", name, shown ? 1 : 0 );
}

// Enumerate: return the head of one category's filter list (0..3 = geo/trig/ent/other),
// matching the binary's CFilterWnd::InitFilterCheckboxes order (over
// geometryFilters/entityFilters/triggerFilters/otherFilters).  Walk ->next_filter.
filter_entry_s *CFilterWnd_GetCategoryHead( int category )
{
    switch ( category )
    {
    case 0: return g_qeglobals.d_filterGlobals_geometryFilters;
    case 1: return g_qeglobals.d_filterGlobals_triggerFilters;
    case 2: return g_qeglobals.d_filterGlobals_entityFilters;
    case 3: return g_qeglobals.d_filterGlobals_otherFilters;
    default: return nullptr;
    }
}

// The six simple show-flag checkboxes (CFilterWnd::GetSettings 0x4140f0).  index →
// (d_xyShowFlags bit, "checked when bit clear?" polarity).  Angles=0x2, Connections=0x4,
// Names=0x8, Blocks=0x10, Coordinates=0x20 are SHOWN when the bit is CLEAR (so checked ==
// !bit); ReverseFilter=0x40 is ON when the bit is SET (checked == bit) — GetSettings'
// `(flags>>6)&1` encodes exactly that inversion.
struct ShowFlagBox { const char *label; int bit; bool checkedWhenClear; };
static const ShowFlagBox s_showFlagBoxes[6] = {
    { "Angles",         0x02, true  },
    { "Connections",    0x04, true  },
    { "Names",          0x08, true  },
    { "Blocks",         0x10, true  },
    { "Coordinates",    0x20, true  },
    { "Reverse Filter", 0x40, false },
};
int  CFilterWnd_ShowFlagCount()                  { return 6; }
const char *CFilterWnd_ShowFlagLabel( int i )    { return (i>=0&&i<6) ? s_showFlagBoxes[i].label : ""; }

// Is checkbox i currently checked (read the d_xyShowFlags bit through its polarity)?
bool CFilterWnd_GetShowFlag( int i )
{
    if ( i < 0 || i >= 6 ) return false;
    int set = (g_qeglobals.d_savedinfo.d_xyShowFlags & s_showFlagBoxes[i].bit) != 0;
    return s_showFlagBoxes[i].checkedWhenClear ? (set == 0) : (set != 0);
}

// Set checkbox i's state → flip the d_xyShowFlags bit accordingly + repaint.  This is the
// programmatic equivalent of the user toggling the box (or the View→Show menu command).
// Every flag has a live draw/filter consumer.
void CFilterWnd_SetShowFlag( int i, bool checked )
{
    if ( i < 0 || i >= 6 ) return;
    const ShowFlagBox &b = s_showFlagBoxes[i];
    bool wantBitSet = b.checkedWhenClear ? !checked : checked;
    if ( wantBitSet ) g_qeglobals.d_savedinfo.d_xyShowFlags |=  b.bit;
    else              g_qeglobals.d_savedinfo.d_xyShowFlags &= ~b.bit;
    ++g_qeglobals.g_filtersUpdated;   // reverse-filter (0x40) feeds FilterBrush's cache
    g_nUpdateBits = -1;               // repaint all views (Names/Coords/Reverse are visible)
}

// ═════════════════════════════════════════════════════════════════════════════
//  LAYER VISIBILITY FILTERS — the LayersDlg → FilterBrush bridge.
//  A layer is "hidden" at runtime by an entry in d_filterGlobals_layerFilters
//  whose condition is `script_layer == <layer name>` (filter case 5) and whose
//  isShown flag is 0.  FilterBrush → FilterBrush_IsFilteredByAnyList →
//  FilterBrush_CheckFilterListHide(layerFilters) → FilterCond_KeyValue(case 5) →
//  Entity_HasEpairMatch(brush->owner, "script_layer", name).  The LayersDlg's
//  CCheckListBox at the bottom maps 1:1 onto these entries.
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// Layers_BuildFilter (0x411950) — prepend a script_layer filter entry.
// Allocates a filter_entry_s + filter_info_s + filter_contents_s,
// wires key="script_layer", value=<name>, surfaceFlags=5 (the case-5 evaluator),
// isShown=1 (visible by default).  Returns the value string.
// ─────────────────────────────────────────────────────────────────────────────
const char *Layers_BuildFilter( const char *layerName )
{
    filter_entry_s *entry = (filter_entry_s *)operator new( sizeof( filter_entry_s ) );
    entry->name            = AllocMaterialString( layerName );
    entry->isShown         = true;
    entry->filter_type_enum= 0;               // not face/fdhide/fdshow — a condition filter
    entry->next_filter     = g_qeglobals.d_filterGlobals_layerFilters;  // prepend
    entry->material_ptr    = nullptr;
    g_qeglobals.d_filterGlobals_layerFilters = entry;

    filter_info_s *info = Filter_NewInfo( FILT_KEYVALUE );

    filter_contents_s *cond = (filter_contents_s *)operator new( sizeof( filter_contents_s ) );
    cond->key   = AllocMaterialString( "script_layer" );
    const char *valStr = AllocMaterialString( layerName );
    cond->value = (char *)valStr;
    cond->flags = 0;                          // epair-exact path
    info->contents_ptr = cond;

    entry->info = info;
    return valStr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filters_FreeCondTree (0x4119D0) — recursively free a condition tree.
// Case 5 (script_layer): free key, value, the contents node, then the info node.
// (The full Filters_Free ALSO writes each entry's isShown to the registry
//  via CWinApp::WriteProfileInt — undesirable for the transient, rebuilt-every-
//  refresh layer filters — so the layer path frees without persisting.)
// ─────────────────────────────────────────────────────────────────────────────
extern "C" void free( void * );
static void Filters_FreeCondTree( filter_info_s *info )
{
    // chase the sibling link per node.
    while ( info )
    {
        filter_info_s *self = info;
        int type = info->surfaceFlags;
        info = info->next;   // next sibling
        filter_contents_s *c = self->contents_ptr;
        switch ( (unsigned)type )
        {
        case 1u: case 2u:                                // AND/OR — recurse children
            Filters_FreeCondTree( (filter_info_s *)c );
            break;
        case 3u: case 6u:
            free( *(void **)c );  free( c );
            break;
        case 4u: case 7u: case 8u:
            free( c );
            break;
        case 5u:                                         // key/value (script_layer)
            free( c->key );  free( c->value );  free( c );
            break;
        default: break;
        }
        free( self );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Layers_FreeVisibilityFilters — free the whole layerFilters list (entry + name +
// condition tree) WITHOUT the registry write, then null the head.  Matches the
// effect of 0x41C200's `Filters_Free(layerFilters); layerFilters = 0;` prologue
// for these synthetic, every-refresh-rebuilt entries.
// ─────────────────────────────────────────────────────────────────────────────
void Layers_FreeVisibilityFilters()
{
    filter_entry_s *f = g_qeglobals.d_filterGlobals_layerFilters;
    while ( f )
    {
        filter_entry_s *next = f->next_filter;
        if ( f->name )
            free( (void *)f->name );
        if ( f->info )
            Filters_FreeCondTree( f->info );             // our entries are non-face (cond tree)
        free( f );
        f = next;
    }
    g_qeglobals.d_filterGlobals_layerFilters = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layers_FindVisibilityFilter — locate a layer's filter entry by name (the dialog
// queries/toggles ->isShown).  Returns nullptr if the layer carries no brushes
// (so no filter entry was built for it).
// ─────────────────────────────────────────────────────────────────────────────
filter_entry_s *Layers_FindVisibilityFilter( const char *layerName )
{
    for ( filter_entry_s *f = g_qeglobals.d_filterGlobals_layerFilters; f; f = f->next_filter )
        if ( f->name && !strcmp( f->name, layerName ) )
            return f;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layers_SetLayerVisible — the runtime visibility toggle (RadiantFilters07 tail,
// 0x4149c0).  Sets a layer-filter entry's isShown, invalidates the per-brush filter
// cache (++g_filtersUpdated — every brush re-evaluates FilterBrush) and forces a
// redraw (g_nUpdateBits = -1).  Returns false if the layer has no filter entry.
// ─────────────────────────────────────────────────────────────────────────────
bool Layers_SetLayerVisible( const char *layerName, bool visible )  // g_nUpdateBits declared at top
{
    filter_entry_s *f = Layers_FindVisibilityFilter( layerName );
    if ( !f )
        return false;
    f->isShown = visible;
    ++g_qeglobals.g_filtersUpdated;   // invalidate FilterBrush cache (b->xx6 != g_filtersUpdated)
    g_nUpdateBits = -1;               // force all views to repaint
    return true;
}

