// Sky-material browser, shell creation, thumbnail copies, and camera see-through policy.
#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s / Ed_Camera
#include "prefs.h"                  // prefData_t / g_PrefsDlg (the SkyBrushOff checkbox)
#include <windows.h>                // GetTickCount (the union-box refresh timer)
#include <imgui/imgui.h>

#include "kiwi_skybox.h"
#include "kiwi_camera.h"            // orbit pivot used by the see-through depth test
#include "kiwi_command.h"
#include "kiwi_str.h"                // KiwiStr_ContainsNoCase
#include "kiwi_texcache.h"           // by-name texture cache
#include "kiwi_texgrave.h"           // deferred release (the face combo)
#include "kiwi_windows.h"
#include "kiwi_walkcache.h"          // KiwiWalkCache_Epoch — the union box's change signal
#include "radiant_registry.h"       // Radiant_ProfileGetInt / SetInt

// Material image structs follow the same include order as texwnd.cpp.
#include <gfx_d3d/r_material.h>     // Material / MaterialTextureDef / textureTable
#include <gfx_d3d/r_gfx.h>          // GfxImage / GfxTexture / MAPTYPE_2D / MAPTYPE_CUBE
// Sky color maps are cubemaps; thumbnails copy one face to an ImGui-compatible 2D texture.
#include <d3d9.h>
#include <gfx_d3d/r_init.h>         // dx (dx.device)

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

// Ported and cross-file entry points; source anchors match their definitions.
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118   int Sys_Printf(const char*,...)
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773  int g_nUpdateBits
extern camera_s   *Ed_Camera();                                              // camwnd.cpp:161    camera_s *Ed_Camera()
extern brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );   // brush.cpp:465     brush_t *Brush_Alloc(const void*,eclass_t*)
extern void        Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:510  void Brush_Create(float*,float*,brush_t*,eclass_t*)
extern void        Brush_BuildWindings( brush_t *def, int bFull );           // brush.cpp:1434    void Brush_BuildWindings(brush_t*,int)
extern void        Select_Deselect( int a1 );                                // select.cpp:1444   void Select_Deselect(int)
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358  bool Radiant_RegisterCommand(const char*,byte,byte,int)
// Static editor helpers are exported through their owning translation units.
extern void        Ed_EnsureCurrentMaterial_Kiwi();                          // xywnd.cpp:1605    void Ed_EnsureCurrentMaterial_Kiwi()
// Reuses the ported Entity_LinkBrush -> Brush_AddToList -> Brush_AddToList2 landing funnel.
extern selbrush_t *KiwiExtrude_LandDef( brush_t *def );                      // kiwi_extrude.cpp:2360  selbrush_t *KiwiExtrude_LandDef(brush_t*)
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp:796   ImGuiID ImGuiShell_DockRoot()
// Entity_Create allocates a new group only while every selected brush is world-owned.
extern eclass_t   *Eclass_ForName( int hasBrushes, const char *name );       // eclass.cpp:1096   eclass_t *Eclass_ForName(int,const char*)
extern entity_s   *Entity_Create( eclass_t *eclass );                        // entity.cpp:1629   entity_s *Entity_Create(eclass_t*)
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:212  void SetKeyValue(entity_s_def*,const char*,const char*)
extern void        Undo_SetIdForEntity( entity_s_def *ent );                 // undo.cpp:663      void Undo_SetIdForEntity(entity_s_def*)
// Entity-definition sentinel and epair lookup support unique SkyBox targetnames.
extern entity_s    entities;                                                 // entity.cpp 0x23F17A0 — entity-DEF list sentinel
extern bool        Entity_HasEpairMatch( entity_s *e, const char *key, const char *val ); // entity.cpp:520  bool Entity_HasEpairMatch(entity_s*,const char*,const char*)
// texwnd_s is TU-local, so enumeration and application stay behind accessors.
extern int         TexWnd_MaterialCount();                                   // texwnd.cpp:2562   int TexWnd_MaterialCount()
extern qtexture_s *TexWnd_MaterialAt( int idx );                             // texwnd.cpp:2564   qtexture_s *TexWnd_MaterialAt(int)
extern void        TexWnd_MaterialDefFor( qtexture_s *q, MaterialDef *out );  // texwnd.cpp:2564   void TexWnd_MaterialDefFor(qtexture_s*,MaterialDef*)
extern void        TexWnd_ApplyMaterialAtIndex( int idx );                   // texwnd.cpp:1263   void TexWnd_ApplyMaterialAtIndex(int)
extern qtexture_s *Texture_GetHandle( const char *name );                    // texwnd.cpp:314    qtexture_s *Texture_GetHandle(const char*)
extern void        Prefs_SavePrefs( prefData_t *p );                         // prefs.cpp:216     void Prefs_SavePrefs(prefData_t*)
// Undo operation names must be string literals because the record stores the pointer.

