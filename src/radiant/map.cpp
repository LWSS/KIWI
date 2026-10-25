#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\map.cpp — map load/save, region caging and the prefab edit stack.
// Load: Map_LoadFromFile 0x486680 -> Map_LoadEntities -> ParseEntity (entity.cpp).
// Save: Map_SaveFile 0x486c00 -> Map_OpenForSave -> MapFile_WriteEntity.

#include "stdafx.h"
#include <commdlg.h>             // GetSaveFileNameA (Map_SaveFile's save-as picker; was
                                 // <afxdlgs.h>/CFileDialog — U-GUARD-2 raw rewrite)
#include "qe3.h"
#include "mainfrm.h"
#include "xywnd.h"               // xywndState_t / Ed_ActiveXY (U-GLOBALS)
#include "prefs.h"               // g_PrefsDlg (m_bCleanTinyBrushes / m_fTinySize)
#include <map>                   // target-name remap (was MFC CMapStringToString before U-SHIM removal)
#include <set>                   // the layer model-prefix dedup set
#include <string>
#include <universal/q_parse.h>  // Com_ParseExt, parseInfo_t, Com_GetParseThreadInfo
// progress feedback for the synchronous load — the legacy console
// pane that used to be the (ugly) feedback is created hidden now.
#include "kiwi_loadprogress.h"  // KiwiLoadProgress_Begin / _SetTitle / _End
// the static XModel geometry pool is keyed on XSurface pointers,
// so Map_NewMap — the one place map models are freed — has to drop it.
#include "kiwi_modelcache.h"    // KiwiModelCache_Shutdown
// the pre-transformed INSTANCE pool is keyed on the same
// XSurface pointers PLUS the model_inst slots, so it dies at exactly the same place.
#include "kiwi_instcache.h"     // KiwiInstCache_Shutdown
#include "kiwi_shadowcache.h"   // KiwiShadowCache_Shutdown
#include "kiwi_walkcache.h"     // KiwiWalkCache_Shutdown
// a face/patch whose lightmap or smoothing channel is dead is invisible in the
// Shift+L render method and compiles unlit; load repairs it, save never writes it.
#include "kiwi_material.h"      // KiwiMtl_HealMapLayersOnLoad / KiwiMtl_HealMapLayers
#include "kiwi_matconvert.h"    // KIWI-UX: reset/load-time loaded-techset health scan
#include "kiwi_fmt.h"           // KIWI: KiwiFmt_Exact (entity origins)
#include "kiwi_ported.h"        // KIWI: shared engine/editor externs
// the 1-2 minute synchronous load, made measurable.
#include <universal/profile.h>
#include <universal/com_memory.h>   // Hunk_Used (com_memory.h:93) — the hunk high-water plot
// the prefab layer-prefix cache lives beside its only consumer,
// DrawBrush_PrefabContents; Map_NewMap is where its entity-DEF keys stop meaning anything.
extern void KiwiPrefabPrefix_Shutdown();   // brush.cpp:7119

// ─── globals from entity.cpp ──────────────────────────────────────────────────
extern bool         g_bRestoreBetween;  // 0x25D5B07

// ─── globals from engine_stubs.cpp / qe3.h extern ────────────────────────────
extern char         g_activeLayer_string[];  // 0x73BF78
extern void        *zero;               // empty string sentinel (engine_stubs.cpp)

// ─── globals from engine (qe3.h) ──────────────────────────────────────────────
extern qeglobals_t  g_qeglobals;        // 0x25F39C0

entity_s    *world_entity = nullptr;    // 0x25D5B30

// ─── map.cpp globals ──────────────────────────────────────────────────────────
char        currentmap[1024] = "";      // 0x23F18D8
int         modified = 0;               // 0x23F179C
int         prefabStackLevel = 0;       // 0x25D5B34
static bool Map_EditingPrefab() { return prefabStackLevel > 0; }   // the binary's inlined helper (map.cpp:360 string)
HCURSOR     hCursor = nullptr;          // 0x240A114
int         g_region_active = 0;        // 0x23F1744
float       region_mins[3] = { -131072.0f, -131072.0f, -131072.0f };  // 0x739C14
float       region_maxs[3] = {  131072.0f,  131072.0f,  131072.0f };  // 0x739D24
// region_sides[4] (0x23F1734) — the 4 region-boundary wall brush instances.  Modeled as one
// contiguous array because the binary index-walks it (loop terminator is the address of
// g_region_active, immediately after); 4 separate globals need not be adjacent.
selbrush_t *region_sides[4] = { nullptr, nullptr, nullptr, nullptr };

// ─── texWndGlob (0x25D7990) — qtextures chain (texwnd.cpp owns the object) ─────
extern qtexture_s *TexWnd_GetMaterialListHead();

// prefab stack storage: g_prefabStack (0x25EB240) — declared in qe3.h.

// ─── prefab_s (qe3.h) ─────────────────────────────────────────────────────────
// The head holds the prefab's instanced entity + brush list sentinels;
// Prefab_NextLevel splices these into the live world on enter.

// ─── surface window flag / dialog ─────────────────────────────────────────────
// surfDlgGlob (surface inspector; .hwnd = HWND when open) comes from qe3.h

// KIWI GLASS
void KiwiGlass_BeforeMapSave();   // kiwi_glass.cpp: every pane's uvscale
// KIWI END

// KIWI: the Particles tab (kiwi_particles.cpp) - save-time script export + per-map reset.
void KiwiParticles_ExportForMap( const char *mapPath, bool fromSave );
void KiwiParticles_ResetForNewMap();

// ─── forward declarations for functions defined in this file ──────────────────
void Brush_FreeMapBrushes();
void Map_NewMap();
void Map_Free();
void Map_BuildBrushData();
entity_s_def *Entity_GetClass( const char *name );
void Map_LoadFromFile( const char *path );
bool Map_SaveFile( const char *path, char regionSave );
void AddRegionBrushes();
void Map_RegionOff();
void Map_ApplyRegion();
int  Map_GetNextAutoTarget();
char *Map_GetNextExportId( int slot );
void FreePrefabLevel( entity_s *first, entity_s *end, entity_s *skip, entitymodel_t *modelClass );
FILE *Map_OpenForSave( const char *path );
char MapFile_WriteEntity( entity_s_def *ent, FILE *fout, char regionSave );
void Prefab_NextLevel( void *brush );
void Prefab_PrevLevel();
void Prefab_LevelBack();

// ─── forward declarations for functions from other files ──────────────────────
// WriteFunc_map_t: the writer function type shared between Entity_WriteSelected
// and Brush_Write.  int(int ctx, const char *fmt, ...) — matches brush.cpp WriteFunc_t
// and entity.cpp WriteFunc_entity_t.
typedef int WriteFunc_map_t( void *ctx, const char *fmt, ... );

// entity.cpp
extern int          Map_LoadEntities( const char *filename, entity_s *entList, char skipLayerFlags );   // defined below (0x486500)
extern void         Map_New();
extern entity_s    *Prefab_Init( struct prefab_s *instList, entity_s_def *def, selbrush_t *activeList );
extern void         Entity_WriteSelected( entity_s_def *ent, WriteFunc_map_t **writer );
extern void         Entity_FreePrefab( entity_s *e );
extern bool         Entity_NeedsAutoExportId( entity_s_def *def );              // entity.cpp 0x487B10

// brush.cpp
extern bool         Brush_RemoveDuplicateFaces( brush_t *b );   // brush.cpp 0x471850
// Brush_Write: WriteWriter_t = WriteFunc_t** (same WriteFunc_map_t type)
extern int          Brush_Write( WriteFunc_map_t **writer, brush_t *b );

// layers.cpp
extern void         Map_InitlLayers();
extern void         Layers_SetMapLayers();
extern void         Layers_RefreshDialog();
extern void         Layers_WriteToFile( FILE *f );   // layer-declaration block writer
extern void         Pointfile_Clear();
extern models_t    *Model_FreeMapModels();   // eclass.cpp 0x480C50 (returns models_t*, discarded here)
extern LRESULT      Texture_ShowInuse();   // texwnd.cpp (0x45B850) — mark/count in-use textures
extern void         Undo_Clear();
extern void         CopySelectedFaceValues();
extern int          AddModelToModelInstBuff( XModel *model, float *axis, float scale );
extern void         ModelInstUpdate( int inst, float (*axis)[3], float scale );
extern void         RemoveModelInstFromBuf( int inst );
extern char         LayeredMaterials_Save();   // 0x416f40 — 1 ok/unchanged, 0 write-fail
extern void         Layers_HighlightActive( const char *name );
// Prefab-edit-in-place layer-state save/restore.  The binary deep-copies the layer tree
// into each prefab slot; here it is a per-level std::map snapshot — see layers.cpp.
extern void         Layers_SavePrefabLayers( int level );
extern void         Layers_RestorePrefabLayers( int level );

// mainfrm.cpp
extern void         MainFrm_BrushList( const char *label, selbrush_t *list );
extern void         Layers_TagModelPath( const char *modelpath );  // layers.cpp 0x418A50

// points.cpp — s_num_points (0x23F1CD8).  Map_NewMap and Map_SaveFile both zero it,
// dropping any displayed leak pointfile on new-map / save.
extern void         Pointfile_ResetPoints();


extern void         CTextureBar_GetSurfaceAttributes( void *bar );

// ── prefab-enter (Prefab_NextLevel 0x489190) deps ─────────────────────────────
extern void         Eclass_FreeModel( models_t *model );                  // eclass.cpp 0x480CB0
// The camera reframe below builds the entity orientation (Entity_GetOrientationMatrix,
// entity.cpp 0x482940) inline via AnglesToAxis.
extern void         AxisToAngles( float *angles, float (*axis)[3] );      // engine_stubs.cpp 0x4A8A00
// draw.cpp 0x4BA6B0 — VectorRotate: out[i] = matrix_row(i+1) · dir.
extern void         VectorRotateByAxis( float *out, const float *axisMatrix, const float *dir );

// The viewport accessors Ed_Camera() / Ed_ActiveXY() (camwnd.cpp / xywnd.h) never return
// NULL (the XY view is a singleton).
extern void         CamWnd_ClearLightPreviews();      // camwnd.cpp (light_preview_count = 0)

// The CXYWnd command bodies as free functions (mainfrm.cpp).  SetViewType takes the plain
// ED_VIEW_* int (xywnd.h), which is exactly the value CXYWnd::EViewType carried.
extern void         XYWnd_CopyClip();                 // CXYWnd::Copy
extern void         XYWnd_PasteClip();                // CXYWnd::Paste
extern void         XYWnd_PositionView();             // CXYWnd::PositionView (0x46DE10)
extern void         XYWnd_SetViewType( int vt );      // CXYWnd::SetViewType  (0x46DF90)

// VA — radiant rotating-buffer va() with slot index
extern char *VA( int slot, const char *fmt, ... );


// 0x486050  Brush_FreeMapBrushes — free the three display lists + every entity inst,
// then assert the entity-def list drained.
void Brush_FreeMapBrushes()
{
    while ( active_brushes.next != &active_brushes )
        Brush_Free( active_brushes.next );

    while ( selected_brushes.next != &selected_brushes )
        Brush_Free( selected_brushes.next );

    while ( filtered_brushes.next != &filtered_brushes )
        Brush_Free( filtered_brushes.next );

    while ( entityInsts.next != &entityInsts )
        Entity_Free( entityInsts.next );

    iassert( entities.next == &entities );   // map.cpp:170
}


