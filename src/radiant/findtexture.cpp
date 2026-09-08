#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Find/replace textures across brushes, patches, and optional prefab sub-maps.
// Hex-Rays invents FPU arguments for 0x493160; its real signature is the one below.
// sub_492E20 receives a 56-byte selbrush_t instance, not the 88-byte brush definition.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <string>

extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ── prefab-recursion dependencies (map.cpp) ───────────────────────────────────
// The (owner->prefab && flag&4) branch enters the selected prefab's referenced .map,
// re-runs the find/replace inside it, and re-saves it to disk — the real editor's
// "recurse into prefabs" behaviour (sub_492E20 0x492E5D..0x492F63).
extern void Prefab_NextLevel( void *a1 );   // map.cpp 0x489190 (ENTER the prefab sub-map)
extern void Prefab_PrevLevel();             // map.cpp 0x489890 (LEAVE, restore parent)
extern void Map_SaveFile( const char *path, char a1, char a2 );  // map.cpp 0x486C00
extern char currentmap[];                   // map.cpp 0x23F18D8 (the loaded prefab .map path)

// ══════════════════════════════════════════════════════════════════════════════
//  g_findReplaceVisited  —  std::set<std::string>  (IDB head/node @0x26656B0/B4).
//
//  The prefab-recursion branch dedups the prefab .map names it has already entered so
//  a diamond/self prefab reference can't recurse forever.  The IDB's container is a
//  textbook MSVC std::set<std::string> RB-tree:
//    * FindReplaceVisited_Find   (sub_4944B0) = lower_bound (sub_494540) + a !(key<*it)
//      ordering check (sub_413C70 = std::string operator<) → set::find.
//    * FindReplaceVisited_Insert (sub_41E590) = _Tree::insert returning pair<it,bool>.
//    * Str_ConstructFromCStr     (0x412760)   = std::string(const char*).
//  Per the #18 STL-collapse rule (the editor owns reader+writer; no cross-module ABI),
//  the faithful translation is an ordinary std::set<std::string> with set::find /
//  set::insert.  The visited set is RESET (Set_EraseTreeRec + reset the 3 RB sentinels,
//  = clear()) at each top-level entry point — the dialog OnOK/OnApply handlers
//  (0x415B50 / 0x415A90) AND LayeredMaterials_texcoords (0x417190) — NOT inside
//  FindReplaceTextures, so the recursion's re-entry of FindReplaceTextures accumulates
//  into the same set rather than wiping it mid-walk.
static std::set<std::string> g_findReplaceVisited;

// Reset the prefab-recursion visited set (IDB: Set_EraseTreeRec(...right) + reset the 3
// RB-tree sentinels + dword_26656B8=0 → clear()).  Called by the dialog handlers and by
// LayeredMaterials_texcoords before each top-level FindReplaceTextures invocation.
void FindReplaceVisited_Reset()
{
    g_findReplaceVisited.clear();
}

// ── brush lists + selection (engine_stubs / map.cpp / select.cpp) ──────────────
extern selbrush_t selected_brushes;          // 0x23F1864
extern selbrush_t active_brushes;            // 0x23F189C
extern void       Select_Deselect( int keepSelected );        // select.cpp 0x48E800
extern int        g_nUpdateBits;             // engine_stubs 0x25D5A74

// ── material name resolution + apply primitives ───────────────────────────────
// NOTE: the IDB inlines LayeredMaterials_GetMaterial(name) / else Texture_GetHandle(name)
// to resolve the replacement material; we call SetMaterial (which IS that dispatch, but
// with the headless-safe degenerate fallback — Texture_GetHandle needs the live renderer).
extern void              SetMaterial( const char *name, patchMesh_material *out );  // materialdef.cpp 0x4315C0
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *m );                     // materialdef.cpp 0x431640
extern int               Init_MaterialLayer( MaterialDef *channel, float src ); // materialdef.cpp 0x472C00
namespace LayerMat { int GetCurrentLayer( MaterialDef *def ); }                     // 0x431B30

// face texdef apply (brush.cpp):
//   Brush_SetFaceTexdefSize (0x476740) — write the {Material*, qtexture*} pair into the
//     face's current-layer slot (the IDB's sub_476740 per-face apply, a1 = a 2-elem array).
//   Brush_SetFaceTexdef     (0x4767e0) — copy a full texdef_sub_t mapping into the slot.
extern void    Brush_SetFaceTexdefSize( const patchMesh_material *material, face_t *f, brush_t *b );  // 0x476740
extern void    sub_4767E0( const texdef_sub_t *texDef, face_t *facePtr, brush_t *brushDef );   // 0x4767E0 Brush_SetFaceTexdef

// rebuild / housekeeping (brush.cpp / select.cpp / map.cpp):
extern void    Brush_BuildWindings( brush_t *b, int bFull );  // 0x477AC0
extern void    SetupVertexSelection();                        // 0x494BC0
extern void    MarkMapModified();                             // 0x499BB0

