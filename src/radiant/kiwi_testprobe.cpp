#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-10-05, user: "do an entire kiwi radiant testing gauntlet to eliminate all these
// precision, rounding, snapping bugs. I also want you to test the camera ... Make a remote
// connection suite"): the probe verbs (kiwi_testprobe.h).  Everything here only READS the
// editor or drives it through the same entry points the viewport uses; nothing is a second
// implementation of an editor operation.
//
//   dump all|selected <file.json>          every saved entity's brushes / patches / keys
//   dump_world <file.json>                 every brush / patch / model placed in WORLD space
//   validate all|selected                  KiwiValid_CheckBrush per brush
//   cam_get                                camera origin / angles / size / ortho / pivot
//   cam_ortho 0|1                          cam_frame x0 y0 z0 x1 y1 z1
//   vp_down btn x y [mods]                 vp_move x y [mods]     vp_up btn x y [mods]
//   vp_wheel steps x y [mods]              vp_click btn x y [mods]
//   vp_drag btn x0 y0 x1 y1 [steps] [mods]
//       the camera viewport's own mouse entry points (KiwiVP_Camera*, what imgui_shell
//       calls): btn 0 LMB, 1 RMB, 2 MMB; mods = shift ctrl alt.  MMB drag = orbit,
//       Shift+MMB / RMB drag = pan, Alt+RMB = look, LMB = select / marquee / tools.
//   xy_drag x0 y0 x1 y1 [steps] [shift] [ctrl]
//       an LMB drag in the 2D view (XYWnd_OnLButtonDown / OnMouseMove / OnLButtonUp ->
//       Drag_Begin / Drag_MouseMoved / MoveSelection / Drag_MouseUp) on a top view at
//       scale 1 centred on the world origin; x/y are world units.
//   patch_edit 0|1                         (1: the classic curve-point mode, Patch_EditPatch,
//       on the selected patches - the one xy_drag's curve-point drag needs; 0: back to brushes)
//   pick x y [object|face|edge|vertex|all] project x y z
//   screenshot camera|xy <file.png>
//   kstart <id>                            kmove x y      kpress x y [shift]
//   khandle x y                            khandle_release
//   kkey <key> [shift] [ctrl] [alt]        ktype <text>   kopt <row> <value>
//   kstate                                 kconfirm
//   uv_affine a b c d                      (the UV editor's Rot / Flip path on the selection)
//   face_align e b f e b f                 (Drag_FaceAlign: dst face takes src face's texture)
//   merge_coplanar e b                     (KiwiValid_MergeCoplanarFaces on one brush)
//   tex_pick e b                           (camera MMB pick: face 0's material, a patch's ST grid)
//   tex_apply_brush e b                    (camera Ctrl+MMB whole-brush apply, stored patch ST)
//   pref noclamp|grid|lightmaplock|texlock|units|gridin [value]   (pref units reload)
//   profile_get <section> <key>            profile_set <section> <key> <int>
//   profile_setstr <section> <key> <string>
//   raw_write <qpath> <text>               (KiwiFS_WriteRawAtomic into raw/)
//   iwi_write <source image> <qpath>       (KiwiIwi_WriteFromFile + KiwiIwi_VerifyOnDisk)
//   con_state                              (construction objects, undo depth, selection)
//   reg_state                              con_pts <index>   (derived faces; an object's points)
//   focus_click e b f                      (an object click recorded on that face: Ctrl+R brush mode)
//   con_label <index> <name> <group>       can <command id>   (KiwiCmd_CanExecute)
//   con_select <index> [index ...]         face_add e b f     (adds to the face selection)
//   cap_commit <type 0..3>                 undo_depth         (the unified journal's depth)
//   simple_patch <wSel> <hSel>             (Curve > Simple Patch Mesh on the selected brush)
//   sg_flags <key>                         (the Script group panel's flag rows)
//   layer_hide <name> <0|1>                (Layers panel Hide/Show + the script_layer filters)
//   find_replace <find> <replace> <flags>  (FindReplaceTextures + the map dirty flag)
//   sg_select <entity> [entity ...]        sg_dump   (every saved entity's epairs + selection)
//   sg_op <op> [a] [b]                     (the Script group tool's actions)
//   snapshot <path>                        (Map_WriteSnapshot - the live check's write)
//   xy_hover x y                           clip_state   (classic clipper points)
//   cam_mode                               (camera draw mode + the Render Method menu id)
//   filter <name> <0|1>                    filtered e b   (Filters checklist / FilterBrush)

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"               // camera_s
#include "prefs.h"                 // g_PrefsDlg->m_bNoClamp
#include "radiant_rtt.h"           // RTT_GetTexture: viewport screenshots
#include "radiant_registry.h"      // profile_get / profile_set (the isolated test profile)
#include "kiwi_units.h"            // pref units / gridin
#include "kiwi_matwriter.h"        // raw_write (KiwiFS_WriteRawAtomic)
#include "kiwi_iwi.h"              // iwi_write
#include <d3dx9.h>                 // D3DXSaveSurfaceToFileA
#include <imgui/imgui.h>           // io.KeyShift / KeyCtrl / KeyAlt: the viewport's modifiers

#include "kiwi_testprobe.h"
#include "kiwi_test.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_keymap.h"           // keybind_* probes (the Command List editor)
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_focus.h"       // focus_click
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_primitive.h"   // pref roundpatch
#include "kiwi_region.h"      // reg_state
#include "kiwi_selection.h"
#include "kiwi_uveditor.h"    // KiwiUvEd_TestAffine
#include "kiwi_validity.h"
#include "kiwi_viewport.h"
#include "xywnd.h"                 // Ed_ActiveXY: xy_drag's view state

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Editor entry points (each checked against its definition).
extern void Entity_GetOrientationMatrix( entity_s *ent, float (*axis)[3] );           // entity.cpp:632
extern void OrientationConcatenate( const orientation_t *orFirst, const orientation_t *orSecond,
                                    orientation_t *out );                              // engine_stubs.cpp:1136
extern char Texture_SetTexture( const int *patchGrid, MaterialDef *material );           // texwnd.cpp 0x45BE50
extern char Radiant_PatchGetTexdef( patchMesh_t *patch, texdef_sub_t *texdef );         // brush.cpp 0x44B620
extern void Brush_SetAllFacesMaterial( brush_t *def, MaterialDef *material, char planarProject,
                                       float useStoredST, char materialOnly );          // brush.cpp 0x476ED0
namespace LayerMat { int GetCurrentLayer( MaterialDef *mtlDef ); }                      // materialdef.cpp 0x431B30
extern bool     Radiant_TryHotkey( unsigned int vk );                        // mainfrm.cpp:1752
extern brush_t *Brush_ApplyTextureProjection( face_t *srcFace, brush_t *b, face_t *dstFace );   // brush.cpp 0x476a30
extern void     XYWnd_OnLButtonDown( HWND hwnd, unsigned int nFlags, int x, int y );   // xywnd.cpp
extern void     XYWnd_OnMouseMove( unsigned int nFlags, int x, int y );                // xywnd.cpp
extern void     Patch_EditPatch();                                                     // pmesh.cpp 0x441FD0
extern void     XYWnd_OnLButtonUp( unsigned int nFlags, int x, int y );                // xywnd.cpp

namespace
{
    bool ParseF( const std::string &s, float &out )
    {
        char *end = nullptr;
        errno = 0;
        const double v = strtod( s.c_str(), &end );
        if ( errno || !end || *end || !std::isfinite( v ) )
            return false;
        out = (float)v;
        return true;
    }

    bool ParseI( const std::string &s, int &out )
    {
        char *end = nullptr;
        errno = 0;
        const long v = strtol( s.c_str(), &end, 10 );
        if ( errno || !end || *end )
            return false;
        out = (int)v;
        return true;
    }

    std::string Lower( std::string s )
    {
        for ( char &c : s )
            c = (char)tolower( (unsigned char)c );
        return s;
    }

    // ── saved-entity indexing (the same order as the `select brush e b` verb) ──
    bool EntityIsSaved( entity_s *ent )
    {
        if ( !ent || !ent->eclass || !ent->eclass->name )
            return false;
        const bool hasBrushes = (void *)ent->brushes.prev != (void *)&ent->def;
        return hasBrushes || strcmp( ent->eclass->name, "worldspawn" ) == 0;
    }

    template <typename Fn>
    void ForEachSavedBrush( Fn fn )                // fn( entityIndex, entity, brushOrdinal, def )
    {
        int e = 0;
        size_t guard = 0;
        for ( entity_s *ent = entities.next; ent && ent != &entities && guard < 1000000; ent = ent->next, ++guard )
        {
            if ( !EntityIsSaved( ent ) )
                continue;
            brush_t *sentinel = (brush_t *)&ent->def;
            int b = 0;
            for ( brush_t *def = (brush_t *)ent->brushes.prev; def && def != sentinel; def = def->onext, ++b )
                fn( e, ent, b, def );
            ++e;
        }
    }

    std::unordered_map<const brush_t *, std::pair<int, int>> IndexBrushes()
    {
        std::unordered_map<const brush_t *, std::pair<int, int>> index;
        ForEachSavedBrush( [&]( int e, entity_s *, int b, brush_t *def ) { index[def] = { e, b }; } );
        return index;
    }

    std::unordered_set<const brush_t *> SelectedDefs()
    {
        std::unordered_set<const brush_t *> out;
        for ( selbrush_t *s = selected_brushes.next; s && s != &selected_brushes; s = s->next )
            if ( s->def )
                out.insert( s->def );
        return out;
    }

    const char *MaterialName( MaterialDef *def )
    {
        if ( !def || ( ( def->lyrMtl != nullptr ) + ( def->radMtl != nullptr ) ) != 1 )
            return "";
        const char *name = reinterpret_cast<const char *>( Materialdef_GetName( def ) );
        return name ? name : "";
    }

    void JsonString( std::string &out, const char *s )
    {
        out += '"';
        for ( ; s && *s; ++s )
        {
            const char c = *s;
            if ( c == '"' || c == '\\' ) { out += '\\'; out += c; }
            else if ( (unsigned char)c < 0x20 ) { char u[8]; _snprintf( u, sizeof( u ), "\\u%04x", c ); out += u; }
            else out += c;
        }
        out += '"';
    }