// 0x486110  Map_NewMap — reset title, free all brushes, re-init the sentinel lists.
void Map_NewMap()
{
    // KIWI (2026-10-24, user: "start saving the camera position/settings/mode/sectionview
    // when the map is closed"): File->New and every map load pass through here, so the map
    // being closed records its view in its .kiwi first (only the `view` records; no-op for an
    // unsaved map or a prefab level).
    if ( currentmap[0] && _stricmp( currentmap, "unnamed.map" ) != 0 && !Map_EditingPrefab() )
    {
        extern bool KiwiCon_SaveViewOnClose( const char *mapPath );   // kiwi_construct.cpp
        KiwiCon_SaveViewOnClose( currentmap );
    }
    strcpy( currentmap, "unnamed.map" );
    Pointfile_ResetPoints();            // s_num_points = 0
    SetWindowTextA( g_qeglobals.d_hwndMain, currentmap );
    g_qeglobals.d_num_entities = 0;

    if ( active_brushes.next )
    {
        Brush_FreeMapBrushes();
        Model_FreeMapModels();
        // The geometry pool is keyed on the XSurface POINTER, and Model_FreeMapModels
        // above just freed every map XModel — so the keys must die with them.
        KiwiModelCache_Shutdown();
        // Same argument, one key wider: the instance pool is keyed on (model_inst slot,
        // XSurface), and both halves have just been freed.
        KiwiInstCache_Shutdown();
        // Third pool, same argument: the sun-preview caches key on selbrush_t*, brush_t*
        // and XModel*, all three just freed.
        KiwiShadowCache_Shutdown();
        // The shared prefab-walk recording holds selbrush_t* into the lists just released.
        // Its epoch is the editor's ONE epoch, so this also ages every memo above.
        KiwiWalkCache_Shutdown();
        // and the prefab layer-prefix cache, keyed on entity DEF
        // pointers that Brush_FreeMapBrushes has just recycled (brush.cpp).
        KiwiPrefabPrefix_Shutdown();
        Map_InitlLayers();
    }
    else
    {
        // Bootstrap sentinels (first call)
        active_brushes.next      = &active_brushes;
        active_brushes.prev      = &active_brushes;
        selected_brushes.next    = &selected_brushes;
        selected_brushes.prev    = &selected_brushes;
        filtered_brushes.next    = &filtered_brushes;
        filtered_brushes.prev    = &filtered_brushes;
        entities.next            = &entities;
        entities.prev            = &entities;
        entityInsts.next         = &entityInsts;
        entityInsts.prev         = &entityInsts;
    }
    world_entity = nullptr;

    // KIWI-UX (RADIANT_UX_DESIGN §7, Phase 4): the construction store holds
    // editor-only scaffolding for the map that is being torn down here, so it goes
    // with it.  Map_NewMap is the ONE point both File->New (via Map_New) and
    // Map_LoadFromFile pass through, and the load path re-fills the store from the
    // sidecar at its own tail afterwards — so this can never race it.
    // KIWI-UX (CLEANUP, A-48): ...and so do the construction UNDO stacks and the
    // unified journal's tickets.  ClearAll alone left the previous map's snapshots
    // live, so a Ctrl+Z after File->New resurrected the old map's scaffolding into
    // the new document (only the LOAD branch reached KiwiCon_LoadSidecar's reset).
    {
        extern void KiwiCon_ResetForNewMap();  // kiwi_construct.cpp
        KiwiCon_ResetForNewMap();
        extern void KiwiOutliner_ResetForNewMap(); // kiwi_outliner.h
        KiwiOutliner_ResetForNewMap();              // KIWI-UX: drop old document row state
        extern void KiwiModelBrowser_ResetForNewMap(); KiwiModelBrowser_ResetForNewMap(); // KIWI-UX
        KiwiParticles_ResetForNewMap();       // KIWI
        // KIWI-UX (ROUND BP, ITEM 1/3): the SECTION goes with the document too.  It
        // is view state, but it is view state that CLAMPS PICKING, and a level
        // latched in one map means nothing in the next — the round-BP autopsy's
        // "an invisible latched plane distorting placement" in its purest form.
        extern void KiwiSection_Reset();       // kiwi_section.h
        KiwiSection_Reset();
        // KIWI-UX: the SUN HELPER's selection, hover and cached orbit target all
        // describe THIS document — the target is a brush AABB and the brushes are
        // being freed around this call — so they go with it.  Same slot and same
        // argument as the section above.
        extern void KiwiSun_ResetForNewMap();  // kiwi_sun.h
        KiwiSun_ResetForNewMap();
        KiwiMatConvert_ResetMapHealth(); // KIWI-UX: health rows belong to the document being torn down
    }
}


// 0x485e50  Map_Free — offer to carry the selection across to the next map (clipboard).
// Does NOT free brushes; that is Map_NewMap's job.
void Map_Free()
{
    g_bRestoreBetween = false;
    if ( selected_brushes.next && selected_brushes.next != &selected_brushes )
    {
        HWND activeWnd = GetActiveWindow();
        if ( MessageBoxA( activeWnd, "Copy selection?", "Radiant", 0x24u ) == IDYES )
        {
            g_bRestoreBetween = true;
            XYWnd_CopyClip();
        }
    }
}


// 0x485EB0  CheckForTinyBrush — 1 (+ warning) if any axis extent is below tinySize.
// i[3] is the matching maxs[] because brush_t.maxs (+0x2C) immediately follows mins (+0x20).
static char CheckForTinyBrush( brush_t *def, int idx, float tinySize )
{
    int n = 0;
    for ( float *i = def->mins; i[3] - *i >= tinySize; ++i )
    {
        if ( ++n >= 3 )
            return 0;
    }
    Sys_Printf( "Possible problem brush (too small) #%i ", idx );
    return 1;
}


// 0x485F00  Map_BuildBrushData — per-brush: rebuild windings, (in vertex/edge mode) rebuild
// the selection handles, bump the version, drop duplicate faces; then cull the brush if it
// has no faces or — when CleanTinyBrushes is on — if it is too small.  Wait cursor throughout.
void Map_BuildBrushData()
{
    if ( !active_brushes.next )
        return;

    HCURSOR prevCursor = SetCursor( LoadCursorA( nullptr, (LPCSTR)IDC_WAIT ) );
    hCursor = prevCursor;

    int tinyIdx = 0;
    selbrush_t *sb    = active_brushes.next;
    selbrush_t *onext = nullptr;             // captured before any free

    if ( active_brushes.next )               // the binary re-checks
    {
        do
        {
            if ( sb == &active_brushes )
                break;

            brush_t *def = sb->def;
            onext        = sb->next;

            Brush_BuildWindings( def, 0 );
            if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
                SetupVertexSelection();
            ++def->version;
            if ( Brush_RemoveDuplicateFaces( sb->def ) )
                Sys_Printf( "Removed duplicate faces from brush\n" );

            if ( sb->def->faces )            // not degenerate (faces != 0)
            {
                if ( !g_PrefsDlg->m_bCleanTinyBrushes )
                {
                    sb = onext;              // keep brush
                    continue;
                }
                if ( !CheckForTinyBrush( sb->def, tinyIdx++, g_PrefsDlg->m_fTinySize ) )
                {
                    sb = onext;              // not tiny → keep
                    continue;
                }
            }

            Brush_Free( sb );                // faces==0 (degenerate) OR tiny
            Sys_Printf( "Removed degenerate brush\n" );
            sb = onext;
        }
        while ( onext );
    }

    if ( hCursor )
    {
        SetCursor( hCursor );
        hCursor = nullptr;
    }
}


// 0x486010  Entity_GetClass — first entity whose "classname" epair matches `name`.
entity_s_def *Entity_GetClass( const char *name )
{
    for ( entity_s_def *e = (entity_s_def *)entities.next;
          e != (entity_s_def *)&entities;
          e  = (entity_s_def *)e->next )
    {
        if ( Entity_HasEpairMatch( e, "classname", name ) )
            return e;
    }
    return nullptr;
}


// 0x486680  Map_LoadFromFile — main map-load entry point.
void Map_LoadFromFile( const char *path )
{
    PROF_SCOPED( "Map_LoadFromFile" );

    HCURSOR cursorA = LoadCursorA( nullptr, (LPCSTR)IDC_WAIT );
    hCursor         = SetCursor( cursorA );

    {
        Map_Free();
    }
    Select_Deselect( 1 );

    // Normalise path: backslash -> forward slash
    char String[1028];
    {
        const char *src = path;
        char       *dst = String;
        while ( *src )
        {
            *dst++ = ( *src == '\\' ) ? '/' : *src;
            ++src;
        }
        *dst = '\0';
    }

    // Everything from here to the tail is straight-line synchronous — nothing pumps — so the
    // progress bracket routes every Sys_Printf below into the window CAPTION.  It cannot be an
    // ImGui modal: an ImGui frame may only start from the pump's WM_PAINT.
    KiwiLoadProgress_Begin( String );

    Sys_Printf( "Map_LoadFile: %s\n", String );

    iassert( prefabStackLevel >= 0 );
    iassert( !Map_EditingPrefab() );   // map.cpp:360

    {
        Map_NewMap();
        Map_InitlLayers();
    }

    // Clear the in-use flag on every registered material, walking the texWndGlob list
    // link (qtexture_s.prev).
    for ( qtexture_s *qt = TexWnd_GetMaterialListHead(); qt; qt = qt->prev )
        qt->is_in_use = false;

    g_qeglobals.d_parsed_brushes   = 0;
    strcpy( currentmap, path );
    g_qeglobals.g_layerCount_maybe = 1;

    Sys_Printf( "Updating layers...\n" );
    {
        Layers_SetMapLayers();
        Layers_RefreshDialog();
    }

    {
        g_qeglobals.d_num_entities = Map_LoadEntities( path, &entities, 0 );
    }

    // world_entity must be NULL before post-process loop (Map_NewMap clears it;
    // LoadEntities does NOT set it — the loop below does).
    iassert( world_entity == NULL );   // map.cpp:372

    // ── Post-process each entity def ─────────────────────────────────────────
    {
    entity_s_def *eDef = (entity_s_def *)entities.next;
    while ( eDef != (entity_s_def *)&entities )
    {
        entity_s_def *nextDef = (entity_s_def *)eDef->next;

        // Create entity instance (links brushes into entityInsts + active_brushes)
        entity_s *eInst = Prefab_Init( (struct prefab_s *)&entityInsts,
                                        eDef,
                                        &active_brushes );

        const char *clsValue = ValueForKey2( eDef, "classname" );

        if ( strcmp( clsValue, "worldspawn" ) == 0 )
        {
            if ( world_entity )
            {
                Sys_Printf( "WARNING: multiple worldspawn\n" );
                Entity_Free( eInst );
            }
            else
            {
                world_entity = eInst;
            }
        }
        else if ( eDef->eclass && (eDef->eclass->classtype & 0x10) != 0 )
        {
            // Model entity: assign model to first brush instance
            selbrush_t *firstBrush = eInst->brushes.ownerNext;
            if ( firstBrush && firstBrush != (selbrush_t *)&eInst->brushes )
                Model_SetModel( (entity_brush_s *)firstBrush, (const float *)&world_orient_matrix );
        }

        eDef = nextDef;
    }
    }

    if ( !world_entity )
    {
        Sys_Printf( "No worldspawn in map.\n" );
        // the function's ONLY early return — close the bracket here
        // too or the caption stays stuck on "Loading ..." forever.
        KiwiLoadProgress_End();
        Map_New();
        return;
    }

    iassert( entityInsts.next == world_entity );   // map.cpp:404

    Sys_Printf( "--- LoadMapFile ---\n" );
    Sys_Printf( "%s\n", String );
    Sys_Printf( "%5i brushes\n",  g_qeglobals.d_parsed_brushes );
    Sys_Printf( "%5i entities\n", g_qeglobals.d_num_entities );
    Sys_Printf( "Map_BuildAllDisplayLists\n" );

    Map_BuildBrushData();

    // ── Find start-position entity ────────────────────────────────────────────
    entity_s_def *startEnt = Entity_GetClass( "info_player_start" );
    if ( !startEnt )
        startEnt = Entity_GetClass( "info_player_deathmatch" );

    // Position camera from start entity (Ed_Camera() / Ed_ActiveXY() are never NULL)
    {
        camera_s     *cam = Ed_Camera();
        xywndState_t *xy  = Ed_ActiveXY();
        cam->angles[0] = 0.0f;
        if ( startEnt )
        {
            float origin[3] = { 0.0f, 0.0f, 0.0f };
            Entity_GetVec3ForKey( startEnt, origin, "origin" );
            origin[2] += 60.0f;

            cam->origin[0] = origin[0];
            cam->origin[1] = origin[1];
            cam->origin[2] = origin[2];

            xy->m_vOrigin[0] = origin[0];
            xy->m_vOrigin[1] = origin[1];
            xy->m_vOrigin[2] = origin[2];

            Entity_GetVec3ForKey( startEnt, cam->angles, "angles" );
        }
        else
        {
            cam->angles[1] = 0.0f;
            cam->origin[0] = 0.0f;
            cam->origin[1] = 0.0f;
            cam->origin[2] = 0.0f;
            xy->m_vOrigin[0] = 0.0f;
            xy->m_vOrigin[1] = 0.0f;
            xy->m_vOrigin[2] = 0.0f;

            // KIWI-UX (shakeout I): POST-INIT hook, this branch ONLY.  A map WITH a
            // start entity already has a meaningful placement and keeps it; a map
            // without one lands the camera at (0,0,0) looking down +X — inside the
            // world with nothing in frame, which is the "scale is way too big"
            // complaint in its purest form.  Modern-input gated, so the classic
            // profile is byte-identical.  See KiwiCam_DefaultSpawn (kiwi_camera.h).
            extern bool KiwiUX_ModernInput();     // kiwi_ux.cpp
            extern void KiwiCam_DefaultSpawn();   // kiwi_camera.cpp
            if ( KiwiUX_ModernInput() )
                KiwiCam_DefaultSpawn();
        }
    }

    Map_RegionOff();
    Texture_ShowInuse();
    modified = 0;
    // Routed through the progress bracket, not SetWindowTextA: the binary's rename happens
    // mid-load and a direct call would be overwritten by the next progress line.
    KiwiLoadProgress_SetTitle( String );

    // ── Layer model-prefix marking for model entities ─────────────────────────
    // NOT a model load: Layers_TagModelPath (layers.cpp) is an O(brushes x layers) string
    // pass over the whole map.
    {
    // Layers_TagModelPath ORs bit 0x10 into every layer whose name has `path` as a prefix —
    // idempotent and keyed on nothing but `path`, and nothing in this loop mutates layerMap.
    // So a per-loop set of already-applied paths is behaviour-identical.  The dedup is at
    // this call site only.
    std::set<std::string> markedPaths;
    for ( selbrush_t *sb = active_brushes.next;
          sb != &active_brushes; sb = sb->next )
    {
        if ( !sb->owner )
            continue;
        entity_s_def *def = (entity_s_def *)sb->owner->def;
        if ( !def || !def->eclass )
            continue;
        if ( (def->eclass->classtype & 0x10) != 0 )
        {
            const char *modelPath = ValueForKey2( def, "model" );
            // apply once per DISTINCT path (see the note above).
            if ( markedPaths.insert( std::string( modelPath ) ).second )
                Layers_TagModelPath( modelPath );
        }
    }
    }   // close "load: layer model-prefix marking"

    Sys_Printf( "Updating layers...\n" );
    {
        Layers_SetMapLayers();
        Layers_RefreshDialog();
    }

    // ── Between-map clipboard paste ───────────────────────────────────────────
    if ( g_bRestoreBetween )
        XYWnd_PasteClip();
    g_bRestoreBetween = false;

    if ( hCursor )
    {
        SetCursor( hCursor );
        hCursor = nullptr;
    }

    // Rebuild brush display lists
    {
        for ( selbrush_t *sb = selected_brushes.next;
              sb != &selected_brushes; sb = sb->next )
            Brush_UpdateSpecialMaterialFlag( sb->def );
        for ( selbrush_t *sb = active_brushes.next;
              sb != &active_brushes; sb = sb->next )
            Brush_UpdateSpecialMaterialFlag( sb->def );
    }

    MainFrm_BrushList( VA( 0, "%s - active_brushes",   "loaded map" ), &active_brushes );
    MainFrm_BrushList( VA( 1, "%s - active_brushes", "loaded map" ), &selected_brushes );

    // KIWI-UX (RADIANT_UX_DESIGN §7, Phase 4): load the construction sidecar
    // `<mapname>.kiwi` that sits next to this .map.  At the TAIL, after the map is
    // fully live, because a sidecar must never be able to affect the map load: a
    // missing one is the normal case and a corrupt one warns and is ignored.
    {
        extern bool KiwiCon_LoadSidecar( const char *mapPath );   // kiwi_construct.cpp
        KiwiCon_LoadSidecar( path );

        // KIWI-UX (ROUND AO) — "Hidden solids are not respected on save and unhide
        // on load."  The sidecar now also carries which SOLIDS were hidden, as
        // ordinals into Map_SaveFile's own brush walk; KiwiCon_LoadSidecar has just
        // parsed those lines into a pending set and this applies them.  It MUST run
        // here and not inside the sidecar reader: the hidden bit lives on the
        // selbrush_t INSTANCE, and the instances only exist once the map load has
        // finished building them — which, at the tail of Map_LoadFromFile, it has.
        // Reads nothing but its own pending set, writes nothing but brushFlags bit 2
        // and the hide depth, and is a no-op for every sidecar written before this
        // round.  Design + the honest limits: kiwi_visibility.h ROUND AO.
        extern void KiwiVis_SidecarLoadApply();                   // kiwi_visibility.h:261
        KiwiVis_SidecarLoadApply();

        // KIWI (2026-10-24): the view this map was closed with (camera, projection, section),
        // over the player-start / default-spawn placement above.  No `view` records = no-op.
        extern void KiwiCamView_ApplyPending();                   // kiwi_camera.h
        KiwiCamView_ApplyPending();

        // KIWI-UX — HEAL DEAD MATERIAL LAYERS.  A face or patch whose LIGHTMAP (or
        // smoothing) channel is missing, unnamed or zero-scaled is invisible in the
        // Shift+L render method AND compiles with no lightmap at all — no baked
        // light, no baked sun shadow.  Fixing the creation paths cannot heal a map
        // that already carries such surfaces, so every load sweeps for them,
        // reinitialises them to exactly what Brush_Create / MakeNewPatch produce, and
        // marks the map modified so a re-save persists the repair.  The full rule and
        // the compiler consequence are on the declaration.
        //
        // HERE, at the tail, for the same reason the sidecar block above gives: the
        // sweep walks the per-entity DEF lists, which only exist once the load has
        // finished building them.  Silent (and free) on an undamaged map.
        KiwiMtl_HealMapLayersOnLoad();                            // kiwi_material.h:269
        KiwiMatConvert_OnMapLoaded(); // KIWI-UX: diagnose loaded world-face/patch techsets after the map is fully live
    }

    // close the progress bracket opened at the head — this settles
    // the window caption on the title KiwiLoadProgress_SetTitle above asked for.
    KiwiLoadProgress_End();

    g_nUpdateBits = -1;
}