// ── current-texture-window MaterialDef (qeglobals_t.random_texture_stuff) ──────
// The IDB indexes &random_texture_stuff[2100 * current_edit_layer] as the active
// MaterialDef, and [...+36] (= mat_texDef.sample_size, byte 36) as a packed sample-size
// float (the flag&8 "Live" path seeds the layer mapping from it).
//   2100 = 0x834 = the per-layer stride (a LayerMat block); the MaterialDef is at its head.


// ══════════════════════════════════════════════════════════════════════════════
//  FindReplaceTexture_Brush  (sub_492E20) — per-brush find/replace worker.
//  `inst` is the selbrush_t instance, not its brush definition.
//  Returns 1 if any face/patch was replaced on this brush.
// ══════════════════════════════════════════════════════════════════════════════
// Forward decl (the prefab branch recurses through FindReplaceTextures).
bool FindReplaceTextures( const char *find, const char *replace, char flags );

static char FindReplaceTexture_Brush( selbrush_t *inst, const char *findName,
                                      const char *replaceName, char flags )
{
    entity_s *owner = inst->owner;
    if ( owner->prefab )
    {
        // flag&4 edits and re-saves referenced prefab maps. The visited set prevents cycles;
        // bit 0 is cleared so recursion walks the whole sub-map.
        char prefabReplaced = 0;
        if ( ( flags & 4 ) != 0 )
        {
            // Prefab .map name = owner->def->modelClass->x02 ([modelClass + 4]).
            entity_s_def *def = (entity_s_def *)owner->def;
            const char   *prefabMap = *(const char **)( (char *)def->modelClass + 4 );

            // g_findReplaceVisited.find(prefabMap) == end() → not yet visited.
            if ( g_findReplaceVisited.find( prefabMap ) == g_findReplaceVisited.end() )
            {
                g_findReplaceVisited.insert( prefabMap );      // mark visited (set::insert)

                Prefab_NextLevel( inst );                      // ENTER the prefab sub-map
                // flags & 0xFE: clear bit0 ("selected only") so the recursion walks ALL of
                // the sub-map's brushes; keep bits 2/4/8 so nested prefabs recurse too.
                prefabReplaced = FindReplaceTextures( findName, replaceName,
                                                      (char)( flags & 0xFE ) ) ? 1 : 0;
                if ( prefabReplaced )
                    Map_SaveFile( currentmap, 0, 0 );          // re-save the modified .map
                Prefab_PrevLevel();                            // LEAVE, restore the parent
            }
        }
        return prefabReplaced;               // prefab owner: never falls through to the
                                             // face/patch walk (disasm jumps to the epilogue)
    }

    char replaced = 0;

    // Patch path: inst->patch (hex-rays "mins[0]") non-null → it's a patch brush.
    if ( inst->patch )
    {
        extern char Patch_FindReplaceTexture( brush_t *b, const char *replaceName,
                                              const char *findName, char flags );   // pmesh.cpp 0x449520
        if ( Patch_FindReplaceTexture( inst->def, replaceName, findName, flags ) )
            replaced = 1;
    }

    // Face walk over the brush DEF's faces.  The IDB reads inst->faceCount (hex-rays
    // "unk1", the 56-byte INSTANCE field @0x18) — but that is the LAZY faceVis cache that
    // sub_477D70 syncs from def->faceCount only when the brush is drawn/selected (0 on a
    // freshly-loaded, undrawn brush — the recurring instance-vs-def trap).  In the GUI the
    // dialog always runs after a draw so it is synced; we read def->faceCount directly (the
    // authoritative count the apply primitives assert against — Brush_SetFaceTexdefSize uses
    // &b->faces[b->faceCount]), so the replace also works headless / pre-draw.
    brush_t *def = inst->def;
    if ( def && def->faceCount )
    {
        face_t  *faces = def->faces;
        for ( int fi = 0; fi < (int)(unsigned)def->faceCount; ++fi, ++faces )
        {
            MaterialDef *md = &faces->mtldef[g_qeglobals.current_edit_layer];

            bool match;
            if ( flags & 2 )
            {
                match = true;                // "replace everywhere, don't test against Find"
            }
            else
            {
                // MtlDef_IsValid + name extraction (sub_492E20 inlined): exactly one of
                // lyrMtl / radMtl; name = (char*)lyrMtl, or radMtl->name (qtexture_s+4).
                // the binary inlines Materialdef_GetName here (MaterialDef.cpp:85 lives in it)
                const char *name = (const char *)Materialdef_GetName( md );
                match = ( _stricmp( name, findName ) == 0 );
            }

            if ( !match )
                continue;

            // flag&8 "Live": also stamp the current-texture-window's layer MAPPING onto
            // this face (Init_MaterialLayer reseeds the active layer from its sample-size,
            // then Brush_SetFaceTexdef copies that texdef in). The sample-size
            // is the float bit-pattern at random_texture_stuff[2100*layer + 36].
            if ( ( flags & 8 ) != 0 )
            {
                int            layer  = g_qeglobals.current_edit_layer;
                MaterialDef   *curMtl = &g_qeglobals.random_texture_stuff[layer].mtl;
                float sampleArg = g_qeglobals.random_texture_stuff[layer].sampleSize;
                Init_MaterialLayer( curMtl, sampleArg );
                int curLayer = LayerMat::GetCurrentLayer( curMtl );
                sub_4767E0( &curMtl->mat_texDef + curLayer,
                            faces, def );
            }

            // Resolve the replacement name to its {lyrMtl, radMtl} pair and write it into
            // the face's current-layer slot (the proven texture-apply path: the IDB's
            // sub_476740 / Brush_SetFaceTexdefSize).  The IDB inlines
            // LayeredMaterials_GetMaterial(name) / else Texture_GetHandle(name) here; we go
            // through SetMaterial (materialdef.cpp) instead — it is EXACTLY that dispatch
            // but with the headless-safe degenerate fallback when the renderer/material
            // system isn't up (Texture_GetHandle needs the renderer; SetMaterial gates on
            // g_radiantFirstLightRendererReady).  This is the same call the texwnd
            // click-apply (proven) uses.
            patchMesh_material pair{};
            SetMaterial( replaceName, &pair );   // its tail iassert carries MaterialDef.cpp:65
            Brush_SetFaceTexdefSize( (const patchMesh_material *)&pair, faces, def );
            replaced = 1;
        }
    }

    // Rebuild the brush after edits (matches the IDB tail: build windings, vertex-sel
    // upkeep, mark modified, bump version).  (def resolved above; always valid for a real
    // brush — guarded so a degenerate node can't AV.)
    if ( def )
    {
        Brush_BuildWindings( def, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++def->version;
    }
    g_nUpdateBits |= 1u;

    return replaced;
}

// ══════════════════════════════════════════════════════════════════════════════
//  FindReplaceTextures  (0x493160) — the public entry.  flag&1 = selected brushes
//  only (else deselect everything and walk active_brushes).  Returns true if any
//  face was replaced.
// ══════════════════════════════════════════════════════════════════════════════
bool FindReplaceTextures( const char *find, const char *replace, char flags )
{
    selbrush_t *list = &selected_brushes;
    if ( ( flags & 1 ) == 0 )
    {
        list = &active_brushes;
        Select_Deselect( 1 );
    }

    bool replaced = false;
    for ( selbrush_t *i = list->next; i != list; i = i->next )
    {
        if ( FindReplaceTexture_Brush( i, find, replace, flags ) )
            replaced = true;
    }

    g_nUpdateBits |= 1u;
    return replaced;
}

// UI-independent action behind CFindTextureDlg's OK / Apply buttons.
void FindTexture_Apply( const char *find, const char *replace,
                        bool bSelectedOnly, bool bForce, bool bRecursePrefabs, bool bLive )
{
    if ( !replace[0] )                 // nothing to replace with → no-op (GtkRadiant: FindReplace skips empty)
        return;

    // Flag byte (IDB OnOK/OnApply 0x415B50 / 0x415A90 — the DDX members at this+116/128/132/140):
    //   bit0 = selected-only, bit1 = force-replace-all, bit2 = recurse-prefabs, bit3 = live.
    char flags = 0;
    if ( bSelectedOnly )   flags |= 1;
    if ( bForce )          flags |= 2;
    if ( bRecursePrefabs ) flags |= 4;
    if ( bLive )           flags |= 8;

    // Reset the prefab-recursion visited set at the top-level entry (the IDB's
    // Set_EraseTreeRec + reset-sentinels before each FindReplaceTextures call) so a
    // fresh OK/Apply starts with an empty visited set; the recursion accumulates into it.
    FindReplaceVisited_Reset();

    FindReplaceTextures( find, replace, flags );
    g_nUpdateBits = -1;
}

// UI-independent read behind the Find field's pre-fill (nullptr when the active layer
// has no single valid material).
const char *FindTexture_GetCurrentMaterialName()
{
    int layer = g_qeglobals.current_edit_layer;
    if ( layer >= 0 && layer < 3 )
    {
        MaterialDef *cur = &g_qeglobals.random_texture_stuff[layer].mtl;
        if ( ( (cur->lyrMtl != nullptr) + (cur->radMtl != nullptr) ) == 1 )
            return (const char *)Materialdef_GetName( cur );
    }
    return nullptr;
}

// ══════════════════════════════════════════════════════════════════════════════
// MFC shell — the find/replace texture modeless popup. The hand-built controls preserve
// the binary IDs and flag layout because the original dialog resource is unavailable.
// U-GUARD: everything above stays COMMON (imgui_panel_findtex.cpp calls FindTexture_Apply
// / FindTexture_GetCurrentMaterialName), and so does byte_73C380 below — texwnd.cpp reads
// that flag OUTSIDE its own fence.
// ══════════════════════════════════════════════════════════════════════════════

// byte_73C380 (0x73c380) — which edit field a picked texture name flows into: 1 = Find
// (sub_415CE0), 0 = Replace (sub_415D40).  The IDB toggles it from the per-edit
// EN_SETFOCUS handlers sub_415DF0 (Find 1097 → 1) / sub_415E00 (Replace 1101 → 0); BSS
// default 0 (Replace).  Read by Texture_SetTexture (texwnd.cpp 0x45be50).
char byte_73C380 = 0;