    void JsonNum( std::string &out, double v )
    {
        if ( !std::isfinite( v ) )                 // Python's json reads these tokens
        {
            out += v != v ? "NaN" : v > 0 ? "Infinity" : "-Infinity";
            return;
        }
        char buf[48];
        _snprintf( buf, sizeof( buf ), "%.9g", v );
        buf[sizeof( buf ) - 1] = '\0';
        out += buf;
    }

    void JsonVec( std::string &out, const float *v )
    {
        out += '[';
        JsonNum( out, v[0] ); out += ',';
        JsonNum( out, v[1] ); out += ',';
        JsonNum( out, v[2] );
        out += ']';
    }

    // ── dump ─────────────────────────────────────────────────────────────────
    bool Dump( bool selectedOnly, const std::string &path, std::string &error )
    {
        const std::unordered_set<const brush_t *> sel = SelectedDefs();
        std::string js = "{\"brushes\":[";
        bool first = true;
        int count = 0;
        ForEachSavedBrush( [&]( int e, entity_s *ent, int b, brush_t *def )
        {
            const bool isSel = sel.count( def ) != 0;
            if ( selectedOnly && !isSel )
                return;
            if ( !first )
                js += ",\n";
            first = false;
            ++count;
            js += "{\"e\":" + std::to_string( e ) + ",\"b\":" + std::to_string( b ) + ",\"cls\":";
            JsonString( js, ent->eclass->name );
            js += ",\"sel\":";
            js += isSel ? "1" : "0";
            js += ",\"fixed\":";
            js += ent->eclass->fixedsize ? "1" : "0";
            js += ",\"mins\":";
            JsonVec( js, def->mins );
            js += ",\"maxs\":";
            JsonVec( js, def->maxs );
            if ( ent->eclass->fixedsize )
            {
                js += ",\"live\":";                 // ent->origin (the epair lags until save)
                JsonVec( js, ent->origin );
                js += ",\"origin\":";
                JsonString( js, ValueForKey2( ent, "origin" ) );
                js += ",\"angles\":";
                JsonString( js, ValueForKey2( ent, "angles" ) );
                js += ",\"model\":";
                JsonString( js, ValueForKey2( ent, "model" ) );
                js += ",\"modelscale\":";
                JsonString( js, ValueForKey2( ent, "modelscale" ) );
            }
            if ( def->patch )
            {
                const patchMesh_t *p = def->patch;
                js += ",\"patch\":{\"w\":" + std::to_string( p->width ) + ",\"h\":" + std::to_string( p->height ) + ",\"ctrl\":[";
                for ( int i = 0; i < p->width && i < 16; ++i )
                    for ( int j = 0; j < p->height && j < 16; ++j )
                    {
                        if ( i || j )
                            js += ',';
                        JsonVec( js, p->ctrl[i][j].xyz );
                    }
                js += "],\"st\":[";
                for ( int i = 0; i < p->width && i < 16; ++i )
                    for ( int j = 0; j < p->height && j < 16; ++j )
                    {
                        if ( i || j )
                            js += ',';
                        js += '[';
                        JsonNum( js, p->ctrl[i][j].texCoord.st[0] );
                        js += ',';
                        JsonNum( js, p->ctrl[i][j].texCoord.st[1] );
                        js += ']';
                    }
                // the tessellated render mesh the editor DRAWS (curveDef; verts[col + row * w])
                js += "],\"subdiv\":" + std::to_string( p->subDivType );
                const curvePatchDef_t *cd = p->curveDef;
                if ( cd && cd->verts && cd->width > 0 && cd->height > 0 )
                {
                    js += ",\"mesh\":{\"w\":" + std::to_string( cd->width ) + ",\"h\":" + std::to_string( cd->height ) + ",\"v\":[";
                    for ( int k = 0; k < cd->width * cd->height; ++k )
                    {
                        if ( k )
                            js += ',';
                        JsonVec( js, cd->verts[k].xyz );
                    }
                    js += "]}";
                }
                js += "}";
            }
            js += ",\"faces\":[";
            for ( int f = 0; def->faces && f < def->faceCount; ++f )
            {
                face_t &fc = def->faces[f];
                if ( f )
                    js += ',';
                js += "{\"pts\":[";
                JsonVec( js, fc.planepts[0] ); js += ',';
                JsonVec( js, fc.planepts[1] ); js += ',';
                JsonVec( js, fc.planepts[2] );
                js += "],\"n\":";
                JsonVec( js, fc.plane.normal );
                js += ",\"d\":";
                JsonNum( js, fc.plane.dist );
                js += ",\"mat\":";
                JsonString( js, MaterialName( &fc.mtldef[0] ) );
                js += ",\"w\":[";
                const winding_t *w = fc.w;
                for ( int k = 0; w && k < w->numpoints; ++k )
                {
                    if ( k )
                        js += ',';
                    JsonVec( js, w->p[k] );
                }
                // channel 0's texture coordinates at each winding point, in repeats - the
                // projection Visuals_InitFaceVis draws (brush.cpp st[i])
                js += "],\"st\":[";
                const texdef_sub_t &td = fc.mtldef[0].mat_texDef;
                float tm[8];
                Face_MoveTexture( td.size, fc.plane.normal, tm, td.shift, td.rotate, td.crossterm );
                for ( int k = 0; w && k < w->numpoints; ++k )
                {
                    const float *p = w->p[k];
                    js += k ? ",[" : "[";
                    JsonNum( js, tm[0] * p[0] + tm[1] * p[1] + tm[2] * p[2] + tm[3] );
                    js += ',';
                    JsonNum( js, tm[4] * p[0] + tm[5] * p[1] + tm[6] * p[2] + tm[7] );
                    js += ']';
                }
                // channel 1 (lightmap) the same way, plus its size (the lock's ch-1 path)
                js += "],\"lst\":[";
                const texdef_sub_t &ld = fc.mtldef[1].mat_texDef;
                float lm[8];
                Face_MoveTexture( ld.size, fc.plane.normal, lm, ld.shift, ld.rotate, ld.crossterm );
                for ( int k = 0; w && k < w->numpoints; ++k )
                {
                    const float *p = w->p[k];
                    js += k ? ",[" : "[";
                    JsonNum( js, lm[0] * p[0] + lm[1] * p[1] + lm[2] * p[2] + lm[3] );
                    js += ',';
                    JsonNum( js, lm[4] * p[0] + lm[5] * p[1] + lm[6] * p[2] + lm[7] );
                    js += ']';
                }
                js += "],\"lsz\":[";
                JsonNum( js, ld.size[0] );  js += ',';
                JsonNum( js, ld.size[1] );
                js += "],\"td\":[";                 // size s t, shift s t, rotate, crossterm
                JsonNum( js, td.size[0] );  js += ',';
                JsonNum( js, td.size[1] );  js += ',';
                JsonNum( js, td.shift[0] ); js += ',';
                JsonNum( js, td.shift[1] ); js += ',';
                JsonNum( js, td.rotate );   js += ',';
                JsonNum( js, td.crossterm );
                js += "]}";
            }
            js += "]}";
        } );
        js += "]}\n";
        FILE *f = fopen( path.c_str(), "wb" );
        if ( !f )
        {
            error = "dump: could not write " + path;
            return false;
        }
        fwrite( js.data(), 1, js.size(), f );
        fclose( f );
        KiwiTest_LogChannel( "DATA", "dump %d brushes -> %s", count, path.c_str() );
        return true;
    }

    // ── dump_world: everything as the editor PLACES it ──────────────────────────
    // KIWI (2026-10-05, user: "verify that the representation on-disk is the same as
    // in-editor and in-game ... positions and rotations of every brush and xmodel"): world
    // space for every brush / patch / model, prefab contents through their prefab's
    // orientation (nested prefabs concatenate - Entity_UpdateModelInst's rule), each model's
    // origin + AnglesToAxis rows + modelscale as Entity_GetOrientationMatrix gives them.
    struct WorldDumper
    {
        std::string js;
        int count = 0;

        void Sep() { if ( count++ ) js += ",\n"; }

        void Keys( entity_s *def )
        {
            static const char *const keys[] = { "classname", "model", "origin", "angles", "angle", "modelscale" };
            js += ",\"keys\":{";
            bool any = false;
            for ( const char *k : keys )
            {
                const char *v = ValueForKey2( def, k );
                if ( !v || !*v )
                    continue;
                if ( any )
                    js += ',';
                any = true;
                JsonString( js, k );
                js += ':';
                JsonString( js, v );
            }
            js += '}';
        }

        void Orient( const char *name, const orientation_t &o )
        {
            js += ",\"";
            js += name;
            js += "\":{\"origin\":";
            JsonVec( js, o.origin );
            js += ",\"axis\":[";
            for ( int r = 0; r < 3; ++r )
            {
                if ( r )
                    js += ',';
                JsonVec( js, o.axis[r] );
            }
            js += "]}";
        }

        void Brush( const std::string &path, int b, brush_t *def, const orientation_t &o )
        {
            Sep();
            js += "{\"kind\":\"" + std::string( def->patch ? "patch" : "brush" ) + "\",\"path\":\"" + path + "\",\"b\":" + std::to_string( b );
            if ( def->patch )
            {
                const patchMesh_t *p = def->patch;
                js += ",\"w\":" + std::to_string( p->width ) + ",\"h\":" + std::to_string( p->height ) + ",\"ctrl\":[";
                for ( int i = 0; i < p->width && i < 16; ++i )
                    for ( int j = 0; j < p->height && j < 16; ++j )
                    {
                        float wp[3];
                        OrientationPosToWorldPos( wp, p->ctrl[i][j].xyz, &o );
                        if ( i || j )
                            js += ',';
                        JsonVec( js, wp );
                    }
                js += "]}";
                return;
            }
            js += ",\"faces\":[";
            for ( int f = 0; def->faces && f < def->faceCount; ++f )
            {
                face_t &fc = def->faces[f];
                if ( f )
                    js += ',';
                float n[3], pts[3][3];
                OrientationDirToWorldDir( n, &o, fc.plane.normal );
                for ( int k = 0; k < 3; ++k )
                    OrientationPosToWorldPos( pts[k], fc.planepts[k], &o );
                js += "{\"mat\":";
                JsonString( js, MaterialName( &fc.mtldef[0] ) );
                js += ",\"pts\":[";
                JsonVec( js, pts[0] ); js += ',';
                JsonVec( js, pts[1] ); js += ',';
                JsonVec( js, pts[2] );
                js += "],\"n\":";
                JsonVec( js, n );
                js += ",\"w\":[";
                const winding_t *w = fc.w;
                for ( int k = 0; w && k < w->numpoints; ++k )
                {
                    float wp[3];
                    OrientationPosToWorldPos( wp, w->p[k], &o );
                    if ( k )
                        js += ',';
                    JsonVec( js, wp );
                }
                js += "]}";
            }
            js += "]}";
        }