// 0x486C00  Map_SaveFile — save the current map (or, regionSave, the region subset).
// Empty `path` pops the Save picker; every real call site passes a non-empty path (only
// the binary's Map_Snapshot out-of-disk fallback could hand it an empty one), so that
// branch exists only so this function can never FATAL — and is GUI-only.
// KIWI: returns whether the map (and its sidecar) reached disk.
bool Map_SaveFile( const char *path, char regionSave )
{
    // save-as branch — resolve `path` from a Save file dialog.
    char *savedPathHeap = nullptr;        // _strdup'd copy
    if ( !path || !path[0] )
    {
        // Raw Save picker with the binary's CFileDialog filter / flags / default extension
        // (CFileDialog wraps GetSaveFileNameA); owner = d_hwndMain.
        char picked[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn = { 0 };
        ofn.lStructSize = sizeof( ofn );
        ofn.hwndOwner   = g_qeglobals.d_hwndMain;
        ofn.lpstrFilter = "Map Files (*.map)\0*.map\0\0";
        ofn.lpstrFile   = picked;
        ofn.nMaxFile    = sizeof( picked );
        ofn.lpstrDefExt = "map";
        ofn.Flags       = OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
        if ( !GetSaveFileNameA( &ofn ) )  // cancel
            return false;
        savedPathHeap = _strdup( picked );
        path = savedPathHeap;
        if ( !path || !path[0] )          // empty pick → no-op
        {
            if ( savedPathHeap ) free( savedPathHeap );
            return false;
        }
    }

    Pointfile_ResetPoints();          // s_num_points = 0

    char String[1028];
    {
        const char *src = path;
        char       *dst = String;
        while ( *src )
        {
            *dst++ = ( *src == '\\' ) ? '/' : *src;
            ++src;
        }
        *dst = '\0';
    }

    Sys_Printf( "Map_SaveFile: %s\n", path );

    // A failed layered-materials save aborts the ENTIRE map save (no .map written, no
    // "Saved.", modified flag left set).
    if ( !LayeredMaterials_Save() )
    {
        if ( savedPathHeap ) free( savedPathHeap );
        return false;
    }

    // KIWI-UX — NEVER SERIALISE A DEAD MATERIAL LAYER.  Brush_Write emits each of a
    // face's three channels as `<name> <sizeS> <sizeT> ...` straight out of the
    // MaterialDef (brush.cpp:4276-4297) and Patch_Write emits the patch's three
    // material NAMES the same way (pmesh.cpp:1136-1142); neither validates, so a
    // channel that is unnamed or zero-scaled in memory is written that way and the
    // damage becomes part of the file — where the map compiler then silently drops
    // the surface from lightmapping.  Repairing HERE, before the first byte, means
    // the writers stay exactly as the binary has them and the file is always valid.
    // Same sweep as the load path, so a save of an already-healed map is a no-op.
    //
    // SAFE MID-SESSION, which is the round-AA hazard this position has to answer:
    // the repair's SetMaterial reaches Texture_GetHandle (texwnd.cpp:314), which
    // REGISTERS an unknown name into the browser's list.  The only names it can
    // pass are the four the editor already registered at boot ($default,
    // lightmap_gray, smoothing_hard, smoothing_smooth — mainfrm.cpp:746-748 /
    // pmesh.cpp:145-147), and Texture_GetHandle returns the existing wrapper for a
    // name already in the list rather than adding one.  So the browser is not
    // touched; only `is_in_use` is set, exactly as an ordinary load would.
    {
        int healedBrushes = 0, healedPatches = 0;
        if ( KiwiMtl_HealMapLayers( &healedBrushes, &healedPatches ) )   // kiwi_material.h:265
            Sys_Printf( "Material layers: repaired %i brush(es) and %i patch(es) before "
                        "writing.\n", healedBrushes, healedPatches );
    }
    // KIWI GLASS
    KiwiGlass_BeforeMapSave();
    // KIWI END

    FILE *fout = Map_OpenForSave( path );
    if ( !fout )
    {
        if ( savedPathHeap ) free( savedPathHeap );
        return false;
    }

    fprintf( fout, "iwmap %i\n", 4 );

    // Layer declarations: layerMap in sorted order, `"<name>" flags <flagstr> [active]`.
    Layers_WriteToFile( fout );

    if ( regionSave )
        AddRegionBrushes();

    iassert( entities.next != &entities );   // map.cpp:534
    iassert( !strcmp( entities.next->eclass->name, "worldspawn" ) );   // map.cpp:535 (derefs eclass unconditionally)

    // Write gate: def-list NON-empty OR worldspawn.  The def-list head is brushes.prev, its
    // sentinel is &def; fixed-size entities always carry their bbox brush, so only a
    // brushless worldspawn needs the OR.
    int entIdx = 0;
    for ( entity_s *eIter = entities.next; eIter != &entities; eIter = eIter->next )
    {
        entity_s_def *eDef = (entity_s_def *)eIter;

        bool hasDefBrushes = ( (void *)eDef->brushes.prev != (void *)&eDef->def );
        bool isWorld       = ( strcmp( eDef->eclass->name, "worldspawn" ) == 0 );   // 0x486f59 derefs eclass unconditionally

        if ( hasDefBrushes || isWorld )
        {
            fprintf( fout, "// entity %i\n", entIdx );
            ++entIdx;
            MapFile_WriteEntity( eDef, fout, regionSave );
        }
    }

    const bool writeFailed = ferror( fout ) != 0;
    const bool closeFailed = fclose( fout ) != 0;

    if ( regionSave && g_region_active )
    {
        for ( int i = 0; i < 4; ++i )
            Brush_Free( region_sides[i] );   // frees all 4 region walls unconditionally
    }

    // KIWI: a failed write/flush must not clear the dirty flag or replace the sidecar.
    if ( writeFailed || closeFailed )
    {
        Sys_Printf( "ERROR: could not finish writing map %s. The map is still modified.\n", path );
        MarkMapModified();
        if ( savedPathHeap ) free( savedPathHeap );
        return false;
    }

    // KIWI-UX (RADIANT_UX_DESIGN §7 / decision D-5, Phase 4): write the construction
    // sidecar `<mapname>.kiwi` beside the .map that was just written.  AFTER the
    // fclose above, so it only ever follows a .map that actually reached disk, and
    // NEVER for a region save — a region save writes a deliberate SUBSET of the
    // map to a scratch path, and stamping the full construction store next to it
    // would leave a sidecar that does not describe its file.  The .map itself is
    // untouched: stock compilers and stock Radiant must keep loading our maps.
    if ( !regionSave )
    {
        extern bool KiwiCon_SaveSidecar( const char *mapPath );   // kiwi_construct.cpp
        if ( !KiwiCon_SaveSidecar( path ) )
        {
            Sys_Printf( "ERROR: map written, but its sidecar was not saved. The map is still modified.\n" );
            MarkMapModified();
            if ( savedPathHeap ) free( savedPathHeap );
            return false;
        }
        // KIWI: the particles' game scripts (maps/createfx/<map>_fx.gsc + the loadfx table).
        KiwiParticles_ExportForMap( path, true );
    }

    Sys_Printf( "Saved.\n" );
    modified = 0;

    if ( !strstr( String, "autosave" ) )
        SetWindowTextA( g_qeglobals.d_hwndMain, String );

    if ( !regionSave )
    {
        MessageBeep( 0x30 );
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER + 1, 0, (LPARAM)"Saved.\n" );
    }

    if ( savedPathHeap ) free( savedPathHeap );   // release the save-as dialog pick
    return true;
}


