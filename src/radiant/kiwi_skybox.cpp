// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_skybox.cpp — KIWI-UX (ROUND AZ, ITEM 3): THE SKY TAB.
// ═════════════════════════════════════════════════════════════════════════════════════
// Read kiwi_skybox.h first.  It carries the whole design — what marks a material as sky
// (D-AZ-A), why the see-through treatment is view-dependent and what it costs (D-AZ-B),
// why the thumbnails add no GPU object (D-AZ-C), why there are two verbs (D-AZ-D), and
// why picking reuses the pref the binary already has (D-AZ-E).  This file is the
// mechanical half.
// ═════════════════════════════════════════════════════════════════════════════════════
#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s (compile fix: the Ed_Camera extern
                                    // below and the see-through camera test use it;
                                    // every kiwi file that declares Ed_Camera pairs
                                    // it with this include — kiwi_camera.cpp:25)
#include "prefs.h"                  // prefData_t / g_PrefsDlg (the SkyBrushOff checkbox)
#include <windows.h>                // GetTickCount (the union-box refresh timer)
#include <imgui/imgui.h>

#include "kiwi_skybox.h"
#include "kiwi_camera.h"            // KIWI-UX (ROUND BB): KiwiCam_LookAt — the ORBIT PIVOT,
                                    // which is the point the inside/outside test has to
                                    // read.  See KiwiSky_SeeThroughFace.
#include "kiwi_command.h"
#include "kiwi_str.h"                // KIWI-UX (CLEANUP, C-66): KiwiStr_ContainsNoCase
#include "kiwi_texcache.h"           // KIWI-UX (CLEANUP, C-65): the by-name texture cache
#include "kiwi_texgrave.h"           // deferred release (the face combo)
#include "kiwi_windows.h"
#include "kiwi_walkcache.h"          // KiwiWalkCache_Epoch — the union box's change signal
#include "radiant_registry.h"       // Radiant_ProfileGetInt / SetInt

// Include order mirrors texwnd.cpp:29-30, the one other radiant TU that walks a
// Material's textureTable down to its GfxImage.
#include <gfx_d3d/r_material.h>     // Material / MaterialTextureDef / textureTable
#include <gfx_d3d/r_gfx.h>          // GfxImage / GfxTexture / MAPTYPE_2D / MAPTYPE_CUBE
// ── KIWI-UX (ROUND BA): the cubemap tile needs the device ───────────────────────────
// A sky material's colormap is a CUBEMAP (the decode is on CubeFaceThumb below), so the
// tile cannot be the engine's own texture pointer and one face has to be copied out.
// Same two headers kiwi_entthumb.cpp:33-35 takes for the identical job.
#include <d3d9.h>
#include <gfx_d3d/r_init.h>         // dx (dx.device)

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

// ── ported / cross-file entry points (each verified against its definition) ─────────
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118   int Sys_Printf(const char*,...)
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773  int g_nUpdateBits
extern camera_s   *Ed_Camera();                                              // camwnd.cpp:161    camera_s *Ed_Camera()
extern brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );   // brush.cpp:465     brush_t *Brush_Alloc(const void*,eclass_t*)
extern void        Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:510  void Brush_Create(float*,float*,brush_t*,eclass_t*)
extern void        Brush_BuildWindings( brush_t *def, int bFull );           // brush.cpp:1434    void Brush_BuildWindings(brush_t*,int)
extern void        Select_Deselect( int a1 );                                // select.cpp:1444   void Select_Deselect(int)
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358  bool Radiant_RegisterCommand(const char*,byte,byte,int)
// xywnd.cpp:1595 — the KIWI forwarder for the static Ed_EnsureCurrentMaterial, the same
// one kiwi_extrude.cpp / kiwi_primitive.cpp / kiwi_entbrowser.cpp seed a new brush from.
extern void        Ed_EnsureCurrentMaterial_Kiwi();                          // xywnd.cpp:1605    void Ed_EnsureCurrentMaterial_Kiwi()
// kiwi_extrude.cpp:2328 — the ported land triple (Entity_LinkBrush -> Brush_AddToList ->
// Brush_AddToList2), exported so nothing re-spells it.
extern selbrush_t *KiwiExtrude_LandDef( brush_t *def );                      // kiwi_extrude.cpp:2360  selbrush_t *KiwiExtrude_LandDef(brush_t*)
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp:796   ImGuiID ImGuiShell_DockRoot()
// ── KIWI-UX (ROUND BB, ITEM 2): the shell lands in a func_group named "SkyBox" ──────
// USER DIRECTIVE, verbatim: *"yeah so just add it to a group"*.  This is round W's
// create-only group path (kiwi_outliner.cpp:1609-1676), reused rather than re-derived:
// the same five entry points, in the same order, under the same merge-trap rule.
extern eclass_t   *Eclass_ForName( int hasBrushes, const char *name );       // eclass.cpp:1096   eclass_t *Eclass_ForName(int,const char*)
extern entity_s   *Entity_Create( eclass_t *eclass );                        // entity.cpp:1629   entity_s *Entity_Create(eclass_t*)
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:212  void SetKeyValue(entity_s_def*,const char*,const char*)
extern void        Undo_SetIdForEntity( entity_s_def *ent );                 // undo.cpp:663      void Undo_SetIdForEntity(entity_s_def*)
// KIWI-UX (CLEANUP, C-61): the two the targetname uniquifier needs.
extern entity_s    entities;                                                 // entity.cpp 0x23F17A0 — entity-DEF list sentinel
extern bool        Entity_HasEpairMatch( entity_s *e, const char *key, const char *val ); // entity.cpp:520  bool Entity_HasEpairMatch(entity_s*,const char*,const char*)
// ── the texture-window accessors (D-AZ-C / D-AZ-D) ─────────────────────────────────
// texwnd_s is TU-local, so the enumeration and the MaterialDef builder come through
// accessors beside TexWnd_GetMaterialListHead (texwnd.cpp:101), which is the pattern that
// file already uses for every out-of-TU reader.
extern int         TexWnd_MaterialCount();                                   // texwnd.cpp:2562   int TexWnd_MaterialCount()
extern qtexture_s *TexWnd_MaterialAt( int idx );                             // texwnd.cpp:2564   qtexture_s *TexWnd_MaterialAt(int)
extern void        TexWnd_MaterialDefFor( qtexture_s *q, MaterialDef *out );  // texwnd.cpp:2564   void TexWnd_MaterialDefFor(qtexture_s*,MaterialDef*)
extern void        TexWnd_ApplyMaterialAtIndex( int idx );                   // texwnd.cpp:1263   void TexWnd_ApplyMaterialAtIndex(int)
extern qtexture_s *Texture_GetHandle( const char *name );                    // texwnd.cpp:314    qtexture_s *Texture_GetHandle(const char*)
extern void        Prefs_SavePrefs( prefData_t *p );                         // prefs.cpp:216     void Prefs_SavePrefs(prefData_t*)
// undo.cpp — the KIWI §4 bracket (kiwi_command.h:1155-1160).  `operation` MUST be a
// string literal; the record stores the pointer.
// (KiwiCmd_UndoBegin / KiwiCmd_UndoCommit come from kiwi_command.h, already included.)

// The two sentinel lists.  Declared exactly as texwnd.cpp:520-521 / kiwi_pick.cpp:35 do.
extern selbrush_t  active_brushes;                                           // map.cpp (0x23F189C)
extern selbrush_t  selected_brushes;                                         // map.cpp (0x23F1864)

namespace
{
    // ── constants ───────────────────────────────────────────────────────────────────
    // SURF_SKY.  universal/surfaceflags.h:57 is the infoParms row `{ "sky", 0, 4, 0x800,
    // 0 }`, cod4map.h:408 is `#define SURF_SKY 0x00000004`, filters.cpp:130 is the same
    // bit in the filter name table, and camwnd.cpp:457-459 seeds `dword_181F51C = 4` with
    // it.  Spelled once, here.
    const int KSKY_SURF_SKY = 4;

    // Tile geometry.  Wider than the entity browser's 84x64 (kiwi_entbrowser.cpp:82-83)
    // because a sky tile shows a SQUARE colormap crop and a long material name, where an
    // entity tile shows a wide isometric box and a short classname.
    const float KSKY_TILE_W   = 104.0f;
    const float KSKY_THUMB    = 88.0f;
    const float KSKY_LABEL_H  = 30.0f;

    // ── KIWI-UX (CLEANUP, C-73): window layout, named file-locally ──────────────────
    // Same convention kiwi_cmdoptions.cpp's KOPT_WIDTH / KOPT_INSET / KOPT_TOPFRAC
    // follows: a widget size that appears in the draw is a named constant, not a bare
    // literal at the call site.
    const float KSKY_SEARCH_W   = 180.0f;   // the "search sky materials" field
    const float KSKY_COMBO_W    =  70.0f;   // the face combo AND the margin/thickness
                                            // inputs — they line up on one row, so one
                                            // number keeps them lined up
    const float KSKY_FOOTER_PAD =  12.0f;   // slack under the two footer rows, so the
                                            // scrolling grid child never eats the last

    // ── KIWI-UX (CLEANUP, C-73): the editor sky colour, spelled ONCE in this file ───
    // The placeholder plate is the same blue the camera paints a sky face with.  The
    // AUTHORITY is Cam_EditorMaterialColor's own table — camwnd.cpp:627,
    // `{ "sky", 0.45f, 0.62f, 0.92f }` — which is a file-local static inside a PORTED
    // translation unit, so there is no named constant to reach without adding an include
    // edge into camwnd.cpp.  It is therefore restated here, converted once and checked:
    // 0.45*255 = 114.75 -> 115, 0.62*255 = 158.1 -> 158, 0.92*255 = 234.6 -> 235.
    const ImU32 KSKY_PLATE_COL = IM_COL32( 115, 158, 235, 255 );

