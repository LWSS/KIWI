#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI: Physics Drop - the editor side (selection, static world gather, poses, undo, panel).
// See kiwi_physdrop.h; the ODE side is kiwi_physdrop_sim.cpp.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_physdrop.h"
#include "kiwi_physdrop_sim.h"
#include "kiwi_angles.h"
#include "kiwi_command.h"
#include "kiwi_droptrace.h"
#include "kiwi_fmt.h"
#include "kiwi_glass.h"     // KIWI_GLASS_CLASSNAME: selected panes ride with the group
#include "kiwi_lines.h"
#include "kiwi_texlock.h"
#include "kiwi_validity.h"
#include "prefs.h"
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_selection.h"
#include "kiwi_shadowcache.h"
#include "kiwi_vec.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <random>
#include <string>
#include <vector>

extern int Undo_LastUndoStamp();                                 // undo.cpp:91

namespace
{
    const int   KPD_MAX_MODELS   = 256;
    const float KPD_DENSITY      = 10.0f / 4096.0f;   // a 16-unit cube weighs the "default" preset's 10
    const float KPD_FRICTION     = 0.6f;              // no physPreset: calmer than "default" (0.5 / 0.5)
    const float KPD_BOUNCE       = 0.25f;
    const float KPD_PUSH         = 160.0f;            // full Scatter: sideways speed, units / s
    const float KPD_HOP          = 60.0f;             // ...upward kick
    const float KPD_SPIN         = 8.0f;              // ...spin, radians / s
    const float KPD_PAD_SIDE     = 384.0f;            // static gather around the falling models
    const float KPD_PAD_DOWN     = 2048.0f;
    const float KPD_PAD_UP       = 256.0f;
    const float KPD_REGION_EDGE  = 192.0f;            // regather when a model gets this close
    const int   KPD_DRAW_BOXES   = 16;                // collision boxes drawn per model

    const char *const KPD_PROFILE = "KiwiPhysDrop";

    struct dropModel_t
    {
        selbrush_t *node    = nullptr;
        brush_t    *def     = nullptr;
        entity_s   *ent     = nullptr;
        XModel     *model   = nullptr;
        float       origin[3];            // the start pose
        double      angles[3];
        double      axis[3][3];
        bool        hadAngles = false;
        std::string anglesText;
        float       mins[3], maxs[3];     // model-local bounds
        float       scale   = 1.0f;
        int         body    = -1;
        float       shownOrigin[3];       // the last pose written, so resting models write nothing
        std::string shownAngles;
    };

    // KIWI (2026-10-15, user: "make it so glass travels with the model if its selected with the
    // group"): a selected glass pane's start faces; each frame it is rebuilt from them, moved
    // rigidly with the group's leader.
    struct dropGlass_t
    {
        selbrush_t              *node = nullptr;
        brush_t                 *def  = nullptr;
        std::vector<float>       planepts;    // 9 per face
        std::vector<MaterialDef> mtl;         // the face's 4 material defs (texdefs)
    };

    const char *EntityKey( const entity_s *e, const char *key )
    {
        for ( const epair_t *ep = e ? e->epairs : nullptr; ep; ep = ep->next )
            if ( ep->key && !_stricmp( ep->key, key ) )
                return ep->value ? ep->value : "";
        return nullptr;
    }

    bool OwnerIsPrefabClass( const selbrush_t *b )
    {
        const entity_s *def = ( b && b->owner ) ? b->owner->def : nullptr;
        return def && def->eclass && ( def->eclass->classtype & 0x10 ) != 0;
    }

    // A model the drop can move: a whole model entity, not a prefab, not inside one.
    bool DropCandidate( selbrush_t *b )
    {
        if ( !b || !b->def || !b->def->owner || !b->owner || b->owner == world_entity || b->owner->prefab )
            return false;
        if ( OwnerIsPrefabClass( b ) || !KiwiDrop_IsModelEntity( b ) )
            return false;
        const entity_s *ent = b->def->owner;
        return ent->eclass && ent->eclass->fixedsize;
    }

    bool BoxesMeet( const float *alo, const float *ahi, const float *blo, const float *bhi )
    {
        return alo[0] <= bhi[0] && ahi[0] >= blo[0] && alo[1] <= bhi[1] && ahi[1] >= blo[1]
            && alo[2] <= bhi[2] && ahi[2] >= blo[2];
    }

    // Contents the game's physics would collide with: the face materials' contents, OR'd
    // the way the compiler builds a brush's contents.
    int FaceContents( face_t *f )
    {
        qtexture_s *t = ( f->mtldef[0].lyrMtl || f->mtldef[0].radMtl ) ? MaterialDef_GetLayeredMaterial( &f->mtldef[0] ) : nullptr;
        return ( t ? t->in_use : 0 ) | f->contents;
    }

    bool PatchCollides( const patchMesh_t *p )
    {
        const int mat = p->texture.radMtl ? p->texture.radMtl->in_use : CONTENTS_SOLID;   // layered: solid
        return ( ( mat | p->contents ) & KiwiPhysSim_ClipMask() ) != 0;
    }