// ═════════════════════════════════════════════════════════════════════════════
// KIWI-UX (ROUND V) — EMERGENCY RESCUE SAVE
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT (verbatim): *"After going AFK for a while and coming back I get this
// crash ... it ruins all progress of your map."*  Round V's other half makes the
// specific AFK crash non-fatal (r_init.cpp R_Hwnd_Resize), but "a fatal error can
// destroy an afternoon of unsaved work" is a class of failure, not one bug — so the
// death path itself now writes the map out before the process goes away.
//
// WHY THIS IS NOT Map_SaveFile.  Map_SaveFile (above, 0x486c00) is the operator-
// attended save and it is unusable from a fatal handler.  Reading it top to bottom,
// three things in it are disqualifying — NONE of them are renderer state (it touches
// no D3D object, no gfx global, nothing in dx.*; the write core is pure fprintf over
// the brush lists), but all three are lethal on a dying process:
//   1. LayeredMaterials_Save (:665 → layeredmaterials.cpp:554) can ABORT the whole
//      save (returns 0 → Map_SaveFile returns having written nothing).
//   2. SetWindowTextA (:733) / SendMessageA to d_hwndStatus (:746) / MessageBeep
//      (:737) are window messages into a half-dead shell; a nested message pump
//      delivers WM_PAINT into the renderer we are dying inside.
//   3. `modified = 0` (:730) LIES: a rescue file is not a save of the real file, and
//      the flag must not claim the operator's work reached its real path.
// So the rescue reproduces Map_SaveFile's WRITE CORE ONLY (:678-708 — the iwmap
// header, Layers_WriteToFile, the entity gate + MapFile_WriteEntity walk), which IS
// pure file I/O: MapFile_WriteEntity (:1390) is fprintf + SetKeyValue + Brush_Write
// (brush.cpp:4204), and Brush_Write is formatting.  regionSave is hard 0, so the
// region filter and its Ed_Camera() read (:1429) are never reached.
//
// THE REAL PATH IS NEVER TOUCHED.  We write `<mapname>_rescue.map` beside it — the
// operator diffs or renames.  Untitled/unnamed maps go to `<exe dir>\rescue.map`
// (data-next-to-exe rule).
//
// GUARDS.  A crash inside the rescue must not mask the error that caused it:
//   • ONE attempt, ever — s_tried is set BEFORE anything can fault.
//   • SEH (__try/__except) around the whole body; a torn brush list that would AV
//     here simply ends the rescue instead of replacing the user's error with 0xC0000005.
//     (No C++ objects with destructors in this frame — required by MSVC for __try.)
//   • No Sys_Printf (win_qe3.cpp:112 → console_print → SendMessageA into the console
//     EDIT), no message boxes, no window messages, no `modified` write.
//
// GATE.  A map is rescued whenever the entity list is non-empty.  Deliberately NOT
// gated on `modified`: MarkMapModified (win_qe3.cpp:189) is called from 73 ported
// edit sites but only two KIWI-UX files (kiwi_transform.cpp, kiwi_validity.cpp), so a
// `modified == 0` reading is not evidence the session is clean — and a false "clean"
// would throw away exactly the work this exists to save.  A redundant rescue file
// costs one write on a path that is already fatal.
//
// Returns true and fills outPath when a file reached disk.
extern bool KiwiCon_SaveSidecar( const char *mapPath );   // kiwi_construct.cpp:3460

static int Kiwi_RescueSave_Write( const char *path )
{
    FILE *fout = fopen( path, "wb" );
    if ( !fout )
        return 0;

    fprintf( fout, "iwmap %i\n", 4 );          // Map_SaveFile:678
    Layers_WriteToFile( fout );                // Map_SaveFile:681

    int entIdx = 0;
    for ( entity_s *eIter = entities.next; eIter != &entities; eIter = eIter->next )
    {
        entity_s_def *eDef = (entity_s_def *)eIter;
        if ( !eDef->eclass )                   // rescue-only guard: Map_SaveFile:698 derefs
            continue;                          // eclass unconditionally (faithful); a fatal
                                               // may have caught the list mid-edit.
        bool hasDefBrushes = ( (void *)eDef->brushes.prev != (void *)&eDef->def );
        bool isWorld       = ( strcmp( eDef->eclass->name, "worldspawn" ) == 0 );
        if ( hasDefBrushes || isWorld )
        {
            fprintf( fout, "// entity %i\n", entIdx );
            ++entIdx;
            MapFile_WriteEntity( eDef, fout, 0 );   // never the region writer
        }
    }

    fclose( fout );
    return 1;
}

// KIWI: the same write core for the live fragile check's snapshot (kiwi_fragile.cpp) - no
// prompts, no sidecar, `modified` untouched, the real path never written, and the open
// document never written to (MapFile_WriteEntity's pure mode).
bool Map_WriteSnapshot( const char *path )
{
    return Kiwi_RescueSave_Write( path ) != 0;
}

bool Kiwi_RescueSave( char *outPath, int outPathSize )
{
    static bool s_tried = false;               // ONE attempt, ever
    if ( s_tried )
        return false;
    s_tried = true;

    if ( !outPath || outPathSize < 4 )
        return false;
    outPath[0] = '\0';

    if ( !entities.next || !entities.prev || entities.next == &entities )
    {
        return false;
    }

    int wrote = 0;
    __try
    {
        char path[1100] = "";

        // <mapname>_rescue.map beside the real file, when there IS a real file.
        // currentmap (map.cpp:44) is "" before any map and "unnamed.map" after Map_New
        // (map.cpp:239); Map_LoadFromFile writes the real path (map.cpp:421).
        if ( currentmap[0] && _stricmp( currentmap, "unnamed.map" ) != 0 )
        {
            _snprintf( path, sizeof( path ), "%s", currentmap );
            path[sizeof( path ) - 1] = '\0';
            size_t n = strlen( path );
            if ( n > 4 && _stricmp( path + n - 4, ".map" ) == 0 )
                path[n - 4] = '\0';
            strncat( path, "_rescue.map", sizeof( path ) - strlen( path ) - 1 );
            wrote = Kiwi_RescueSave_Write( path );
        }

        // Untitled — or the map's own directory refused the write (read-only tree,
        // network share gone).  Fall back to the exe's directory: KIWI data lives next
        // to the exe, never behind a registry redirection.
        if ( !wrote )
        {
            char exeDir[MAX_PATH] = "";
            if ( ::GetModuleFileNameA( NULL, exeDir, sizeof( exeDir ) ) )
            {
                char *slash = strrchr( exeDir, '\\' );
                if ( slash ) *slash = '\0';
                _snprintf( path, sizeof( path ), "%s\\rescue.map", exeDir );
            }
            else
            {
                _snprintf( path, sizeof( path ), "rescue.map" );
            }
            path[sizeof( path ) - 1] = '\0';
            wrote = Kiwi_RescueSave_Write( path );
        }

        if ( wrote )
        {
            // The construction sidecar next to it, under the same one-shot SEH frame.
            // KiwiCon_SaveSidecar (kiwi_construct.cpp:3460) is tmp-file + rename, pure
            // file I/O, and no-ops (deleting a stale file) when the store is empty.
            KiwiCon_SaveSidecar( path );
            _snprintf( outPath, outPathSize, "%s", path );
            outPath[outPathSize - 1] = '\0';
        }
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
        // A fault inside the rescue must not replace the original error.  Whatever
        // reached disk before the fault stays; we simply stop.
        return false;
    }

    return wrote != 0;
}


// 0x487220  AddRegionBrushes — 4 wall brushes caging the region box.
void AddRegionBrushes()
{
    if ( !g_region_active )
        return;

    MaterialDef mat;
    SetMaterial( "region", (patchMesh_material *)&mat );
    {
        float ss = 0.25f;
        Init_MaterialLayer( &mat, ss );
    }

    // Four boundary-wall slabs around the region box.  The ±16/±1 offsets are DOUBLE
    // constants, as in the binary; ±131072 are FLOAT.  Walls 2-4 reuse the components
    // walls 1/3 left in mins[]/maxs[] (the binary re-assigns only the changed ones).
    float mins[3], maxs[3];

    // Wall 1 (-X slab)
    mins[0] = (float)( region_mins[0] - 16.0 );
    maxs[0] = (float)( region_mins[0] +  1.0 );
    mins[1] = (float)( region_mins[1] - 16.0 );
    maxs[1] = (float)( region_maxs[1] + 16.0 );
    mins[2] = -131072.0f;
    maxs[2] =  131072.0f;
    {
        brush_t *b = Brush_Alloc( &mat, nullptr );
        Brush_Create( mins, maxs, b, nullptr );
        region_sides[0] = Brush_AddToList( b, world_entity );
    }

    // Wall 2 (+X slab) — reuses mins[1..2]/maxs[1..2]
    mins[0] = (float)( region_maxs[0] -  1.0 );
    maxs[0] = (float)( region_maxs[0] + 16.0 );
    {
        brush_t *b = Brush_Alloc( &mat, nullptr );
        Brush_Create( mins, maxs, b, nullptr );
        region_sides[1] = Brush_AddToList( b, world_entity );
    }

    // Wall 3 (-Y slab) — reuses mins[2]/maxs[2]
    mins[0] = (float)( region_mins[0] - 16.0 );
    maxs[0] = (float)( region_maxs[0] + 16.0 );
    mins[1] = (float)( region_mins[1] - 16.0 );
    maxs[1] = (float)( region_mins[1] +  1.0 );
    {
        brush_t *b = Brush_Alloc( &mat, nullptr );
        Brush_Create( mins, maxs, b, nullptr );
        region_sides[2] = Brush_AddToList( b, world_entity );
    }

    // Wall 4 (+Y slab) — reuses mins[0]/maxs[0]/mins[2]/maxs[2]
    mins[1] = (float)( region_maxs[1] -  1.0 );
    maxs[1] = (float)( region_maxs[1] + 16.0 );
    {
        brush_t *b = Brush_Alloc( &mat, nullptr );
        Brush_Create( mins, maxs, b, nullptr );
        region_sides[3] = Brush_AddToList( b, world_entity );
    }

    // Link each wall into the world entity.
    for ( int i = 0; i < 4; ++i )
    {
        selbrush_t *inst = region_sides[i];
        Brush_AddToList2( inst );
        Entity_LinkBrush( inst->def, (entity_s *)world_entity->def );
        Brush_RebuildAfterEdit( inst->def );
        iassert( region_sides[i]->def->owner == world_entity->def );
        iassert( region_sides[i]->owner != world_entity );
        inst->owner = world_entity;
    }
}


// 0x487530  Map_RegionOff — reset the region to ±131072 and un-filter everything it covers.
void Map_RegionOff()
{
    region_maxs[0] =  131072.0f;
    region_maxs[1] =  131072.0f;
    region_maxs[2] =  131072.0f;
    region_mins[0] = -131072.0f;
    region_mins[1] = -131072.0f;
    region_mins[2] = -131072.0f;
    g_region_active = 0;

    // Each move is gated on a 3-axis AABB overlap against def->mins/maxs (the
    // complement of Map_ApplyRegion's test); a brush failing any axis stays filtered.
    selbrush_t *sb = filtered_brushes.next;
    while ( sb != &filtered_brushes )
    {
        selbrush_t *next = sb->next;

        bool inRegion = true;
        for ( int ax = 0; ax < 3; ++ax )
        {
            if ( region_maxs[ax] < sb->def->mins[ax] ||
                 region_mins[ax] > sb->def->maxs[ax] )
            {
                inRegion = false;
                break;
            }
        }

        if ( inRegion )
        {
            Brush_RemoveFromList( sb );
            // Re-insert at head of active list
            sb->next                  = active_brushes.next;
            active_brushes.next->prev = sb;
            active_brushes.next       = sb;
            sb->prev                  = &active_brushes;
        }

        sb = next;
    }

    g_nUpdateBits = -1;
}


// 0x487650  Map_ApplyRegion — move every brush outside the region onto the filtered list.
void Map_ApplyRegion()
{
    g_region_active = 1;

    selbrush_t *sb = active_brushes.next;
    while ( sb != &active_brushes )
    {
        selbrush_t *next = sb->next;

        bool inRegion = true;
        for ( int ax = 0; ax < 3; ++ax )
        {
            if ( region_maxs[ax] < sb->def->mins[ax] ||
                 region_mins[ax] > sb->def->maxs[ax] )
            {
                inRegion = false;
                break;
            }
        }

        if ( !inRegion )
        {
            Brush_RemoveFromList( sb );
            sb->next                    = filtered_brushes.next;
            filtered_brushes.next->prev = sb;
            filtered_brushes.next       = sb;
            sb->prev                    = &filtered_brushes;
        }

        sb = next;
    }

    g_nUpdateBits = -1;
}

// Region setters (Region menu): Map_RegionXY 0x4877d0, Map_RegionTallBrush 0x487860,
// Map_RegionBrush 0x4878e0, Map_RegionSelectedBrushes 0x487720.  Tall/Brush delete the
// defining brush after caging: the brush becomes the region, then is removed.
extern void       Select_GetBounds( float *mins, float *maxs );  // select.cpp (0x48fb10)

void Map_RegionXY()
{
    Map_RegionOff();
    xywndState_t *xy = Ed_ActiveXY();
    // Half the view extent in world units, centred on m_vOrigin.
    region_mins[0] = xy->m_vOrigin[0] - (double)xy->m_nWidth  * 0.5 / xy->m_fScale;
    region_maxs[0] = (double)xy->m_nWidth  * 0.5 / xy->m_fScale + xy->m_vOrigin[0];
    region_mins[1] = xy->m_vOrigin[1] - (double)xy->m_nHeight * 0.5 / xy->m_fScale;
    region_maxs[1] = 0.5 * (double)xy->m_nHeight / xy->m_fScale + xy->m_vOrigin[1];
    // BINARY BUG, kept: +131072 goes to BOTH Z bounds, so the Z slab is degenerate and
    // XY-region hides the whole map.
    region_mins[2] = 131072.0f;
    region_maxs[2] = 131072.0f;
    Map_ApplyRegion();
}

void Map_RegionTallBrush()
{
    if ( !QE_SingleBrush() )
        return;
    selbrush_t *b = selected_brushes.next;
    Map_RegionOff();
    region_mins[0] = b->def->mins[0];
    region_mins[1] = b->def->mins[1];
    region_maxs[0] = b->def->maxs[0];
    region_maxs[1] = b->def->maxs[1];
    region_mins[2] = -131072.0f;   // full vertical extent
    region_maxs[2] =  131072.0f;
    Select_Delete();
    Map_ApplyRegion();
}