    // ROUND AX's budget lesson, applied to a second browser: registering a material
    // loads its images, which is the expensive call, so at most this many per frame and
    // only for tiles that are actually on screen.
    const int KSKY_REG_PER_FRAME = 1;

    // How stale the sky-brush union box is allowed to get.  KiwiSky_SeeThroughFace is
    // called PER FACE, so the box cannot be rebuilt on demand; it is rebuilt at most
    // every KSKY_BOUNDS_MS and the per-face cost is a `GetTickCount` compare plus six
    // float compares.  250 ms is imperceptible for "did I just fly inside the shell"
    // and it is two orders of magnitude cheaper than a per-frame walk of every brush.
    const unsigned KSKY_BOUNDS_MS = 250;

    // KIWI-UX (ROUND BC, ITEM 3): the depth tie-break in KiwiSky_SeeThroughFace.  Half
    // a unit — smaller than anything the grid snaps to, large enough that a wall the
    // pivot is parked exactly on cannot alternate between film and texture per frame.
    const float KSKY_FILM_EPS = 0.5f;

    // Shell defaults, in world units.  512 clears the geometry by a comfortable margin
    // without pushing the sky so far out that the map is a dot inside it; 64 is a
    // conventional sky-brush thickness and is a multiple of every grid size the editor
    // offers, so the shell lands on-grid whatever the user is working at.
    const float KSKY_DEF_MARGIN = 512.0f;
    const float KSKY_DEF_THICK  = 64.0f;
    const float KSKY_MIN_HALF   = 256.0f;    // an empty map still gets a real box
    // The engine world bound (brush.cpp:1470-1471 seeds its AABB at ±131072).  The shell
    // is clamped inside it because Brush_Create Com_Error()s on a degenerate box and a
    // brush outside the world is not compilable geometry.
    const float KSKY_WORLD_BOUND = 131072.0f;