// Ported brush-list sentinels; selected brushes remain part of map and sky walks.
extern selbrush_t  active_brushes;                                           // map.cpp (0x23F189C)
extern selbrush_t  selected_brushes;                                         // map.cpp (0x23F1864)

namespace
{
    // SURF_SKY is value 4 (bit 2) in material surfaceFlags (cod4map.h and filters.cpp).
    const int KSKY_SURF_SKY = 4;

    // Square thumbnail plus label; dimensions are ImGui pixels.
    const float KSKY_TILE_W   = 104.0f;
    const float KSKY_THUMB    = 88.0f;
    const float KSKY_LABEL_H  = 30.0f;

    // Shared control widths keep the face, margin, and thickness widgets aligned.
    const float KSKY_SEARCH_W   = 180.0f;   // the "search sky materials" field
    const float KSKY_COMBO_W    =  70.0f;   // aligned face/margin/thickness controls
    const float KSKY_FOOTER_PAD =  12.0f;   // space below the two footer rows

    // Matches Cam_EditorMaterialColor's file-local sky RGB (camwnd.cpp:627).
    const ImU32 KSKY_PLATE_COL = IM_COL32( 115, 158, 235, 255 );

    // Register or copy at most one visible material tile per frame.
    const int KSKY_REG_PER_FRAME = 1;

    // Bounds may lag geometry by 250 ms; normal per-face work stays O(1).
    const unsigned KSKY_BOUNDS_MS = 250;

    // World-unit tie-break keeps a face at the pivot from flickering.
    const float KSKY_FILM_EPS = 0.5f;

    // Shell sizes are world units; defaults land on every supported editor grid.
    const float KSKY_DEF_MARGIN = 512.0f;
    const float KSKY_DEF_THICK  = 64.0f;
    const float KSKY_MIN_HALF   = 256.0f;    // an empty map still gets a real box
    // Brush_Create rejects degenerate boxes; brush.cpp seeds world bounds at +/-131072.
    const float KSKY_WORLD_BOUND = 131072.0f;

    // 128 texels exceeds the 88-pixel draw size, so previews never magnify.
    const int KSKY_TILE_PX = 128;
    // D3D cube-face order: +X, -X, +Y, -Y, +Z, -Z.
    const int KSKY_FACE_COUNT   = 6;
    const char *const KSKY_FACE_NAMES[KSKY_FACE_COUNT] =
        { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };

    // Image_CubemapFace and r_image.cpp preserve D3D face order. Shipped skies use raw
    // world directions, so horizon faces need these clockwise quarter-turns to put
    // world-up at tile top; -Y is already upright and is the default.
    const int KSKY_FACE_ROT[KSKY_FACE_COUNT] = { 1, 3, 2, 0, 0, 0 };
    const int KSKY_FACE_DEFAULT = 3;

    // An axis thinner than half the map extent is a slab, not an enclosing shell axis.
    const float KSKY_FLAT_RATIO = 0.5f;

    const char *KSKY_SECTION   = "KiwiSky";
    const char *KSKY_SEE_ENTRY = "SeeThrough";
    const char *KSKY_MRG_ENTRY = "ShellMargin";
    const char *KSKY_THK_ENTRY = "ShellThickness";
    const char *KSKY_FACE_ENTRY = "TileFace";

    bool  s_seeThrough = true;
    float s_margin     = KSKY_DEF_MARGIN;
    float s_thick      = KSKY_DEF_THICK;
    int   s_face       = KSKY_FACE_DEFAULT;  // chosen cubemap tile face
    bool  s_prefsRead  = false;

    // Store selection by name because the material registry can grow and re-sort.
    char  s_selName[128] = { 0 };            // "" = nothing picked
    char  s_search[64] = { 0 };
    int   s_regBudget  = 0;                  // registrations left this frame