void Map_RegionBrush()
{
    if ( !QE_SingleBrush() )
        return;
    selbrush_t *b = selected_brushes.next;
    Map_RegionOff();
    region_mins[0] = b->def->mins[0];
    region_mins[1] = b->def->mins[1];
    region_mins[2] = b->def->mins[2];
    region_maxs[0] = b->def->maxs[0];
    region_maxs[1] = b->def->maxs[1];
    region_maxs[2] = b->def->maxs[2];
    Select_Delete();
    Map_ApplyRegion();
}

void Map_RegionSelectedBrushes()
{
    Map_RegionOff();
    if ( selected_brushes.next == &selected_brushes )
    {
        Sys_Printf( "Tried to region with no selection...\n" );
        return;
    }
    g_region_active = 1;
    Select_GetBounds( region_mins, region_maxs );

    // Move the entire active list into filtered_brushes (hide it)…
    filtered_brushes.next        = active_brushes.next;
    filtered_brushes.prev        = active_brushes.prev;
    active_brushes.next->prev    = &filtered_brushes;
    active_brushes.prev->next    = &filtered_brushes;
    // …then move the selection into active_brushes (show only the selection)…
    active_brushes.next          = selected_brushes.next;
    active_brushes.prev          = selected_brushes.prev;
    selected_brushes.next->prev  = &active_brushes;
    selected_brushes.prev->next  = &active_brushes;
    // …and empty the selection list.
    selected_brushes.next        = &selected_brushes;
    selected_brushes.prev        = &selected_brushes;
    g_nUpdateBits = -1;
}


// 0x487950  Map_GetNextAutoTarget — 1 + the highest numeric suffix among all
// "targetname"/"target" epairs beginning "auto".
int Map_GetNextAutoTarget()
{
    int maxId = 0;

    for ( entity_s *e = entities.next; e != &entities; e = e->next )
    {
        for ( epair_t *p = e->epairs; p; p = p->next )
        {
            const char *key = p->key;
            if ( _stricmp( key, "targetname" ) != 0 && _stricmp( key, "target" ) != 0 )
                continue;
            const char *tn = p->value;
            iassert( tn );
            if ( tn && !strncmp( tn, "auto", 4 ) )
            {
                int v = (int)atol( tn + 4 );
                if ( v > maxId ) maxId = v;
            }
        }
    }

    return maxId + 1;
}


// 0x487A70  Map_GetNextExportId — 1 + the highest "export" epair value, as a VA string.
char *Map_GetNextExportId( int slot )
{
    int maxId = 0;

    for ( entity_s *e = entities.next; e != &entities; e = e->next )
    {
        for ( epair_t *p = e->epairs; p; p = p->next )
        {
            if ( _stricmp( p->key, "export" ) != 0 )
                continue;
            const char *tn = p->value;
            iassert( tn );
            if ( tn && *tn )
            {
                int v = (int)atol( tn );
                if ( v > maxId ) maxId = v;
            }
        }
    }

    return VA( slot, "%i", maxId + 1 );
}


// 0x488DF0  Entity_WriteSelected_R — write every entity to a MemFile writer.
// `writer` is brush.cpp's WriteWriter_t (WriteFunc_t**); the writer ptr is also its own ctx.
void Entity_WriteSelected_R( WriteFunc_map_t **writer )
{
    iassert( entities.next != &entities );   // map.cpp:1323
    iassert( !strcmp( entities.next->eclass->name, "worldspawn" ) );   // map.cpp:1324 (derefs eclass unconditionally)

    int entIdx = 0;
    for ( entity_s_def *e = (entity_s_def *)entities.next;
          e != (entity_s_def *)&entities;
          e  = (entity_s_def *)e->next, ++entIdx )
    {
        WriteFunc_map_t *fn = *writer;
        fn( writer, "// entity %i\n", entIdx );
        Entity_WriteSelected( e, writer );
    }
}


// 0x488FC0  PrefabList_ClearModelClass — FreePrefabLevel's tree walk, but instead of freeing
// it NULLs the cached def->modelClass of every already-realized instance of the prefab being
// entered.  `modelClass` is that prefab's modelClass POINTER, not a numeric level; it is
// compared against def->modelClass, NOT def->eclass.
static void PrefabList_ClearModelClass( entity_s *first, entity_s *end, entity_s *skip,
                                        entitymodel_t *modelClass )
{
    for ( entity_s *i = first; i != end; i = i->next )
    {
        entity_s_def *def = (entity_s_def *)i->def;
        if ( ( def->eclass->classtype & 0x10 ) == 0 )   // not ECLASS_PREFAB
            continue;
        if ( !def->modelClass )
            continue;
        if ( i == skip )
            continue;
        if ( def->modelClass == modelClass )
        {
            def->modelClass = nullptr;
        }
        else if ( i->prefab )
        {
            prefab_s *pf = (prefab_s *)i->prefab;
            PrefabList_ClearModelClass( pf->next_entity, (entity_s *)pf, skip, modelClass );
        }
    }
}

// 0x4890F0  Prefab_FreeAllInstances — free every realized instance of one prefab across the
// whole stack (each saved slot's instance list + the live entityInsts), then clear their def
// fields.
static void Prefab_FreeAllInstances( entity_s *skip, entitymodel_t *modelClass )
{
    for ( int level = 0; level < prefabStackLevel; ++level )
        FreePrefabLevel( g_prefabStack[level].entityInstsNext, &entityInsts, skip, modelClass );
    FreePrefabLevel( entityInsts.next, &entityInsts, skip, modelClass );

    for ( int level = 0; level < prefabStackLevel; ++level )
        PrefabList_ClearModelClass( g_prefabStack[level].entityInstsNext, &entityInsts, skip, modelClass );
    PrefabList_ClearModelClass( entityInsts.next, &entityInsts, skip, modelClass );
}


// 0x489030  FreePrefabLevel — recursively free the realized child instances of one prefab.
// `modelClass` is the prefab's modelClass pointer, matched against def->modelClass,
// NOT def->eclass.
void FreePrefabLevel( entity_s *first, entity_s *end, entity_s *skip, entitymodel_t *modelClass )
{
    for ( entity_s *i = first; i != end; i = i->next )
    {
        if ( !i->prefab )
            continue;

        entity_s_def *def = (entity_s_def *)i->def;
        {
            entity_s *e = i;                     // the binary's local
            iassert( e->def->eclass->nShowFlags & ECLASS_PREFAB );   // map.cpp:1416
        }

        if ( i == skip )
            continue;

        if ( def->modelClass == modelClass )
        {
            Entity_FreePrefab( i );
            if ( i->modelInst )
            {
                RemoveModelInstFromBuf( i->modelInst );
                i->modelInst = 0;
            }
            ++*(ushort *)&def->version_prob_wrong;  // ++def->version (word)
            // clear the def-brush's modelFailed byte
            if ( i->brushes.ownerNext && i->brushes.ownerNext != (selbrush_t *)&i->brushes )
            {
                brush_t *bdef = i->brushes.ownerNext->def;
                if ( bdef )
                    bdef->modelFailed = 0;
            }
        }
        else
        {
            entity_s *subList = ((prefab_s *)i->prefab)->next_entity;
            entity_s *subEnd  = (entity_s *)i->prefab;
            FreePrefabLevel( subList, subEnd, skip, modelClass );
        }
    }
}


// 0x48CC70  Map_OpenForSave — open the save file "wb"; NULL (and a console error) on failure.
// KIWI: the Perforce add / open-for-edit prompts are gone; a read-only file now says so
// instead of failing silently.
FILE *Map_OpenForSave( const char *path )
{
    FILE *result = fopen( path, "wb" );
    if ( !result )
    {
        FILE *probe = fopen( path, "rb" );
        if ( probe )
        {
            fclose( probe );
            Sys_Printf( "ERROR!!!! '%s' is read-only - not saved!\n", path );
        }
        else
            Sys_Printf( "ERROR!!!! Couldn't open '%s'!\n", path );
    }
    return result;
}


// Region save: does the brush def overlap the region box on all 3 axes (0x4874F0, inlined
// at both MapFile_WriteEntity call sites)?
static bool Brush_OverlapsRegion( const brush_t *b )
{
    for ( int ax = 0; ax < 3; ++ax )
        if ( region_maxs[ax] < b->mins[ax] || region_mins[ax] > b->maxs[ax] )
            return false;
    return true;
}

// 0x4844E0  MapFile_WriteEntity — write one entity block to fout.
// The binary's writer is a NormalFileWrapper { vftable, fout } pair; Brush_Write
// dispatches through (*writer)(writer, fmt, ...).  Modelled here by a static
// WriteFunc_map_t* over the FILE* — the vtable's only method is an fprintf.
char MapFile_WriteEntity( entity_s_def *ent, FILE *fout, char regionSave )
{
    static FILE            *s_writer_file = nullptr;
    static WriteFunc_map_t *s_writer_fn   = nullptr;
    struct NormalFileWriter
    {
        static int Write( void *ctx, const char *fmt, ... )
        {
            (void)ctx;
            va_list ap;
            va_start( ap, fmt );
            int r = vfprintf( s_writer_file, fmt, ap );
            va_end( ap );
            return r;
        }
    };

    s_writer_file = fout;
    s_writer_fn   = NormalFileWriter::Write;
    WriteFunc_map_t **writer = &s_writer_fn;

    // Region save: an info_player_start is replaced by the live camera position.
    if ( regionSave )
    {
        if ( strcmp( ValueForKey2( ent, "classname" ), "info_player_start" ) == 0 )
        {
            float camOri[3] = { 0, 0, 0 };
            float camAng[3] = { 0, 0, 0 };
            {
                const camera_s *cam = Ed_Camera();   // never NULL
                camOri[0] = cam->origin[0];
                camOri[1] = cam->origin[1];
                camOri[2] = cam->origin[2];
                camAng[1] = cam->angles[1];
            }
            fprintf( fout, "{\n" );
            fprintf( fout, "\"classname\" \"info_player_start\"\n" );
            // KIWI: exact - "%.1f" / "%.0f" spawned the region test off the camera.
            char camText[160], yawText[48];
            fprintf( fout, "\"origin\" \"%s\"\n", KiwiFmt_Vec3Exact( camText, sizeof( camText ), camOri ) );
            return (char)fprintf( fout, "\"angles\" \"0 %s 0\"\n}\n", KiwiFmt_Exact( yawText, sizeof( yawText ), camAng[1] ) );
        }

        // Region filter: walk the DEF-list — first = brushes.prev, sentinel = &def, advance
        // via brush_t.onext.  Skipping is UNCONDITIONAL when nothing overlaps; there is no
        // fixed-size exception.
        bool anyInRegion = false;
        brush_t *precSentinel = (brush_t *)&ent->def;
        for ( brush_t *b = (brush_t *)ent->brushes.prev; b != precSentinel; b = b->onext )
        {
            if ( Brush_OverlapsRegion( b ) ) { anyInRegion = true; break; }
        }

        if ( !anyInRegion )
            return 0;
    }

    fprintf( fout, "{\n" );

    // Fixed-size entity: emit origin + optional layer line.  The per-entity layer is the
    // bbox brush's parent_layer_string; the binary strcmps it against "000_Global" BEFORE
    // null-checking it, and only writes the line on mismatch.
    char originStr[132];
    originStr[0] = '\0';
    if ( ent->eclass && ent->eclass->fixedsize )
    {
        // KIWI: exact origins (kiwi_fmt.h).  The binary's "%.1f" moved every model, light
        // and spawn to the 0.1 grid on each save - modular pieces opened 0.1 seams.
        char ox[40], oy[40], oz[40];
        sprintf( originStr, "%s %s %s", KiwiFmt_Exact( ox, sizeof( ox ), ent->origin[0] ),
                 KiwiFmt_Exact( oy, sizeof( oy ), ent->origin[1] ), KiwiFmt_Exact( oz, sizeof( oz ), ent->origin[2] ) );
        // KIWI: the writer is pure - this text goes to disk only; the "origin" epair is not
        // rewritten (ent->origin is the truth; the live fragile check's timer was freeing
        // every origin string).

        const char *layerName = ( (brush_t *)ent->brushes.prev )->parent_layer_string;
        if ( strcmp( layerName, "000_Global" ) != 0 )
        {
            // Inlined MapLoad_ParseBrush_Layer: this writer is a bare FILE*, not its callback.
            iassert( layerName );
            fprintf( fout, "layer \"%s\"\n", layerName );
        }
    }

    // Epairs as `"key" "value"` — Project_WriteQuotedString (0x484460) escapes \ and ".
    auto writeQuoted = [&]( const char *s )
    {
        if ( !s ) s = "";
        fputc( '"', fout );
        for ( const char *c = s; *c; ++c )
        {
            if ( *c == '\\' || *c == '"' )
                fputc( '\\', fout );
            fputc( *c, fout );
        }
        fputc( '"', fout );
    };
    // KIWI: the origin text goes where SetKeyValue would have put it: in place, or first
    // (Epair_SetRaw prepends a missing key)
    bool originInPlace = false;
    if ( originStr[0] )
    {
        for ( epair_t *ep = ent->epairs; ep; ep = ep->next )
            if ( !strcmp( ep->key, "origin" ) ) { originInPlace = true; break; }
        if ( !originInPlace )
        {
            writeQuoted( "origin" );
            fputc( ' ', fout );
            writeQuoted( originStr );
            fputc( '\n', fout );
        }
    }
    for ( epair_t *ep = ent->epairs; ep; ep = ep->next )
    {
        writeQuoted( ep->key );
        fputc( ' ', fout );
        writeQuoted( originInPlace && !strcmp( ep->key, "origin" ) ? originStr : ep->value );
        fputc( '\n', fout );
    }

    // Brush blocks for non-fixed-size entities — same DEF-list walk as the region
    // filter above; each node IS the brush_t def.
    if ( ent->eclass && !ent->eclass->fixedsize )
    {
        brush_t *sentinel = (brush_t *)&ent->def;
        int      brushIdx = 0;
        for ( brush_t *b = (brush_t *)ent->brushes.prev; b != sentinel; b = b->onext )
        {
            if ( regionSave && !Brush_OverlapsRegion( b ) )   // outside the region box
                continue;
            fprintf( fout, "// brush %i\n", brushIdx++ );
            Brush_Write( writer, b );
        }
    }

    return (char)fprintf( fout, "}\n" );
}