    // ── KIWI-UX (ROUND BA) — the cube-face tile ─────────────────────────────────────
    // Edge length of the copied-out tile, in texels.  128 is the smallest power of two
    // that still exceeds KSKY_THUMB (88) at 1:1, so the tile never magnifies; the source
    // faces are 256-1024 square, so this is always a DOWNsample.
    const int KSKY_TILE_PX = 128;
    // Which cube face a tile shows.  D3DCUBEMAP_FACE_POSITIVE_X = 0 ... NEGATIVE_Z = 5.
    const int KSKY_FACE_COUNT   = 6;
    const char *const KSKY_FACE_NAMES[KSKY_FACE_COUNT] =
        { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };

    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BB, ITEM 3) — "THE PREVIEWS ARE SIDEWAYS", AND WHY THEY WERE
    // ═══════════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: *"the previews are sideways(+Y default I think)"*.  Correct
    // on the symptom; one entry off on the fix, and the difference is now MEASURED rather
    // than reasoned about.
    //
    // D-BA6 said which face carries the horizon *"lives in a compiled shader"* and made it
    // a UI choice.  THAT WAS ANSWERABLE ALL ALONG — not from the shader, but from the ART.
    // main/images/*_ft.iwi is a plain DXT1 cubemap with a 28-byte header and six faces in
    // D3D face order (Image_CubemapFace, r_image_load_common.cpp:7-11, is the identity, and
    // r_image.cpp:328-351 uploads face i to D3D face i).  Decoding aftermath_ft, sp_bog_ft,
    // fallujah_ft and chechnya_ft and LOOKING at the six faces settles it, identically for
    // all four:
    //
    //   face 0 (+X)  horizon runs VERTICALLY, sky on the LEFT      -> world up = image LEFT
    //   face 1 (-X)  horizon runs VERTICALLY, sky on the RIGHT     -> world up = image RIGHT
    //   face 2 (+Y)  horizon horizontal, ground ABOVE sky          -> world up = image DOWN
    //   face 3 (-Y)  horizon horizontal, sky above ground          -> UPRIGHT
    //   face 4 (+Z)  the sun / zenith cap                          -> no canonical up
    //   face 5 (-Z)  the flat ground disc / nadir cap              -> no canonical up
    //
    // Read that table backwards and the engine's convention falls out: the sky cubemap is
    // sampled by the RAW WORLD DIRECTION (x east, y north, z UP) with no swizzle, so world
    // +Z lands on D3D's +Z FACE — the zenith cap — and the four horizon faces inherit D3D's
    // own per-face (sc,tc) basis, which is written for a Y-up world.  That is exactly the
    // 90/90/180/0 pattern above, and it is a property of the ENGINE, not of an art set,
    // which is why four unrelated skies agree to the quarter-turn.
    //
    // TWO CONSEQUENCES, and the round ships both:
    //   * THE DEFAULT IS FACE 3 (-Y) — the one face that needs no rotation at all.  The
    //     user's "+Y" is the same axis 180 degrees out (face 2 is the upside-down twin),
    //     which is what a glance at a hazy horizon strip would give you.
    //   * EVERY OTHER FACE IS ROTATED INTO UPRIGHT IN THE COPY.  The combo exists so the
    //     browser can be pointed at a different face; showing that face sideways would be
    //     the same bug one click further along.  D3DXLoadSurfaceFromSurface preserves the
    //     stored orientation (it is a filtered resample, not a reorient), so the turn is
    //     applied by hand in the copy's own lock pass — which this file already opens to
    //     bake alpha, so it costs one 64 KB scratch buffer per material and no extra pass.
    // Quarter-turns CLOCKWISE to bring world-up to the top of the tile, per face:
    const int KSKY_FACE_ROT[KSKY_FACE_COUNT] = { 1, 3, 2, 0, 0, 0 };
    // The default face, spelled once.  See the table above: 3 == "-Y" == already upright.
    const int KSKY_FACE_DEFAULT = 3;

    // ── KIWI-UX (ROUND BA) — the flat-sky rescue (see FlattenRescue) ────────────────
    // An axis of the sky union box counts as NOT ENCLOSING when the sky's extent along it
    // is under this fraction of the MAP's extent along the same axis.  A sky CEILING over
    // a 8192-wide map is 8192 x 8192 x 64: two axes enclose, one does not.
    const float KSKY_FLAT_RATIO = 0.5f;

    const char *KSKY_SECTION   = "KiwiSky";
    const char *KSKY_SEE_ENTRY = "SeeThrough";
    const char *KSKY_MRG_ENTRY = "ShellMargin";
    const char *KSKY_THK_ENTRY = "ShellThickness";
    const char *KSKY_FACE_ENTRY = "TileFace";

    // ── persisted state ─────────────────────────────────────────────────────────────
    // KIWI-UX (CLEANUP, C-70): a plain bool.  This was `int s_seeThrough = -1` with a
    // "-1 = not read from the profile yet" comment, but nothing ever tested for -1 —
    // `s_prefsRead` is the loaded latch and every read below runs after ReadPrefs().
    bool  s_seeThrough = true;
    float s_margin     = KSKY_DEF_MARGIN;
    float s_thick      = KSKY_DEF_THICK;
    int   s_face       = KSKY_FACE_DEFAULT;  // ROUND BA: which cube face the tiles show
                                             // ROUND BB: ...and the default is the UPRIGHT
                                             // one, measured from the shipped art above.
    bool  s_prefsRead  = false;

    // ── live UI state ───────────────────────────────────────────────────────────────
    // ── KIWI-UX (CLEANUP, C-56): THE SELECTION IS A NAME, NOT AN INDEX ─────────────
    // This used to be `int s_selIndex` — an index into texwnd's SORTED array — while
    // GatherSky's own comment (:581-583) states that the registry "grows when a map
    // loads and when a material is first referenced".  Pick a sky material, load a map
    // (or trigger any lazy registration that re-sorts), press Apply, and a DIFFERENT
    // material was applied, silently: a range check cannot detect a re-sort.  The name
    // is stable and is already what the thumbnail cache keys on (:443); the index is
    // re-resolved at every use through SelectedMaterialIndex() below.
    char  s_selName[128] = { 0 };            // "" = nothing picked
    char  s_search[64] = { 0 };
    int   s_regBudget  = 0;                  // registrations left this frame

    // ── KIWI-UX (ROUND BA): the cube-face tile cache ────────────────────────────────
    // One MANAGED 2D texture per sky material, keyed by material NAME (the qtexture_s*
    // is stable but the name is what survives a material-list rebuild).  MANAGED, not
    // DEFAULT: the round-AX rule is that every DEFAULT-pool object has to be walked into
    // the reset list by hand, and a MANAGED texture survives Reset by the D3D9 contract.
    // It is released from KiwiSky_ReleaseForReset anyway, for exactly the reason
    // radiant_rtt.cpp:190-198 gives for the entity thumbnails: one already registered
    // teardown function is a device-reset story that cannot be forgotten, and no
    // ImTextureID may outlive ImGuiShell_InvalidateDeviceObjects.
    // KIWI-UX (CLEANUP, C-65): the entry struct, the map and the release loop are
    // kiwiTexEntry_t / kiwiTexCache_t / KiwiTexCache_ReleaseAll (kiwi_texcache.h)
    // — kiwi_entthumb.cpp carried a character-identical copy of all three.  The
    // cube-face BUILD below stays here.
    kiwiTexCache_t s_thumbs;

    // ── the cached sky-brush union box (D-AZ-B) ─────────────────────────────────────
    bool     s_boundsHave  = false;
    bool     s_boundsAny   = false;          // false = no sky brushes in the map at all
    float    s_boundsMin[3] = { 0.0f, 0.0f, 0.0f };
    float    s_boundsMax[3] = { 0.0f, 0.0f, 0.0f };
    unsigned s_boundsStamp = 0;
    unsigned s_boundsEpoch = 0;              // KiwiWalkCache_Epoch() at the last rebuild

    void ReadPrefs()
    {
        if ( s_prefsRead )
            return;
        s_prefsRead = true;
        // DEFAULT ON.  The complaint this item comes from is that the shell is in the
        // way, so the state that answers it is the one a user gets without asking.
        s_seeThrough = Radiant_ProfileGetInt( KSKY_SECTION, KSKY_SEE_ENTRY, 1 ) != 0;
        s_margin = (float)Radiant_ProfileGetInt( KSKY_SECTION, KSKY_MRG_ENTRY, (int)KSKY_DEF_MARGIN );
        s_thick  = (float)Radiant_ProfileGetInt( KSKY_SECTION, KSKY_THK_ENTRY, (int)KSKY_DEF_THICK );
        if ( !( s_margin >= 0.0f ) || s_margin > 65536.0f ) s_margin = KSKY_DEF_MARGIN;
        if ( !( s_thick  >= 1.0f ) || s_thick  > 8192.0f  ) s_thick  = KSKY_DEF_THICK;
        s_face = Radiant_ProfileGetInt( KSKY_SECTION, KSKY_FACE_ENTRY, KSKY_FACE_DEFAULT );
        if ( s_face < 0 || s_face >= KSKY_FACE_COUNT ) s_face = KSKY_FACE_DEFAULT;
    }

    // The leaf name, i.e. everything after the last '/'.  camwnd.cpp:2801 does exactly
    // this (`strrchr( mn, '/' )`, then `sl ? sl + 1 : mn`) and the fallback half of the
    // predicate has to agree with it or the tab and the camera would disagree about
    // which materials are sky.
    const char *LeafName( const char *name )
    {
        if ( !name )
            return nullptr;
        const char *sl = strrchr( name, '/' );
        return sl ? sl + 1 : name;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BA) — WHY EVERY TILE SAID "no map", AND THE COPY THAT FIXES IT
    // ═══════════════════════════════════════════════════════════════════════════════
    // USER REPORT (round AZ shipped, screenshot): every Sky tile showed the "no map"
    // placeholder.  Round AZ's D-AZ-C ends its guard list with *"`mapType != MAPTYPE_2D`
    // — a cubemap/volume is not an ImGui texture"* and treated that as a rare edge.  It
    // is not an edge.  IT IS EVERY SKY MATERIAL THERE IS, and the shipped assets say so
    // three separate ways:
    //
    //   1. THE MATERIAL.  main/materials/sky_aftermath, 116 bytes, decoded field by
    //      field: techSetName "sky", textureCount 1, constantCount 0, refStateBits
    //      { 0x08128812, 0x0D }, surfaceFlags 0x34 (SURF_SKY 0x04 is in it), and ONE
    //      textureTable entry — name "colorMap", semantic 2 (TS_COLOR_MAP), samplerState
    //      0xEB, image "aftermath_ft".  All 45 sky_* materials in main/materials have
    //      exactly this shape: one colorMap, image "<map>_ft".
    //   2. THE IMAGE.  main/images/aftermath_ft.iwi header: tag "IWi", version 6, format
    //      11 (DXT1), FLAGS 0xC6 — and 0x04 is IMG_FLAG_CUBEMAP (r_image.h:24-26).
    //      1024x1024, file size 6 x (1024*1024/2) + 28 = 3,145,756, i.e. SIX FACES.
    //      Image_Setup routes that flag to Image_CreateCubeTexture_PC
    //      (r_image.cpp:1406-1408), which writes `mapType = MAPTYPE_CUBE`
    //      (r_image.cpp:1194) and creates an IDirect3DCubeTexture9 in D3DPOOL_MANAGED.
    //      Spot-checked ac130_ft / fallujah_ft / chechnya_ft / sp_bog_ft: all 0xC6.
    //   3. THE ENGINE REQUIRES IT.  R_SetSkyImage (r_bsp_load_obj.cpp:1169-1181) looks up
    //      the entry whose nameHash is R_HashString("colorMap") and Com_Errors with
    //      *"colorMap '%s' for sky material '%s' is not a cubemap"* if its image is not
    //      MAPTYPE_CUBE.  A sky material with a 2D colormap is not a thing that loads.
    //
    // So the semantic==2 walk found the right entry every time and the LAST guard threw
    // it away every time.  There is also NO fallback inside the material: textureCount is
    // 1 — the table has nothing else in it to show.
    //
    // THE FIX IS A COPY, AND IT IS THE ONLY OPTION THAT IS NOT A LIE.  `AddImage` needs an
    // IDirect3DTexture9; the engine owns an IDirect3DCubeTexture9; ImGui's D3D9 backend
    // binds an ImTextureID as a 2D texture, so handing it the cube pointer would bind a
    // mismatched resource.  One face is copied into a small MANAGED 2D texture with
    // D3DXLoadSurfaceFromSurface (d3dx9 is already in this build — stdafx.h:33 includes
    // it and scripts/radiant/CMakeLists.txt:116 links ${D3DX_LIB}), which does the DXT1
    // decompress and the downsample in one call and works on the MANAGED, lockable
    // surfaces both sides are.
    //
    // SO THIS FILE OWNS TEXTURES — D-AZ-C's CUBE arm.  The rule it must not break is
    // round AX's ("every DEFAULT-pool object walked into the reset list by hand"), and
    // it does not — the pool is MANAGED (survives Reset by contract, exactly as
    // kiwi_entthumb.cpp:261-262 chose for the same reason), and the cache is released
    // from KiwiSky_ReleaseForReset, which RTT_ReleaseForReset calls beside the entity
    // thumbnails' one.  No DEFAULT-pool object is created here at all.
    // ═══════════════════════════════════════════════════════════════════════════════

    // Copy one face of `cube` into a fresh MANAGED KSKY_TILE_PX square.  Null on any D3D
    // failure, which the caller turns into a FAILED entry (the tile keeps its placeholder).
    IDirect3DTexture9 *CubeFaceThumb( IDirect3DCubeTexture9 *cube, int face )
    {
        if ( !cube || !dx.device || face < 0 || face >= KSKY_FACE_COUNT )
            return nullptr;

        IDirect3DSurface9 *src = nullptr;
        if ( cube->GetCubeMapSurface( (D3DCUBEMAP_FACES)face, 0, &src ) < 0 || !src )
            return nullptr;

        IDirect3DTexture9 *tex = nullptr;
        if ( dx.device->CreateTexture( KSKY_TILE_PX, KSKY_TILE_PX, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &tex, nullptr ) < 0 || !tex )
        {
            src->Release();
            return nullptr;
        }

        IDirect3DSurface9 *dst = nullptr;
        if ( tex->GetSurfaceLevel( 0, &dst ) < 0 || !dst )
        {
            tex->Release();
            src->Release();
            return nullptr;
        }

        const HRESULT hr = D3DXLoadSurfaceFromSurface( dst, nullptr, nullptr,
                                                       src, nullptr, nullptr,
                                                       D3DX_FILTER_LINEAR, 0 );
        dst->Release();
        src->Release();
        if ( hr < 0 )
        {
            tex->Release();
            return nullptr;
        }

        // FORCE THE ALPHA OPAQUE, which also retires D-AZ-C's accepted cosmetic edge
        // (*"a sky colormap that genuinely carries an alpha channel shows the tile
        // background through it"*).  DXT1 without the 1-bit-alpha flag decodes to 255
        // anyway, but a DXT3/DXT5 sky would not, and this file now owns a CPU-writable
        // copy so the entity browser's answer (kiwi_entthumb.cpp:292, `p | 0xFF000000u`)
        // is available at no cost.  A locked MANAGED texture is a system-memory write;
        // the driver re-uploads on next use.
        //
        // ── KIWI-UX (ROUND BB, ITEM 3): ...AND TURN THE FACE UPRIGHT IN THE SAME PASS.
        // The per-face quarter-turn and how it was measured are on KSKY_FACE_ROT above.
        // The scratch copy is taken ONLY when there is a turn to apply, so the default
        // face (3 / -Y, already upright) costs exactly what it cost in round BA.
        D3DLOCKED_RECT lr = {};
        if ( tex->LockRect( 0, &lr, nullptr, 0 ) >= 0 )
        {
            const int N   = KSKY_TILE_PX;
            const int rot = KSKY_FACE_ROT[face];
            // NAMED `scratch`, not `src`: the surface pointer above is still in scope.
            std::vector< unsigned > scratch;
            if ( rot )
            {
                scratch.resize( (size_t)N * N );
                for ( int y = 0; y < N; ++y )
                    memcpy( &scratch[(size_t)y * N],
                            (unsigned char *)lr.pBits + (size_t)y * lr.Pitch,
                            (size_t)N * 4 );
            }
            for ( int y = 0; y < N; ++y )
            {
                unsigned *row = (unsigned *)( (unsigned char *)lr.pBits + (size_t)y * lr.Pitch );
                for ( int x = 0; x < N; ++x )
                {
                    // dst(x,y) <- src(sx,sy).  1 = 90 CW: (y, N-1-x).  2 = 180:
                    // (N-1-x, N-1-y).  3 = 270 CW (= 90 CCW): (N-1-y, x).
                    unsigned p;
                    switch ( rot )
                    {
                    case 1:  p = scratch[(size_t)( N - 1 - x ) * N + y];             break;
                    case 2:  p = scratch[(size_t)( N - 1 - y ) * N + ( N - 1 - x )]; break;
                    case 3:  p = scratch[(size_t)x * N + ( N - 1 - y )];             break;
                    default: p = row[x];                                           break;
                    }
                    row[x] = p | 0xFF000000u;
                }
            }
            tex->UnlockRect( 0 );
        }
        return tex;
    }

    // ── the material's colormap, as an ImGui texture (D-AZ-C, amended by ROUND BA) ───
    // `mayBuild` is the per-frame budget: only an on-screen tile with budget left may pay
    // for a cube copy, the same gate the registration below uses.
    // Guards, each load-bearing:
    //   * `textureTable` null           — an unloaded / default material
    //   * `semantic != 2`               — TS_COLOR_MAP (r_image.h:51).  Skipping this and
    //                                     taking [0] gives the normalMap about half the
    //                                     time; the table is hash-sorted, which is the
    //                                     same trap texwnd.cpp:140-141 documents.
    //   * `delayLoadPixels`             — `GfxTexture` is a UNION (r_gfx.h:203-210) and
    //                                     before upload the live arm is `loadDef`, NOT a
    //                                     texture.  Handing that to ImGui is a crash.
    // The MAPTYPE_2D arm is unchanged from round AZ and still stores nothing: a material
    // whose colormap really is 2D (a "sky"-NAMED material that is not engine sky) hands
    // ImGui the engine's own pointer, re-read every frame.  Only the CUBE arm caches.
    IDirect3DTexture9 *TileTexture( qtexture_s *q, bool mayBuild, bool *outBuilt )
    {
        if ( outBuilt )
            *outBuilt = false;
        if ( !q )
            return nullptr;
        Material *mtl = q->next;
        if ( !mtl || !mtl->textureTable )
            return nullptr;

        GfxImage *cubeImg = nullptr;
        for ( int i = 0; i < (int)mtl->textureCount; ++i )
        {
            if ( mtl->textureTable[i].semantic != 2 )       // TS_COLOR_MAP
                continue;
            GfxImage *img = mtl->textureTable[i].u.image;
            if ( !img || img->delayLoadPixels )
                continue;
            if ( img->mapType == MAPTYPE_2D && img->texture.map )
                return img->texture.map;                    // the round-AZ arm, untouched
            if ( img->mapType == MAPTYPE_CUBE && img->texture.cubemap && !cubeImg )
                cubeImg = img;
        }
        if ( !cubeImg )
            return nullptr;

        // The cube arm.  One entry per material name; `failed` is resolved once so a
        // driver that refuses the copy costs one attempt, not one per frame.
        const std::string key( q->name ? q->name : "" );
        kiwiTexCache_t::iterator it = s_thumbs.find( key );
        if ( it != s_thumbs.end() )
            return it->second.tex;                          // null when `failed`
        if ( !mayBuild )
            return nullptr;

        kiwiTexEntry_t e;
        e.tex    = CubeFaceThumb( cubeImg->texture.cubemap, s_face );
        e.failed = ( e.tex == nullptr );
        s_thumbs[key] = e;
        if ( outBuilt )
            *outBuilt = true;
        if ( e.failed )
            Sys_Printf( "Sky: could not copy cube face %s of '%s' - keeping the placeholder.\n",
                        KSKY_FACE_NAMES[s_face], q->name ? q->name : "(unnamed)" );
        return e.tex;
    }

    // Drop every cached tile.  Used by the face combo (every tile is now the wrong face)
    // and by KiwiSky_ReleaseForReset.  Idempotent.
    // `deferred` is the FACE COMBO's arm: it runs during the ImGui UI build, so the
    // interfaces go to the graveyard rather than depending on where in the panel the combo
    // is drawn.  The RESET caller must NOT defer — it runs outside any frame and its whole
    // job is that nothing app-owned is alive when Reset() lands.
    void DropThumbs( bool deferred )
    {
        if ( !deferred )
        {
            KiwiTexCache_ReleaseAll( s_thumbs );      // KIWI-UX (CLEANUP, C-65)
            return;
        }
        for ( kiwiTexCache_t::iterator it = s_thumbs.begin(); it != s_thumbs.end(); ++it )
        {
            KiwiTexGrave_Release( it->second.tex );   // null-tolerant (the `failed` rows)
            it->second.tex = nullptr;
        }
        s_thumbs.clear();
    }

    bool MapBounds( float mins[3], float maxs[3] );      // defined below RebuildBounds

    // ── KIWI-UX (ROUND BA): THE FLAT-SKY RESCUE ─────────────────────────────────────
    // D-AZ-B's inside/outside test is `is the eye inside the union box of the sky
    // brushes`, and it silently assumed the sky brushes form a SHELL.  They very often do
    // not.  A sky CEILING — one flat slab over the map, which is what most CoD4 outdoor
    // maps actually ship — has a union box of, say, 8192 x 8192 x 64.  Nothing can be
    // inside 64 units of Z, so the eye is OUTSIDE on the Z axis from every position there
    // is, `SeeThroughFace` is true for ever, and the sky is film even while you are
    // standing under it in the level.  Round AZ's "step inside and it goes textured"
    // never fires for that map at all.
    //
    // THE RESCUE, and why it is the MAP's box and not an epsilon.  An axis whose sky
    // extent is under KSKY_FLAT_RATIO of the MAP's extent on the same axis is not
    // enclosing anything, so it cannot answer "am I in the map or looking at it" — and
    // on exactly those axes the question the mapper means is the MAP's own extent.  So
    // that axis's range is widened to cover the map's.  Consequences, checked case by
    // case:
    //   * a real six-brush SHELL — every axis encloses (the shell is built from the map
    //     bounds plus a margin, so sky extent > map extent), nothing is widened, round
    //     AZ's behaviour is bit-identical;
    //   * a sky CEILING — X and Y already enclose; Z widens to the map's Z, so standing
    //     in the level is INSIDE (textured) and orbiting above the map is OUTSIDE (film),
    //     which is the whole point of the feature;
    //   * a sky FLOOR or a single backdrop WALL — same, on its own axis;
    //   * NO sky brushes — `s_boundsAny` is false and this never runs.
    // The ratio is a ratio and not an absolute so it is scale-free: it says "this slab is
    // thin compared to the thing it is over", which is the actual property.
    void FlattenRescue()
    {
        if ( !s_boundsAny )
            return;
        float mn[3] = { 0.0f, 0.0f, 0.0f };
        float mx[3] = { 0.0f, 0.0f, 0.0f };
        if ( !MapBounds( mn, mx ) )
            return;
        for ( int k = 0; k < 3; ++k )
        {
            const float skyExt = s_boundsMax[k] - s_boundsMin[k];
            const float mapExt = mx[k] - mn[k];
            if ( mapExt <= 0.0f || skyExt >= KSKY_FLAT_RATIO * mapExt )
                continue;                        // this axis encloses; leave it alone
            if ( mn[k] < s_boundsMin[k] ) s_boundsMin[k] = mn[k];
            if ( mx[k] > s_boundsMax[k] ) s_boundsMax[k] = mx[k];
        }
    }

    // ── the sky-brush union box ─────────────────────────────────────────────────────
    // A brush is sky when its FACE 0's current-layer material is sky.  Walking both
    // sentinel lists, because a selected sky brush is still part of the shell.
    void RebuildBounds()
    {
        s_boundsAny = false;
        for ( int list = 0; list < 2; ++list )
        {
            selbrush_t *head = list ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                brush_t *def = b->def;
                if ( !KiwiSky_IsSkyBrush( def ) )
                    continue;
                for ( int k = 0; k < 3; ++k )
                {
                    if ( !s_boundsAny || def->mins[k] < s_boundsMin[k] ) s_boundsMin[k] = def->mins[k];
                    if ( !s_boundsAny || def->maxs[k] > s_boundsMax[k] ) s_boundsMax[k] = def->maxs[k];
                }
                s_boundsAny = true;
            }
        }
        FlattenRescue();                 // ROUND BA — see above
        s_boundsHave  = true;
        s_boundsStamp = ::GetTickCount();
        s_boundsEpoch = KiwiWalkCache_Epoch();
    }

    // The whole map's bounds, for the shell.  Deliberately UNCONDITIONAL over both lists
    // — KiwiFocus_SelectionBounds (kiwi_focus.h:82) falls back to the visible world only
    // when nothing is selected, and "wrap the map" must not mean "wrap the selection"
    // just because something happened to be selected when the button was pressed.
    // Hidden brushes ARE included: a hidden brush is still geometry the sky has to
    // enclose, and a shell that stops short of it is a leak the mapper would only find at
    // compile time.
    bool MapBounds( float mins[3], float maxs[3] )
    {
        bool any = false;
        for ( int list = 0; list < 2; ++list )
        {
            selbrush_t *head = list ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                brush_t *def = b->def;
                if ( !def )
                    continue;
                for ( int k = 0; k < 3; ++k )
                {
                    if ( !any || def->mins[k] < mins[k] ) mins[k] = def->mins[k];
                    if ( !any || def->maxs[k] > maxs[k] ) maxs[k] = def->maxs[k];
                }
                any = true;
            }
        }
        return any;
    }

    // ── the tab's material list ─────────────────────────────────────────────────────
    // Rebuilt every frame rather than cached: the registry grows when a map loads and
    // when a material is first referenced, and a sky list that is one map behind is
    // worse than a loop over a few thousand pointers with an int test in it.
    void GatherSky( std::vector<int> &out )
    {
        const int n = TexWnd_MaterialCount();
        for ( int i = 0; i < n; ++i )
        {
            qtexture_s *q = TexWnd_MaterialAt( i );
            if ( !q || !q->name )
                continue;
            if ( !KiwiSky_IsSkyMaterial( q ) )
                continue;
            // Case-insensitive substring, on the FULL name so a folder can be typed.
            // KIWI-UX (CLEANUP, C-66): this was hand-inlined here with a
            // `( *a | 32 ) == ( *b | 32 )` fold, which is not ASCII-correct —
            // it makes backslash and '|' compare equal, and these ARE paths.
            // KiwiStr_ContainsNoCase answers TRUE for an empty needle, which is
            // exactly what the `if ( s_search[0] )` guard used to hand-code.
            if ( !KiwiStr_ContainsNoCase( q->name, s_search ) )
                continue;
            out.push_back( i );
        }
    }

    // KIWI-UX (CLEANUP, C-56): resolve the picked NAME to a live index.  Returns -1
    // when nothing is picked OR when the picked material is no longer in the
    // registry; the callers tell those two apart so the console line is honest.  A
    // linear scan over a list the tab already walks to draw itself.
    int SelectedMaterialIndex()
    {
        if ( !s_selName[0] )
            return -1;
        const int n = TexWnd_MaterialCount();
        for ( int i = 0; i < n; ++i )
        {
            qtexture_s *q = TexWnd_MaterialAt( i );
            if ( q && q->name && _stricmp( q->name, s_selName ) == 0 )
                return i;
        }
        return -1;
    }

    // ── the verbs ───────────────────────────────────────────────────────────────────
    // APPLY.  One line of real work, deliberately (D-AZ-D): TexWnd_ApplyMaterialAtIndex
    // IS the texture-browser thumbnail click, so this inherits the ported
    // Brush_SetTexture apply — one undo record covering BOTH selected_brushes and
    // g_SelectedFaces (select.cpp:1815-1879) — and round AK's patch re-naturalize fence
    // (texwnd.cpp:1188).  A second apply written here would have been a second undo shape
    // and a second place to forget the fence.
    bool ApplyToSelection()
    {
        const int selIdx = SelectedMaterialIndex();      // KIWI-UX (CLEANUP, C-56)
        if ( selIdx < 0 )
        {
            if ( s_selName[0] )
                Sys_Printf( "Sky: \"%s\" is no longer in the material registry - pick a "
                            "sky material in the Sky tab again.\n", s_selName );
            else
                Sys_Printf( "Sky: pick a sky material in the Sky tab first.\n" );
            return true;
        }
        if ( selected_brushes.next == &selected_brushes )
        {
            Sys_Printf( "Sky: nothing selected - select the brushes that should become sky.\n" );
            return true;
        }
        qtexture_s *q = TexWnd_MaterialAt( selIdx );
        TexWnd_ApplyMaterialAtIndex( selIdx );
        KiwiSky_InvalidateBounds();      // the shell just changed shape
        Sys_Printf( "Sky: applied %s to the selection.\n",
                    ( q && q->name ) ? q->name : "(unnamed)" );
        g_nUpdateBits = -1;
        return true;
    }

    // One box of the shell.  Returns false and frees the def when the box is degenerate,
    // so a caller can build five walls and refuse the sixth rather than Com_Error out of
    // the middle of an open undo bracket.
    bool ShellBox( const float lo[3], const float hi[3], const MaterialDef *md )
    {
        float a[3], b[3];
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = lo[k];
            b[k] = hi[k];
            if ( a[k] < -KSKY_WORLD_BOUND ) a[k] = -KSKY_WORLD_BOUND;
            if ( b[k] >  KSKY_WORLD_BOUND ) b[k] =  KSKY_WORLD_BOUND;
            // Brush_Create Com_Error()s on a backwards or zero box (brush.cpp:507-513),
            // so the guard is here — the same argument kiwi_entbrowser.cpp:613-620 and
            // kiwi_primitive.cpp's AllocBoxDef both make.
            if ( b[k] - a[k] < 1.0f )
                return false;
        }
        brush_t *def = Brush_Alloc( md, nullptr );
        if ( !def )
            return false;
        Brush_Create( a, b, def, nullptr );      // (mins,maxs) — the DISASM-confirmed arg
                                                 // order; hex-rays prints the __fastcall
                                                 // args swapped (xywnd.cpp:1585-1586).
        Brush_BuildWindings( def, 1 );
        KiwiExtrude_LandDef( def );
        return true;
    }

    // CREATE SKYBOX SHELL.
    //
    // ── THE MATH, stated so it can be checked rather than trusted ───────────────────
    // Let [mn,mx] be the map bounds, M the margin and T the wall thickness.
    //     inner = [ mn - M      , mx + M       ]     the volume the sky encloses
    //     outer = [ inner - T   , inner + T    ]     the outside of the shell
    // and the six boxes tile the difference WITHOUT OVERLAP:
    //     floor  x[outer]  y[outer]  z[outer.lo , inner.lo]
    //     roof   x[outer]  y[outer]  z[inner.hi , outer.hi]
    //     -X     x[outer.lo, inner.lo]  y[outer]           z[inner]
    //     +X     x[inner.hi, outer.hi]  y[outer]           z[inner]
    //     -Y     x[inner]               y[outer.lo,inner.lo] z[inner]
    //     +Y     x[inner]               y[inner.hi,outer.hi] z[inner]
    // Floor and roof take the full OUTER footprint, the X walls take the full outer Y but
    // only the inner Z, and the Y walls take only the inner X and inner Z.  Each of the
    // twelve edge prisms and eight corner cubes is therefore claimed by exactly one box.
    // Overlapping boxes would be six coincident faces the compiler has to resolve and a
    // mapper has to notice; this tiles.
    //
    // KIWI-UX (CLEANUP, C-61) — A FREE `targetname` FOR THE SHELL'S func_group.
    // "SkyBox" if nothing holds it, else "SkyBox2", "SkyBox3", …  `targetname` is a
    // LINK key here (camwnd.cpp:4755 resolves targets by exact match), so a second
    // shell sharing the first one's name is a broken link, not a cosmetic duplicate.
    // Walks the entity-DEF list the same way Region_FindTargetEntity does.
    void SkyShellTargetName( char *out, size_t n )
    {
        for ( int suffix = 1; suffix <= 999; ++suffix )
        {
            if ( suffix == 1 )
                _snprintf( out, n, "SkyBox" );
            else
                _snprintf( out, n, "SkyBox%i", suffix );
            out[n - 1] = '\0';

            bool taken = false;
            for ( entity_s *cur = entities.next; cur && cur != &entities && !taken; cur = cur->next )
                taken = Entity_HasEpairMatch( cur, "targetname", out );
            if ( !taken )
                return;
        }
        // 999 shells in one map is not a case worth a second mechanism: the last
        // spelling stands, and the duplicate is then visible in the Outliner.
    }

    // SIX BRUSHES, NOT ONE HOLLOW SOLID, because a brush in this editor is CONVEX — a
    // hollow box IS six brushes, which is what the CSG hollow verb produces too.
    bool CreateShell()
    {
        ReadPrefs();
        const int selIdx = SelectedMaterialIndex();      // KIWI-UX (CLEANUP, C-56)
        if ( selIdx < 0 )
        {
            if ( s_selName[0] )
                Sys_Printf( "Sky: \"%s\" is no longer in the material registry - pick a "
                            "sky material in the Sky tab again.\n", s_selName );
            else
                Sys_Printf( "Sky: pick a sky material in the Sky tab first.\n" );
            return true;
        }
        qtexture_s *q = TexWnd_MaterialAt( selIdx );
        if ( !q || !q->name )
        {
            // KIWI-UX (CLEANUP, C-68): this returned true ("handled") without a line,
            // so the verb did nothing and said nothing.  Same early-out, now audible.
            Sys_Printf( "Sky: the picked sky material has no entry in the registry any "
                        "more - pick one in the Sky tab again.\n" );
            return true;
        }

        float mn[3] = { 0.0f, 0.0f, 0.0f };
        float mx[3] = { 0.0f, 0.0f, 0.0f };
        if ( !MapBounds( mn, mx ) )
        {
            // An empty map still gets a real box, centred on the origin, so the verb is
            // usable as a STARTING point and not only as a finishing one.
            for ( int k = 0; k < 3; ++k ) { mn[k] = -KSKY_MIN_HALF; mx[k] = KSKY_MIN_HALF; }
        }

        // The material template.  TexWnd_MaterialDefFor is the texture browser's own
        // clicked-material builder (texwnd.cpp:674), so the shell's faces get exactly the
        // MaterialDef a thumbnail click would have applied — including the texdef scale
        // derived from the material's auto-scale and the layer sample size.  Building one
        // by hand here would have been a second, quietly divergent spelling of that.
        Ed_EnsureCurrentMaterial_Kiwi();
        MaterialDef md;
        memset( &md, 0, sizeof( md ) );
        TexWnd_MaterialDefFor( q, &md );

        const float M = s_margin;
        const float T = s_thick;
        float ilo[3], ihi[3], olo[3], ohi[3];
        for ( int k = 0; k < 3; ++k )
        {
            ilo[k] = mn[k] - M;
            ihi[k] = mx[k] + M;
            olo[k] = ilo[k] - T;
            ohi[k] = ihi[k] + T;
        }

        // §23's undo ordering: deselect FIRST so the bracket clones nothing that is about
        // to be dropped, then open it, then create (kiwi_primitive.cpp:1169-1177).
        Select_Deselect( 1 );
        KiwiCmd_UndoBegin( "create skybox shell" );

        int built = 0;
        float lo[3], hi[3];
        // floor
        lo[0] = olo[0]; lo[1] = olo[1]; lo[2] = olo[2];
        hi[0] = ohi[0]; hi[1] = ohi[1]; hi[2] = ilo[2];
        built += ShellBox( lo, hi, &md ) ? 1 : 0;
        // roof
        lo[0] = olo[0]; lo[1] = olo[1]; lo[2] = ihi[2];
        hi[0] = ohi[0]; hi[1] = ohi[1]; hi[2] = ohi[2];
        built += ShellBox( lo, hi, &md ) ? 1 : 0;
        // -X
        lo[0] = olo[0]; lo[1] = olo[1]; lo[2] = ilo[2];
        hi[0] = ilo[0]; hi[1] = ohi[1]; hi[2] = ihi[2];
        built += ShellBox( lo, hi, &md ) ? 1 : 0;
        // +X
        lo[0] = ihi[0]; lo[1] = olo[1]; lo[2] = ilo[2];
        hi[0] = ohi[0]; hi[1] = ohi[1]; hi[2] = ihi[2];
        built += ShellBox( lo, hi, &md ) ? 1 : 0;
        // -Y
        lo[0] = ilo[0]; lo[1] = olo[1]; lo[2] = ilo[2];
        hi[0] = ihi[0]; hi[1] = ilo[1]; hi[2] = ihi[2];
        built += ShellBox( lo, hi, &md ) ? 1 : 0;
        // +Y
        lo[0] = ilo[0]; lo[1] = ihi[1]; lo[2] = ilo[2];
        hi[0] = ihi[0]; hi[1] = ohi[1]; hi[2] = ihi[2];
        built += ShellBox( lo, hi, &md ) ? 1 : 0;

        // ═══════════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND BB, ITEM 2) — THE SHELL LANDS IN A func_group NAMED "SkyBox"
        // ═══════════════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: *"yeah so just add it to a group"*.  Six loose
        // brushes at the top of the Outliner is six rows that are never edited and
        // always in the way; one folder is one row, and it is also the answer to the
        // cost round BB's item 1a accepts (see KiwiSky_SeeThroughFace) — a named group
        // is a one-click HIDE when the shell is between you and the map.
        //
        // THIS IS ROUND W'S CREATE-ONLY PATH, REUSED VERBATIM (kiwi_outliner.cpp:1609-
        // 1676), and the three rules it derived all hold here BY CONSTRUCTION rather
        // than by test:
        //   * THE MERGE TRAP.  Entity_Create has three arms and only the third
        //     ALLOCATES; a selection containing any NON-world brush makes it merge into
        //     that brush's existing entity instead (entity.cpp:1669-1710), which
        //     Undo_SetIdForEntity would then stamp — one Ctrl+Z deleting a group the
        //     user never made.  Here the selection is exactly the brushes LandDef just
        //     created (Select_Deselect(1) ran before the bracket and KiwiExtrude_LandDef
        //     lands each def owner=world_entity, SELECTED — kiwi_extrude.cpp:544-549),
        //     so the create arm is the only reachable one.
        //   * ONE UNDO RECORD PER GESTURE (§4).  Entity_Create runs INSIDE the bracket
        //     KiwiCmd_UndoBegin already opened, and Undo_SetIdForEntity is the same tail
        //     pmesh.cpp:7396 and kiwi_outliner.cpp:1655 attach — so "create skybox
        //     shell" is still exactly one Ctrl+Z, now covering the group as well.
        //   * NAME VIA A targetname EPAIR.  A func_group has no other name; that is
        //     what the Outliner reads back (kiwi_outliner.cpp:354-356).  "SkyBox" is
        //     the directive's own spelling.  KIWI-UX (CLEANUP, C-61): it IS uniquified
        //     now.  `targetname` is a LINK key in this tree — camwnd.cpp:4755 resolves
        //     targets by exact match and mainfrm.cpp's Select_AllByKeyValue selects
        //     every holder — so a duplicate is not a visible duplicate row, it is a
        //     broken entity link.  Second shell in a map gets "SkyBox2", and so on;
        //     the console line below names whichever one was taken.
        // Skipped when nothing was built: an empty group would be a row that owns
        // nothing, and a bracket around a no-op is what kiwi_command.h forbids.
        char groupName[64];
        SkyShellTargetName( groupName, sizeof( groupName ) );
        if ( built > 0 )
        {
            eclass_t *ec = Eclass_ForName( 0, "func_group" );      // eclass.cpp:1138
            if ( !ec )
                Sys_Printf( "Sky: no func_group entity definition - the shell brushes "
                            "are in worldspawn.\n" );
            else if ( entity_s *inst = Entity_Create( ec ) )
            {
                if ( entity_s_def *def = (entity_s_def *)inst->def )
                {
                    Undo_SetIdForEntity( def );
                    SetKeyValue( def, "targetname", groupName );
                }
            }
        }

        KiwiCmd_UndoCommit();
        KiwiSky_InvalidateBounds();
        g_nUpdateBits = -1;

        if ( built == 6 )
            Sys_Printf( "Sky: skybox shell created (6 brushes in the func_group "
                        "\"%s\", %s, margin %.0f, thickness %.0f).\n",
                        groupName, q->name, M, T );
        else
            Sys_Printf( "Sky: skybox shell created with %d of 6 brushes - the map bounds "
                        "plus margin would have pushed the rest outside the world "
                        "(+/-%.0f).  Reduce the margin.\n", built, KSKY_WORLD_BOUND );
        return true;
    }

    // ── one tile ────────────────────────────────────────────────────────────────────
    void DrawTile( int matIndex )
    {
        qtexture_s *q = TexWnd_MaterialAt( matIndex );
        if ( !q )
            return;

        ImGui::PushID( matIndex );
        const ImVec2 cell( KSKY_TILE_W, KSKY_THUMB + KSKY_LABEL_H );
        const ImVec2 p0 = ImGui::GetCursorScreenPos();

        // A REAL item id, for the same reason kiwi_entbrowser.cpp:351-353 gives: the
        // hover/active state and the double-click test both want a live item.
        ImGui::InvisibleButton( "##skytile", cell );
        const bool hovered  = ImGui::IsItemHovered();
        const bool clicked  = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        const bool dblClick = hovered && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left );
        const bool onScreen = ImGui::IsRectVisible( p0, ImVec2( p0.x + cell.x, p0.y + cell.y ) );

        // KIWI-UX (CLEANUP, C-56): the NAME is the selection; the index is this
        // frame's position in a list that can re-sort between frames.
        if ( clicked && q->name )
        {
            strncpy( s_selName, q->name, sizeof( s_selName ) - 1 );
            s_selName[sizeof( s_selName ) - 1] = '\0';
        }
        if ( dblClick )
            ApplyToSelection();

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const bool sel = ( q->name && s_selName[0] && _stricmp( q->name, s_selName ) == 0 );

        const ImVec2 t0( p0.x + 2.0f, p0.y + 2.0f );
        const ImVec2 t1( p0.x + KSKY_THUMB - 2.0f, p0.y + KSKY_THUMB - 2.0f );

        // ROUND AX's budget, applied here: only a tile that is ON SCREEN may pay for a
        // registration, and only KSKY_REG_PER_FRAME of them per frame.  Without the
        // visibility gate the queue order is list order, not reading order, and the row
        // under the cursor fills last (kiwi_entthumb.h's finding, verbatim).
        if ( onScreen && !q->next && s_regBudget > 0 && q->name )
        {
            --s_regBudget;
            Texture_GetHandle( q->name );      // lazy registration (texwnd.cpp:320)
        }

        // ROUND BA: the SAME budget covers the cube-face copy, because it is the same
        // kind of cost (a DXT1 decompress + downsample of a 1024-square face) and a grid
        // that scrolls past forty new sky materials must not do forty of them in a frame.
        // `built` spends the budget only when a copy actually happened.
        bool built = false;
        IDirect3DTexture9 *tex = onScreen
                               ? TileTexture( q, s_regBudget > 0, &built )
                               : nullptr;
        if ( built )
            --s_regBudget;
        if ( tex )
        {
            // NO DRAW CALLBACK, deliberately — kiwi_entbrowser.cpp:429-434's finding:
            // an ImDrawList callback SPLITS the draw list, which in a grid of tiles is
            // the one thing not to do.  ROUND BA retires D-AZ-C's accepted alpha edge
            // without one: the cube arm owns its copy, so CubeFaceThumb bakes alpha to
            // 0xFF in the copy loop the way kiwi_entthumb.cpp:292 does.  (The MAPTYPE_2D
            // arm still hands over the engine's own pointer and still carries the edge.)
            dl->AddImage( (ImTextureID)(intptr_t)tex, t0, t1 );
        }
        else
        {
            // Not resolved yet, or resolved to nothing.  A sky-blue plate rather than a
            // hole, so the grid does not flicker between empty and full while materials
            // register and faces are copied.
            dl->AddRectFilled( t0, t1, KSKY_PLATE_COL, 3.0f );
            const char *dots = q->next ? "no map" : "...";
            dl->AddText( ImVec2( t0.x + 6.0f, t0.y + 6.0f ), IM_COL32( 255, 255, 255, 190 ), dots );
        }

        dl->AddRect( t0, t1,
                     sel      ? IM_COL32( 255, 190,  60, 255 )
                     : hovered ? IM_COL32( 220, 220, 220, 200 )
                               : IM_COL32(  90,  90,  90, 160 ),
                     3.0f, 0, sel ? 2.5f : 1.0f );

        // The LEAF name on the label, with the full path in the tooltip: sky materials
        // live several folders deep and the leaf is the only part that differs between
        // them.
        const char *leaf = LeafName( q->name );
        ImGui::PushClipRect( ImVec2( p0.x, p0.y + KSKY_THUMB ),
                             ImVec2( p0.x + cell.x, p0.y + cell.y ), true );
        dl->AddText( ImVec2( p0.x + 3.0f, p0.y + KSKY_THUMB + 3.0f ),
                     ImGui::GetColorU32( ImGuiCol_Text ), leaf ? leaf : "(unnamed)" );
        ImGui::PopClipRect();

        if ( hovered && q->name )
            ImGui::SetTooltip( "%s\n%s\nDouble-click to apply to the selection.",
                               q->name,
                               ( q->color_or_surfacetype_filter & KSKY_SURF_SKY )
                                   ? "SURF_SKY (from the material header)"
                                   : "matched by name (no SURF_SKY in the header)" );

        ImGui::PopID();
    }
}   // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════════════
//  the predicate (D-AZ-A) — ONE spelling
// ═════════════════════════════════════════════════════════════════════════════════════
bool KiwiSky_IsSkyMaterial( const qtexture_s *q )
{
    if ( !q )
        return false;
    // 1. THE DATA'S OWN ANSWER.  color_or_surfacetype_filter (@0x1C, qe3.h:65) is the
    //    material's full surfaceFlags word, written from the material header at both
    //    registration paths (texwnd.cpp:305 / :414).
    if ( ( q->color_or_surfacetype_filter & KSKY_SURF_SKY ) != 0 )
        return true;
    // 2. ...AND ONLY WHEN THE DATA SAID NOTHING, the name.  Gated on the whole word being
    //    zero rather than OR'd unconditionally: a material whose header DID load and does
    //    NOT carry SURF_SKY is not sky, whatever it is called.  That is the difference
    //    between this predicate and the three name tests it is replacing.
    if ( q->color_or_surfacetype_filter != 0 )
        return false;
    const char *leaf = LeafName( q->name );
    return leaf && strstr( leaf, "sky" ) != nullptr;
}

