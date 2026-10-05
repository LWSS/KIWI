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
//   pick x y [object|face|edge|vertex|all] project x y z
//   screenshot camera|xy <file.png>
//   kstart <id>                            kmove x y      kpress x y [shift]
//   khandle x y                            khandle_release
//   kkey <key> [shift] [ctrl] [alt]        ktype <text>   kopt <row> <value>
//   kstate                                 kconfirm
//   pref noclamp|grid [value]              profile_get <section> <key>
//   profile_set <section> <key> <int>

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"               // camera_s
#include "prefs.h"                 // g_PrefsDlg->m_bNoClamp
#include "radiant_rtt.h"           // RTT_GetTexture: viewport screenshots
#include "radiant_registry.h"      // profile_get / profile_set (the isolated test profile)
#include <d3dx9.h>                 // D3DXSaveSurfaceToFileA
#include <imgui/imgui.h>           // io.KeyShift / KeyCtrl / KeyAlt: the viewport's modifiers

#include "kiwi_testprobe.h"
#include "kiwi_test.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_validity.h"
#include "kiwi_viewport.h"

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
extern entity_s entities;                                                    // entity.cpp:325
extern selbrush_t active_brushes;                                            // engine_stubs.cpp:787 (0x23F189C)
extern char    *ValueForKey2( const entity_s *e, const char *key );          // entity.cpp:91
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *m );              // materialdef.cpp 0x431640
extern void Face_MoveTexture( const float *surfDef, const float *normal, float *outVecs,
                              const float *uvBase, float rotate, float crossterm );   // texturevecs.cpp 0x45A1C0
extern void Entity_GetOrientationMatrix( entity_s *ent, float (*axis)[3] );           // entity.cpp:632
extern void OrientationConcatenate( const orientation_t *orFirst, const orientation_t *orSecond,
                                    orientation_t *out );                              // engine_stubs.cpp:1136
extern void OrientationPosToWorldPos( float *out, const float *pos, const orientation_t *orient );  // draw.cpp:24
extern void OrientationDirToWorldDir( float *out, const orientation_t *orient, const float *dir );  // draw.cpp:35
extern camera_s *Ed_Camera();                                                // camwnd.cpp:167
extern void     CamWnd_BuildMatrix();                                        // camwnd.cpp:216
extern bool     Radiant_TryHotkey( unsigned int vk );                        // mainfrm.cpp:1752
extern float    grid_sizes[];                                                // engine_stubs.cpp:780
extern int      g_nUpdateBits;                                               // engine_stubs.cpp:782

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
        if ( !need( 2, 3, "pref noclamp|grid [value]" ) ) return true;
        const std::string what = Lower( w[1] );
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
        error = "pref: noclamp or grid";
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
    return false;
}