// 0x489190  Prefab_NextLevel — push one level onto the prefab edit stack: save the current
// map state into the 2168-byte (0x878) slot, then swap the live world lists (entities /
// entityInsts / active_brushes) to the selected misc_prefab's instanced sub-map so it can be
// edited in place.  Prefab_PrevLevel reads back the same slot layout.
// `brush` = a brush instance to enter directly; NULL enters the single selected misc_prefab
// (Radiant_PrefabEnter passes NULL).
void Prefab_NextLevel( void *brush )
{
    selbrush_t *sb = (selbrush_t *)brush;

    // Precondition: an explicit brush, OR exactly one selected ECLASS_PREFAB instance.
    bool ok = ( sb != nullptr );
    if ( !ok )
    {
        selbrush_t *sel = selected_brushes.next;
        if ( sel != &selected_brushes &&
             sel->next == &selected_brushes &&                       // exactly one selected
             sel->owner &&
             sel->owner->def &&
             ( ((entity_s *)sel->owner->def)->eclass->classtype & 0x10 ) != 0 )
        {
            ok = true;
        }
    }
    if ( !ok )
    {
        Sys_Printf( "Must have a single prefab selected.\n" );
        return;
    }

    if ( prefabStackLevel == 16 )
    {
        Sys_Printf( "Max prefab nesting exceeded.\n" );
        return;
    }

    // Sys_PrintActiveBrushes("before entering prefab") — NOT a console line: it
    // repopulates the debug brush-list window, exactly as Prefab_PrevLevel does inline.
    MainFrm_BrushList( VA( 0, "%s - active_brushes", "before entering prefab" ), &active_brushes );
    MainFrm_BrushList( VA( 1, "%s - active_brushes", "before entering prefab" ), &selected_brushes );

    // Resolve the brush instance + its prefab.
    selbrush_t *prefabBrush = sb ? sb : selected_brushes.next;

    entity_s  *owner  = prefabBrush->owner;
    prefab_s  *prefab = (prefab_s *)owner->prefab;
    if ( !prefab )
    {
        Sys_Printf( "Prefab not loaded.\n" );
        return;
    }

    // The prefab's DEFINITION entity-list container (modelClass->model), whose
    // entities.prev / entities.next are the def `entities` sentinel head.
    entity_s_def *def        = (entity_s_def *)owner->def;
    entitymodel_t *modelClass = def->modelClass;
    modelnode_t *defEntsRoot = modelClass->model;
    entity_s *defEntsNext    = (entity_s *)defEntsRoot->entities.next;
    entity_s *defEntsPrev = defEntsRoot->entities.prev;

    // Drop the cached light previews — they belong to the parent map's brushes,
    // which are about to be swapped out (same reset as mainfrm.cpp's Cmd_OnClearPreviewList).
    CamWnd_ClearLightPreviews();

    // Free any previously-realized prefab instances for this owner (recursive).
    Prefab_FreeAllInstances( prefabBrush->owner, modelClass );

    if ( !sb )
        Select_Deselect( 1 );

    Eclass_FreeModel( modelClass );

    // ── save current world state into the (pre-increment) stack slot ──────────
    const int oldLevel = prefabStackLevel;
    prefabLevel_t *slot = &g_prefabStack[prefabStackLevel];
    ++prefabStackLevel;

    slot->regionActive  = g_region_active;
    slot->regionMins[0] = region_mins[0];
    slot->regionMins[1] = region_mins[1];
    slot->regionMins[2] = region_mins[2];
    slot->regionMaxs[0] = region_maxs[0];
    slot->regionMaxs[1] = region_maxs[1];
    slot->regionMaxs[2] = region_maxs[2];
    if ( g_region_active )
        Map_RegionOff();

    strcpy( slot->mapName, currentmap );
    slot->modified = modified;

    // List heads (PrevLevel reads these back).
    slot->activeNext      = active_brushes.next;
    slot->activePrev      = active_brushes.prev;
    slot->entitiesNext    = entities.next;
    slot->entitiesPrev    = entities.prev;
    slot->entityInstsPrev = entityInsts.prev;
    slot->entityInstsNext = entityInsts.next;
    slot->prefabBrush     = prefabBrush;

    // Layer snapshot — the binary deep-copies the layer tree into the slot; here layerMap
    // is a std::map, so this is a per-level snapshot.
    Layers_SavePrefabLayers( oldLevel );

    strcpy( slot->activeLayer, g_activeLayer_string );

    if ( sb )
    {
        slot->selectedNext = selected_brushes.next;
        slot->selectedPrev = selected_brushes.prev;
    }
    else
    {
        slot->selectedNext = nullptr;
        slot->selectedPrev = nullptr;
    }

    // ── camera / XY reframe (Ed_ActiveXY() / Ed_Camera() are never NULL) ─────────
    {
        xywndState_t *xy  = Ed_ActiveXY();
        camera_s     *cam = Ed_Camera();

        // Save the parent view (PrevLevel reads these back).
        slot->xyOrigin[0]  = xy->m_vOrigin[0];
        slot->xyOrigin[1]  = xy->m_vOrigin[1];
        slot->xyScale      = xy->m_fScale;
        slot->xyViewType   = (int)xy->m_nViewType;
        slot->camOrigin[0] = cam->origin[0];
        slot->camOrigin[1] = cam->origin[1];
        slot->camOrigin[2] = cam->origin[2];
        slot->camAngles[0] = cam->angles[0];
        slot->camAngles[1] = cam->angles[1];
        slot->camAngles[2] = cam->angles[2];

        // Reframe the live camera into the prefab's local space.  entAxis is the entity
        // orientation matrix, origin-first [4][3] (row0 = origin, rows 1..3 = rotation).
        //   OrientationWorldPosToLocalPos (0x4BA610):  localPt[i] = row(i+1) · (worldPt − origin)
        //   VectorRotateByAxis (0x4BA6B0):             rotated[i] = row(i+1) · dir
        float entAxis[4][3];
        {
            float angles[3] = { 0, 0, 0 };
            Entity_GetVec3ForKey( (entity_s_def *)prefabBrush->owner->def, angles, "angles" );
            AnglesToAxis( angles, entAxis + 1 );
            entAxis[0][0] = prefabBrush->owner->origin[0];
            entAxis[0][1] = prefabBrush->owner->origin[1];
            entAxis[0][2] = prefabBrush->owner->origin[2];
        }
        // localOrigin = rows1..3 · (camOrigin − entOrigin)   [== OrientationWorldPosToLocalPos]
        float d[3] = { cam->origin[0] - entAxis[0][0],
                       cam->origin[1] - entAxis[0][1],
                       cam->origin[2] - entAxis[0][2] };
        float localOrigin[3];
        localOrigin[0] = entAxis[1][0] * d[0] + entAxis[1][1] * d[1] + entAxis[1][2] * d[2];
        localOrigin[1] = entAxis[2][0] * d[0] + entAxis[2][1] * d[1] + entAxis[2][2] * d[2];
        localOrigin[2] = entAxis[3][0] * d[0] + entAxis[3][1] * d[1] + entAxis[3][2] * d[2];

        float camAxis[3][3];
        AnglesToAxis( cam->angles, camAxis );
        float rotAxis[3][3];
        VectorRotateByAxis( rotAxis[0], &entAxis[0][0], camAxis[0] );
        VectorRotateByAxis( rotAxis[1], &entAxis[0][0], camAxis[1] );
        VectorRotateByAxis( rotAxis[2], &entAxis[0][0], camAxis[2] );

        float newAngles[3];
        AxisToAngles( newAngles, rotAxis );
        // Wrap angles[0] to [-180,180).
        float wrapped = newAngles[0] * 0.002777777845039964f;   // /360
        newAngles[0] = ( wrapped - floorf( wrapped + 0.5f ) ) * 360.0f;
        newAngles[2] = 0.0f;

        cam->origin[0] = localOrigin[0];
        cam->origin[1] = localOrigin[1];
        cam->origin[2] = localOrigin[2];
        cam->angles[0] = newAngles[0];
        cam->angles[1] = newAngles[1];
        cam->angles[2] = newAngles[2];

        // Re-centre the 2D view: zero exactly the TWO in-plane m_vOrigin components —
        // indices (viewType==0) and (viewType!=2)+1 — depth axis untouched.
        int vt = (int)xy->m_nViewType;
        xy->m_vOrigin[ (vt == 0) ? 1 : 0 ] = 0.0f;
        xy->m_vOrigin[ (vt != 2) ? 2 : 1 ] = 0.0f;
    }

    // ── swap the live world lists to the prefab's sub-map ─────────────────────
    entities.prev = (entity_s *)(intptr_t)defEntsPrev;
    entities.next = defEntsNext;
    defEntsNext->prev = &entities;
    ( (entity_s *)(intptr_t)defEntsPrev )->next = &entities;

    entityInsts.next = prefab->next_entity;
    entityInsts.prev = prefab->prev_entity;
    prefab->next_entity->prev = &entityInsts;
    prefab->prev_entity->next = &entityInsts;

    world_entity = prefab->next_entity;

    active_brushes.next = prefab->active_brushlist_next;
    active_brushes.prev = prefab->active_brushlist;
    active_brushes.next->prev = &active_brushes;
    active_brushes.prev->next = &active_brushes;

    // The reset RUNS on an explicit-brush entry (the selection was just saved into
    // the slot).  On the NULL path Select_Deselect(1) above already emptied it.
    if ( sb )
    {
        selected_brushes.next = &selected_brushes;
        selected_brushes.prev = &selected_brushes;
    }

    // Stale the version stamps so every brush/patch/entity re-displays in the sub-map.
    for ( selbrush_t *i = active_brushes.next; i != &active_brushes; i = i->next )
    {
        i->version = (__int16)( i->def->version - 1 );
        // Also stamp the patch INSTANCE's cached version from the patch DEF version.
        if ( i->patch )
            i->patch->version = (__int16)( i->patch->def->version - 1 );
    }
    for ( entity_s *j = entityInsts.next; j != &entityInsts; j = j->next )
    {
        // def version is the word at def->version_prob_wrong; the instance version is
        // the low word of entity_s.version.
        ushort dv = *(ushort *)&j->def->version_prob_wrong;
        *(ushort *)&j->version = (ushort)( dv - 1 );
    }

    modified = 0;

    // Build currentmap = "<project mapspath>\<prefab model>" with '/' separators.
    {
        const char *mapsPath = g_qeglobals.d_project_entity
                             ? ValueForKey2( g_qeglobals.d_project_entity, "mapspath" ) : "";
        strcpy( currentmap, mapsPath );
        size_t n = strlen( currentmap );
        if ( n && currentmap[n - 1] != '\\' )
        {
            currentmap[n] = '\\';
            currentmap[n + 1] = '\0';
            ++n;
        }
        // append the misc_prefab def's "model" epair (the referenced .map path)
        strcat( currentmap, ValueForKey2( prefabBrush->owner->def, "model" ) );
        for ( char *k = currentmap; *k; ++k )
            if ( *k == '\\' ) *k = '/';
    }

    // The prefab_s head-pointers now live in the world sentinels, so the head itself is
    // free-able.  Then invalidate the owner def's cached modelClass and bump its version so
    // the instance re-realizes on leave, and clear the def-brush's modelFailed byte.
    {
        entity_s     *owner2 = prefabBrush->owner;
        free( owner2->prefab );
        owner2->prefab = nullptr;
        entity_s_def *odef = (entity_s_def *)owner2->def;
        if ( odef )
        {
            odef->modelClass = nullptr;                    // re-realize on leave
            ++*(ushort *)&odef->version_prob_wrong;// ++version (word)
        }
        if ( prefabBrush->def )
            prefabBrush->def->modelFailed = 0;
    }

    Map_InitlLayers();
    g_qeglobals.g_layerCount_maybe = 1;
    Sys_Printf( "Updating layers...\n" );
    Layers_SetMapLayers();
    Layers_RefreshDialog();
    Undo_Clear();
    g_nUpdateBits = -1;
    SetWindowTextA( g_qeglobals.d_hwndMain, currentmap );

    MainFrm_BrushList( VA( 0, "%s - active_brushes",   "after entering prefab" ), &active_brushes );
    MainFrm_BrushList( VA( 1, "%s - active_brushes",   "after entering prefab" ), &selected_brushes );
}