// ── KIWI-UX (ROUND BB): the BRUSH-level predicate, exported ────────────────────────
// The "is this brush a sky brush" test was written out longhand inside RebuildBounds and
// round BB needs the same question answered in kiwi_focus.cpp (frame-all must not frame
// the shell).  ONE spelling, for the same reason D-AZ-A gave for the material test.
// FACE 0's material at the CURRENT EDIT LAYER.
bool KiwiSky_IsSkyBrush( const brush_t *def )
{
    if ( !def || !def->faces || def->faceCount <= 0 )
        return false;
    const MaterialDef *md = &def->faces[0].mtldef[g_qeglobals.current_edit_layer];
    return KiwiSky_IsSkyMaterial( md->radMtl );
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  the render treatment (D-AZ-B)
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiSky_InvalidateBounds()
{
    s_boundsHave = false;
}

// ── KIWI-UX (ROUND BA): the tile cache's device-reset hook ──────────────────────────
// Called from RTT_ReleaseForReset (radiant_rtt.cpp) beside KiwiEntThumb_ReleaseForReset,
// for the reasons radiant_rtt.cpp:190-198 already states for that one: the pool is
// MANAGED and would survive Reset by contract, but releasing here makes this feature's
// whole device-reset story ONE already registered function, and it guarantees no
// ImTextureID outlives the ImGuiShell_InvalidateDeviceObjects that runs immediately
// before.  Idempotent — the D3DERR_INVALIDCALL second-chance arm runs the list twice.
// Cost after an alt-tab: the visible tiles re-copy one per frame.
void KiwiSky_ReleaseForReset()
{
    DropThumbs( false );      // IMMEDIATE — see DropThumbs and kiwi_texgrave.h D-BU-E
}

bool KiwiSky_SeeThroughFace( const float *facePoint )
{
    ReadPrefs();
    if ( !s_seeThrough )
        return false;                     // toggle off: sky is always its own texture

    const unsigned now = ::GetTickCount();
    // Unsigned wrap is deliberate and correct across the 49-day GetTickCount rollover:
    // (now - stamp) is the true elapsed count in modular arithmetic.
    //
    // KIWI: the 250 ms window is now the POLL of a change signal,
    // not the rebuild schedule.  The box is a function of every brush's def->mins/maxs, and
    // KiwiWalkCache_Epoch is the editor's "some brush changed" counter (it is bumped by
    // MarkMapModified and by every link/unlink/free funnel), so an unchanged map re-walks
    // both brush lists never instead of four times a second — and this walk runs INSIDE the
    // camera draw, per face, so its 250 ms expiry landed in one arbitrary frame.
    if ( !s_boundsHave
      || ( ( now - s_boundsStamp ) >= KSKY_BOUNDS_MS && s_boundsEpoch != KiwiWalkCache_Epoch() ) )
        RebuildBounds();

    if ( !s_boundsAny )
        return false;                     // no sky brushes: nothing to see through, and
                                          // a map that never used this feature is
                                          // untouched by it

    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BB, ITEM 1a) — THE TEST WAS READING A POINT THE USER CANNOT PUT
    //  INSIDE THE SHELL.
    // ═══════════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: *"skybox still doesn't show, even when manuvering the camera
    // into the hull"*, plus the user's own diagnosis: *"i think with plasticity style
    // controls, it's impossible to fly into this skybox cube.  You need to setup the
    // camera to ignore it as an object I'm guessing?"*  That diagnosis is right, and the
    // mechanism is in kiwi_camera.cpp rather than in any collision code:
    //
    //   * KIWI'S CAMERA IS AN ORBIT RIG.  The EYE is not steered; it is DERIVED.  Every
    //     mover writes `origin = lookAt - forward * s_dist` (KiwiCam_LookAlong :935,
    //     KiwiCam_FrameBounds :1019-1022), and KiwiCam_FlyTick ends at KiwiCam_Translate,
    //     which moves the eye AND the pivot by the same delta (:842).  So the eye is
    //     rigidly leashed s_dist BEHIND the pivot and no amount of flying closes that gap.
    //   * ONLY THE WHEEL CHANGES s_dist, and s_dist runs to KCAM_MAX_DIST (65536).
    //   * UNDER ORTHO — which is KIWI's DEFAULT since round M — s_dist IS THE ZOOM:
    //     KiwiCam_OrthoHalfHeight is `s_dist * tan(fov/2) * 0.75`.  Framing a 4096-unit
    //     map puts the pseudo-eye thousands of units back by definition.
    //   * AND FRAME-ALL USED TO INCLUDE THE SHELL ITSELF, so the distance a map opens at
    //     is derived from the very box the user then wants to be inside of.
    //
    // Round AZ tested `Ed_Camera()->origin` — the EYE.  Put together, the eye is outside
    // the shell at every working zoom level, `SeeThroughFace` was true for ever, and the
    // sky was the see-through film for ever.  That is the whole of the report: the
    // TEXTURED arm renders correctly and always did (the user confirmed it mid-round —
    // *"ah it works if you select it"* — because the selected-brush pass at
    // camwnd.cpp:3055-3072 draws the face's OWN material with TECHNIQUE_UNLIT and never
    // consults this predicate, i.e. it is the textured arm with the film skipped).
    //
    // THE POINT THE FEATURE MEANS IS THE PIVOT.  "Am I working inside the sky volume" is
    // a question about where the user's attention is, not about where a derived eye sits,
    // and under ortho there is no eye to ask — KiwiCam_OrthoHalfHeight makes the origin a
    // PSEUDO-eye whose only job is to carry the zoom.  The eye is still OR'd in, not
    // dropped: a perspective fly-through that leaves the pivot behind is genuinely inside,
    // and OR is the reading under which every previously-INSIDE camera stays inside.
    //
    // WHAT IT COSTS, stated rather than buried: with the pivot at the map centre — where
    // it is whenever the user is working — see-through now fires only when the pivot
    // itself leaves the shell, so framing the whole map from outside shows the shell
    // TEXTURED instead of as film.  The escape is one click and it is round BB's own item
    // 2: the shell is a func_group named "SkyBox", so the Outliner (or H) hides it.
    //
    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BC, ITEM 3 / D-BC-A) — THAT COST WAS THE REPORTED BUG
    // ═══════════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: *"yeah the grid broke totally in the last update"* /
    // *"doesn't render at all"*.  The paragraph directly above is the whole diagnosis
    // written down as an accepted trade-off, and it was not one: under the ORTHO
    // default the pivot is inside the shell whenever the user is working, so the walls
    // went opaque, and the near wall — drawn depthTest LESSEQUAL / depthWrite DISABLE
    // (color_only.sm), i.e. it paints without occluding — covered the ground lattice
    // that camwnd.cpp:2691 submits BEFORE the world.  The map survived because sky
    // writes no depth; the grid did not, because it was already on the screen.
    // kiwi_skybox.h D-BC-A carries the full argument.
    //
    // THE RULE IS DEPTH AGAINST THE PIVOT.  A sky face is in the way iff it stands
    // between the viewer and what the viewer is looking at.  That is one dot product,
    // it has the same answer from either side of the shell, and it satisfies BOTH
    // reports: near walls become film (the map, the grid and the axes are visible from
    // "outside"), far walls stay textured (real sky behind the map, and every wall is
    // textured when you are standing in the level).
    const camera_s *c = Ed_Camera();
    const float    *pivot = KiwiCam_LookAt();      // kiwi_camera.cpp — never null
    if ( !c )
        return false;                     // no view at all: round AZ's answer, unchanged

    if ( facePoint && pivot )
    {
        // c->vpn is this frame's basis: CamWnd_SetupScene ran CamWnd_BuildMatrix before
        // the fill loop, and KiwiSky_SeeThroughFace is only called from inside it.
        const float fz = ( facePoint[0] - c->origin[0] ) * c->vpn[0]
                       + ( facePoint[1] - c->origin[1] ) * c->vpn[1]
                       + ( facePoint[2] - c->origin[2] ) * c->vpn[2];
        const float pz = ( pivot[0] - c->origin[0] ) * c->vpn[0]
                       + ( pivot[1] - c->origin[1] ) * c->vpn[1]
                       + ( pivot[2] - c->origin[2] ) * c->vpn[2];
        // The epsilon is a tie-break, not a tolerance: a wall the pivot is sitting
        // exactly ON must pick one answer and hold it, or it flickers frame to frame.
        // "Not in front" is the answer that keeps sky looking like sky.
        return fz < pz - KSKY_FILM_EPS;
    }

    // No face point (the frame-level question): round BB's containment answer, kept
    // verbatim so a caller that cannot name a face gets the behaviour it always got.
    if ( !pivot )
        return false;
    bool inside = true;
    for ( int k = 0; k < 3; ++k )
        if ( pivot[k] < s_boundsMin[k] || pivot[k] > s_boundsMax[k] )
            inside = false;
    if ( !inside )
    {
        inside = true;
        for ( int k = 0; k < 3; ++k )
            if ( c->origin[k] < s_boundsMin[k] || c->origin[k] > s_boundsMax[k] )
                inside = false;
    }
    return !inside;                       // OUTSIDE -> film; INSIDE -> this is the sky
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  the dock window
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiSky_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_SKY );
    if ( !open || !*open )
        return;                           // closed: no Begin, no End, no cost

    if ( KiwiWindows_JustOpened( KIWI_WIN_SKY ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_SKY ), open ) )
    {
        ReadPrefs();
        s_regBudget = KSKY_REG_PER_FRAME;

        ImGui::SetNextItemWidth( KSKY_SEARCH_W );
        ImGui::InputTextWithHint( "##skysearch", "search sky materials",
                                  s_search, sizeof( s_search ) );

        ImGui::SameLine();
        bool see = s_seeThrough;
        if ( ImGui::Checkbox( "See-through from outside", &see ) )
        {
            s_seeThrough = see;
            Radiant_ProfileSetInt( KSKY_SECTION, KSKY_SEE_ENTRY, s_seeThrough ? 1 : 0 );
            g_nUpdateBits = -1;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                "Sky brushes render as a translucent tinted film while you are working\n"
                "OUTSIDE them, so the shell does not hide the map, and go fully textured\n"
                "as soon as you are inside.  Off = always textured.\n"
                "\"Inside\" is the ORBIT PIVOT (the point you are working at) or the eye —\n"
                "the eye alone is never inside, because it sits one zoom-distance behind\n"
                "the pivot.  Hide the SkyBox group when the shell is in the way.\n"
                "A sky that is a single slab (a ceiling, a floor, one backdrop wall) counts\n"
                "as \"inside\" anywhere within the map's own bounds on that axis." );

        // ── KIWI-UX (ROUND BA): which cube face the tiles show ──────────────────────
        // A sky colormap is a CUBEMAP (the decode is on CubeFaceThumb), so a tile is one
        // face of it.  Which face carries the horizon depends on the sky shader's own
        // swizzle, which is inside a compiled shader; the combo settles it by eye.
        ImGui::SameLine();
        ImGui::SetNextItemWidth( KSKY_COMBO_W );
        const bool faceOpen = ImGui::BeginCombo( "face", KSKY_FACE_NAMES[s_face] );
        // Hover is sampled HERE, on the combo's own item, and not after EndCombo: once the
        // popup has been submitted the "last item" is the popup's, so a tooltip hung off
        // the end would be describing the wrong widget.
        const bool faceHovered = ImGui::IsItemHovered();
        if ( faceOpen )
        {
            for ( int i = 0; i < KSKY_FACE_COUNT; ++i )
            {
                const bool sel = ( i == s_face );
                if ( ImGui::Selectable( KSKY_FACE_NAMES[i], sel ) && i != s_face )
                {
                    s_face = i;
                    Radiant_ProfileSetInt( KSKY_SECTION, KSKY_FACE_ENTRY, s_face );
                    DropThumbs( true );    // every cached tile is now the wrong face.
                                           // DEFERRED: this is mid UI build.
                }
                if ( sel )
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if ( faceHovered )
            ImGui::SetTooltip(
                "Which face of the sky cubemap the tiles show.\n"
                "Every CoD4 sky material's colorMap is a six-face cubemap, so a flat tile\n"
                "has to pick one.  -Y is the horizon face that is stored upright; +X, -X\n"
                "and +Y are the other three horizon faces and are turned upright in the\n"
                "copy.  +Z is the zenith cap and -Z the ground cap." );

        // D-AZ-E: this IS g_PrefsDlg->sky_brush_off, the pref the binary already has.
        // Shown INVERTED because "pickable" is the thing a mapper thinks in.
        ImGui::SameLine();
        bool pickable = ( g_PrefsDlg->sky_brush_off == 0 );
        if ( ImGui::Checkbox( "Sky pickable", &pickable ) )
        {
            g_PrefsDlg->sky_brush_off = pickable ? 0 : 1;
            Prefs_SavePrefs( g_PrefsDlg );   // mainfrm.cpp:5902's own pair for cmd 33169
            g_nUpdateBits = -1;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                "Off = clicks pass THROUGH sky brushes to whatever is behind them, so\n"
                "working inside the map does not keep selecting the shell.\n"
                "This is the editor's own \"disable selection of sky\" preference." );

        ImGui::Separator();

        std::vector<int> rows;
        GatherSky( rows );

        if ( rows.empty() )
        {
            ImGui::TextWrapped(
                "No sky materials found.  A material counts as sky when its header "
                "carries SURF_SKY, or (only when the header carries no flags at all) "
                "when its name contains \"sky\".  Load a map, or check that the "
                "materials folder is beside the executable." );
        }

        // The grid, in its own scroll region so the buttons below stay put.
        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + KSKY_FOOTER_PAD;
        ImGui::BeginChild( "##skygrid", ImVec2( 0.0f, -footer ), 0 );
        {
            const float cellW = KSKY_TILE_W + ImGui::GetStyle().ItemSpacing.x;
            const float avail = ImGui::GetContentRegionAvail().x;
            int perRow = (int)( avail / cellW );
            if ( perRow < 1 )
                perRow = 1;

            int col = 0;
            for ( size_t i = 0; i < rows.size(); ++i )
            {
                if ( col > 0 )
                    ImGui::SameLine();
                DrawTile( rows[i] );
                if ( ++col >= perRow )
                    col = 0;
            }
        }
        ImGui::EndChild();

        ImGui::Separator();

        // KIWI-UX (CLEANUP, C-56): resolved by name, so a re-sorted registry cannot
        // silently move the readout (or the two buttons) onto a different material.
        const int selIdxNow = SelectedMaterialIndex();
        qtexture_s *selQ = ( selIdxNow >= 0 ) ? TexWnd_MaterialAt( selIdxNow ) : nullptr;
        ImGui::TextUnformatted( selQ && selQ->name ? selQ->name
                                                   : ( s_selName[0] ? "(picked sky material is gone)"
                                                                    : "(no sky material selected)" ) );

        ImGui::BeginDisabled( selQ == nullptr );
        if ( ImGui::Button( "Apply to selection" ) )
            ApplyToSelection();
        ImGui::SameLine();
        if ( ImGui::Button( "Create skybox shell" ) )
            CreateShell();
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::SetNextItemWidth( KSKY_COMBO_W );
        if ( ImGui::InputFloat( "margin", &s_margin, 0.0f, 0.0f, "%.0f" ) )
        {
            if ( !( s_margin >= 0.0f ) || s_margin > 65536.0f ) s_margin = KSKY_DEF_MARGIN;
            Radiant_ProfileSetInt( KSKY_SECTION, KSKY_MRG_ENTRY, (int)s_margin );
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( KSKY_COMBO_W );
        if ( ImGui::InputFloat( "thickness", &s_thick, 0.0f, 0.0f, "%.0f" ) )
        {
            if ( !( s_thick >= 1.0f ) || s_thick > 8192.0f ) s_thick = KSKY_DEF_THICK;
            Radiant_ProfileSetInt( KSKY_SECTION, KSKY_THK_ENTRY, (int)s_thick );
        }
    }
    ImGui::End();
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  §15 palette
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiSky_RegisterCommands()
{
    // Unbound, searchable — the same deal every §9 window entry gets
    // (kiwi_windows.cpp:296-301).  The two VERBS are registered as well, unlike the
    // entity browser's internal drop id: "make this sky" and "build me a skybox" are
    // things a mapper means on purpose and would want on a key.
    Radiant_RegisterCommand( "KiwiWindowSky",  0, 0, KIWI_CMD_WINDOW_SKY );
    Radiant_RegisterCommand( "KiwiSkyApply",   0, 0, KIWI_CMD_SKY_APPLY );
    Radiant_RegisterCommand( "KiwiSkyShell",   0, 0, KIWI_CMD_SKY_SHELL );
}

bool KiwiSky_DispatchInstant( unsigned int cmdId )
{
    // The window TOGGLE is NOT here — KiwiWindows_DispatchInstant owns every §9 flag.
    // This file owns only the verbs, the same split round W made for the Outliner and
    // round AU made for the entity browser.
    if ( cmdId == (unsigned int)KIWI_CMD_SKY_APPLY )
        return ApplyToSelection();
    if ( cmdId == (unsigned int)KIWI_CMD_SKY_SHELL )
        return CreateShell();
    return false;
}