        // `ent` carries the keys and brush defs; `inst` the prefab instance (it may be ent).
        void Entity( const std::string &path, entity_s *inst, entity_s *ent, const orientation_t &parent, int depth )
        {
            if ( !ent || !ent->eclass )
                return;
            if ( ent->eclass->fixedsize )
            {
                orientation_t local, world;
                Entity_GetOrientationMatrix( ent, (float (*)[3])&local );
                OrientationConcatenate( &local, &parent, &world );
                const bool prefab = ( ent->eclass->classtype & 0x10 ) != 0;   // CLASS_PREFAB
                Sep();
                js += "{\"kind\":\"" + std::string( prefab ? "prefab" : "model" ) + "\",\"path\":\"" + path + "\"";
                Keys( ent );
                Orient( "world", world );
                float scale = (float)atof( ValueForKey2( ent, "modelscale" ) );
                js += ",\"scale\":";
                JsonNum( js, scale > 0.0f ? scale : 1.0f );
                js += '}';
                prefab_s *pf = (prefab_s *)( inst->prefab ? inst->prefab : ent->prefab );
                if ( !prefab || !pf || depth > 8 )
                    return;
                int i = 0;
                for ( entity_s *in = pf->next_entity; in && in != (entity_s *)pf; in = in->next, ++i )
                    Entity( path + "/" + std::to_string( i ), in, in->def ? in->def : in, world, depth + 1 );
                return;
            }
            brush_t *sentinel = (brush_t *)&ent->def;
            int b = 0;
            for ( brush_t *def = (brush_t *)ent->brushes.prev; def && def != sentinel; def = def->onext, ++b )
                Brush( path, b, def, parent );
        }
    };

    // A world entity is a DEF; its placed instance (prefab_s, model inst) is the owner of its
    // brush instance on the active / selected lists (SetupModelInst builds it there).
    entity_s *InstanceOf( entity_s *def )
    {
        selbrush_t *const heads[2] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : heads )
            for ( selbrush_t *s = head->next; s && s != head; s = s->next )
                if ( s->def && (entity_s *)s->def->owner == def && s->owner )
                    return s->owner;
        return def;
    }

    bool DumpWorld( const std::string &path, std::string &error )
    {
        WorldDumper d;
        d.js = "{\"items\":[";
        orientation_t ident = {};
        ident.axis[0][0] = ident.axis[1][1] = ident.axis[2][2] = 1.0f;
        int e = 0;
        size_t guard = 0;
        for ( entity_s *ent = entities.next; ent && ent != &entities && guard < 1000000; ent = ent->next, ++guard )
        {
            const bool fixed = ent->eclass && ent->eclass->fixedsize;
            if ( !EntityIsSaved( ent ) && !fixed )
                continue;
            d.Entity( std::to_string( e ), fixed ? InstanceOf( ent ) : ent, ent, ident, 0 );
            ++e;
        }
        d.js += "]}\n";
        FILE *f = fopen( path.c_str(), "wb" );
        if ( !f )
        {
            error = "dump_world: could not write " + path;
            return false;
        }
        fwrite( d.js.data(), 1, d.js.size(), f );
        fclose( f );
        KiwiTest_LogChannel( "DATA", "dump_world %d items -> %s", d.count, path.c_str() );
        return true;
    }

    // ── validate ─────────────────────────────────────────────────────────────
    void Validate( bool selectedOnly )
    {
        const std::unordered_set<const brush_t *> sel = SelectedDefs();
        int checked = 0, bad = 0;
        ForEachSavedBrush( [&]( int e, entity_s *ent, int b, brush_t *def )
        {
            if ( ( selectedOnly && !sel.count( def ) ) || def->patch || ent->eclass->fixedsize )
                return;
            ++checked;
            const char *why = "invalid";
            if ( !KiwiValid_CheckBrush( def, &why ) )
            {
                ++bad;
                KiwiTest_LogChannel( "DATA", "invalid %d %d %s", e, b, why ? why : "invalid" );
            }
        } );
        KiwiTest_LogChannel( "DATA", "validate checked %d invalid %d", checked, bad );
    }

    // ── camera ───────────────────────────────────────────────────────────────
    void CamReport( const char *what )
    {
        camera_s *c = Ed_Camera();
        const float *look = KiwiCam_LookAt();
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        if ( !look )
            look = zero;
        KiwiTest_LogChannel( "DATA", "%s origin %.6f %.6f %.6f angles %.6f %.6f %.6f size %d %d ortho %d dist %.6f look %.6f %.6f %.6f",
                             what, c->origin[0], c->origin[1], c->origin[2],
                             c->angles[0], c->angles[1], c->angles[2], c->width, c->height,
                             KiwiCam_Ortho() ? 1 : 0, KiwiCam_Distance(), look[0], look[1], look[2] );
    }

    void CamChanged()
    {
        CamWnd_BuildMatrix();
        g_nUpdateBits = -1;
        KiwiTest_WaitFrames( 2 );                  // let the views render the new pose
    }

    // Modifier keys for one injected event: this thread's key state (GetKeyState readers)
    // and ImGui's io flags (the viewport's readers), restored afterwards.  The user's own
    // keyboard is another process's input and is never touched.  GetAsyncKeyState readers
    // (fly keys, the Ctrl+Z fast path) cannot be fed this way.
    struct mods_t
    {
        bool shift = false, ctrl = false, alt = false;
    };

    class ScopedMods
    {
    public:
        explicit ScopedMods( const mods_t &m )
        {
            GetKeyboardState( m_saved );
            BYTE state[256];
            memcpy( state, m_saved, sizeof( state ) );
            state[VK_SHIFT]   = state[VK_LSHIFT]   = m.shift ? 0x80 : 0x00;
            state[VK_CONTROL] = state[VK_LCONTROL] = m.ctrl  ? 0x80 : 0x00;
            state[VK_MENU]    = state[VK_LMENU]    = m.alt   ? 0x80 : 0x00;
            state[VK_RSHIFT]  = state[VK_RCONTROL] = state[VK_RMENU] = 0x00;
            state[VK_LWIN]    = state[VK_RWIN]     = 0x00;
            SetKeyboardState( state );
            ImGuiIO &io = ImGui::GetIO();
            m_io[0] = io.KeyShift; m_io[1] = io.KeyCtrl; m_io[2] = io.KeyAlt;
            io.KeyShift = m.shift; io.KeyCtrl = m.ctrl; io.KeyAlt = m.alt;
            // ...and KIWI's own queries (CurrentMods, KiwiCmd_CtrlHeld), which would otherwise
            // read the physical keyboard - a "0" typed while the user held Shift became ")".
            KiwiCmd_SetTestMods( ( m.shift ? 1 : 0 ) | ( m.alt ? 2 : 0 ) | ( m.ctrl ? 4 : 0 ) );
        }
        ~ScopedMods()
        {
            ImGuiIO &io = ImGui::GetIO();
            io.KeyShift = m_io[0]; io.KeyCtrl = m_io[1]; io.KeyAlt = m_io[2];
            SetKeyboardState( m_saved );
            KiwiCmd_SetTestMods( 0 );
        }
    private:
        BYTE m_saved[256];
        bool m_io[3];
    };

    // Trailing shift / ctrl / alt words from `from` on; false = an unknown word.
    bool ParseMods( const std::vector<std::string> &w, size_t from, mods_t &m, std::string &error )
    {
        for ( size_t i = from; i < w.size(); ++i )
        {
            const std::string k = Lower( w[i] );
            if ( k == "shift" ) m.shift = true;
            else if ( k == "ctrl" ) m.ctrl = true;
            else if ( k == "alt" ) m.alt = true;
            else
            {
                error = "modifier must be shift, ctrl or alt: " + w[i];
                return false;
            }
        }
        return true;
    }

    // "legacy" = the modern layer declined; the shell would hand it to CamWnd_On*.
    void VpLog( const char *what, bool taken )
    {
        KiwiTest_LogChannel( "DATA", "vp %s %s", what, taken ? "taken" : "legacy" );
    }

    // ── keys ─────────────────────────────────────────────────────────────────
    // A key name: one letter / digit, a named key, or vk<n> (a raw virtual-key code).
    bool KeyCode( const std::string &name, unsigned int &vk )
    {
        const std::string k = Lower( name );
        if ( k.size() == 1 )
        {
            const char c = (char)toupper( (unsigned char)k[0] );
            if ( ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ) { vk = (unsigned char)c; return true; }
            if ( c == '.' ) { vk = 0xBE; return true; }        // VK_OEM_PERIOD
            if ( c == '-' ) { vk = 0xBD; return true; }        // VK_OEM_MINUS
            if ( c == '[' ) { vk = 0xDB; return true; }
            if ( c == ']' ) { vk = 0xDD; return true; }
            if ( c == '/' ) { vk = 0xBF; return true; }
            if ( c == ' ' ) { vk = VK_SPACE; return true; }
            return false;
        }
        if ( k.size() > 2 && k[0] == 'v' && k[1] == 'k' )
        {
            int v = 0;
            if ( !ParseI( k.substr( 2 ), v ) || v <= 0 || v > 255 )
                return false;
            vk = (unsigned int)v;
            return true;
        }
        static const struct { const char *name; unsigned int vk; } s_named[] = {
            { "tab", VK_TAB }, { "enter", VK_RETURN }, { "return", VK_RETURN }, { "esc", VK_ESCAPE },
            { "escape", VK_ESCAPE }, { "space", VK_SPACE }, { "del", VK_DELETE }, { "delete", VK_DELETE },
            { "backspace", VK_BACK }, { "minus", 0xBD }, { "period", 0xBE }, { "lbracket", 0xDB },
            { "rbracket", 0xDD }, { "slash", 0xBF }, { "left", VK_LEFT }, { "right", VK_RIGHT },
            { "up", VK_UP }, { "down", VK_DOWN }, { "home", VK_HOME }, { "end", VK_END },
            { "f1", VK_F1 }, { "f2", VK_F2 }, { "f3", VK_F3 }, { "f4", VK_F4 }, { "f5", VK_F5 },
            { "f6", VK_F6 }, { "f7", VK_F7 }, { "f8", VK_F8 }, { "f9", VK_F9 }, { "f10", VK_F10 },
            { "f11", VK_F11 }, { "f12", VK_F12 },
        };
        for ( const auto &n : s_named )
            if ( k == n.name ) { vk = n.vk; return true; }
        return false;
    }

    // The real key path minus the OS: the funnel (modal tools, palette, typed numbers),
    // then the hotkey table, under ScopedMods.
    void InjectKey( unsigned int vk, const mods_t &m )
    {
        {
            ScopedMods scoped( m );
            if ( !KiwiUX_KeyFunnel( vk ) )
                Radiant_TryHotkey( vk );
        }
        g_nUpdateBits = -1;
    }

    void ToolState()
    {
        KiwiEditorCommand *cmd = KiwiCmd_Active();
        if ( !cmd )
        {
            KiwiTest_LogChannel( "DATA", "tool none" );
            return;
        }
        const kiwiOption_t *opts = nullptr;
        const int n = cmd->CommandOptions( &opts );
        std::string values;
        for ( int i = 0; i < n; ++i )
        {
            char buf[96];
            _snprintf( buf, sizeof( buf ), "%s%s=%d", i ? "; " : "", opts && opts[i].label ? opts[i].label : "?",
                       cmd->OptionValue( i ) );
            buf[sizeof( buf ) - 1] = '\0';
            values += buf;
        }
        const char *hud = cmd->HudStatus();
        KiwiTest_LogChannel( "DATA", "tool %s | hud %s | invalid %d | gated %d | options %s", cmd->Name(),
                             hud ? hud : "", cmd->HudInvalid() ? 1 : 0, KiwiCmd_NumericGated() ? 1 : 0,
                             values.c_str() );
    }
}