    // One MANAGED 2D copy per material name. Reset and face changes release the cache;
    // reset still releases before ImGui device objects are invalidated.
    kiwiTexCache_t s_thumbs;

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
        // Default on: the shell should not obscure a map before preferences are changed.
        s_seeThrough = Radiant_ProfileGetInt( KSKY_SECTION, KSKY_SEE_ENTRY, 1 ) != 0;
        s_margin = (float)Radiant_ProfileGetInt( KSKY_SECTION, KSKY_MRG_ENTRY, (int)KSKY_DEF_MARGIN );
        s_thick  = (float)Radiant_ProfileGetInt( KSKY_SECTION, KSKY_THK_ENTRY, (int)KSKY_DEF_THICK );
        if ( !( s_margin >= 0.0f ) || s_margin > 65536.0f ) s_margin = KSKY_DEF_MARGIN;
        if ( !( s_thick  >= 1.0f ) || s_thick  > 8192.0f  ) s_thick  = KSKY_DEF_THICK;
        s_face = Radiant_ProfileGetInt( KSKY_SECTION, KSKY_FACE_ENTRY, KSKY_FACE_DEFAULT );
        if ( s_face < 0 || s_face >= KSKY_FACE_COUNT ) s_face = KSKY_FACE_DEFAULT;
    }

    // Keep the leaf-name fallback identical to camwnd.cpp's last-'/' rule.
    const char *LeafName( const char *name )
    {
        if ( !name )
            return nullptr;
        const char *sl = strrchr( name, '/' );
        return sl ? sl + 1 : name;
    }

    // Real sky color maps are cubemaps, while ImGui's D3D9 backend binds 2D textures.
    // Copy one face into a MANAGED texture; no DEFAULT-pool resource is created.
    // D3DXLoadSurfaceFromSurface decompresses and downsamples in one operation.

    // Return a fresh MANAGED 2D face copy, or null on any D3D setup/copy failure.
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

        // The lock pass forces opaque alpha and applies KSKY_FACE_ROT in the same write.
        D3DLOCKED_RECT lr = {};
        if ( tex->LockRect( 0, &lr, nullptr, 0 ) >= 0 )
        {
            const int N   = KSKY_TILE_PX;
            const int rot = KSKY_FACE_ROT[face];
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
                    // Sample the inverse clockwise rotation from `scratch`.
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

    // Locate TS_COLOR_MAP. GfxTexture is a union, so delayLoadPixels must be false
    // before reading its texture arm. Engine-owned 2D textures are not cached;
    // cubemap face copies are cached and `mayBuild` budgets only a new copy.
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
                return img->texture.map;                    // engine-owned 2D arm
            if ( img->mapType == MAPTYPE_CUBE && img->texture.cubemap && !cubeImg )
                cubeImg = img;
        }
        if ( !cubeImg )
            return nullptr;

        // Cache failures too, avoiding a failed D3D copy attempt every frame.
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

    // Face changes defer release until after the ImGui draw; reset releases immediately.
    void DropThumbs( bool deferred )
    {
        if ( !deferred )
        {
            KiwiTexCache_ReleaseAll( s_thumbs );      // immediate reset release
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

    // Flat sky slabs cannot enclose the viewer on their thin axis. Widen only axes whose
    // sky extent is under half the map extent; full shells remain unchanged.
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

    // Union active and selected sky brushes; classification is face 0 at the current layer.
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
        FlattenRescue();                 // widen non-enclosing slab axes
        s_boundsHave  = true;
        s_boundsStamp = ::GetTickCount();
        s_boundsEpoch = KiwiWalkCache_Epoch();
    }

    // Map bounds include active, selected, and hidden brushes: shell creation wraps
    // geometry, not merely the current selection or visible subset.
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

    // Rebuild each frame because material registration grows and re-sorts the registry.
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
            // Search the full path; an empty needle intentionally includes every row.
            if ( !KiwiStr_ContainsNoCase( q->name, s_search ) )
                continue;
            out.push_back( i );
        }
    }

    // Resolve the stable selected name against the live registry; -1 is missing/unpicked.
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

    // Reuse texwnd's apply funnel so brush/face undo and material normalization agree.
    bool ApplyToSelection()
    {
        const int selIdx = SelectedMaterialIndex();      // resolve stable name
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

    // Clamp a slab before allocation; reject any axis thinner than one world unit.
    bool ShellBox( const float lo[3], const float hi[3], const MaterialDef *md )
    {
        float a[3], b[3];
        for ( int k = 0; k < 3; ++k )
        {
            a[k] = lo[k];
            b[k] = hi[k];
            if ( a[k] < -KSKY_WORLD_BOUND ) a[k] = -KSKY_WORLD_BOUND;
            if ( b[k] >  KSKY_WORLD_BOUND ) b[k] =  KSKY_WORLD_BOUND;
            if ( b[k] - a[k] < 1.0f )
                return false;
        }
        brush_t *def = Brush_Alloc( md, nullptr );
        if ( !def )
            return false;
        Brush_Create( a, b, def, nullptr );      // (mins,maxs); Hex-Rays swaps __fastcall args (xywnd.cpp:1585)
        Brush_BuildWindings( def, 1 );
        KiwiExtrude_LandDef( def );
        return true;
    }

    // Six non-overlapping convex slabs tile outer-minus-inner. Floor/roof take outer XY;
    // X walls take outer Y and inner Z, while Y walls take inner X and inner Z.
    // targetname is a link key, so choose SkyBox, SkyBox2, ... from the entity-def list.
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
        // After 999 candidates, leave SkyBox999; the duplicate remains visible for review.
    }

    // A hollow box requires six convex brushes.
    bool CreateShell()
    {
        ReadPrefs();
        const int selIdx = SelectedMaterialIndex();      // resolve stable name
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
            Sys_Printf( "Sky: the picked sky material has no entry in the registry any "
                        "more - pick one in the Sky tab again.\n" );
            return true;
        }

        float mn[3] = { 0.0f, 0.0f, 0.0f };
        float mx[3] = { 0.0f, 0.0f, 0.0f };
        if ( !MapBounds( mn, mx ) )
        {
            // Empty maps start with a centered box extending KSKY_MIN_HALF on each axis.
            for ( int k = 0; k < 3; ++k ) { mn[k] = -KSKY_MIN_HALF; mx[k] = KSKY_MIN_HALF; }
        }

        // Use texwnd's MaterialDef builder to preserve auto-scale and layer sample size.
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

        // Deselect before undo begins so its snapshot excludes brushes about to be dropped.
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

        // Entity_Create allocates only when all selected brushes are world-owned; LandDef
        // guarantees that here. Group inside the existing undo bracket and stamp the new
        // entity before assigning its unique targetname. Skip grouping when no slab exists.
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

    void DrawTile( int matIndex )
    {
        qtexture_s *q = TexWnd_MaterialAt( matIndex );
        if ( !q )
            return;

        ImGui::PushID( matIndex );
        const ImVec2 cell( KSKY_TILE_W, KSKY_THUMB + KSKY_LABEL_H );
        const ImVec2 p0 = ImGui::GetCursorScreenPos();

        // InvisibleButton supplies the live item ID needed by hover and click state.
        ImGui::InvisibleButton( "##skytile", cell );
        const bool hovered  = ImGui::IsItemHovered();
        const bool clicked  = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        const bool dblClick = hovered && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left );
        const bool onScreen = ImGui::IsRectVisible( p0, ImVec2( p0.x + cell.x, p0.y + cell.y ) );

        // Store and compare names because registry re-sorts can move material indices.
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

        // Only visible tiles may spend the one-per-frame registration budget.
        if ( onScreen && !q->next && s_regBudget > 0 && q->name )
        {
            --s_regBudget;
            Texture_GetHandle( q->name );      // lazy registration (texwnd.cpp:320)
        }

        // Cube decompression/downsampling shares the budget; spend it only on a new copy.
        bool built = false;
        IDirect3DTexture9 *tex = onScreen
                               ? TileTexture( q, s_regBudget > 0, &built )
                               : nullptr;
        if ( built )
            --s_regBudget;
        if ( tex )
        {
            // Direct AddImage avoids per-tile callbacks; cube copies already have opaque alpha.
            dl->AddImage( (ImTextureID)(intptr_t)tex, t0, t1 );
        }
        else
        {
            // Stable sky-blue placeholder avoids flicker while resources resolve.
            dl->AddRectFilled( t0, t1, KSKY_PLATE_COL, 3.0f );
            const char *dots = q->next ? "no map" : "...";
            dl->AddText( ImVec2( t0.x + 6.0f, t0.y + 6.0f ), IM_COL32( 255, 255, 255, 190 ), dots );
        }

        dl->AddRect( t0, t1,
                     sel      ? IM_COL32( 255, 190,  60, 255 )
                     : hovered ? IM_COL32( 220, 220, 220, 200 )
                               : IM_COL32(  90,  90,  90, 160 ),
                     3.0f, 0, sel ? 2.5f : 1.0f );

        // Draw the leaf name; the tooltip retains the full path.
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