// 0x489890  Prefab_PrevLevel — pop one prefab-edit level, restoring the saved map state.
void Prefab_PrevLevel()
{
    iassert( prefabStackLevel > 0 );

    MainFrm_BrushList( VA( 0, "%s - active_brushes",   "before leaving prefab" ), &active_brushes );
    MainFrm_BrushList( VA( 1, "%s - active_brushes", "before leaving prefab" ), &selected_brushes );

    Select_Deselect( 1 );
    Brush_FreeMapBrushes();
    --prefabStackLevel;

    // Restore from the (already decremented) level's slot.
    prefabLevel_t *slot = &g_prefabStack[prefabStackLevel];

    entities.next = slot->entitiesNext;
    entities.prev = slot->entitiesPrev;

    iassert( entities.next->prev == &entities );
    iassert( entities.prev->next == &entities );

    entityInsts.prev = slot->entityInstsPrev;
    entityInsts.next = slot->entityInstsNext;

    iassert( entityInsts.next->prev == &entityInsts );
    iassert( entityInsts.prev->next == &entityInsts );

    active_brushes.next = slot->activeNext;
    active_brushes.prev = slot->activePrev;

    world_entity = entityInsts.next;

    iassert( active_brushes.next->prev == &active_brushes );
    iassert( active_brushes.prev->next == &active_brushes );

    if ( slot->selectedNext )
    {
        selbrush_t *sn = slot->selectedNext;
        selbrush_t *sp = slot->selectedPrev;
        if ( sp )
        {
            selected_brushes.next = sn;
            selected_brushes.prev = sp;
        }
    }

    strncpy( currentmap, slot->mapName, 1024 );
    currentmap[1023] = '\0';

    modified        = slot->modified;
    g_region_active = slot->regionActive;

    region_mins[0] = slot->regionMins[0];
    region_mins[1] = slot->regionMins[1];
    region_mins[2] = slot->regionMins[2];
    region_maxs[0] = slot->regionMaxs[0];
    region_maxs[1] = slot->regionMaxs[1];
    region_maxs[2] = slot->regionMaxs[2];

    // The prefab brush the user was editing.
    entity_brush_s *prefabBrush = (entity_brush_s *)slot->prefabBrush;
    Model_SetModel( prefabBrush, (const float *)&world_orient_matrix );
    Map_InitlLayers();

    // Restore the parent map's layers (the binary clears the live tree, then deep-copies
    // the slot's back).
    Layers_RestorePrefabLayers( prefabStackLevel );

    // Reload the prefab brush's "model" layers, then refresh the live layer set.
    {
        // "" when there is no "model" key
        Layers_TagModelPath( ValueForKey2( prefabBrush->owner->def, "model" ) );
        Sys_Printf( "Updating layers...\n" );
        Layers_SetMapLayers();
        Layers_RefreshDialog();
    }

    // Active layer name
    strncpy( g_activeLayer_string, slot->activeLayer, 255 );
    g_activeLayer_string[255] = '\0';
    Layers_HighlightActive( g_activeLayer_string );

    // Camera restore (Ed_Camera() is never NULL)
    {
        camera_s *cam = Ed_Camera();
        cam->origin[0] = slot->camOrigin[0];
        cam->origin[1] = slot->camOrigin[1];
        cam->origin[2] = slot->camOrigin[2];
        cam->angles[0] = slot->camAngles[0];
        cam->angles[1] = slot->camAngles[1];
        cam->angles[2] = slot->camAngles[2];
    }

    // XY restore (Ed_ActiveXY() is never NULL).
    {
        xywndState_t *xy = Ed_ActiveXY();
        // PositionView() runs BEFORE the m_vOrigin/m_fScale restore — it overwrites
        // m_vOrigin[nDim1/nDim2], so the restore afterwards is what wins.
        XYWnd_PositionView();
        xy->m_vOrigin[0] = slot->xyOrigin[0];
        xy->m_vOrigin[1] = slot->xyOrigin[1];
        // BINARY BUG, kept: the scale slot is read TWICE — m_vOrigin[2] gets the saved
        // m_fScale.  NextLevel only ever saved origin[0]/[1]/scale.
        xy->m_vOrigin[2] = slot->xyScale;
        xy->m_fScale     = slot->xyScale;
        XYWnd_SetViewType( slot->xyViewType );
    }

    // The binary's `if (surfDlgGlob.hwnd) { SetTexMods(); Select_SetTexture_2(&g_dlgSurface); }` —
    // Surf_UpdateInspector (KIWI: SetTexMods is gone), a no-op when the inspector is closed.
    Surf_UpdateInspector();
    // NO-MFC: the texture-bar push is skipped — CTextureBar is an MFC CWnd embedded in
    // CMainFrame and went with the frame (KIWI: texturebar.cpp is deleted), so there is
    // nothing to push.  Same treatment as surfacedlg.cpp:396.

    BrushList_RebuildDisplay( &active_brushes );
    BrushList_RebuildDisplay( &selected_brushes );
    BrushList_RebuildDisplay( &filtered_brushes );
    CopySelectedFaceValues();

    if ( g_region_active )
        Map_ApplyRegion();

    Undo_Clear();
    g_nUpdateBits = -1;
    SetWindowTextA( g_qeglobals.d_hwndMain, currentmap );

    MainFrm_BrushList( VA( 0, "%s - active_brushes",   "after leaving prefab" ), &active_brushes );
    MainFrm_BrushList( VA( 1, "%s - active_brushes", "after leaving prefab" ), &selected_brushes );
}


// 0x489D50  Prefab_LevelBack — pop every prefab level.
void Prefab_LevelBack()
{
    while ( true )
    {
        iassert( prefabStackLevel >= 0 );
        if ( prefabStackLevel <= 0 )
            break;
        Prefab_PrevLevel();
    }
}


// The two prefab-nesting command bodies, free functions next to their Prefab_NextLevel /
// Prefab_PrevLevel cores; Radiant_DispatchCommandDirect (mainfrm.cpp, 33173 / 33174) calls
// them.  The dirty-prefab prompt is Radiant_ConfirmModified.
extern bool Radiant_ConfirmModified();   // mainfrm.cpp (CMainFrame::ConfirmModified 0x49A030)

// 0x42BF70  Edit→Enter Prefab (menu 33173).
void Radiant_PrefabEnter()
{
    Prefab_NextLevel( nullptr );
}

// 0x42BF80  Edit→Leave Prefab (menu 33174).
void Radiant_PrefabLeave()
{
    iassert( prefabStackLevel >= 0 );
    if ( prefabStackLevel > 0 && ( !modified || Radiant_ConfirmModified() ) )
        Prefab_PrevLevel();
}


// 0x488C70  Map_ImportFile — File→Load: merge another .map into the current one.  Runs the
// shared parse session (spaceDelimited=0 / negativeNumbers=1, plus the iwmap<4 space-delimit
// fixup), places the entities/brushes via Map_ImportBuffer, then rebuilds brush data.
// The binary's backslash→slash loop writes into a scratch buffer it never reads
// AND only stores on the '\\' branch — LoadFile/Com_BeginParseSession both take the RAW
// path.  Not reproduced; `path` is passed through as the binary does.
extern void  Com_BeginParseSession( const char *name );         // q_parse
extern int   Map_ReadVersion( const void **text );              // layers.cpp 0x4861F0
extern char *Map_ImportBuffer( const char **text, int version );// defined below (0x487C90)

void Map_ImportFile( const char *path )
{
    HCURSOR wait = SetCursor( LoadCursorA( 0, (LPCSTR)IDC_WAIT ) );

    void *buf = nullptr;
    bool didImport = false;   // the updateBits/modified tail runs on LoadFile FAIL or on a successful import
    if ( LoadFile( path, &buf ) == -1 )
    {
        didImport = true;     // the binary does this on load failure — odd, but kept
    }
    else
    {
        Com_BeginParseSession( path );
        ParseThreadInfo *pi = Com_GetParseThreadInfo();
        pi->parseInfo[pi->parseInfoNum].spaceDelimited  = 0;
        pi->parseInfo[pi->parseInfoNum].negativeNumbers = 1;

        const void *textPtr = buf;
        int mapVer = Map_ReadVersion( &textPtr );
        if ( mapVer >= 0 )
        {
            if ( mapVer < 4 )
                pi->parseInfo[pi->parseInfoNum].spaceDelimited = 1;
            Map_ImportBuffer( (const char **)&textPtr, mapVer );

            Com_EndParseSession();
            free( buf );
            Map_BuildBrushData();
            didImport = true;
        }
        else
        {
            // mapVer < 0: end session + free, but DON'T touch updateBits/modified.
            Com_EndParseSession();
            free( buf );
        }
    }

    if ( didImport )
    {
        g_nUpdateBits = -1;
        modified = 1;
    }
    if ( wait )
        SetCursor( wait );
}

// ═════════════════════════════════════════════════════════════════════════════
//  RELOCATED HOME — these functions' embedded asserts name THIS file as
//  their source.
// ═════════════════════════════════════════════════════════════════════════════
// deps of the parse bodies:
extern void      Entity_Free_R( entity_s *e );                        // entity.cpp
extern entity_s *ParseEntity( const char **text, int version, char rawOnly, char keepFileLayers );  // entity.cpp 0x483E70
extern void      Map_ParseLayers( const void **text, int skipFlags ); // 0x486280
extern bool      g_bScreenUpdates;                                    // entity.cpp