bool KiwiTestProbe_Execute( const std::vector<std::string> &w, std::string &error )
{
    if ( w.empty() )
        return false;
    const std::string verb = Lower( w[0] );
    auto need = [&]( size_t lo, size_t hi, const char *usage ) -> bool
    {
        if ( w.size() < lo || w.size() > hi )
        {
            error = std::string( "usage: " ) + usage;
            return false;
        }
        return true;
    };
    auto floats = [&]( size_t from, size_t n, float *out ) -> bool
    {
        for ( size_t i = 0; i < n; ++i )
            if ( !ParseF( w[from + i], out[i] ) )
            {
                error = "argument " + std::to_string( from + i ) + " is not a number";
                return false;
            }
        return true;
    };
    auto ints = [&]( size_t from, size_t n, int *out ) -> bool
    {
        for ( size_t i = 0; i < n; ++i )
            if ( !ParseI( w[from + i], out[i] ) )
            {
                error = "argument " + std::to_string( from + i ) + " is not an integer";
                return false;
            }
        return true;
    };

    // ── geometry ─────────────────────────────────────────────────────────────
    if ( verb == "dump" )
    {
        if ( !need( 3, 3, "dump all|selected <file.json>" ) ) return true;
        Dump( Lower( w[1] ) == "selected", w[2], error );
        return true;
    }
    if ( verb == "dump_world" )
    {
        if ( !need( 2, 2, "dump_world <file.json>" ) ) return true;
        DumpWorld( w[1], error );
        return true;
    }
    if ( verb == "validate" )
    {
        if ( !need( 2, 2, "validate all|selected" ) ) return true;
        Validate( Lower( w[1] ) == "selected" );
        return true;
    }

    // ── camera ───────────────────────────────────────────────────────────────
    if ( verb == "cam_get" )
    {
        CamReport( "cam" );
        return true;
    }
    if ( verb == "vp_down" || verb == "vp_up" || verb == "vp_click" )
    {
        if ( !need( 4, 7, "vp_down|vp_up|vp_click btn x y [shift] [ctrl] [alt]" ) ) return true;
        int v[3];
        mods_t m;
        if ( !ints( 1, 3, v ) || !ParseMods( w, 4, m, error ) ) return true;
        if ( v[0] < 0 || v[0] > 2 ) { error = "btn is 0 (LMB), 1 (RMB) or 2 (MMB)"; return true; }
        {
            ScopedMods scoped( m );
            if ( verb != "vp_up" )
                VpLog( "down", KiwiVP_CameraButtonDown( v[0], v[1], v[2], m.shift, m.ctrl ) );
            if ( verb != "vp_down" )
                VpLog( "up", KiwiVP_CameraButtonUp( v[0], v[1], v[2] ) );
        }
        CamChanged();
        return true;
    }
    if ( verb == "vp_move" )
    {
        if ( !need( 3, 6, "vp_move x y [shift] [ctrl] [alt]" ) ) return true;
        int v[2];
        mods_t m;
        if ( !ints( 1, 2, v ) || !ParseMods( w, 3, m, error ) ) return true;
        {
            ScopedMods scoped( m );
            if ( !KiwiVP_CameraMouseMove( v[0], v[1] ) )
                KiwiVP_CameraHover( v[0], v[1], true );   // no gesture: the hover pick
        }
        CamChanged();
        return true;
    }
    // KIWI (2026-10-27): the mouse-forward fly without a physical button - fly_hold 1 begins
    // it (no cursor pin / raw input), fly_hold 0 ends it; fly_raw dx dy feeds raw counts.
    if ( verb == "fly_hold" )
    {
        if ( !need( 2, 2, "fly_hold 0|1" ) ) return true;
        if ( w[1] == "1" )
        {
            if ( !KiwiVP_FlyBegin( true ) )
                error = "fly_hold: the camera is busy (another gesture is live)";
        }
        else
            KiwiVP_FlyEnd();
        KiwiTest_LogChannel( "DATA", "fly %d", KiwiVP_FlyActive() ? 1 : 0 );
        return true;
    }
    if ( verb == "fly_raw" )
    {
        if ( !need( 3, 3, "fly_raw dx dy" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        KiwiVP_FlyRawDelta( v[0], v[1] );
        KiwiVP_CameraTick( true );                 // drain it now, as the pump's tick does
        CamChanged();
        CamReport( "cam" );
        return true;
    }
    if ( verb == "vp_wheel" )
    {
        if ( !need( 4, 7, "vp_wheel steps x y [shift] [ctrl] [alt]" ) ) return true;
        float steps;
        int v[2];
        mods_t m;
        if ( !floats( 1, 1, &steps ) || !ints( 2, 2, v ) || !ParseMods( w, 4, m, error ) ) return true;
        {
            ScopedMods scoped( m );
            VpLog( "wheel", KiwiVP_CameraWheel( steps, v[0], v[1] ) );
        }
        CamChanged();
        CamReport( "cam" );
        return true;
    }
    if ( verb == "vp_drag" )
    {
        // Press, `steps` integer-pixel moves (as a mouse reports them), release - all in
        // this tick, each move through the same entry point the shell calls per frame.
        if ( !need( 6, 10, "vp_drag btn x0 y0 x1 y1 [steps] [shift] [ctrl] [alt]" ) ) return true;
        int v[5], steps = 8;
        size_t modsAt = 6;
        if ( !ints( 1, 5, v ) ) return true;
        if ( w.size() > 6 && ParseI( w[6], steps ) )
            modsAt = 7;
        mods_t m;
        if ( !ParseMods( w, modsAt, m, error ) ) return true;
        if ( v[0] < 0 || v[0] > 2 ) { error = "btn is 0 (LMB), 1 (RMB) or 2 (MMB)"; return true; }
        if ( steps < 1 ) steps = 1;
        CamReport( "before" );
        {
            ScopedMods scoped( m );
            VpLog( "down", KiwiVP_CameraButtonDown( v[0], v[1], v[2], m.shift, m.ctrl ) );
            for ( int s = 1; s <= steps; ++s )
                KiwiVP_CameraMouseMove( v[1] + ( v[3] - v[1] ) * s / steps, v[2] + ( v[4] - v[2] ) * s / steps );
            VpLog( "up", KiwiVP_CameraButtonUp( v[0], v[3], v[4] ) );
        }
        CamChanged();
        CamReport( "after" );
        return true;
    }
    // A fixed XY view so a world point is an exact pixel; the user's view comes back after.
    struct FixedXYView
    {
        xywndState_t *xy;
        xywndState_t  saved;
        FixedXYView() : xy( Ed_ActiveXY() ), saved( *xy )
        {
            xy->m_nViewType = ED_VIEW_XY;
            xy->m_fScale    = 1.0f;
            xy->m_vOrigin[0] = xy->m_vOrigin[1] = 0.0f;
            xy->m_nWidth = xy->m_nHeight = 4096;
        }
        ~FixedXYView()
        {
            xy->m_nViewType = saved.m_nViewType;
            xy->m_fScale    = saved.m_fScale;
            memcpy( xy->m_vOrigin, saved.m_vOrigin, sizeof( xy->m_vOrigin ) );
            xy->m_nWidth    = saved.m_nWidth;
            xy->m_nHeight   = saved.m_nHeight;
            g_nUpdateBits = -1;
        }
        int px( float wx ) const { return (int)floorf( wx + 0.5f ) + xy->m_nWidth / 2; }
        int py( float wy ) const { return xy->m_nHeight / 2 - (int)floorf( wy + 0.5f ); }
    };
    if ( verb == "xy_drag" )
    {
        if ( !need( 5, 8, "xy_drag x0 y0 x1 y1 [steps] [shift] [ctrl]" ) ) return true;
        float v[4];
        int steps = 8;
        size_t modsAt = 5;
        if ( !floats( 1, 4, v ) ) return true;
        if ( w.size() > 5 && ParseI( w[5], steps ) )
            modsAt = 6;
        mods_t m;
        if ( !ParseMods( w, modsAt, m, error ) ) return true;
        if ( m.alt ) { error = "xy_drag takes shift / ctrl only"; return true; }
        if ( steps < 1 ) steps = 1;
        FixedXYView view;
        const unsigned int keys = ( m.shift ? MK_SHIFT : 0 ) | ( m.ctrl ? MK_CONTROL : 0 );
        {
            ScopedMods scoped( m );
            XYWnd_OnLButtonDown( view.xy->m_hWnd, MK_LBUTTON | keys, view.px( v[0] ), view.py( v[1] ) );
            for ( int s = 1; s <= steps; ++s )
                XYWnd_OnMouseMove( MK_LBUTTON | keys, view.px( v[0] + ( v[2] - v[0] ) * s / steps ),
                                   view.py( v[1] + ( v[3] - v[1] ) * s / steps ) );
            XYWnd_OnLButtonUp( keys, view.px( v[2] ), view.py( v[3] ) );
        }
        return true;
    }
    if ( verb == "xy_hover" )
    {
        // xy_hover x y: a button-less XY mouse move (hover), in xy_drag's fixed view
        if ( !need( 3, 3, "xy_hover x y" ) ) return true;
        float v[2];
        if ( !floats( 1, 2, v ) ) return true;
        FixedXYView view;
        XYWnd_OnMouseMove( 0, view.px( v[0] ), view.py( v[1] ) );
        return true;
    }
    if ( verb == "filter" )
    {
        // filter <name> <0|1>: the Filters panel's checklist toggle (1 = shown, 0 = filtered out)
        if ( !need( 3, 3, "filter <name> <0|1>" ) ) return true;
        filter_entry_s *hit = nullptr;
        for ( int c = 0; c < 4 && !hit; ++c )
            for ( filter_entry_s *f = CFilterWnd_GetCategoryHead( c ); f; f = f->next_filter )
                if ( f->name && !_stricmp( f->name, w[1].c_str() ) ) { hit = f; break; }
        if ( !hit ) { KiwiTest_LogChannel( "ERROR", "filter: no entry %s", w[1].c_str() ); return true; }
        RadiantFilters_ToggleEntry( hit, w[2] != "0" );
        KiwiTest_LogChannel( "DATA", "filter %s shown %d", hit->name, hit->isShown ? 1 : 0 );
        return true;
    }
    if ( verb == "filtered" )
    {
        // filtered e b: FilterBrush( node, 1 ) for that saved brush (1 = hidden)
        if ( !need( 3, 3, "filtered e b" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        brush_t *def = nullptr;
        ForEachSavedBrush( [&]( int e, entity_s *, int b, brush_t *d ) { if ( e == v[0] && b == v[1] ) def = d; } );
        for ( selbrush_t *list : { &active_brushes, &selected_brushes } )
            for ( selbrush_t *n = list->next; def && n != list; n = n->next )
                if ( n->def == def )
                {
                    KiwiTest_LogChannel( "DATA", "filtered %d %d %d", v[0], v[1], FilterBrush( n, 1 ) ? 1 : 0 );
                    return true;
                }
        KiwiTest_LogChannel( "ERROR", "filtered: no live node for %d %d", v[0], v[1] );
        return true;
    }
    if ( verb == "cam_mode" )
    {
        // cam_mode: the camera's draw mode (0 wire .. 4 textures) and the stored menu id
        KiwiTest_LogChannel( "DATA", "cam_mode %d menu %d", Ed_Camera()->draw_mode,
                             g_qeglobals.d_savedinfo.iTextMenu );
        return true;
    }
    if ( verb == "clip_state" )
    {
        // clip_state: clip mode and the three clip points ("clip_pt <n> <set> x y z")
        KiwiTest_LogChannel( "DATA", "clip_mode %d", g_bClipMode ? 1 : 0 );
        const CClipPoint *pts[3] = { &g_Clip1, &g_Clip2, &g_Clip3 };
        for ( int i = 0; i < 3; ++i )
            KiwiTest_LogChannel( "DATA", "clip_pt %d %d %g %g %g", i + 1, pts[i]->m_bSet ? 1 : 0,
                                 pts[i]->m_ptClip[0], pts[i]->m_ptClip[1], pts[i]->m_ptClip[2] );
        return true;
    }
    if ( verb == "patch_edit" )
    {
        if ( !need( 2, 2, "patch_edit 0|1" ) ) return true;
        if ( w[1] != "0" )
            Patch_EditPatch();
        else
            g_qeglobals.d_select_mode = sel_brush;
        KiwiTest_LogChannel( "DATA", "patch_edit mode %d points %d", (int)g_qeglobals.d_select_mode,
                             g_qeglobals.d_numpoints );
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "cam_ortho" )
    {
        if ( !need( 2, 2, "cam_ortho 0|1" ) ) return true;
        KiwiCam_SetOrtho( w[1] != "0" );
        CamChanged();
        CamReport( "cam" );
        return true;
    }
    if ( verb == "cam_frame" )
    {
        if ( !need( 7, 7, "cam_frame x0 y0 z0 x1 y1 z1" ) ) return true;
        float v[6];
        if ( !floats( 1, 6, v ) ) return true;
        KiwiCam_FrameBounds( v, v + 3 );
        CamChanged();
        CamReport( "cam" );
        return true;
    }
    if ( verb == "pick" )
    {
        if ( !need( 3, 4, "pick x y [object|face|edge|vertex|all]" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        sel_mask_t mask = SEL_MASK_OBJECT | SEL_MASK_FACE;
        if ( w.size() == 4 )
        {
            const std::string m = Lower( w[3] );
            mask = m == "object" ? SEL_MASK_OBJECT : m == "face" ? SEL_MASK_FACE : m == "edge" ? SEL_MASK_EDGE
                 : m == "vertex" ? SEL_MASK_VERTEX : SEL_MASK_EVERYTHING;
        }
        ray_t ray;
        if ( !Pick_RayFromImagePos( v[0], v[1], &ray ) )
        {
            error = "pick: no camera ray (viewport not sized yet?)";
            return true;
        }
        const pick_result_t r = Pick( ray, mask );
        if ( !r.valid )
        {
            KiwiTest_LogChannel( "DATA", "pick none" );
            return true;
        }
        int e = -1, b = -1;
        if ( r.item.brush && r.item.brush->def )
        {
            const auto index = IndexBrushes();
            const auto it = index.find( r.item.brush->def );
            if ( it != index.end() ) { e = it->second.first; b = it->second.second; }
        }
        static const char *const s_kind[] = { "vertex", "edge", "face", "object" };
        const int kind = (int)r.item.kind;
        KiwiTest_LogChannel( "DATA", "pick %s e %d b %d face %d point %.6f %.6f %.6f px %.3f",
                             kind >= 0 && kind < 4 ? s_kind[kind] : "other", e, b, r.item.faceIndex,
                             r.point[0], r.point[1], r.point[2], r.screenDist );
        return true;
    }
    if ( verb == "project" )
    {
        if ( !need( 4, 4, "project x y z" ) ) return true;
        float p[3], x, y;
        if ( !floats( 1, 3, p ) ) return true;
        if ( Pick_WorldToImage( p, &x, &y ) )
            KiwiTest_LogChannel( "DATA", "project %.3f %.3f", x, y );
        else
            KiwiTest_LogChannel( "DATA", "project off" );
        return true;
    }
    if ( verb == "screenshot" )
    {
        if ( !need( 3, 3, "screenshot camera|xy <file.png>" ) ) return true;
        IDirect3DTexture9 *tex = RTT_GetTexture( Lower( w[1] ) == "xy" ? RTT_XY : RTT_CAMERA );
        IDirect3DSurface9 *surf = nullptr;
        if ( !tex || FAILED( tex->GetSurfaceLevel( 0, &surf ) ) || !surf )
        {
            error = "screenshot: that view has no render target yet";
            return true;
        }
        const HRESULT hr = D3DXSaveSurfaceToFileA( w[2].c_str(), D3DXIFF_PNG, surf, nullptr, nullptr );
        surf->Release();
        if ( FAILED( hr ) )
            error = "screenshot: D3DXSaveSurfaceToFile failed";
        else
            KiwiTest_LogChannel( "DATA", "screenshot %s", w[2].c_str() );
        return true;
    }

    // ── modal tools, driven like the viewport drives them ─────────────────────
    if ( verb == "kstart" )
    {
        // (a bare id: an unquoted # starts a comment everywhere but after `cmd`)
        if ( !need( 2, 2, "kstart <command id>" ) ) return true;
        int id = 0;
        if ( !ParseI( w[1], id ) )
        {
            error = "kstart takes a numeric command id";
            return true;
        }
        if ( !KiwiCmd_Start( id ) )
            error = "kstart: the command refused to start (CanExecute / Begin)";
        g_nUpdateBits = -1;
        KiwiTest_WaitFrames( 1 );
        return true;
    }
    if ( verb == "kmove" || verb == "kpress" || verb == "khandle" )
    {
        if ( !need( 3, 4, "kmove|kpress|khandle x y [shift]" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        const bool shift = w.size() == 4 && Lower( w[3] ) == "shift";
        if ( verb == "kmove" )
            KiwiCmd_MouseMove( v[0], v[1] );
        else if ( verb == "kpress" )
        {
            if ( !KiwiCmd_MouseButton( 0, v[0], v[1], shift ) )
                error = "kpress: no live tool took the press";
        }
        else if ( !KiwiCmd_HandleGrab( v[0], v[1] ) )
            error = "khandle: no lollipop handle there";
        g_nUpdateBits = -1;
        KiwiTest_WaitFrames( 1 );
        return true;
    }
    if ( verb == "khandle_release" )
    {
        KiwiCmd_HandleRelease();
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "kkey" )
    {
        if ( !need( 2, 5, "kkey <key> [shift] [ctrl] [alt]" ) ) return true;
        unsigned int vk = 0;
        if ( !KeyCode( w[1], vk ) )
        {
            error = "kkey: unknown key " + w[1];
            return true;
        }
        mods_t m;
        if ( !ParseMods( w, 2, m, error ) ) return true;
        InjectKey( vk, m );
        KiwiTest_WaitFrames( 1 );
        return true;
    }
    if ( verb == "ktype" )
    {
        if ( !need( 2, 2, "ktype <text>" ) ) return true;
        for ( char c : w[1] )
        {
            unsigned int vk = 0;
            if ( !KeyCode( std::string( 1, c ), vk ) )
            {
                error = std::string( "ktype: cannot type '" ) + c + "'";
                return true;
            }
            InjectKey( vk, mods_t() );
        }
        KiwiTest_WaitFrames( 1 );
        return true;
    }
    if ( verb == "uv_affine" )
    {
        if ( !need( 5, 5, "uv_affine a b c d" ) ) return true;
        float m[4];
        if ( !floats( 1, 4, m ) ) return true;
        if ( !KiwiUvEd_TestAffine( m[0], m[1], m[2], m[3] ) )
            error = "uv_affine: nothing selected";
        g_nUpdateBits = -1;
        KiwiTest_WaitFrames( 1 );
        return true;
    }
    if ( verb == "merge_coplanar" )
    {
        // KiwiValid_MergeCoplanarFaces on one brush (the vertex-G fold merge); logs the count.
        if ( !need( 3, 3, "merge_coplanar e b" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        brush_t *def = nullptr;
        ForEachSavedBrush( [&]( int e, entity_s *, int b, brush_t *d ) { if ( e == v[0] && b == v[1] ) def = d; } );
        if ( !def )
        {
            error = "merge_coplanar: no such brush";
            return true;
        }
        KiwiTest_LogChannel( "DATA", "merged %d", KiwiValid_MergeCoplanarFaces( def ) );
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "face_align" )
    {
        // Drag_FaceAlign's projection: dst face takes src face's texture (brush ordinals as in dump).
        if ( !need( 7, 7, "face_align e b f e b f" ) ) return true;
        int v[6];
        if ( !ints( 1, 6, v ) ) return true;
        brush_t *src = nullptr, *dst = nullptr;
        ForEachSavedBrush( [&]( int e, entity_s *, int b, brush_t *def )
        {
            if ( e == v[0] && b == v[1] ) src = def;
            if ( e == v[3] && b == v[4] ) dst = def;
        } );
        if ( !src || !dst || v[2] < 0 || v[2] >= src->faceCount || v[5] < 0 || v[5] >= dst->faceCount )
        {
            error = "face_align: no such brush / face";
            return true;
        }
        Brush_ApplyTextureProjection( &src->faces[v[2]], dst, &dst->faces[v[5]] );
        Brush_BuildWindings( dst, 1 );
        g_nUpdateBits = -1;
        KiwiTest_WaitFrames( 1 );
        return true;
    }
    if ( verb == "tex_pick" || verb == "tex_apply_brush" )
    {
        // Drag_Begin's camera MMB pick / Ctrl+MMB whole-brush apply (drag.cpp branches 1 and 2);
        // modern input gives MMB to the orbit, so vp_click cannot reach them.
        if ( !need( 3, 3, "tex_pick|tex_apply_brush e b" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        brush_t *def = nullptr;
        ForEachSavedBrush( [&]( int e, entity_s *, int b, brush_t *d ) { if ( e == v[0] && b == v[1] ) def = d; } );
        if ( !def || def->faceCount <= 0 )
        {
            error = verb + ": no such brush";
            return true;
        }
        const int layer = g_qeglobals.current_edit_layer;
        if ( verb == "tex_pick" )
        {
            MaterialDef *md = &def->faces[0].mtldef[layer];
            if ( def->patch )
                Radiant_PatchGetTexdef( def->patch, &md->mat_texDef + LayerMat::GetCurrentLayer( md ) );
            Texture_SetTexture( (const int *)def->patch, md );
        }
        else
        {
            Brush_SetAllFacesMaterial( def, (MaterialDef *)&g_qeglobals.random_texture_stuff[layer], 1, 1.0f, 0 );
        }
        g_nUpdateBits = -1;
        KiwiTest_WaitFrames( 1 );
        return true;
    }
    if ( verb == "kopt" )
    {
        if ( !need( 3, 3, "kopt <row> <value>" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        KiwiEditorCommand *cmd = KiwiCmd_Active();
        const kiwiOption_t *opts = nullptr;
        if ( !cmd || v[0] < 0 || v[0] >= cmd->CommandOptions( &opts ) )
        {
            error = "kopt: no live tool or no such option row";
            return true;
        }
        cmd->OptionChanged( v[0], v[1] );
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "kstate" )
    {
        ToolState();
        return true;
    }
    if ( verb == "kconfirm" )
    {
        if ( !KiwiCmd_Active() )
            error = "kconfirm: no live tool";
        else
            KiwiCmd_Confirm();
        g_nUpdateBits = -1;
        KiwiTest_WaitFrames( 1 );
        return true;
    }

    // ── preferences (the isolated test profile; see KiwiTest_IsolateProfile) ────
    if ( verb == "pref" )
    {
        if ( !need( 2, 3, "pref noclamp|grid|lightmaplock|texlock|circlesides|roundpatch [value]" ) ) return true;
        const std::string what = Lower( w[1] );
        if ( what == "roundpatch" )
        {
            // KIWI (2026-10-31): Extrude/Cylinder patch mode (the P toggle), saved per profile.
            if ( w.size() == 3 )
                KiwiPrim_SetPatchMode( w[2] != "0" );
            KiwiTest_LogChannel( "DATA", "pref roundpatch %d", KiwiPrim_PatchMode() ? 1 : 0 );
            return true;
        }
        if ( what == "circlesides" )
        {
            // KIWI (2026-10-31): the sides a circle - and an outline's arcs - extrude with.
            if ( w.size() == 3 )
            {
                int sides;
                if ( !ints( 2, 1, &sides ) ) return true;
                Radiant_ProfileSetInt( "KiwiUX", "CircleExtrudeSides", sides );
            }
            KiwiTest_LogChannel( "DATA", "pref circlesides %d",
                                 Radiant_ProfileGetInt( "KiwiUX", "CircleExtrudeSides", 16 ) );
            return true;
        }
        if ( what == "texlock" )
        {
            if ( w.size() == 3 )
                g_PrefsDlg->m_bTextureLock = w[2] != "0";
            KiwiTest_LogChannel( "DATA", "pref texlock %d", g_PrefsDlg->m_bTextureLock ? 1 : 0 );
            return true;
        }
        if ( what == "lightmaplock" )
        {
            if ( w.size() == 3 )
                g_PrefsDlg->m_bLightmapLock = w[2] != "0";
            KiwiTest_LogChannel( "DATA", "pref lightmaplock %d", g_PrefsDlg->m_bLightmapLock ? 1 : 0 );
            return true;
        }
        if ( what == "noclamp" )
        {
            if ( w.size() == 3 )
                g_PrefsDlg->m_bNoClamp = w[2] != "0";
            KiwiTest_LogChannel( "DATA", "pref noclamp %d", g_PrefsDlg->m_bNoClamp ? 1 : 0 );
            return true;
        }
        if ( what == "grid" )
        {
            if ( w.size() == 3 )
            {
                float inches;
                if ( !floats( 2, 1, &inches ) ) return true;
                if ( !KiwiCmd_SetGridSpacing( inches ) )
                {
                    error = "pref grid: not a grid step";
                    return true;
                }
            }
            KiwiTest_LogChannel( "DATA", "pref grid %g", grid_sizes[g_qeglobals.d_gridsize] );
            return true;
        }
        if ( what == "units" || what == "gridin" )
        {
            // the modern unit settings through their setters (the prefs fields' path);
            // `pref units reload` re-reads both from the profile
            if ( w.size() == 3 )
            {
                if ( what == "units" && Lower( w[2] ) == "reload" )
                    KiwiUnits_ReloadForTest();
                else if ( what == "units" )
                    KiwiUnits_SetPerInch( (float)strtod( w[2].c_str(), nullptr ) );
                else
                    KiwiUnits_SetGridSpacingInches( (float)strtod( w[2].c_str(), nullptr ) );
            }
            KiwiTest_LogChannel( "DATA", "pref units %g gridin %g", (double)KiwiUnits_PerInch(),
                                 (double)KiwiUnits_GridSpacingInches() );
            return true;
        }
        error = "pref: noclamp, grid, lightmaplock, texlock, units or gridin";
        return true;
    }
    if ( verb == "profile_get" )
    {
        if ( !need( 3, 3, "profile_get <section> <key>" ) ) return true;
        KiwiTest_LogChannel( "DATA", "profile %s %s %s", w[1].c_str(), w[2].c_str(),
                             Radiant_ProfileGetString( w[1].c_str(), w[2].c_str(), "" ).c_str() );
        return true;
    }
    if ( verb == "profile_set" )
    {
        if ( !need( 4, 4, "profile_set <section> <key> <int>" ) ) return true;
        int v;
        if ( !ints( 3, 1, &v ) ) return true;
        Radiant_ProfileSetInt( w[1].c_str(), w[2].c_str(), v );
        return true;
    }
    if ( verb == "profile_setstr" )
    {
        if ( !need( 4, 4, "profile_setstr <section> <key> <string>" ) ) return true;
        Radiant_ProfileSetString( w[1].c_str(), w[2].c_str(), w[3].c_str() );
        return true;
    }
    if ( verb == "raw_write" )
    {
        // raw_write <qpath> <text>: KiwiFS_WriteRawAtomic (the material / .iwi writers' path)
        if ( !need( 3, 3, "raw_write <qpath> <text>" ) ) return true;
        char err[256] = { 0 };
        const bool ok = KiwiFS_WriteRawAtomic( w[1].c_str(), w[2].c_str(), (unsigned)w[2].size(), err, sizeof( err ) );
        KiwiTest_LogChannel( "DATA", "raw_write %d %s", ok ? 1 : 0, ok ? "" : err );
        return true;
    }
    if ( verb == "iwi_write" )
    {
        // iwi_write <source image> <qpath>: KiwiIwi_WriteFromFile (ARGB8) then KiwiIwi_VerifyOnDisk
        if ( !need( 3, 3, "iwi_write <source image> <qpath>" ) ) return true;
        kiwiIwiOptions_t opt = {};
        opt.encoding = KIWI_IWI_ENC_COLOR;
        kiwiIwiResult_t res = {};
        char err[256] = { 0 };
        bool ok = KiwiIwi_WriteFromFile( w[1].c_str(), w[2].c_str(), &opt, &res, err, sizeof( err ) );
        if ( ok )
            ok = KiwiIwi_VerifyOnDisk( w[2].c_str(), err, sizeof( err ) );
        KiwiTest_LogChannel( "DATA", "iwi_write %d %dx%d %d %s", ok ? 1 : 0, res.width, res.height,
                             res.fileSize, ok ? "" : err );
        return true;
    }

    // ── construction store / command gates ──────────────────────────────────────
    if ( verb == "con_state" )
    {
        // one DATA line: count, construction undo depth, selected objects; then one per object
        std::string sel;
        for ( int i = 0; i < KiwiConSel_Count(); ++i )
        {
            const kconSelItem_t *it = KiwiConSel_At( i );
            if ( it && it->kind == KCONSEL_OBJECT )
                sel += ( sel.empty() ? "" : "," ) + std::to_string( it->object );
        }
        KiwiTest_LogChannel( "DATA", "con_state %d %d %s", KiwiCon_Count(), KiwiCon_UndoDepth(),
                             sel.empty() ? "-" : sel.c_str() );
        for ( int i = 0; i < KiwiCon_Count(); ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // KIWI (2026-10-31): circles and arcs also report radius, angles and sample count.
            KiwiTest_LogChannel( "DATA", "con %d type %d pts %d closed %d group %d name %s radius %.6f ang %.6f %.6f verts %d", i,
                                 (int)o->type, (int)( o->pts.size() / 3 ), o->closed ? 1 : 0, o->group,
                                 o->name.empty() ? "-" : o->name.c_str(), o->radius, o->ang0, o->ang1,
                                 KiwiCon_VertCount( *o ) );
        }
        return true;
    }
    // KIWI (2026-10-27): the derived construction faces and an object's world points, for
    // the construction suite (tools/kiwitest/gauntlet_construct.py).
    if ( verb == "reg_state" )
    {
        // one DATA line: region count; then one per region (plane-space area, world centroid)
        const std::vector<kregion_t> &regs = KiwiRegion_All();
        KiwiTest_LogChannel( "DATA", "reg_state %d", (int)regs.size() );
        for ( size_t i = 0; i < regs.size(); ++i )
        {
            const kregion_t &r = regs[i];
            KiwiTest_LogChannel( "DATA", "reg %d pts %d area %.6f normal %.6f %.6f %.6f centroid "
                                 "%.6f %.6f %.6f src %d holes %d", (int)i,
                                 (int)( r.pts.size() / 2 ), fabsf( KiwiRegion_SignedArea( r.pts ) ),
                                 r.plane.normal[0], r.plane.normal[1], r.plane.normal[2],
                                 r.centroid[0], r.centroid[1], r.centroid[2], r.sourceObject,
                                 (int)r.holes.size() );
        }
        return true;
    }
    if ( verb == "con_pts" )
    {
        // con_pts <index>: the object's stored world points, one DATA line
        if ( !need( 2, 2, "con_pts <index>" ) ) return true;
        int index;
        if ( !ints( 1, 1, &index ) ) return true;
        const kconObject_t *o = KiwiCon_At( index );
        if ( !o )
        {
            error = "con_pts: no such construction object";
            return true;
        }
        std::string s;
        char buf[64];
        for ( size_t k = 0; k < o->pts.size(); ++k )
        {
            _snprintf( buf, sizeof( buf ), " %.6f", o->pts[k] );
            buf[sizeof( buf ) - 1] = '\0';
            s += buf;
        }
        KiwiTest_LogChannel( "DATA", "con_pts %d%s", index, s.c_str() );
        return true;
    }
    if ( verb == "con_label" )
    {
        // con_label <index> <name> <group name>: name the object and put it in a new group
        if ( !need( 4, 4, "con_label <index> <name> <group name>" ) ) return true;
        int index;
        if ( !ints( 1, 1, &index ) ) return true;
        if ( !KiwiCon_At( index ) )
        {
            error = "con_label: no such construction object";
            return true;
        }
        KiwiCon_SetName( index, w[2].c_str() );
        KiwiCon_SetGroup( index, KiwiCon_NewGroup( w[3].c_str() ) );
        return true;
    }
    if ( verb == "con_select" )
    {
        // con_select <index> [index ...]: replace the construction selection (whole objects)
        if ( !need( 2, 9, "con_select <index> [index ...]" ) ) return true;
        std::vector<int> objs( w.size() - 1 );
        if ( !ints( 1, objs.size(), &objs[0] ) ) return true;
        KiwiConSel_SelectObjects( &objs[0], (int)objs.size() );
        return true;
    }
    if ( verb == "face_add" )
    {
        // face_add e b f: add one face of that brush to the face selection (keeps the rest)
        if ( !need( 4, 4, "face_add e b f" ) ) return true;
        int v[3];
        if ( !ints( 1, 3, v ) ) return true;
        brush_t *def = nullptr;
        ForEachSavedBrush( [&]( int e, entity_s *, int b, brush_t *d ) { if ( e == v[0] && b == v[1] ) def = d; } );
        selbrush_t *node = nullptr;
        for ( selbrush_t *list : { &selected_brushes, &active_brushes } )
            for ( selbrush_t *n = list->next; def && !node && n != list; n = n->next )
                if ( n->def == def )
                    node = n;
        if ( !node || v[2] < 0 || v[2] >= def->faceCount )
        {
            error = "face_add: no such brush face";
            return true;
        }
        Sel_Add( KiwiSel(), Sel_MakeFace( node, v[2] ) );
        Sel_SyncToLegacy();
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "focus_click" )
    {
        // KIWI (2026-10-27): focus_click e b f - record an object click on that face's centre,
        // as a camera click on a whole brush does (KiwiFocus_NoteObjectClick); brush-mode
        // Ctrl+R splits across the face the selecting click landed on.
        if ( !need( 4, 4, "focus_click e b f" ) ) return true;
        int v[3];
        if ( !ints( 1, 3, v ) ) return true;
        brush_t *def = nullptr;
        ForEachSavedBrush( [&]( int e, entity_s *, int b, brush_t *d ) { if ( e == v[0] && b == v[1] ) def = d; } );
        selbrush_t *node = nullptr;
        for ( selbrush_t *list : { &selected_brushes, &active_brushes } )
            for ( selbrush_t *n = list->next; def && !node && n != list; n = n->next )
                if ( n->def == def )
                    node = n;
        if ( !node || !def->faces || v[2] < 0 || v[2] >= def->faceCount || !def->faces[v[2]].w
          || def->faces[v[2]].w->numpoints < 3 )
        {
            error = "focus_click: no such brush face";
            return true;
        }
        const winding_t *wd = def->faces[v[2]].w;
        float c[3] = { 0.0f, 0.0f, 0.0f };
        for ( int i = 0; i < wd->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
                c[k] += wd->p[i][k] / (float)wd->numpoints;
        KiwiFocus_NoteObjectClick( node, c, def->faces[v[2]].plane.normal );
        return true;
    }
    if ( verb == "simple_patch" )
    {
        // simple_patch <wSel> <hSel>: Curve > Simple Patch Mesh's Create on the selected brush
        // (PatchDensity_Commit; sel indexes {3,5,7,9,11,13,15}) - a flat, open patch
        if ( !need( 3, 3, "simple_patch <wSel> <hSel>" ) ) return true;
        int v[2];
        if ( !ints( 1, 2, v ) ) return true;
        extern void PatchDensity_Commit( bool terrain, int wSel, int hSel );   // patchdialog.cpp
        PatchDensity_Commit( false, v[0], v[1] );
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "sg_flags" )
    {
        // sg_flags <key>: the Script group panel's flag list (ScriptGroupFlags_Gather), one DATA
        // line per row, then the count
        if ( !need( 2, 2, "sg_flags <key>" ) ) return true;
        extern void ScriptGroupFlags_Gather( const char *key, std::vector<std::string> &rows );
        std::vector<std::string> rows;
        ScriptGroupFlags_Gather( w[1].c_str(), rows );
        for ( size_t i = 0; i < rows.size(); ++i )
            KiwiTest_LogChannel( "DATA", "sg_flag %s", rows[i].c_str() );
        KiwiTest_LogChannel( "DATA", "sg_flags %d", (int)rows.size() );
        return true;
    }
    if ( verb == "cap_commit" )
    {
        // cap_commit <type 0..3>: the open-patch cap prompt's OK (Cap_Commit), no prompt
        if ( !need( 2, 2, "cap_commit <type 0..3>" ) ) return true;
        int type;
        if ( !ints( 1, 1, &type ) ) return true;
        extern void Cap_Commit( int nType );           // pmesh.cpp
        Cap_Commit( type );
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "layer_hide" )
    {
        // layer_hide <name> <0|1>: the Layers panel's Hide/Show (Layers_SetHidden), then one
        // "layer_filter <name> <shown>" DATA line per script_layer visibility filter
        if ( !need( 3, 3, "layer_hide <name> <0|1>" ) ) return true;
        int hide;
        if ( !ints( 2, 1, &hide ) ) return true;
        extern bool Layers_SetHidden( const char *layerName, bool hidden );   // layersdlg.cpp
        KiwiTest_LogChannel( "DATA", "layer_hide %d", Layers_SetHidden( w[1].c_str(), hide != 0 ) ? 1 : 0 );
        for ( filter_entry_s *f = g_qeglobals.d_filterGlobals_layerFilters; f; f = f->next_filter )
            KiwiTest_LogChannel( "DATA", "layer_filter %s %d", f->name ? f->name : "", f->isShown ? 1 : 0 );
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "find_replace" )
    {
        // find_replace <find> <replace> <flags>: Find/Replace's core (FindReplaceTextures) and
        // the map dirty flag around it
        if ( !need( 4, 4, "find_replace <find> <replace> <flags>" ) ) return true;
        int flags;
        if ( !ints( 3, 1, &flags ) ) return true;
        extern bool FindReplaceTextures( const char *find, const char *replace, char flags );  // findtexture.cpp
        const int before = modified ? 1 : 0;
        const bool hit = FindReplaceTextures( w[1].c_str(), w[2].c_str(), (char)flags );
        KiwiTest_LogChannel( "DATA", "find_replace %d modified %d %d", hit ? 1 : 0, before, modified ? 1 : 0 );
        return true;
    }
    if ( verb == "sg_select" || verb == "sg_dump" )
    {
        // sg_select <entity> [entity ...]: replace the selection with those saved entities' nodes
        // (point entities included).  sg_dump: every saved entity's epairs, in list order, as
        // "sg_kv <entity> <sel> |key|=|value|" (the bars keep trailing spaces visible).
        std::vector<entity_s *> saved;
        size_t guard = 0;
        for ( entity_s *ent = entities.next; ent && ent != &entities && guard < 1000000; ent = ent->next, ++guard )
            if ( EntityIsSaved( ent ) )
                saved.push_back( ent );
        if ( verb == "sg_select" )
        {
            if ( !need( 2, 64, "sg_select <entity> [entity ...]" ) ) return true;
            Select_Deselect( 1 );
            std::vector<selbrush_t *> nodes;
            for ( size_t i = 1; i < w.size(); ++i )
            {
                int idx;
                if ( !ints( i, 1, &idx ) ) return true;
                if ( idx < 0 || (size_t)idx >= saved.size() )
                {
                    KiwiTest_LogChannel( "ERROR", "sg_select: no saved entity %d", idx );
                    return true;
                }
                for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
                    if ( b->owner && b->owner->def == saved[idx] )
                        nodes.push_back( b );
            }
            for ( selbrush_t *b : nodes )
                Select_Brush( b, 0, 0, 0 );
            g_nUpdateBits = -1;
            return true;
        }
        for ( size_t i = 0; i < saved.size(); ++i )
        {
            bool sel = false;
            for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
                if ( b->owner && b->owner->def == saved[i] ) { sel = true; break; }
            for ( epair_t *ep = saved[i]->epairs; ep; ep = ep->next )
                KiwiTest_LogChannel( "DATA", "sg_kv %d %d |%s|=|%s|", (int)i, sel ? 1 : 0, ep->key, ep->value );
        }
        return true;
    }
    if ( verb == "sg_op" )
    {
        // sg_op <op> [a] [b]: the Script group tool's actions -
        //   code <c> | team <key> | add | disassociate | type
        //   addkey <k> <v> | removekey <k> <v> | addtrig <k> <v> | removetrig <k> <v>
        if ( !need( 2, 4, "sg_op <op> [a] [b]" ) ) return true;
        const std::string &op = w[1];
        const char *a = w.size() > 2 ? w[2].c_str() : "";
        const char *b = w.size() > 3 ? w[3].c_str() : "";
        extern void ScriptGroupColorCode_Apply( const char *code );                     // scriptgroup.cpp
        extern void ScriptGroupTeam_Apply( const char *teamKey );
        extern void ScriptGroup_AddColorToSelection();
        extern void ScriptGroupDisassociate_Apply();
        extern void ScriptGroup_Type();
        extern void ScriptGroupAddSubKey_Apply( const char *subKey, const char *subValue );
        extern void ScriptGroupRemoveSubKey_Apply( const char *subKey, const char *subValue );
        extern int  ScriptGroup_AddKeyToSelectedTriggers( const char *key, const char *value );
        extern int  ScriptGroup_RemoveKeyFromSelectedTriggers( const char *key, const char *value );
        if      ( op == "code" )         ScriptGroupColorCode_Apply( a );
        else if ( op == "team" )         ScriptGroupTeam_Apply( a );
        else if ( op == "add" )          ScriptGroup_AddColorToSelection();
        else if ( op == "disassociate" ) ScriptGroupDisassociate_Apply();
        else if ( op == "type" )         ScriptGroup_Type();
        else if ( op == "addkey" )       ScriptGroupAddSubKey_Apply( a, b );
        else if ( op == "removekey" )    ScriptGroupRemoveSubKey_Apply( a, b );
        else if ( op == "addtrig" )      ScriptGroup_AddKeyToSelectedTriggers( a, b );
        else if ( op == "removetrig" )   ScriptGroup_RemoveKeyFromSelectedTriggers( a, b );
        else
        {
            KiwiTest_LogChannel( "ERROR", "sg_op: unknown op %s", op.c_str() );
            return true;
        }
        g_nUpdateBits = -1;
        return true;
    }
    if ( verb == "snapshot" )
    {
        // snapshot <path>: the live fragile check's map write (Map_WriteSnapshot)
        if ( !need( 2, 2, "snapshot <path>" ) ) return true;
        extern bool Map_WriteSnapshot( const char *path );   // map.cpp
        KiwiTest_LogChannel( "DATA", "snapshot %d", Map_WriteSnapshot( w[1].c_str() ) ? 1 : 0 );
        return true;
    }
    if ( verb == "undo_depth" )
    {
        // the unified undo journal's depth (KiwiUndo_UndoDepth)
        extern int KiwiUndo_UndoDepth();               // kiwi_undo.cpp
        KiwiTest_LogChannel( "DATA", "undo_depth %d", KiwiUndo_UndoDepth() );
        return true;
    }
    if ( verb == "can" )
    {
        // can <command id>: the palette predicate (KiwiCmd_CanExecute)
        if ( !need( 2, 2, "can <command id>" ) ) return true;
        int id;
        if ( !ints( 1, 1, &id ) ) return true;
        KiwiTest_LogChannel( "DATA", "can %d %d", id, KiwiCmd_CanExecute( id ) ? 1 : 0 );
        return true;
    }

    // ── keybinds (KIWI 2026-10-27: the Command List editor, kiwi_keymap.h) ──────────
    //   keybind_get <name>                 DATA keybind <name> <id> <custom> <chords|none>
    //   keybind_set <name> <chord> [slot]  slot = index to replace, default: add
    //   keybind_remove <name> <slot>       keybind_reset_cmd <name>
    //   keybind_reset                      Reset to Default (Modern + both user sections)
    //   keybind_reload                     re-read every layer from disk (a restart)
    //   keybind_who <chord>                DATA keybind_who <chord> <winner id> <reserved?>
    if ( verb.compare( 0, 8, "keybind_" ) == 0 )
    {
        auto lookup = [&]( const std::string &name ) -> int
        {
            const int id = KiwiKeybind_FindCommand( name.c_str() );
            if ( !id )
                error = "no command named " + name;
            return id;
        };
        auto chordArg = [&]( const std::string &text, kiwiChord_t *c ) -> bool
        {
            if ( KiwiKeybind_Parse( text.c_str(), c ) && c->vk )
                return true;
            error = "not a chord: " + text;
            return false;
        };
        if ( verb == "keybind_get" )
        {
            if ( !need( 2, 2, "keybind_get <name>" ) ) return true;
            const int id = lookup( w[1] );
            if ( !id ) return true;
            kiwiChord_t chords[KIWI_KEYBIND_MAX * 4];
            const int n = KiwiKeybind_Get( id, chords, KIWI_KEYBIND_MAX * 4 );
            std::string list;
            for ( int k = 0; k < n; ++k )
            {
                char text[64];
                KiwiKeybind_Format( chords[k], text, sizeof( text ) );
                if ( k ) list += ",";
                list += text;
            }
            KiwiTest_LogChannel( "DATA", "keybind %s %d %d %s", w[1].c_str(), id,
                                 KiwiKeybind_IsCustom( id ) ? 1 : 0, n ? list.c_str() : "none" );
            return true;
        }
        if ( verb == "keybind_set" )
        {
            if ( !need( 3, 4, "keybind_set <name> <chord> [slot]" ) ) return true;
            const int id = lookup( w[1] );
            kiwiChord_t c;
            if ( !id || !chordArg( w[2], &c ) ) return true;
            int slot = -1;
            if ( w.size() == 4 && !ints( 3, 1, &slot ) ) return true;
            if ( const char *why = KiwiKeybind_Reserved( c, id ) )
            {
                error = std::string( "reserved: " ) + why;
                return true;
            }
            if ( !KiwiKeybind_Assign( id, slot, c ) )
                error = "assign failed";
            return true;
        }
        if ( verb == "keybind_remove" )
        {
            if ( !need( 3, 3, "keybind_remove <name> <slot>" ) ) return true;
            const int id = lookup( w[1] );
            int slot;
            if ( !id || !ints( 2, 1, &slot ) ) return true;
            KiwiKeybind_Remove( id, slot );
            return true;
        }
        if ( verb == "keybind_reset_cmd" )
        {
            if ( !need( 2, 2, "keybind_reset_cmd <name>" ) ) return true;
            const int id = lookup( w[1] );
            if ( id ) KiwiKeybind_ResetCommand( id );
            return true;
        }
        if ( verb == "keybind_reset" )
        {
            KiwiKeybind_ResetAll();
            return true;
        }
        if ( verb == "keybind_reload" )
        {
            KiwiKeybind_Reload();
            return true;
        }
        if ( verb == "keybind_who" )
        {
            if ( !need( 2, 2, "keybind_who <chord>" ) ) return true;
            kiwiChord_t c;
            if ( !chordArg( w[1], &c ) ) return true;
            const char *why = KiwiKeybind_Reserved( c, 0 );
            KiwiTest_LogChannel( "DATA", "keybind_who %s %d %s", w[1].c_str(),
                                 KiwiKeybind_Winner( c ), why ? "reserved" : "free" );
            return true;
        }
    }
    return false;
}