bool KiwiSky_IsSkyMaterial( const qtexture_s *q )
{
    if ( !q )
        return false;
    // Loaded surfaceFlags are authoritative. Only a zero word permits the legacy
    // leaf-name fallback, matching camwnd.cpp's last-'/' spelling.
    if ( ( q->color_or_surfacetype_filter & KSKY_SURF_SKY ) != 0 )
        return true;
    if ( q->color_or_surfacetype_filter != 0 )
        return false;
    const char *leaf = LeafName( q->name );
    return leaf && strstr( leaf, "sky" ) != nullptr;
}

// Shared brush classification uses face 0 at the current edit layer.
bool KiwiSky_IsSkyBrush( const brush_t *def )
{
    if ( !def || !def->faces || def->faceCount <= 0 )
        return false;
    const MaterialDef *md = &def->faces[0].mtldef[g_qeglobals.current_edit_layer];
    return KiwiSky_IsSkyMaterial( md->radMtl );
}

void KiwiSky_InvalidateBounds()
{
    s_boundsHave = false;
}

// Reset release is immediate and idempotent; visible tiles rebuild one per frame.
void KiwiSky_ReleaseForReset()
{
    DropThumbs( false );      // reset cannot defer
}

bool KiwiSky_SeeThroughFace( const float *facePoint )
{
    ReadPrefs();
    if ( !s_seeThrough )
        return false;                     // toggle off: sky is always its own texture

    const unsigned now = ::GetTickCount();
    // Unsigned subtraction is correct across GetTickCount's 49-day rollover.
    // After 250 ms, compare the shared geometry epoch before rebuilding both brush lists.
    if ( !s_boundsHave
      || ( ( now - s_boundsStamp ) >= KSKY_BOUNDS_MS && s_boundsEpoch != KiwiWalkCache_Epoch() ) )
        RebuildBounds();

    if ( !s_boundsAny )
        return false;                     // no sky brushes, so no treatment

    // Film a face only when it is closer to the eye than the orbit pivot. Eye/pivot
    // containment is nearly constant in ortho and made near opaque sky walls erase the
    // grid; view depth keeps near walls translucent and far walls textured from either
    // side of the shell. facePoint and pivot are world-space; vpn projects view depth.
    const camera_s *c = Ed_Camera();
    const float    *pivot = KiwiCam_LookAt();      // orbit pivot
    if ( !c )
        return false;                     // no camera view

    if ( facePoint && pivot )
    {
        // CamWnd_BuildMatrix populated this frame's vpn before the face-fill loop.
        const float fz = ( facePoint[0] - c->origin[0] ) * c->vpn[0]
                       + ( facePoint[1] - c->origin[1] ) * c->vpn[1]
                       + ( facePoint[2] - c->origin[2] ) * c->vpn[2];
        const float pz = ( pivot[0] - c->origin[0] ) * c->vpn[0]
                       + ( pivot[1] - c->origin[1] ) * c->vpn[1]
                       + ( pivot[2] - c->origin[2] ) * c->vpn[2];
        // Epsilon is a world-unit tie-break; a face at the pivot remains textured.
        return fz < pz - KSKY_FILM_EPS;
    }

    // Null facePoint retains the cached containment fallback for frame-level callers.
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
    return !inside;                       // containment fallback: outside becomes film
}

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

        // A flat tile must choose one face; -Y is the measured upright horizon default.
        ImGui::SameLine();
        ImGui::SetNextItemWidth( KSKY_COMBO_W );
        const bool faceOpen = ImGui::BeginCombo( "face", KSKY_FACE_NAMES[s_face] );
        // Sample hover before EndCombo changes ImGui's last item to the popup.
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
                    DropThumbs( true );    // face changed; defer release until frame end
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

        // Reuse sky_brush_off, inverted to the user-facing "pickable" wording.
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

        // Keep the grid scrollable while footer controls remain fixed.
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

        // Resolve the name again so registry re-sorts cannot retarget footer actions.
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

void KiwiSky_RegisterCommands()
{
    // Register the window and verbs unbound; the command palette supplies discovery.
    Radiant_RegisterCommand( "KiwiWindowSky",  0, 0, KIWI_CMD_WINDOW_SKY );
    Radiant_RegisterCommand( "KiwiSkyApply",   0, 0, KIWI_CMD_SKY_APPLY );
    Radiant_RegisterCommand( "KiwiSkyShell",   0, 0, KIWI_CMD_SKY_SHELL );
}

bool KiwiSky_DispatchInstant( unsigned int cmdId )
{
    // Window toggling belongs to KiwiWindows_DispatchInstant; this owns only the verbs.
    if ( cmdId == (unsigned int)KIWI_CMD_SKY_APPLY )
        return ApplyToSelection();
    if ( cmdId == (unsigned int)KIWI_CMD_SKY_SHELL )
        return CreateShell();
    return false;
}