// ─────────────────────────────────────────────────────────────────────────────
// 0x486500  Map_LoadEntities — load entities from a file into the given doubly-linked
// entity list (TAIL splice, ParseEntity(mapVer,0,1), its own parse session).
// ─────────────────────────────────────────────────────────────────────────────
int Map_LoadEntities( const char *filename, entity_s *entList, char skipLayerFlags )
{
    iassert( entList->next == entList );   // map.cpp:304
    iassert( entList->prev == entList );   // map.cpp:305

    int   count = 0;
    void *buf   = nullptr;
    // RE-ENTERED once per prefab (Prefab_Load -> Eclass_RealizeModel -> here).  LoadFile is a
    // raw fopen/fread (no FS, no iwd — cmdlib.cpp).
    {
        if ( LoadFile( filename, &buf ) == -1 )
            goto done;
    }

    {
        Com_BeginParseSession( filename );
        ParseThreadInfo *pi = Com_GetParseThreadInfo();
        pi->parseInfo[pi->parseInfoNum].spaceDelimited  = 0;
        pi->parseInfo[pi->parseInfoNum].negativeNumbers = 1;

        const void *textPtr  = buf;
        int         mapVer   = Map_ReadVersion( &textPtr );
        if ( mapVer >= 0 )
        {
            if ( mapVer < 4 )
                pi->parseInfo[pi->parseInfoNum].spaceDelimited = 1;
            if ( mapVer > 3 )
                Map_ParseLayers( &textPtr, skipLayerFlags != 0 );

            extern void IncRef( entity_s *e, entity_s *list );   // entity.cpp 0x483bf0
            for ( entity_s *e = ParseEntity( (const char **)&textPtr, mapVer, 0, 1 );
                  e;
                  e = ParseEntity( (const char **)&textPtr, mapVer, 0, 1 ) )
            {
                ++count;
                IncRef( e, entList );                    // inlined in the binary
            }
        }

        Com_EndParseSession();
    }

done:
    free( buf );
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x487c90  Map_ImportBuffer — the PASTE parse-and-place.
//
// Parses a .map text buffer (produced by Entity_WriteSelected_R into the clipboard)
// back into live brushes/entities and selects them. This is the engine behind
// CXYWnd::Paste and (in the binary) Clone_Selection. The parse machinery is the
// ParseEntity / Brush_Parse / Prefab_Init chain (same as Map_LoadEntities).
//
// Control flow:
//   1. Select_Deselect(1); Undo bracket ("import buffer"); d_parsed_brushes = 0.
//   2. For each entity ParseEntity returns:
//        * worldspawn: MERGE its brushes into world_entity's def-list (Entity_Unlink/
//          LinkBrush), rebuild windings, instance (Brush_AddToList) + select
//          (Brush_AddToList2); then Entity_Free_R the now-empty parsed worldspawn def.
//        * other: Prefab_Init it into the entityInsts/active list, splice the DEF into
//          the `entities` list, then SELECT every instance brush (Select_Brush). The
//          undo records get stamped onto the new entity/brushes so the paste undoes.
//   3. Final pass: instance the model for misc_model/prefab entities (classtype&0x10),
//      rebuild windings on the freshly-selected brushes, MarkMapModified, Undo_End.
//
// Auto-renumber:
//   4. Before the parse loop: seed autoTarget = Map_GetNextAutoTarget(); census every
//      active brush's owner script_linkName/script_linkTo numbers (deduped) and
//      generate a fresh replacement number per existing number.
//   5. Per pasted entity (skipped when g_bRestoreBetween): target/targetname colliding
//      with an existing entity remap through a shared remap table to "auto%i"
//      (only values starting "auto" are rewritten); script_linkName/script_linkTo
//      numbers colliding with the census remap to their fresh replacements;
//      Entity_NeedsAutoExportId entities get a fresh "export" id.
//   6. Final pass strips a pasted entity whose target == its own targetname (both keys).
// ─────────────────────────────────────────────────────────────────────────────
// KIWI: when set, the import runs INSIDE the caller's open undo record instead of opening
// (and closing) its own "import buffer" one - the pasted entities / brushes are stamped with
// the caller's id, so a command that pastes several times and edits the result is ONE
// Ctrl+Z (kiwi_pasteplace.cpp).  The binary's behaviour is the flag's default, false.
bool g_kiwiImportInCallerUndo = false;

char *Map_ImportBuffer( const char **text, int version )
{
    Select_Deselect( 1 );
    if ( !g_kiwiImportInCallerUndo )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( "import buffer" );
    }
    g_qeglobals.d_parsed_brushes = 0;

    // ── Auto-target seed + script_link number census.
    //    Walk every ACTIVE brush's owner-entity and collect the script_linkName /
    //    script_linkTo numbers already used in the scene (deduped), then generate a
    //    fresh replacement number for each (smallest positive int not used in the
    //    scene and not already handed out).
    int autoTarget = Map_GetNextAutoTarget();

    int linkNums[MAX_LINKS];     // numbers already in the scene
    int newLinkNums[MAX_LINKS];  // fresh replacement per existing number
    int linkTotal = 0;
    LinkList_t links;            // Map_ParseLinkList parse buffer

    for ( selbrush_t *ab = active_brushes.next; ab != &active_brushes; ab = ab->next )
    {
        entity_s_def *ownerDef = (entity_s_def *)ab->owner->def;

        const char *ln = ValueForKey2( ownerDef, "script_linkName" );
        if ( *ln )
        {
            int num = (int)atol( ln );
            int k = 0;
            while ( k < linkTotal && linkNums[k] != num )
                ++k;
            if ( k >= linkTotal )
            {
                linkNums[linkTotal++] = num;
                iassert(linkTotal < MAX_LINKS);   // map.cpp:1006
            }
        }

        const char *lt = ValueForKey2( ownerDef, "script_linkTo" );
        if ( *lt )
        {
            Map_ParseLinkList( &links, lt );
            iassert(links.size <= MAX_LINKS);   // map.cpp:1015
            for ( int i = 0; i < links.size; ++i )
            {
                int num = links.id[i];
                int k = 0;
                while ( k < linkTotal && linkNums[k] != num )
                    ++k;
                if ( k >= linkTotal )
                {
                    linkNums[linkTotal++] = num;
                    iassert(linkTotal < MAX_LINKS);   // map.cpp:1036
                }
            }
        }
    }

    for ( int j = 0; j < linkTotal; ++j )
    {
        int  newNum = 0;
        bool inExisting;
        do
        {
            for ( ;; )
            {
                ++newNum;
                inExisting = false;
                for ( int k = 0; k < linkTotal; ++k )
                    if ( linkNums[k] == newNum ) { inExisting = true; break; }
                bool handedOut = false;
                for ( int k = 0; k < j; ++k )
                    if ( newLinkNums[k] == newNum ) { handedOut = true; break; }
                if ( !handedOut )
                    break;
            }
        }
        while ( inExisting );
        iassert(newNum != 0);   // map.cpp:1067
        newLinkNums[j] = newNum;
    }

    // old target/targetname → fresh "auto%i" name, shared across all pasted entities
    // (the binary's CMapStringToString; only keyed lookup/insert, so its hash-bucket
    //  order is irrelevant.)
    std::map<std::string, std::string> targetRemap;

    g_qeglobals.d_num_entities = 0;

    for ( entity_s *ent = ParseEntity( text, version, 0, 0 ); ent; ent = ParseEntity( text, version, 0, 0 ) )
    {
        entity_s_def *eDef = (entity_s_def *)ent;
        undo_s       *u    = g_lastundo;

        // ── Undo-record stamping. Associates the parsed entity + its brush defs with
        //    the current undo record so Undo removes the paste.
        if ( u && !u->done && ent != (entity_s *)world_entity->def )
        {
            eDef->epairEdits = u->id;
            brush_t *sentinel = (brush_t *)&eDef->def;
            for ( brush_t *b = (brush_t *)eDef->brushes.prev; b != sentinel; b = b->onext )
                b->ownerPrev = (entity_s *)(intptr_t)u->id;
        }
        {
            brush_t *sentinel = (brush_t *)&eDef->def;
            for ( brush_t *b = (brush_t *)eDef->brushes.prev; b && b != sentinel; b = b->onext )
            {
                if ( u && !u->done )
                {
                    b->ownerPrev = (entity_s *)(intptr_t)u->id;
                    entity_s_def *owner = (entity_s_def *)b->owner;
                    if ( owner->eclass && *(int *)&owner->eclass->fixedsize )
                    {
                        owner->epairEdits = u->id;
                        u = g_lastundo;
                    }
                }
            }
        }

        if ( Entity_HasEpairMatch( ent, "classname", "worldspawn" ) )
        {
            // ── WORLDSPAWN MERGE. Move every brush DEF from the parsed worldspawn entity
            //    onto the live worldspawn DEF, instance + select each. The walk captures
            //    `next` BEFORE the relink (relink rewrites onext).
            //
            //    instance-vs-def: the brush DEF's owner must be the worldspawn DEF
            //    (world_entity->def), NOT the world_entity INSTANCE —
            //    Entity_LinkBrush links into a DEF's def-list and sets owner=that def, and
            //    Brush_AddToList(def, instance) then asserts def->owner == instance->def
            //    (brush.cpp:2386). Passing world_entity (the instance) sets owner=instance
            //    and trips that assert. The binary's inline relinks into
            //    world_entity->def->def with owner=the DEF.
            entity_s *worldDef = (entity_s *)world_entity->def;
            brush_t  *sentinel = (brush_t *)&eDef->def;
            brush_t  *b        = (brush_t *)eDef->brushes.prev;   // def-list first element
            while ( b != sentinel )
            {
                brush_t *next = b->onext;                        // capture before relink

                Entity_UnlinkBrush( b );                         // off the parsed worldspawn def-list
                Entity_LinkBrush( b, worldDef );                 // onto live worldspawn DEF (owner=DEF, refCount++, Brush_ApplyEntityColor)

                g_bScreenUpdates = false;
                Brush_BuildWindings( b, 1 );
                if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
                    SetupVertexSelection();
                ++b->version;
                selbrush_t *inst = Brush_AddToList( b, world_entity );   // instance (refCount->2; owner=world_entity)
                if ( inst->next || inst->prev )
                    Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                Brush_AddToList2( inst );                                // select it
                g_bScreenUpdates = true;

                b = next;
            }
            Entity_Free_R( ent );                               // drop the now-empty parsed worldspawn def
            continue;
        }

        // ── NON-WORLDSPAWN ENTITY. Instance it, splice the DEF into the `entities` list,
        //    then select every instance brush.
        entity_s *inst = Prefab_Init( (prefab_s *)&entityInsts, eDef, &active_brushes );

        // ── Auto-renumber: rewrite pasted target/targetname/
        //    script_link* ids that collide with entities already in the map, so a
        //    paste-on-top doesn't cross-wire the existing entities. Skipped for the
        //    between-maps clipboard restore (g_bRestoreBetween), exactly as the binary.
        if ( !g_bRestoreBetween )
        {
            // target: colliding with an existing entity's "target" → remapped "auto%i".
            // Only values that themselves start with "auto" are rewritten.
            std::string oldVal = ValueForKey2( eDef, "target" );
            if ( !oldVal.empty() )
            {
                entity_s *match = entities.next;
                while ( match != &entities && !Entity_HasEpairMatch( match, "target", oldVal.c_str() ) )
                    match = match->next;
                if ( match != &entities )
                {
                    std::string newName;
                    auto it = targetRemap.find( oldVal );
                    if ( it != targetRemap.end() )
                    {
                        newName = it->second;
                    }
                    else
                    {
                        newName = VA( 0, "auto%i", autoTarget );
                        ++autoTarget;
                        targetRemap[ oldVal ] = newName;
                    }
                    if ( !strncmp( oldVal.c_str(), "auto", 4 ) )
                        SetKeyValue( eDef, "target", newName.c_str() );
                }
            }

            // auto export id: script_struct-class entities get a fresh one.
            if ( Entity_NeedsAutoExportId( eDef ) )
                SetKeyValue( eDef, "export", Map_GetNextExportId( 0 ) );

            // targetname: same remap table as target (a target/targetname PAIR maps to
            // the same fresh name, keeping intra-paste references wired together).
            oldVal = ValueForKey2( eDef, "targetname" );
            if ( !oldVal.empty() )
            {
                entity_s *match = entities.next;
                while ( match != &entities && !Entity_HasEpairMatch( match, "targetname", oldVal.c_str() ) )
                    match = match->next;
                if ( match != &entities )
                {
                    std::string newName;
                    auto it = targetRemap.find( oldVal );
                    if ( it != targetRemap.end() )
                    {
                        newName = it->second;
                    }
                    else
                    {
                        newName = VA( 0, "auto%i", autoTarget );
                        ++autoTarget;
                        targetRemap[ oldVal ] = newName;
                    }
                    if ( !strncmp( oldVal.c_str(), "auto", 4 ) )
                        SetKeyValue( eDef, "targetname", newName.c_str() );
                    else
                        SetKeyValue( eDef, "targetname", oldVal.c_str() );   // the binary re-sets the original
                }
            }

            // script_linkName: a number already used in the scene → its fresh replacement.
            const char *ln = ValueForKey2( eDef, "script_linkName" );
            if ( *ln )
            {
                int num = (int)atol( ln );
                int k = 0;
                while ( k < linkTotal && linkNums[k] != num )
                    ++k;
                if ( k < linkTotal )
                    SetKeyValue( eDef, "script_linkName", VA( 0, "%i", newLinkNums[k] ) );
            }

            // script_linkTo: renumber each colliding id in the list; non-colliding ids
            // pass through unchanged. Rebuilt as "%i %i ... " like the binary.
            const char *lt = ValueForKey2( eDef, "script_linkTo" );
            if ( *lt )
            {
                Map_ParseLinkList( &links, lt );
                iassert(links.size <= MAX_LINKS);   // map.cpp:1192
                char linkBuf[1024];
                linkBuf[0] = 0;
                for ( int i = 0; i < links.size; ++i )
                {
                    int num = links.id[i];
                    int k = 0;
                    while ( k < linkTotal && linkNums[k] != num )
                        ++k;
                    strcat( linkBuf, va( "%i ", k < linkTotal ? newLinkNums[k] : num ) );
                }
                SetKeyValue( eDef, "script_linkTo", linkBuf );
            }
        }

        // Splice the DEF onto the `entities` list (TAIL insert).
        eDef->next            = &entities;
        eDef->prev            = entities.prev;
        entities.prev->next   = ent;
        entities.prev         = ent;
        ++g_qeglobals.d_num_entities;

        // Select every instance brush of this pasted entity.
        for ( selbrush_t *ib = inst->brushes.ownerNext; ib != &inst->brushes; ib = ib->ownerNext )
            Select_Brush( ib, 1, 0, 0 );
    }

    // ── Final pass A: instance the 3D model for prefab insts (classtype & 0x10).
    for ( entity_s *entInst = entityInsts.next; entInst != &entityInsts; entInst = entInst->next )
    {
        iassert( entInst );   // map.cpp:1242
        entity_s_def *iDef = (entity_s_def *)entInst->def;
        iassert( entInst->def );   // map.cpp:1243
        iassert( entInst->def->eclass );   // map.cpp:1244
        if ( ( iDef->eclass->classtype & 0x10 ) != 0 )
        {
            entity_brush_s *fb = entInst->brushes.ownerNext;
            if ( fb != &entInst->brushes )
                Model_SetModel( fb, (const float *)&world_orient_matrix );
        }
    }

    // ── Final pass B: rebuild windings on the freshly-selected brushes, then strip
    //    self-loops: a pasted entity whose "target" now equals its
    //    own "targetname" (the renumber mapped both through the same table) gets BOTH
    //    keys removed — DeleteKey doesn't fire the Checkkey_* revalidation, so the
    //    binary calls them explicitly after each delete.
    for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
    {
        brush_t *def = sb->def;
        Brush_RebuildAfterEdit( def );

        entity_s_def *ownerDef = (entity_s_def *)sb->owner->def;
        if ( !Entity_HasEpairMatch( (entity_s *)ownerDef, "classname", "worldspawn" ) )
        {
            // key lookup case-insensitive, VALUE compare case-sensitive;
            // both default to "" — an entity with neither key also compares equal,
            // making the deletes no-ops (as in the binary).
            const char *tgt = ValueForKey2( ownerDef, "target" );
            const char *tnm = ValueForKey2( ownerDef, "targetname" );
            if ( !strcmp( tgt, tnm ) )
            {
                DeleteKey( &ownerDef->epairs, "targetname" );
                Checkkey_Model( ownerDef, "targetname" );
                Checkkey_Color( ownerDef, "targetname" );
                DeleteKey( &ownerDef->epairs, "target" );
                Checkkey_Model( ownerDef, "target" );
                Checkkey_Color( ownerDef, "target" );
            }
        }
    }

    g_nUpdateBits = -1;
    modified      = 1;
    if ( !g_kiwiImportInCallerUndo )
        Undo_End();
    return nullptr;
}