    void AddBoxTris( const float corners[8][3] )
    {
        // Faces of the corner cube (bit j = +axis j), each wound so its front faces out.
        static const int quads[6][4] = {
            { 0, 4, 6, 2 }, { 1, 3, 7, 5 },     // -x, +x
            { 0, 1, 5, 4 }, { 2, 6, 7, 3 },     // -y, +y
            { 0, 2, 3, 1 }, { 4, 5, 7, 6 } };   // -z, +z
        for ( int q = 0; q < 6; ++q )
        {
            KiwiPhysSim_AddStaticTri( corners[quads[q][0]], corners[quads[q][1]], corners[quads[q][2]], false );
            KiwiPhysSim_AddStaticTri( corners[quads[q][0]], corners[quads[q][2]], corners[quads[q][3]], false );
        }
    }

    class KiwiPhysDropCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Physics Drop"; }

        bool CanExecute() override { return KiwiPhysDrop_CanExecute(); }

        bool Begin() override
        {
            m_models.clear();
            m_running = false;
            int skipped = 0;
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            {
                if ( !DropCandidate( b ) )
                    continue;
                entity_s *ent = b->def->owner;
                bool dup = false;
                for ( const dropModel_t &m : m_models )
                    dup = dup || m.ent == ent;
                if ( dup )
                    continue;
                if ( (int)m_models.size() >= KPD_MAX_MODELS )
                {
                    ++skipped;
                    continue;
                }
                dropModel_t m;
                float angles[3];
                if ( !KiwiDrop_GetModelInfo( b, m.mins, m.maxs, angles, &m.scale, m.origin, &m.model ) )
                    continue;
                m.node = b;
                m.def  = b->def;
                m.ent  = ent;
                Copy3( ent->origin, m.origin );
                const char *text = EntityKey( ent, "angles" );
                m.hadAngles  = text != nullptr;
                m.anglesText = text ? text : "";
                m.angles[0] = m.angles[1] = m.angles[2] = 0.0;
                if ( text && sscanf( text, "%lf %lf %lf", &m.angles[0], &m.angles[1], &m.angles[2] ) != 3 )
                    m.angles[0] = m.angles[1] = m.angles[2] = 0.0;
                KiwiAng_ToAxis( m.angles, m.axis );
                Copy3( m.origin, m.shownOrigin );          // what the entity shows now
                m.shownAngles = m.anglesText;
                m_models.push_back( m );
            }
            if ( m_models.empty() )
            {
                Sys_Printf( "Physics Drop: select one or more models first.\n" );
                return false;
            }
            m_glass.clear();
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            {
                const char *cls = ( b->def && !b->patch && b->def->faces ) ? EntityKey( b->def->owner, "classname" ) : nullptr;
                if ( !cls || _stricmp( cls, KIWI_GLASS_CLASSNAME ) )
                    continue;
                dropGlass_t g;
                g.node = b;
                g.def  = b->def;
                for ( int f = 0; f < b->def->faceCount; ++f )
                {
                    const face_t &fc = b->def->faces[f];
                    g.planepts.insert( g.planepts.end(), &fc.planepts[0][0], &fc.planepts[0][0] + 9 );
                    g.mtl.insert( g.mtl.end(), fc.mtldef, fc.mtldef + 4 );
                }
                m_glass.push_back( g );
            }
            if ( skipped )
                Sys_Printf( "Physics Drop: %i models over the %i limit stay where they are.\n", skipped, KPD_MAX_MODELS );

            m_scatter = Radiant_ProfileGetInt( KPD_PROFILE, "Scatter", 35 );
            m_result  = Radiant_ProfileGetInt( KPD_PROFILE, "Result", 0 ) ? 1 : 0;
            if ( m_scatter < 0 )   m_scatter = 0;
            if ( m_scatter > 100 ) m_scatter = 100;
            m_seed = (int)( ::GetTickCount() % 9999u ) + 1;
            m_floor = MapFloor() - 1024.0f;

            KiwiCmd_UndoBegin( "physics drop" );
            StartSim();
            return true;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( ( mods & 6u ) || vk != 'N' )
                return false;
            m_seed = m_seed % 9999 + 1;
            Redrop();
            return true;
        }

        // Still falling: settle first (the same fixed steps, so the same result).
        void Commit() override
        {
            if ( m_running )
            {
                for ( int guard = 0; guard < 400; ++guard )
                {
                    EnsureRegion();
                    if ( !KiwiPhysSim_Advance( 0.25f ) )
                        break;
                }
                m_running = false;
            }
            if ( !LiveModels() )
            {
                Sys_Printf( "Physics Drop: the models changed under the drop; nothing kept.\n" );
                KiwiPhysSim_End();
                m_models.clear();
                m_glass.clear();
                return;
            }
            ApplyPoses();

            int lost = 0, props = 0, noColl = 0, noPreset = 0;
            for ( const dropModel_t &m : m_models )
                if ( Lost( m ) )
                    ++lost;
            if ( m_result == 1 )
            {
                for ( const dropModel_t &m : m_models )
                {
                    if ( BodyOf( m ) < 0 || Lost( m ) )
                        continue;
                    const char *cls = EntityKey( m.ent, "classname" );
                    bool hasPreset = false;
                    if ( !KiwiPhysSim_ModelCanBeDynamic( m.model, &hasPreset ) )
                    {
                        ++noColl;                       // the game refuses it (no collision LOD)
                        continue;
                    }
                    if ( !cls || _stricmp( cls, "dyn_model" ) )
                        SetKeyValue( m.ent, "classname", "dyn_model" );
                    if ( !EntityKey( m.ent, "type" ) )
                        SetKeyValue( m.ent, "type", "clutter" );
                    if ( !hasPreset && !EntityKey( m.ent, "physPreset" ) )
                        ++noPreset;
                    ++props;
                }
            }
            KiwiPhysSim_End();

            Sys_Printf( "Physics Drop: %i model%s placed after %.1f s (seed %i).\n",
                        (int)m_models.size() - lost, (int)m_models.size() - lost == 1 ? "" : "s",
                        (double)m_simTime, m_seed );
            if ( lost )
                Sys_Printf( "Physics Drop: %i model%s fell out of the map and stayed where %s.\n",
                            lost, lost == 1 ? "" : "s", lost == 1 ? "it was" : "they were" );
            if ( !m_glass.empty() )
                Sys_Printf( "Physics Drop: %i glass pane%s moved with the group.\n",
                            (int)m_glass.size(), m_glass.size() == 1 ? "" : "s" );
            m_glass.clear();
            if ( m_result == 1 )
                Sys_Printf( "Physics Drop: %i physics prop%s (dyn_model).\n", props, props == 1 ? "" : "s" );
            if ( noColl )
                Sys_Printf( "Physics Drop: %i model%s kept static - not loaded or no collision LOD (the game refuses that dyn_model).\n",
                            noColl, noColl == 1 ? "" : "s" );
            if ( noPreset )
                Sys_Printf( "Physics Drop: %i prop%s without a physPreset - the game uses \"default\" and prints an error.\n",
                            noPreset, noPreset == 1 ? "" : "s" );
            m_models.clear();
            Sel_InvalidateFromLegacy();
            g_nUpdateBits = -1;
        }

        // The bracket's own Undo_Undo puts the originals back (deselected, stamped); select
        // them again (kiwi_dupe.cpp's Mirror RevertObjects).
        void Cancel() override
        {
            m_running = false;
            KiwiPhysSim_End();
            m_models.clear();
            m_glass.clear();                    // the undo below puts the panes back
            KiwiCmd_UndoCancel();
            const intptr_t stamp = (intptr_t)Undo_LastUndoStamp();
            std::vector<selbrush_t *> back;
            for ( selbrush_t *b = active_brushes.next; b && b != &active_brushes; b = b->next )
                if ( b->def && (intptr_t)b->def->def == stamp )
                    back.push_back( b );
            for ( selbrush_t *b : back )
                Select_Brush( b, 0, 0, 0 );
            Sel_InvalidateFromLegacy();
            g_nUpdateBits = -1;
        }

        // Clicks do nothing (no pause / resume): the camera stays free while things fall.
        bool WantsClicks() const override { return true; }
        bool Click() override { return true; }
        bool ShowsSnap() const override { return false; }
        bool GestureMoved() const override { return true; }

        const char *HudStatus() const override
        {
            const int total = (int)m_models.size();
            if ( m_running && m_leader >= 0 )
                _snprintf( m_hud, sizeof( m_hud ), "falling %.1f s  -  group of %i rides the largest  -  seed %i",
                           (double)m_simTime, total, m_seed );
            else if ( m_running )
                _snprintf( m_hud, sizeof( m_hud ), "falling %.1f s  -  %i of %i at rest  -  seed %i",
                           (double)m_simTime, m_resting, total, m_seed );
            else
                _snprintf( m_hud, sizeof( m_hud ), "at rest after %.1f s  -  %i model%s  -  seed %i",
                           (double)m_simTime, total, total == 1 ? "" : "s", m_seed );
            m_hud[sizeof( m_hud ) - 1] = '\0';
            return m_hud;
        }

        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_keys[] = { { "N", "New seed" } };
            *out = s_keys;
            return 1;
        }

        int CommandOptions( const kiwiOption_t **out ) const override
        {
            static const char *const s_results[] = { "Static prop", "Physics prop" };
            static const kiwiOption_t s_opts[] = {
                { "Scatter", KOPT_INT,  nullptr,   0, 0, 0.0f, 100.0f,  -1 },
                { "Seed",    KOPT_INT,  nullptr,   0, 0, 1.0f, 9999.0f, -1 },
                { "Result",  KOPT_ENUM, s_results, 2, 0, 0.0f, 0.0f,    -1 },
            };
            *out = s_opts;
            return 3;
        }

        int OptionValue( int opt ) const override
        {
            return opt == 0 ? m_scatter : opt == 1 ? m_seed : m_result;
        }

        void OptionChanged( int opt, int value ) override
        {
            if ( opt == 0 )
            {
                m_scatter = value < 0 ? 0 : value > 100 ? 100 : value;
                Radiant_ProfileSetInt( KPD_PROFILE, "Scatter", m_scatter );
                Redrop();
            }
            else if ( opt == 1 )
            {
                m_seed = value < 1 ? 1 : value > 9999 ? 9999 : value;
                Redrop();
            }
            else if ( opt == 2 )
            {
                m_result = value ? 1 : 0;
                Radiant_ProfileSetInt( KPD_PROFILE, "Result", m_result );
            }
        }

        // The collision boxes while they fall.
        void DrawWorld() override
        {
            if ( !m_running )
                return;
            static float corners[KPD_DRAW_BOXES][8][3];
            KiwiLines_Color( 0.35f, 0.8f, 0.45f );
            for ( const dropModel_t &m : m_models )
            {
                if ( m.body < 0 || KiwiPhysSim_BodyLost( m.body ) )
                    continue;
                const int n = KiwiPhysSim_PartBoxes( m.body, corners, KPD_DRAW_BOXES );
                for ( int i = 0; i < n; ++i )
                    for ( int k = 0; k < 8; ++k )
                        for ( int j = 0; j < 3; ++j )
                            if ( !( k & ( 1 << j ) ) && !KiwiLines_Add( corners[i][k], corners[i][k | ( 1 << j )] ) )
                                return;
            }
        }

        int LineBudget() const override { return 12 * m_partCount + 8; }

        void Tick()
        {
            if ( !m_running )
                return;
            if ( !LiveModels() )
            {
                Sys_Printf( "Physics Drop: the models changed under the drop; cancelled.\n" );
                KiwiCmd_Cancel();
                return;
            }
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter( &now );
            QueryPerformanceFrequency( &freq );
            float dt = (float)( (double)( now.QuadPart - m_lastTick.QuadPart ) / (double)freq.QuadPart );
            m_lastTick = now;
            if ( !( dt > 0.0f ) || dt > 0.05f )
                dt = 0.05f;
            EnsureRegion();
            const bool more = KiwiPhysSim_Advance( dt );
            ApplyPoses();
            if ( !more )
            {
                m_running = false;
                Sys_Printf( "Physics Drop: at rest after %.1f s - Enter / RMB keeps it, N drops again with a new seed.\n",
                            (double)KiwiPhysSim_Time() );
            }
        }

        bool Running() const { return m_running; }

    private:
        // Everything the drop holds still exists as it did at Begin.
        bool LiveModels() const
        {
            if ( m_models.empty() )
                return false;
            std::vector<selbrush_t *> nodes;
            nodes.reserve( m_models.size() );
            for ( const dropModel_t &m : m_models )
                nodes.push_back( m.node );
            std::unique_ptr<bool[]> live( new bool[nodes.size()]() );
            Sel_BrushesLive( nodes.data(), (int)nodes.size(), live.get() );
            for ( size_t i = 0; i < m_models.size(); ++i )
            {
                const dropModel_t &m = m_models[i];
                if ( !live[i] || m.node->def != m.def || m.def->owner != m.ent )
                    return false;
            }
            if ( !m_glass.empty() )
            {
                std::vector<selbrush_t *> panes;
                for ( const dropGlass_t &g : m_glass )
                    panes.push_back( g.node );
                std::unique_ptr<bool[]> paneLive( new bool[panes.size()]() );
                Sel_BrushesLive( panes.data(), (int)panes.size(), paneLive.get() );
                for ( size_t i = 0; i < m_glass.size(); ++i )
                    if ( !paneLive[i] || m_glass[i].node->def != m_glass[i].def )
                        return false;
            }
            return true;
        }

        static float MapFloor()
        {
            float z = FLT_MAX;
            for ( selbrush_t *list : { &active_brushes, &selected_brushes } )
                for ( selbrush_t *b = list->next; b && b != list; b = b->next )
                    if ( b->def && _finite( b->def->mins[2] ) )
                        z = (std::min)( z, b->def->mins[2] );
            return z < FLT_MAX ? z : -65536.0f;
        }

        bool IsDropped( const entity_s *ent ) const
        {
            for ( const dropModel_t &m : m_models )
                if ( m.ent == ent )
                    return true;
            return false;
        }

        // Model pose -> the entity: origin exact, angles as their exact text (kiwi_fmt.h).
        void SetPose( dropModel_t &m, const float origin[3], const char *anglesText )
        {
            if ( m.shownAngles == ( anglesText ? anglesText : "" ) && m.shownOrigin[0] == origin[0]
              && m.shownOrigin[1] == origin[1] && m.shownOrigin[2] == origin[2] )
                return;
            const float move[3] = { origin[0] - m.ent->origin[0], origin[1] - m.ent->origin[1], origin[2] - m.ent->origin[2] };
            Brush_Move( move, m.def, 0 );                 // the proxy, the model instance, versions
            Copy3( origin, m.ent->origin );               // one rounding, not origin + move
            if ( anglesText )
                SetKeyValue( m.ent, "angles", anglesText );
            else if ( EntityKey( m.ent, "angles" ) )
            {
                SetKeyValue( m.ent, "angles", "0 0 0" );  // upright, then no key, as it started
                DeleteKey( &m.ent->epairs, "angles" );
            }
            Copy3( origin, m.shownOrigin );
            m.shownAngles = anglesText ? anglesText : "";
        }

        void RestoreStart( dropModel_t &m )
        {
            SetPose( m, m.origin, m.hadAngles ? m.anglesText.c_str() : nullptr );
        }

        // Unturned = the start text verbatim; else exact angles, where a component within
        // 0.001 deg of its start keeps the start value and pitch / roll within 0.01 deg of a
        // right angle snap onto it (resting flat leaves solver noise there).
        void AnglesText( const dropModel_t &m, const float axisF[3][3], char *buf, size_t n, bool *same ) const
        {
            double axis[3][3];
            double diff = 0.0;
            for ( int a = 0; a < 3; ++a )
                for ( int b = 0; b < 3; ++b )
                {
                    axis[a][b] = axisF[a][b];
                    diff = (std::max)( diff, fabs( axis[a][b] - m.axis[a][b] ) );
                }
            *same = diff < 2.0e-6;
            if ( *same )
                return;
            double ang[3];
            KiwiAng_FromAxis( axis, ang );
            for ( int i = 0; i < 3; ++i )
            {
                double d = fmod( ang[i] - m.angles[i], 360.0 );
                if ( d > 180.0 )  d -= 360.0;
                if ( d < -180.0 ) d += 360.0;
                if ( fabs( d ) < 0.001 )
                {
                    ang[i] = m.angles[i];
                    continue;
                }
                if ( i == 1 )
                    continue;
                double snapped = floor( ang[i] / 90.0 + 0.5 ) * 90.0;
                if ( snapped >= 360.0 )
                    snapped -= 360.0;
                if ( fabs( ang[i] - snapped ) < 0.01 || fabs( ang[i] - snapped - 360.0 ) < 0.01 )
                    ang[i] = snapped;
            }
            const float out[3] = { (float)ang[0], (float)ang[1], (float)ang[2] };
            KiwiFmt_Vec3Exact( buf, n, out, 0 );
        }

        // The body that moves this model: its own, or the group leader's.
        int BodyOf( const dropModel_t &m ) const
        {
            return m_leader >= 0 ? m_models[(size_t)m_leader].body : m.body;
        }

        bool Lost( const dropModel_t &m ) const
        {
            const int body = BodyOf( m );
            return body >= 0 && KiwiPhysSim_BodyLost( body );
        }

        // A follower takes the leader's move from its start pose: p' = O1 + A1^T * A0 * (p - O0).
        bool FollowerPose( const dropModel_t &m, float origin[3], float axis[3][3] ) const
        {
            const dropModel_t &l = m_models[(size_t)m_leader];
            float lo[3], la[3][3];
            if ( l.body < 0 || !KiwiPhysSim_BodyPose( l.body, lo, la ) )
                return false;
            double rel[3], loc[3];
            for ( int k = 0; k < 3; ++k )
                rel[k] = (double)m.origin[k] - (double)l.origin[k];
            for ( int r = 0; r < 3; ++r )
                loc[r] = l.axis[r][0] * rel[0] + l.axis[r][1] * rel[1] + l.axis[r][2] * rel[2];
            for ( int k = 0; k < 3; ++k )
                origin[k] = (float)( lo[k] + la[0][k] * loc[0] + la[1][k] * loc[1] + la[2][k] * loc[2] );
            for ( int a = 0; a < 3; ++a )
            {
                double v[3];
                for ( int r = 0; r < 3; ++r )
                    v[r] = l.axis[r][0] * m.axis[a][0] + l.axis[r][1] * m.axis[a][1] + l.axis[r][2] * m.axis[a][2];
                for ( int k = 0; k < 3; ++k )
                    axis[a][k] = (float)( la[0][k] * v[0] + la[1][k] * v[1] + la[2][k] * v[2] );
            }
            return true;
        }

        // A pane back on its start faces; with a pose, then moved rigidly by the leader's motion
        // (rotation about the leader's start origin, then the translation), texture-locked like R / G.
        void PlaceGlass( dropGlass_t &g, const float *leaderOrigin, const float ( *leaderAxis )[3] )
        {
            brush_t *def = g.def;
            if ( def->faceCount * 9 != (int)g.planepts.size() )
                return;
            for ( int f = 0; f < def->faceCount; ++f )
            {
                memcpy( &def->faces[f].planepts[0][0], &g.planepts[(size_t)f * 9], sizeof( float ) * 9 );
                memcpy( def->faces[f].mtldef, &g.mtl[(size_t)f * 4], sizeof( MaterialDef ) * 4 );
            }
            KiwiValid_Rebuild( def );
            if ( !leaderOrigin )
                return;
            const dropModel_t &l = m_models[(size_t)m_leader];
            // M = A1^T A0 turns a start direction into its dropped one; x' = O0 + M (x - O0) + (O1 - O0).
            double M[3][3], turn[3][3], origin[3];
            for ( int j = 0; j < 3; ++j )
                for ( int k = 0; k < 3; ++k )
                    M[j][k] = leaderAxis[0][j] * l.axis[0][k] + leaderAxis[1][j] * l.axis[1][k] + leaderAxis[2][j] * l.axis[2][k];
            for ( int i = 0; i < 3; ++i )
            {
                origin[i] = l.origin[i];
                for ( int j = 0; j < 3; ++j )
                    turn[i][j] = M[j][i];                  // texture rows turn as row' = row M^T
            }
            const byte rotLock[3]  = { (byte)( g_PrefsDlg->m_bRotateLock != 0 ), (byte)( g_PrefsDlg->m_bLightmapLock != 0 ), 1 };
            const byte moveLock[3] = { (byte)( g_PrefsDlg->m_bTextureLock != 0 ), (byte)( g_PrefsDlg->m_bLightmapLock != 0 ), 1 };
            const float move[3] = { leaderOrigin[0] - l.origin[0], leaderOrigin[1] - l.origin[1], leaderOrigin[2] - l.origin[2] };
            double rows[3][KTEXLOCK_LAYERS][8];
            for ( int f = 0; f < def->faceCount; ++f )
            {
                face_t *fc = &def->faces[f];
                Face_TexLockAffineBegin( fc, fc->plane.normal, turn, origin, rotLock, rows );
                for ( int p = 0; p < 3; ++p )
                {
                    double rel[3];
                    for ( int k = 0; k < 3; ++k )
                        rel[k] = (double)fc->planepts[p][k] - origin[k];
                    for ( int k = 0; k < 3; ++k )
                        fc->planepts[p][k] = (float)( origin[k] + M[k][0] * rel[0] + M[k][1] * rel[1] + M[k][2] * rel[2] );
                }
                Face_TexLockAffineEnd( fc, rows, rotLock );
                Face_TexLockTranslate( fc, move, moveLock );
                for ( int p = 0; p < 3; ++p )
                    for ( int k = 0; k < 3; ++k )
                        fc->planepts[p][k] += move[k];
            }
            KiwiValid_Rebuild( def );
        }

        void ApplyGlass()
        {
            if ( m_glass.empty() || m_leader < 0 )
                return;
            const dropModel_t &l = m_models[(size_t)m_leader];
            float origin[3] = { 0.0f, 0.0f, 0.0f }, axis[3][3] = {};
            const bool posed = !Lost( l ) && l.body >= 0 && KiwiPhysSim_BodyPose( l.body, origin, axis );
            for ( dropGlass_t &g : m_glass )
                PlaceGlass( g, posed ? origin : nullptr, axis );
        }

        // A model that fell out of the map goes back to where it started.
        void ApplyPoses()
        {
            ApplyGlass();
            for ( dropModel_t &m : m_models )
            {
                const bool follower = m_leader >= 0 && &m != &m_models[(size_t)m_leader];
                if ( !follower && m.body < 0 )
                    continue;
                if ( Lost( m ) )
                {
                    RestoreStart( m );
                    continue;
                }
                float origin[3], axis[3][3];
                if ( follower ? !FollowerPose( m, origin, axis ) : !KiwiPhysSim_BodyPose( m.body, origin, axis ) )
                    continue;
                char text[160];
                bool same = false;
                AnglesText( m, axis, text, sizeof( text ), &same );
                if ( same )
                    SetPose( m, origin, m.hadAngles ? m.anglesText.c_str() : nullptr );
                else
                    SetPose( m, origin, text );
            }
            m_resting = KiwiPhysSim_RestingCount();
            m_simTime = KiwiPhysSim_Time();
            g_nUpdateBits = -1;
        }

        void Redrop()
        {
            KiwiPhysSim_End();
            for ( dropModel_t &m : m_models )
                RestoreStart( m );
            for ( dropGlass_t &g : m_glass )
                PlaceGlass( g, nullptr, nullptr );
            StartSim();
            g_nUpdateBits = -1;
        }

        // The model's collision boxes (its physGeoms, else its bounds), mass and surface.
        void Describe( const dropModel_t &m, kpdBodyDesc_t &d ) const
        {
            Copy3( m.origin, d.origin );
            for ( int a = 0; a < 3; ++a )
                for ( int b = 0; b < 3; ++b )
                    d.axis[a][b] = (float)m.axis[a][b];
            d.mass     = 0.0f;
            d.friction = KPD_FRICTION;
            d.bounce   = KPD_BOUNCE;
            if ( !KiwiPhysSim_ModelShape( m.model, m.scale, d.parts, &d.mass, &d.friction, &d.bounce ) )
            {
                kpdPart_t p;
                memset( &p, 0, sizeof( p ) );
                for ( int k = 0; k < 3; ++k )
                {
                    p.center[k]  = 0.5f * ( m.mins[k] + m.maxs[k] ) * m.scale;
                    p.half[k]    = 0.5f * ( m.maxs[k] - m.mins[k] ) * m.scale;
                    p.axis[k][k] = 1.0f;
                }
                d.parts.push_back( p );
            }
            if ( !( d.mass > 0.0f ) )
            {
                float vol = 0.0f;
                for ( const kpdPart_t &p : d.parts )
                    vol += 8.0f * p.half[0] * p.half[1] * p.half[2];
                d.mass = vol * KPD_DENSITY;
                d.mass = d.mass < 1.0f ? 1.0f : d.mass > 500.0f ? 500.0f : d.mass;
            }
        }

        void StartSim()
        {
            KiwiPhysSim_Begin();
            KiwiPhysSim_SetFloor( m_floor );
            std::mt19937 rng( (unsigned)m_seed );
            std::uniform_real_distribution<float> u01( 0.0f, 1.0f );
            const float s = (float)m_scatter / 100.0f;
            m_partCount = 0;
            // KIWI (2026-10-15, user: "keep all of them static except the largest one and just
            // move that one with physics and translate the rest based on that biggest prop"): a
            // group drops as one - only its largest model is a body, the rest ride on it rigidly.
            m_leader = -1;
            if ( m_models.size() > 1 || !m_glass.empty() )
            {
                float best = -1.0f;
                for ( size_t i = 0; i < m_models.size(); ++i )
                {
                    const dropModel_t &m = m_models[i];
                    const float vol = ( m.maxs[0] - m.mins[0] ) * ( m.maxs[1] - m.mins[1] )
                                    * ( m.maxs[2] - m.mins[2] ) * m.scale * m.scale * m.scale;
                    if ( vol > best )
                    {
                        best     = vol;
                        m_leader = (int)i;
                    }
                }
            }
            for ( size_t mi = 0; mi < m_models.size(); ++mi )
            {
                dropModel_t &m = m_models[mi];
                kpdBodyDesc_t d;
                Describe( m, d );
                // Draw every number whether or not the body is used, so a seed is a seed.
                const float yaw   = u01( rng ) * 2.0f * (float)KIWI_PI;
                const float push  = s * KPD_PUSH * ( 0.4f + 0.6f * u01( rng ) );
                const float hop   = s * KPD_HOP * u01( rng );
                const float z     = 2.0f * u01( rng ) - 1.0f;
                const float t     = u01( rng ) * 2.0f * (float)KIWI_PI;
                const float spin  = s * KPD_SPIN * ( 0.4f + 0.6f * u01( rng ) );
                const float r     = sqrtf( (std::max)( 0.0f, 1.0f - z * z ) );
                d.linVel[0] = cosf( yaw ) * push;
                d.linVel[1] = sinf( yaw ) * push;
                d.linVel[2] = hop;
                d.angVel[0] = r * cosf( t ) * spin;
                d.angVel[1] = r * sinf( t ) * spin;
                d.angVel[2] = z * spin;
                m.body = ( m_leader < 0 || (int)mi == m_leader ) ? KiwiPhysSim_AddBody( d ) : -1;
                if ( m.body >= 0 )
                    m_partCount += (std::min)( KiwiPhysSim_PartCount( m.body ), KPD_DRAW_BOXES );
            }
            m_haveRegion = false;
            EnsureRegion();
            m_running = true;
            m_resting = 0;
            m_simTime = 0.0f;
            QueryPerformanceCounter( &m_lastTick );
        }

        // Keeps the gathered static world around the falling models (padded far downward).
        void EnsureRegion()
        {
            float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            bool any = false;
            for ( const dropModel_t &m : m_models )
            {
                float bl[3], bh[3];
                if ( m.body < 0 || KiwiPhysSim_BodyLost( m.body ) || !KiwiPhysSim_BodyBounds( m.body, bl, bh ) )
                    continue;
                for ( int k = 0; k < 3; ++k )
                {
                    lo[k] = (std::min)( lo[k], bl[k] );
                    hi[k] = (std::max)( hi[k], bh[k] );
                }
                any = true;
            }
            if ( !any )
                return;
            if ( m_haveRegion )
            {
                bool inside = true;
                for ( int k = 0; k < 3; ++k )
                    inside = inside && lo[k] - KPD_REGION_EDGE >= m_regionLo[k] && hi[k] + KPD_REGION_EDGE <= m_regionHi[k];
                if ( inside )
                    return;
            }
            for ( int k = 0; k < 3; ++k )
            {
                const float padLo = k == 2 ? KPD_PAD_DOWN : KPD_PAD_SIDE;
                const float padHi = k == 2 ? KPD_PAD_UP : KPD_PAD_SIDE;
                m_regionLo[k] = m_haveRegion ? (std::min)( m_regionLo[k], lo[k] - padLo ) : lo[k] - padLo;
                m_regionHi[k] = m_haveRegion ? (std::max)( m_regionHi[k], hi[k] + padHi ) : hi[k] + padHi;
            }
            m_haveRegion = true;
            Gather();
        }

        // Everything solid the models can land on inside the region: visible brushes (face
        // windings), patches (render grid, both sides), other models (mesh, else bounds box).
        void Gather()
        {
            KiwiPhysSim_ClearStatic();
            const int mask = KiwiPhysSim_ClipMask();
            for ( selbrush_t *list : { &active_brushes, &selected_brushes } )
            {
                for ( selbrush_t *b = list->next; b && b != list; b = b->next )
                {
                    if ( !b->def || !Pick_BrushPickable( b ) )
                        continue;
                    bool riding = false;
                    for ( const dropGlass_t &g : m_glass )
                        riding = riding || g.def == b->def;
                    if ( riding )
                        continue;                         // a pane moving with the group
                    if ( KiwiDrop_IsModelEntity( b ) )
                    {
                        GatherModel( b );
                        continue;
                    }
                    const entity_s *owner = b->owner;
                    if ( owner && owner != world_entity && owner->def && owner->def->eclass && owner->def->eclass->fixedsize )
                        continue;                         // lights, spawns, nodes...: no collision
                    if ( !BoxesMeet( b->def->mins, b->def->maxs, m_regionLo, m_regionHi ) )
                        continue;
                    if ( b->patch && b->patch->def )
                        GatherPatch( b->patch->def );
                    else
                        GatherBrush( b->def, mask );
                }
            }
            KiwiPhysSim_BuildStatic();
        }

        void GatherBrush( brush_t *def, int mask )
        {
            int contents = 0;
            for ( int i = 0; i < def->faceCount; ++i )
                contents |= FaceContents( &def->faces[i] );
            if ( !( contents & mask ) )
                return;                                   // trigger / hint / skip / portal / clip...
            float center[3];
            for ( int k = 0; k < 3; ++k )
                center[k] = 0.5f * ( def->mins[k] + def->maxs[k] );
            for ( int i = 0; i < def->faceCount; ++i )
            {
                const winding_t *w = def->faces[i].w;
                if ( !w || w->numpoints < 3 )
                    continue;
                // Newell normal, turned away from the brush centre: convex, so that is out.
                float nrm[3] = { 0.0f, 0.0f, 0.0f }, mid[3] = { 0.0f, 0.0f, 0.0f };
                for ( int p = 0; p < w->numpoints; ++p )
                {
                    const float *a = w->p[p], *c = w->p[( p + 1 ) % w->numpoints];
                    nrm[0] += ( a[1] - c[1] ) * ( a[2] + c[2] );
                    nrm[1] += ( a[2] - c[2] ) * ( a[0] + c[0] );
                    nrm[2] += ( a[0] - c[0] ) * ( a[1] + c[1] );
                    for ( int k = 0; k < 3; ++k )
                        mid[k] += a[k] / (float)w->numpoints;
                }
                const float out[3] = { mid[0] - center[0], mid[1] - center[1], mid[2] - center[2] };
                const bool flip = nrm[0] * out[0] + nrm[1] * out[1] + nrm[2] * out[2] < 0.0f;
                for ( int p = 1; p + 1 < w->numpoints; ++p )
                {
                    if ( flip ) KiwiPhysSim_AddStaticTri( w->p[0], w->p[p + 1], w->p[p], false );
                    else        KiwiPhysSim_AddStaticTri( w->p[0], w->p[p], w->p[p + 1], false );
                }
            }
        }

        void GatherPatch( const patchMesh_t *p )
        {
            const curvePatchDef_t *g = p->curveDef;
            if ( !g || !g->verts || g->width < 2 || g->height < 2 || !PatchCollides( p ) )
                return;
            for ( int y = 0; y + 1 < g->height; ++y )
                for ( int x = 0; x + 1 < g->width; ++x )
                {
                    const float *v00 = g->verts[y * g->width + x].xyz;
                    const float *v10 = g->verts[y * g->width + x + 1].xyz;
                    const float *v01 = g->verts[( y + 1 ) * g->width + x].xyz;
                    const float *v11 = g->verts[( y + 1 ) * g->width + x + 1].xyz;
                    float lo[3], hi[3];
                    for ( int k = 0; k < 3; ++k )
                    {
                        lo[k] = (std::min)( (std::min)( v00[k], v10[k] ), (std::min)( v01[k], v11[k] ) );
                        hi[k] = (std::max)( (std::max)( v00[k], v10[k] ), (std::max)( v01[k], v11[k] ) );
                    }
                    if ( !BoxesMeet( lo, hi, m_regionLo, m_regionHi ) )
                        continue;
                    KiwiPhysSim_AddStaticTri( v00, v10, v11, true );
                    KiwiPhysSim_AddStaticTri( v00, v11, v01, true );
                }
        }

        void GatherModel( selbrush_t *b )
        {
            if ( !b->owner || b->owner->prefab || OwnerIsPrefabClass( b ) || ( b->def && IsDropped( b->def->owner ) ) )
                return;
            float mins[3], maxs[3], angles[3], scale, origin[3], rlo[3], rhi[3];
            XModel *model = nullptr;
            if ( !KiwiDrop_GetModelInfo( b, mins, maxs, angles, &scale, origin, &model ) )
                return;
            float corners[8][3];
            if ( !KiwiDrop_TransformBounds( mins, maxs, angles, scale, rlo, rhi, corners ) )
                return;
            for ( int k = 0; k < 3; ++k )
            {
                rlo[k] += origin[k];
                rhi[k] += origin[k];
            }
            if ( !BoxesMeet( rlo, rhi, m_regionLo, m_regionHi ) )
                return;
            const float *verts = nullptr;
            const unsigned short *indices = nullptr;
            int indexCount = 0;
            if ( !model || !KiwiShadowCache_ModelGeo( model, &verts, &indices, &indexCount ) || indexCount < 3 )
            {
                for ( int c = 0; c < 8; ++c )
                    for ( int k = 0; k < 3; ++k )
                        corners[c][k] += origin[k];
                AddBoxTris( corners );
                return;
            }
            float axis[3][3];
            AnglesToAxis( angles, axis );
            for ( int i = 0; i + 2 < indexCount; i += 3 )
            {
                float w[3][3], lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for ( int v = 0; v < 3; ++v )
                {
                    const float *l = verts + 3 * indices[i + v];
                    for ( int k = 0; k < 3; ++k )
                    {
                        w[v][k] = origin[k] + ( axis[0][k] * l[0] + axis[1][k] * l[1] + axis[2][k] * l[2] ) * scale;
                        lo[k] = (std::min)( lo[k], w[v][k] );
                        hi[k] = (std::max)( hi[k], w[v][k] );
                    }
                }
                if ( BoxesMeet( lo, hi, m_regionLo, m_regionHi ) )
                    KiwiPhysSim_AddStaticTri( w[0], w[1], w[2], true );
            }
        }

        std::vector<dropModel_t> m_models;
        bool          m_running    = false;
        int           m_scatter    = 35;
        int           m_seed       = 1;
        int           m_result     = 0;          // 0 = static prop, 1 = physics prop (dyn_model)
        int           m_resting    = 0;
        int           m_partCount  = 0;
        int           m_leader     = -1;         // a group: the one simulated model, else -1
        std::vector<dropGlass_t> m_glass;        // selected panes riding on the leader
        float         m_simTime    = 0.0f;
        float         m_floor      = -65536.0f;
        bool          m_haveRegion = false;
        float         m_regionLo[3] = { 0.0f, 0.0f, 0.0f };
        float         m_regionHi[3] = { 0.0f, 0.0f, 0.0f };
        LARGE_INTEGER m_lastTick   = {};
        mutable char  m_hud[160]   = { 0 };
    };

    KiwiPhysDropCommand s_physDrop;
}

bool KiwiPhysDrop_CanExecute()
{
    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        if ( DropCandidate( b ) )
            return true;
    return false;
}

bool KiwiPhysDrop_Active()
{
    return KiwiCmd_Active() == &s_physDrop;
}

bool KiwiPhysDrop_Resting()
{
    return KiwiPhysDrop_Active() && !s_physDrop.Running();
}

void KiwiPhysDrop_Tick()
{
    if ( KiwiPhysDrop_Active() )
        s_physDrop.Tick();
}

void KiwiPhysDrop_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiPhysicsDrop", 0, 0, KIWI_CMD_PHYS_DROP );
}

KiwiEditorCommand *KiwiPhysDrop_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_PHYS_DROP ) ? &s_physDrop : 0;
}
