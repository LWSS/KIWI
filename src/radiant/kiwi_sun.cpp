#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// Worldspawn sun helper implementation; kiwi_sun.h defines its direction invariants.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "kiwi_sun.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_ux.h"
#include "kiwi_vec.h"       // the one spelling of Dot3/Sub3/Mad3/Cross3/Norm3
#include "kiwi_walkcache.h" // KiwiWalkCache_Epoch — the "some brush changed" counter
#include "kiwi_windows.h"   // the §9 dock-window flag the tab is gated on
#include "radiant_frame.h"  // Radiant_RegisterCommand

#include <universal/com_math.h>   // AngleVectors — the SAME function both compilers call

#include <imgui/imgui.h>

#include <math.h>
#include <stdio.h>

// ── ported / sibling entry points (each verified against its DEFINITION) ─────
extern camera_s *Ed_Camera();                                              // camwnd.cpp:161
extern int       g_nUpdateBits;                                            // engine_stubs.cpp:773
extern int       Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118
extern void      MarkMapModified();                                        // win_qe3.cpp:195
extern entity_s *world_entity;                                             // map.cpp:62
extern char     *ValueForKey2( int e, const char *key );                   // entity.cpp:89
extern int       Entity_GetVec3ForKey( entity_s_def *e, float *out, const char *key ); // entity.cpp:99
extern float     Entity_GetFloatValueForKey( int e, const char *key );     // entity.cpp:109
// File-local elsewhere; needed to re-dock this window when it reopens.
extern ImGuiID   ImGuiShell_DockRoot();                                    // imgui_shell.cpp:796
extern void      SetKeyValue( entity_s_def *e, const char *key, const char *value );   // entity.cpp:212
extern void      Undo_ClearRedo();                                         // undo.cpp:176
extern void      Undo_GeneralStart( const char *operation );               // undo.cpp:367
extern void      Undo_AddEntity_W( entity_s *a1 );                         // undo.cpp:633
extern void      Undo_End();                                               // undo.cpp:686

namespace
{
    // Negative pitch puts the sun above the horizon; -89 is overhead, -1 grazing.
    const float KSUN_PITCH_MIN   = -89.0f;
    const float KSUN_PITCH_MAX   =  -1.0f;
    // Match the Rotate command's KX_ANGLE_STEP (kiwi_transform.cpp:94).
    const float KSUN_ANGLE_STEP  =   5.0f;
    // One viewport height is one turn; match KCam_OrbitDegPerPx's fallback
    // (kiwi_camera.cpp:59-64) when camera height is unusable.
    const float KSUN_DEG_PER_PX_FALLBACK = 0.35f;

    // ── where the glyph sits, relative to the orbit target ──────────────────
    const float KSUN_ORBIT_SCALE  =      1.5f;   // x the target's bounding-sphere radius
    const float KSUN_MIN_RADIUS   =    256.0f;   // so a tiny brush does not swallow the glyph
    // Match KCAM_MAX_DIST_PERSP so the camera can still pull back to the glyph.
    const float KSUN_MAX_RADIUS   =  65536.0f;
    // Empty maps use a nominal cube so the frustum has a cross-section.
    const float KSUN_EMPTY_RADIUS =   2048.0f;
    const float KSUN_EMPTY_HALF   =    512.0f;

    // ── the glyph, in PIXELS (screen-constant, like every handle in this layer) ─
    const float KSUN_DISC_PIX   = 12.0f;
    const float KSUN_SPOKE_PIX  = 20.0f;   // spoke tips, measured from the centre
    const float KSUN_ARROW_PIX  = 46.0f;   // shaft length, along -sunDir
    const float KSUN_HEAD_PIX   =  8.0f;
    const float KSUN_PICK_PIX   = 14.0f;
    const int   KSUN_DISC_SEGS  = 20;
    const int   KSUN_SPOKES     =  8;
    const int   KSUN_RAY_GRID   =  3;      // KSUN_RAY_GRID^2 interior direction rays
    // Worst case is 54 lines; kiwi_sun.h carries the budget tally.
    const int   KSUN_MAX_SEGMENTS = 64;

    // Gold/amber avoids the established selection, hover, construction, and section hues.
    const float KSUN_COL_GLYPH  [3] = { 1.00f, 0.78f, 0.24f };
    const float KSUN_COL_HOT    [3] = { 1.00f, 0.95f, 0.66f };
    const float KSUN_COL_ARROW  [3] = { 0.98f, 0.62f, 0.18f };
    const float KSUN_COL_FRUSTUM[3] = { 0.95f, 0.66f, 0.22f };
    const float KSUN_COL_RAY    [3] = { 0.58f, 0.40f, 0.14f };

    // ── THE COMPILER'S OWN DEFAULT SUN DIRECTION ────────────────────────────
    // Compiler fallback vector (cod4rad/cmdline.c:28-30); convert it through the
    // same round trip used by dragging instead of duplicating an angle literal.
    const float KSUN_DEFAULT_DIR[3] = { 0.4418350f, 0.5680700f, 0.6943130f };

    const float KSUN_RAD2DEG = 57.29577951308232f;

    // ── state ───────────────────────────────────────────────────────────────
    bool s_selected = false;
    bool s_hot      = false;        // the glyph is under the cursor
    bool s_grabbed  = false;

    // Re-derive live angles from the latched pair plus accumulated travel each frame.
    float s_grabPitch = 0.0f;
    float s_grabYaw   = 0.0f;
    float s_accX      = 0.0f;
    float s_accY      = 0.0f;
    float s_dragPitch = 0.0f;
    float s_dragYaw   = 0.0f;
    // Feed deltas because RecomputeDrag may rewind refused travel at the pitch clamp.
    int   s_lastX     = 0;
    int   s_lastY     = 0;

    // Cache target bounds against KiwiWalkCache_Epoch, which covers map edits and
    // brush link/unlink/free funnels (same scheme as kiwi_skybox.cpp:1103-1105).
    bool     s_haveTarget  = false;
    unsigned s_targetEpoch = 0;
    float    s_targetMins  [3] = { 0.0f, 0.0f, 0.0f };
    float    s_targetMaxs  [3] = { 0.0f, 0.0f, 0.0f };
    float    s_targetCentre[3] = { 0.0f, 0.0f, 0.0f };
    float    s_orbitRadius     = KSUN_EMPTY_RADIUS;

    void Repaint()
    {
        g_nUpdateBits |= W_CAMERA;
    }

    entity_s *WorldDef()
    {
        return world_entity ? world_entity->def : nullptr;
    }

    // Use the compilers' AngleVectors spelling so angle semantics cannot drift.
    void DirFromAngles( float pitch, float yaw, float out[3] )
    {
        const float ang[3] = { pitch, yaw, 0.0f };
        AngleVectors( ang, out, nullptr, nullptr );      // universal/com_math.h:293
    }

    void AnglesFromDir( const float dir[3], float *outPitch, float *outYaw )
    {
        float z = dir[2];
        if ( z >  1.0f ) z =  1.0f;                      // asin's domain, defensively
        if ( z < -1.0f ) z = -1.0f;
        *outPitch = -asinf( z ) * KSUN_RAD2DEG;
        *outYaw   = atan2f( dir[1], dir[0] ) * KSUN_RAD2DEG;
    }

    float WrapDeg( float deg )
    {
        float d = fmodf( deg, 360.0f );
        if ( d >   180.0f ) d -= 360.0f;
        else if ( d <= -180.0f ) d += 360.0f;
        return d;
    }

    // The same spelling the Rotate command's snap uses (kiwi_transform.cpp:3505).
    float SnapDeg( float deg )
    {
        return floorf( deg / KSUN_ANGLE_STEP + 0.5f ) * KSUN_ANGLE_STEP;
    }

    // ── the orbit target: the UNION of all map geometry ─────────────────────
    // KIWI-UX: this used to prefer the single BIGGEST brush, which made the sun
    // orbit one skybox wall on maps whose shell is built from several chunks
    // (user report).  The whole-map union is what "orbit the map" means.
    void RebuildTarget()
    {
        bool  haveUnion = false;
        float umn[3]    = { 0.0f, 0.0f, 0.0f };
        float umx[3]    = { 0.0f, 0.0f, 0.0f };

        // Hidden and selected brushes still receive sunlight; patches are brush
        // defs here, so both sentinel lists cover all target geometry.
        for ( int list = 0; list < 2; ++list )
        {
            selbrush_t *head = list ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                brush_t *def = b->def;
                if ( !def )
                    continue;
                const float ex = def->maxs[0] - def->mins[0];
                const float ey = def->maxs[1] - def->mins[1];
                const float ez = def->maxs[2] - def->mins[2];
                if ( ex < 0.0f || ey < 0.0f || ez < 0.0f )
                    continue;                    // bounds never built: not geometry yet

                for ( int k = 0; k < 3; ++k )
                {
                    if ( !haveUnion || def->mins[k] < umn[k] ) umn[k] = def->mins[k];
                    if ( !haveUnion || def->maxs[k] > umx[k] ) umx[k] = def->maxs[k];
                }
                haveUnion = true;
            }
        }

        if ( haveUnion )
        {
            Copy3( umn, s_targetMins );
            Copy3( umx, s_targetMaxs );
        }
        else
        {
            for ( int k = 0; k < 3; ++k )
            {
                s_targetMins[k] = -KSUN_EMPTY_HALF;
                s_targetMaxs[k] =  KSUN_EMPTY_HALF;
            }
        }

        float span[3];
        Sub3( s_targetMaxs, s_targetMins, span );
        for ( int k = 0; k < 3; ++k )
            s_targetCentre[k] = ( s_targetMins[k] + s_targetMaxs[k] ) * 0.5f;

        if ( haveUnion )
        {
            const float bounding = Len3( span ) * 0.5f;      // bounding-sphere radius
            float r = bounding * KSUN_ORBIT_SCALE;
            if ( r < KSUN_MIN_RADIUS ) r = KSUN_MIN_RADIUS;
            if ( r > KSUN_MAX_RADIUS ) r = KSUN_MAX_RADIUS;
            s_orbitRadius = r;
        }
        else
        {
            s_orbitRadius = KSUN_EMPTY_RADIUS;
        }
    }

    void EnsureTarget()
    {
        const unsigned epoch = KiwiWalkCache_Epoch();    // kiwi_walkcache.h:38
        if ( s_haveTarget && s_targetEpoch == epoch )
            return;
        RebuildTarget();
        s_targetEpoch = epoch;
        s_haveTarget  = true;
    }

    // Match the compilers' three-component parse test (cod4rad/mapio.c:496-505).
    bool WorldAngles( float *outPitch, float *outYaw )
    {
        entity_s_def *wd = WorldDef();
        if ( !wd )
            return false;
        float ang[3] = { 0.0f, 0.0f, 0.0f };
        if ( !Entity_GetVec3ForKey( wd, ang, "sundirection" ) )
            return false;
        *outPitch = ang[0];
        *outYaw   = ang[1];
        return true;
    }

    // sunDir (unit, pointing AT the sun), the glyph position and the world-per-pixel
    // scale there.  False when there is no sun, or the camera cannot size a marker.
    bool SunFrame( float outDir[3], float outPos[3], float *outWpp )
    {
        float pitch = 0.0f, yaw = 0.0f;
        if ( !KiwiSun_Angles( &pitch, &yaw ) )
            return false;
        DirFromAngles( pitch, yaw, outDir );
        EnsureTarget();
        Mad3( s_targetCentre, outDir, s_orbitRadius, outPos );
        if ( outWpp )
        {
            *outWpp = KiwiCam_WorldPerPixel( outPos );   // kiwi_camera.h:295
            if ( !( *outWpp > 0.0f ) )
                return false;
        }
        return true;
    }

    float DegPerPx()
    {
        const camera_s *c = Ed_Camera();
        if ( !c || c->height < 16 )
            return KSUN_DEG_PER_PX_FALLBACK;
        return 360.0f / (float)c->height;
    }

    // Re-derive the dragged pair from the latch plus the accumulated travel.
    void RecomputeDrag()
    {
        const float k = DegPerPx();

        // Rewind refused travel so the first reverse pixel moves off the clamp
        // (the KiwiCam_OrbitDrag rule, kiwi_camera.cpp:790-799).
        float pitch = s_grabPitch + s_accY * k;
        if ( pitch > KSUN_PITCH_MAX )
        {
            pitch  = KSUN_PITCH_MAX;
            s_accY = ( KSUN_PITCH_MAX - s_grabPitch ) / k;
        }
        else if ( pitch < KSUN_PITCH_MIN )
        {
            pitch  = KSUN_PITCH_MIN;
            s_accY = ( KSUN_PITCH_MIN - s_grabPitch ) / k;
        }
        float yaw = s_grabYaw + s_accX * k;

        // Use the shared command snap context. Exempt zero travel so Ctrl-click
        // remains a no-op, matching the Rotate ring (kiwi_transform.cpp:3504).
        const bool travelled = ( s_accX != 0.0f || s_accY != 0.0f );
        if ( travelled && KiwiCmd_SnapEngaged() )        // kiwi_command.h:1320
        {
            // Derive the snapped range because the horizon bounds are not step multiples.
            const float snapLo = ceilf ( KSUN_PITCH_MIN / KSUN_ANGLE_STEP ) * KSUN_ANGLE_STEP;
            const float snapHi = floorf( KSUN_PITCH_MAX / KSUN_ANGLE_STEP ) * KSUN_ANGLE_STEP;
            pitch = SnapDeg( pitch );
            if ( pitch < snapLo )      pitch = snapLo;
            else if ( pitch > snapHi ) pitch = snapHi;
            yaw = SnapDeg( yaw );
        }

        s_dragPitch = pitch;
        s_dragYaw   = WrapDeg( yaw );
    }

    // Funnel drag and tab writes through one undo record per gesture;
    // Undo_AddEntity_W skips the worldspawn brush walk (undo.cpp:638).
    void WriteKey( const char *key, const char *value, const char *operation )
    {
        entity_s_def *wd = WorldDef();
        if ( !wd || !key || !value )
            return;

        Undo_ClearRedo();
        Undo_GeneralStart( operation );
        Undo_AddEntity_W( wd );
        SetKeyValue( wd, key, value );
        Undo_End();

        // SetKeyValue does not mark the map modified (win_ent.cpp:276-304), so this
        // helper must do it after committing the undo record.
        MarkMapModified();
        g_nUpdateBits = -1;
    }

    void FormatSunVec3( char *out, size_t outSize, float x, float y, float z )
    {
        char sx[48], sy[48], sz[48];
        _snprintf( out, outSize, "%s %s %s",
                   KiwiFmt_Num( sx, sizeof( sx ), x, 6 ),
                   KiwiFmt_Num( sy, sizeof( sy ), y, 6 ),
                   KiwiFmt_Num( sz, sizeof( sz ), z, 6 ) );
        out[outSize - 1] = '\0';
    }

    void WriteAngles( float pitch, float yaw, const char *operation )
    {
        char buf[64];
        FormatSunVec3( buf, sizeof( buf ), pitch, yaw, 0.0f );
        WriteKey( "sundirection", buf, operation );
    }

    // ── the glyph ───────────────────────────────────────────────────────────
    // Camera-facing ring matching kiwi_section.cpp's EmitDisc (:126).
    void EmitRing( const camera_s *c, const float p[3], float r, int segs )
    {
        float prev[3], pt[3];
        for ( int i = 0; i <= segs; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % segs ) ) / (float)segs;
            const float cx = cosf( th ) * r;
            const float cy = sinf( th ) * r;
            for ( int k = 0; k < 3; ++k )
                pt[k] = p[k] + c->vright[k] * cx + c->vup[k] * cy;
            if ( i > 0 && !KiwiLines_Add( prev, pt ) )
                return;
            Copy3( pt, prev );
        }
    }

    void EmitGlyph( const camera_s *c, const float dir[3], const float pos[3], float wpp )
    {
        const bool  lit = s_selected || s_hot || s_grabbed;
        const float *col = lit ? KSUN_COL_HOT : KSUN_COL_GLYPH;
        KiwiLines_Color( col[0], col[1], col[2] );

        EmitRing( c, pos, KSUN_DISC_PIX * wpp, KSUN_DISC_SEGS );

        const float r0 = KSUN_DISC_PIX  * wpp * 1.35f;
        const float r1 = KSUN_SPOKE_PIX * wpp;
        for ( int i = 0; i < KSUN_SPOKES; ++i )
        {
            const float th = ( 6.283185307179586f * (float)i ) / (float)KSUN_SPOKES;
            const float cx = cosf( th );
            const float cy = sinf( th );
            float a[3], b[3];
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = pos[k] + c->vright[k] * cx * r0 + c->vup[k] * cy * r0;
                b[k] = pos[k] + c->vright[k] * cx * r1 + c->vup[k] * cy * r1;
            }
            if ( !KiwiLines_Add( a, b ) )
                return;
        }

        // The direction arrow: down -sunDir, i.e. the way the light travels.
        KiwiLines_Color( KSUN_COL_ARROW[0], KSUN_COL_ARROW[1], KSUN_COL_ARROW[2] );
        float tail[3];
        Mad3( pos, dir, -KSUN_ARROW_PIX * wpp, tail );
        if ( !KiwiLines_Add( pos, tail ) )
            return;

        // Camera-basis head avoids collapsing when the shaft points at the eye.
        const float h = KSUN_HEAD_PIX * wpp;
        float back[3];
        Mad3( tail, dir, h * 2.0f, back );
        float p[3];
        for ( int s = 0; s < 4; ++s )
        {
            const float sx = ( s == 0 ) ? 1.0f : ( s == 1 ) ? -1.0f : 0.0f;
            const float sy = ( s == 2 ) ? 1.0f : ( s == 3 ) ? -1.0f : 0.0f;
            for ( int k = 0; k < 3; ++k )
                p[k] = back[k] + c->vright[k] * sx * h + c->vup[k] * sy * h;
            if ( !KiwiLines_Add( tail, p ) )
                return;
        }
    }

    // ── the projected frustum ───────────────────────────────────────────────
    // Orthonormal (u, v, L) makes the projected box reconstruction exact.
    void FrustumPoint( const float u[3], const float v[3], const float L[3],
                       float a, float b, float l, float out[3] )
    {
        for ( int k = 0; k < 3; ++k )
            out[k] = u[k] * a + v[k] * b + L[k] * l;
    }

    void EmitFrustum( const float dir[3], const float pos[3] )
    {
        // L is the direction the light TRAVELS: away from the sun, into the map.
        const float L[3] = { -dir[0], -dir[1], -dir[2] };

        // Switch references near the zenith so the perpendicular basis stays valid.
        float ref[3] = { 0.0f, 0.0f, 1.0f };
        if ( fabsf( L[2] ) > 0.9f )
        {
            ref[0] = 1.0f; ref[1] = 0.0f; ref[2] = 0.0f;
        }
        float u[3], v[3];
        Cross3( ref, L, u );
        if ( !Norm3( u ) )
            return;
        Cross3( L, u, v );                       // unit already: L and u are unit and perpendicular

        float corners[KIWI_BOX_CORNERS][3];
        KiwiBox_Corners( s_targetMins, s_targetMaxs, corners );   // kiwi_lines.h:175

        float u0 = 0.0f, u1 = 0.0f, v0 = 0.0f, v1 = 0.0f, lFar = 0.0f;
        for ( int i = 0; i < KIWI_BOX_CORNERS; ++i )
        {
            const float du = Dot3( corners[i], u );
            const float dv = Dot3( corners[i], v );
            const float dl = Dot3( corners[i], L );
            if ( i == 0 )
            {
                u0 = u1 = du;
                v0 = v1 = dv;
                lFar = dl;
                continue;
            }
            if ( du < u0 ) u0 = du;  else if ( du > u1 ) u1 = du;
            if ( dv < v0 ) v0 = dv;  else if ( dv > v1 ) v1 = dv;
            if ( dl > lFar ) lFar = dl;
        }

        const float lNear = Dot3( pos, L );       // the plane the glyph sits on
        if ( !( lFar > lNear ) )
            return;                               // the target is not in front of the sun

        // The two end rectangles and the four long edges.
        const float ca[4] = { u0, u1, u1, u0 };
        const float cb[4] = { v0, v0, v1, v1 };
        KiwiLines_Color( KSUN_COL_FRUSTUM[0], KSUN_COL_FRUSTUM[1], KSUN_COL_FRUSTUM[2] );
        float nearP[4][3], farP[4][3];
        for ( int i = 0; i < 4; ++i )
        {
            FrustumPoint( u, v, L, ca[i], cb[i], lNear, nearP[i] );
            FrustumPoint( u, v, L, ca[i], cb[i], lFar,  farP [i] );
        }
        for ( int i = 0; i < 4; ++i )
        {
            if ( !KiwiLines_Add( nearP[i], nearP[( i + 1 ) & 3] ) ) return;
            if ( !KiwiLines_Add( farP [i], farP [( i + 1 ) & 3] ) ) return;
            if ( !KiwiLines_Add( nearP[i], farP[i] ) )              return;
        }

        // Half-step interior rays show direction without overlapping box edges.
        KiwiLines_Color( KSUN_COL_RAY[0], KSUN_COL_RAY[1], KSUN_COL_RAY[2] );
        for ( int i = 0; i < KSUN_RAY_GRID; ++i )
        {
            const float fa = ( (float)i + 0.5f ) / (float)KSUN_RAY_GRID;
            for ( int j = 0; j < KSUN_RAY_GRID; ++j )
            {
                const float fb = ( (float)j + 0.5f ) / (float)KSUN_RAY_GRID;
                float a[3], b[3];
                FrustumPoint( u, v, L, u0 + ( u1 - u0 ) * fa, v0 + ( v1 - v0 ) * fb, lNear, a );
                FrustumPoint( u, v, L, u0 + ( u1 - u0 ) * fa, v0 + ( v1 - v0 ) * fb, lFar,  b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }
    }

    // The recompile note, said once per placement / commit rather than per frame.
    void SayRecompile()
    {
        Sys_Printf( "Sun: `sundirection` is read at BSP + light time "
                    "(sun shadows and the primary sun light), so recompile the map "
                    "for the change to reach the baked lighting.\n" );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  the command
// ═════════════════════════════════════════════════════════════════════════════
void KiwiSun_RegisterCommands()
{
    // Creation/window verbs remain unbound and searchable through the palette.
    Radiant_RegisterCommand( "KiwiPlaceSun", 0, 0, KIWI_CMD_PLACE_SUN );
    // KiwiWindows_DispatchInstant owns all dock-window toggles.
    Radiant_RegisterCommand( "KiwiWindowSun", 0, 0, KIWI_CMD_WINDOW_SUN );
}

bool KiwiSun_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned)KIWI_CMD_PLACE_SUN )
        return false;
    KiwiSun_Place();
    return true;
}

bool KiwiSun_CanPlace()
{
    return WorldDef() != nullptr;
}

void KiwiSun_Place()
{
    entity_s_def *wd = WorldDef();
    if ( !wd )
    {
        Sys_Printf( "Place Sun: there is no worldspawn to write the keys onto.\n" );
        return;
    }

    // Use the compiler direction plus the daylight keys both compilers consume
    // (cod4rad/mapio.c:303-382; cod4map/primarylights.cpp:1388-1403).
    // Do not write sunIsPrimaryLight: absence already means enabled (:1388-1390).
    float dPitch = 0.0f, dYaw = 0.0f;
    AnglesFromDir( KSUN_DEFAULT_DIR, &dPitch, &dYaw );
    char dirBuf[64];
    FormatSunVec3( dirBuf, sizeof( dirBuf ), dPitch, dYaw, 0.0f );

    struct sunKey_t { const char *key; const char *value; };
    enum { KSUN_DEFAULT_KEYS = 7 };
    const sunKey_t defaults[KSUN_DEFAULT_KEYS] =
    {
        { "sundirection",    dirBuf              },
        { "sunlight",        "1.6"               },
        { "suncolor",        "0.99 0.98 0.86"    },
        { "sundiffusecolor", "0.94 0.94 1"       },
        { "diffusefraction", "0.4"               },
        { "ambient",         "0.12"              },
        { "_color",          "1 1 1"             },
    };
    const int count = KSUN_DEFAULT_KEYS;

    // Decide all missing keys before opening undo. ValueForKey2 matches keys
    // case-insensitively (entity.cpp:92); an empty value remains fillable.
    bool missing[KSUN_DEFAULT_KEYS];
    int  toWrite = 0;
    for ( int i = 0; i < count; ++i )
    {
        const char *have = ValueForKey2( (int)(intptr_t)wd, defaults[i].key );
        missing[i] = !have || !have[0];
        if ( missing[i] )
            ++toWrite;
    }

    if ( toWrite > 0 )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( "place sun" );
        Undo_AddEntity_W( wd );
        for ( int i = 0; i < count; ++i )
            if ( missing[i] )
                SetKeyValue( wd, defaults[i].key, defaults[i].value );
        Undo_End();
        MarkMapModified();
        g_nUpdateBits = -1;
        Sys_Printf( "Sun placed: %d worldspawn key%s written "
                    "(existing values were left alone).\n",
                    toWrite, ( toWrite == 1 ) ? "" : "s" );
        SayRecompile();
    }
    else
    {
        Sys_Printf( "Sun: this map already carries every sun key - selecting the "
                    "existing one.  Drag its glyph to move it.\n" );
    }

    // Selection is also the no-write result when all keys already exist.
    s_selected = true;
    s_hot      = false;
    s_grabbed  = false;

    // Report the distant glyph position because large-map users may not see it.
    {
        float dir[3], pos[3];
        if ( SunFrame( dir, pos, nullptr ) )      // SunFrame runs EnsureTarget itself
            Sys_Printf( "Sun glyph at (%.0f %.0f %.0f), %.0f units from the map's "
                        "centre - zoom out or frame the map to see it.\n",
                        pos[0], pos[1], pos[2], s_orbitRadius );
    }
    Repaint();
}

// Remove the worldspawn sun keys so a fresh sun can be placed.  The inverse of
// KiwiSun_Place (which only writes ABSENT keys, so re-placing needs these gone
// first).  Undo-bracketed as one record.
extern void DeleteKey( epair_t **head, const char *key );      // entity.cpp 0x483720
void KiwiSun_Delete()
{
    entity_s_def *wd = WorldDef();
    if ( !wd )
    {
        Sys_Printf( "Delete Sun: there is no worldspawn.\n" );
        return;
    }

    // The same 7 keys KiwiSun_Place manages (sun direction + the daylight keys both
    // compilers consume).
    static const char *const sunKeys[7] =
    {
        "sundirection", "sunlight", "suncolor", "sundiffusecolor",
        "diffusefraction", "ambient", "_color",
    };

    int present = 0;
    for ( int i = 0; i < 7; ++i )
    {
        const char *have = ValueForKey2( (int)(intptr_t)wd, sunKeys[i] );
        if ( have && have[0] )
            ++present;
    }
    if ( present == 0 )
    {
        Sys_Printf( "Delete Sun: this map has no sun keys to remove.\n" );
        return;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( "delete sun" );
    Undo_AddEntity_W( wd );
    for ( int i = 0; i < 7; ++i )
        DeleteKey( &wd->epairs, sunKeys[i] );   // entity_s::epairs @0x74; no-op if absent
    Undo_End();

    s_selected = false;
    s_hot      = false;
    s_grabbed  = false;

    MarkMapModified();
    g_nUpdateBits = -1;
    Sys_Printf( "Sun deleted: %d worldspawn sun key%s removed - use Place Sun for a fresh one.\n",
                present, ( present == 1 ) ? "" : "s" );
    SayRecompile();
    Repaint();
}

// ═════════════════════════════════════════════════════════════════════════════
//  state
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiSun_Exists()
{
    float p = 0.0f, y = 0.0f;
    return WorldAngles( &p, &y );
}

bool KiwiSun_Selected()
{
    return s_selected && KiwiSun_Exists();
}

void KiwiSun_Select()
{
    if ( s_selected )
        return;
    s_selected = true;
    Repaint();
}

void KiwiSun_ClearSelection()
{
    if ( !s_selected && !s_hot )
        return;
    s_selected = false;
    s_hot      = false;
    Repaint();
}

bool KiwiSun_Angles( float *outPitch, float *outYaw )
{
    if ( !outPitch || !outYaw )
        return false;
    if ( s_grabbed )
    {
        *outPitch = s_dragPitch;
        *outYaw   = s_dragYaw;
        return true;
    }
    return WorldAngles( outPitch, outYaw );
}

void KiwiSun_ResetForNewMap()
{
    s_selected    = false;
    s_hot         = false;
    s_grabbed     = false;
    s_accX        = 0.0f;
    s_accY        = 0.0f;
    s_haveTarget  = false;
    s_targetEpoch = 0;
    s_orbitRadius = KSUN_EMPTY_RADIUS;
    Repaint();
}

// ═════════════════════════════════════════════════════════════════════════════
//  input
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiSun_GlyphHit( int imgX, int imgY )
{
    if ( !KiwiUX_ModernInput() )
        return false;
    float dir[3], pos[3];
    if ( !SunFrame( dir, pos, nullptr ) )
        return false;
    float tx = 0.0f, ty = 0.0f;
    if ( !Pick_WorldToImage( pos, &tx, &ty ) )   // kiwi_pick.h:109
        return false;                            // behind the eye
    const float dx = tx - (float)imgX;
    const float dy = ty - (float)imgY;
    return sqrtf( dx * dx + dy * dy ) <= KSUN_PICK_PIX;
}

bool KiwiSun_HandleDown( int imgX, int imgY )
{
    if ( !KiwiSun_Selected() )
        return false;                            // the click that selects it may not orbit it
    if ( !KiwiSun_GlyphHit( imgX, imgY ) )
        return false;

    float pitch = 0.0f, yaw = 0.0f;
    if ( !WorldAngles( &pitch, &yaw ) )
        return false;

    // Absolute-from-latch travel keeps a press without movement a no-op.
    s_grabPitch = pitch;
    s_grabYaw   = yaw;
    s_accX      = 0.0f;
    s_accY      = 0.0f;
    s_dragPitch = pitch;
    s_dragYaw   = WrapDeg( yaw );
    s_lastX     = imgX;
    s_lastY     = imgY;
    s_grabbed   = true;
    s_hot       = true;
    return true;
}

void KiwiSun_HandleDrag( int imgX, int imgY )
{
    if ( !s_grabbed )
        return;

    s_accX += (float)( imgX - s_lastX );
    s_accY += (float)( imgY - s_lastY );
    s_lastX = imgX;
    s_lastY = imgY;

    RecomputeDrag();
    Repaint();
}

void KiwiSun_HandleUp()
{
    if ( !s_grabbed )
        return;
    const float pitch = s_dragPitch;
    const float yaw   = s_dragYaw;
    s_grabbed = false;
    s_accX    = 0.0f;
    s_accY    = 0.0f;

    float was = 0.0f, wasYaw = 0.0f;
    if ( WorldAngles( &was, &wasYaw )
      && fabsf( was - pitch ) < 0.0005f && fabsf( WrapDeg( wasYaw ) - yaw ) < 0.0005f )
    {
        Repaint();                               // the grab highlight came off
        return;                                  // a press that never travelled: no record
    }

    // ONE record for the whole drag — nothing was written while it ran.
    WriteAngles( pitch, yaw, "move sun" );
    Sys_Printf( "Sun: pitch %.2f, yaw %.2f.\n", pitch, yaw );
    SayRecompile();
}

void KiwiSun_HandleAbort()
{
    if ( !s_grabbed )
        return;
    s_grabbed = false;                           // nothing was committed, so the
    s_accX    = 0.0f;                            // worldspawn already holds the
    s_accY    = 0.0f;                            // pre-drag pair
    Repaint();
}

void KiwiSun_Hover( int imgX, int imgY, bool over )
{
    // Hover is not selection-gated; it advertises the glyph before the first click.
    const bool was = s_hot;
    s_hot = over && KiwiSun_GlyphHit( imgX, imgY );
    if ( s_hot != was )
        Repaint();
}

bool KiwiSun_Grabbed()
{
    return s_grabbed;
}

// ═════════════════════════════════════════════════════════════════════════════
//  render
// ═════════════════════════════════════════════════════════════════════════════
void KiwiSun_DrawWorld()
{
    if ( !KiwiUX_ModernInput() )
        return;
    const camera_s *c = Ed_Camera();
    if ( !c || c->width < 1 || c->height < 1 )
        return;

    float dir[3], pos[3], wpp = 0.0f;
    if ( !SunFrame( dir, pos, &wpp ) )
        return;                                  // no sun key: this file costs nothing

    KiwiLines_Begin( KSUN_MAX_SEGMENTS, 2 );
    EmitGlyph( c, dir, pos, wpp );
    if ( s_selected || s_grabbed )
        EmitFrustum( dir, pos );
    KiwiLines_Flush();
}

// ═════════════════════════════════════════════════════════════════════════════
//  THE DOCK TAB — kiwi_sun.h "THE DOCK TAB IS THE DISCOVERY SURFACE"
// ═════════════════════════════════════════════════════════════════════════════
namespace
{
    // Panel layout, named file-locally the way kiwi_skybox.cpp's KSKY_* constants are.
    const float KSUNUI_FIELD_W = 96.0f;

    // ── THE LIVE EDIT BUFFERS, AND WHY THEY EXIST ───────────────────────────
    // Buffer live ImGui edits and commit on deactivation/Enter so one gesture makes
    // one undo record. Reload inactive fields so external edits and undo appear.
    float s_uiPitch    = 0.0f;
    float s_uiYaw      = 0.0f;
    float s_uiLight    = 0.0f;
    float s_uiColor[3] = { 1.0f, 1.0f, 1.0f };
    bool  s_uiEditing[3] = { false, false, false };   // pitch / yaw / sunlight
    bool  s_uiEditingColor = false;

    // Frame the union of target and glyph; either alone can hide the other.
    // Camera motion plus KIWI-owned selection needs no undo bracket (kiwi_focus.h).
    void SelectAndFrame()
    {
        float dir[3], pos[3];
        if ( !SunFrame( dir, pos, nullptr ) )
            return;
        KiwiSun_Select();

        float mins[3], maxs[3];
        for ( int k = 0; k < 3; ++k )
        {
            mins[k] = ( s_targetMins[k] < pos[k] ) ? s_targetMins[k] : pos[k];
            maxs[k] = ( s_targetMaxs[k] > pos[k] ) ? s_targetMaxs[k] : pos[k];
        }
        KiwiCam_FrameBounds( mins, maxs );           // kiwi_camera.h:285
    }
}

void KiwiSun_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_SUN );
    if ( !open || !*open )
        return;                           // closed: no Begin, no End, no cost

    if ( KiwiWindows_JustOpened( KIWI_WIN_SUN ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_SUN ), open ) )
    {
        entity_s_def *wd    = WorldDef();
        const bool    have  = KiwiSun_Exists();

        // ── place ───────────────────────────────────────────────────────────
        // Existing suns stay enabled because placement fills only missing keys; the
        // label makes clear that existing values are not overwritten.
        ImGui::BeginDisabled( !KiwiSun_CanPlace() );
        if ( ImGui::Button( have ? "Fill missing sun keys" : "Place Sun" ) )
            KiwiSun_Place();               // shared by the tab, command, and Add menu
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                "Writes the worldspawn sun keys that are MISSING (sundirection,\n"
                "sunlight, suncolor, sundiffusecolor, diffusefraction, ambient,\n"
                "_color) and selects the helper.  Values the map already carries\n"
                "are never overwritten, and a press that writes nothing takes no\n"
                "undo step." );

        ImGui::SameLine();
        ImGui::BeginDisabled( !have );
        if ( ImGui::Button( "Select Sun" ) )
            SelectAndFrame();
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                "Selects the sun glyph (which turns the projected frustum on) and\n"
                "frames the camera on the glyph AND the whole map together.\n"
                "The glyph orbits well outside the geometry, so on a large map this\n"
                "is how you find it." );

        ImGui::SameLine();
        ImGui::BeginDisabled( !have );
        if ( ImGui::Button( "Delete Sun" ) )
            KiwiSun_Delete();              // remove all sun keys so Place Sun writes fresh ones
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                "Removes ALL worldspawn sun keys (sundirection, sunlight, suncolor,\n"
                "sundiffusecolor, diffusefraction, ambient, _color) as one undo step,\n"
                "so Place Sun re-creates a fresh default sun.  Use this to re-place\n"
                "a sun whose values you want reset (Place Sun alone never overwrites\n"
                "existing keys)." );

        ImGui::Separator();

        // ── status ──────────────────────────────────────────────────────────
        if ( !have )
        {
            ImGui::TextWrapped(
                "No sun on this map.  The sun is not an entity - it is a set of "
                "worldspawn key/values that both map compilers read, and "
                "\"sundirection\" is the one that aims it.  Press Place Sun." );
        }
        else
        {
            float pitch = 0.0f, yaw = 0.0f;
            KiwiSun_Angles( &pitch, &yaw );
            ImGui::Text( "Sun placed  -  pitch %.2f deg, yaw %.2f deg%s",
                         pitch, yaw,
                         KiwiSun_Grabbed() ? "  (dragging)"
                                           : ( KiwiSun_Selected() ? "  (selected)" : "" ) );
        }

        ImGui::Separator();
        ImGui::BeginDisabled( !have || wd == nullptr );

        // ── pitch / yaw ─────────────────────────────────────────────────────
        // Reload inactive fields so orbit drags and Ctrl+Z appear immediately.
        {
            float livePitch = 0.0f, liveYaw = 0.0f;
            if ( KiwiSun_Angles( &livePitch, &liveYaw ) )
            {
                if ( !s_uiEditing[0] ) s_uiPitch = livePitch;
                if ( !s_uiEditing[1] ) s_uiYaw   = liveYaw;
            }
        }

        ImGui::SetNextItemWidth( KSUNUI_FIELD_W );
        const bool pitchEnter =
            ImGui::InputFloat( "pitch", &s_uiPitch, 0.0f, 0.0f, "%.2f",
                               ImGuiInputTextFlags_EnterReturnsTrue );
        s_uiEditing[0] = ImGui::IsItemActive();
        const bool pitchDone = pitchEnter || ImGui::IsItemDeactivatedAfterEdit();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                "Height in the sky, in degrees.  NEGATIVE is above the horizon\n"
                "(AngleVectors' forward is (cos p cos y, cos p sin y, -sin p), so\n"
                "-89 is nearly overhead and -1 is nearly grazing).  Clamped to that\n"
                "range, exactly as the orbit drag is." );

        ImGui::SameLine();
        ImGui::SetNextItemWidth( KSUNUI_FIELD_W );
        const bool yawEnter =
            ImGui::InputFloat( "yaw", &s_uiYaw, 0.0f, 0.0f, "%.2f",
                               ImGuiInputTextFlags_EnterReturnsTrue );
        s_uiEditing[1] = ImGui::IsItemActive();
        const bool yawDone = yawEnter || ImGui::IsItemDeactivatedAfterEdit();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Compass direction, in degrees, wrapped to (-180, 180]." );

        if ( pitchDone || yawDone )
        {
            // Typed and dragged angles share the same clamp and wrap.
            if ( !( s_uiPitch >= KSUN_PITCH_MIN ) ) s_uiPitch = KSUN_PITCH_MIN;
            if ( s_uiPitch > KSUN_PITCH_MAX )       s_uiPitch = KSUN_PITCH_MAX;
            s_uiYaw = WrapDeg( s_uiYaw );
            WriteAngles( s_uiPitch, s_uiYaw, "sun direction" );
            SayRecompile();
        }

        // ── the two keys a mapper reaches for next ──────────────────────────
        // Limit this helper to aim, brightness, and colour; ambient-model keys stay
        // in the entity window. BeginDisabled does not skip reads, so guard `wd`
        // before Entity_Get* walks e->epairs (entity.cpp:102,111).
        if ( wd && !s_uiEditing[2] )
            s_uiLight = Entity_GetFloatValueForKey( (int)(intptr_t)wd, "sunlight" );

        ImGui::SetNextItemWidth( KSUNUI_FIELD_W );
        const bool litEnter =
            ImGui::InputFloat( "sunlight", &s_uiLight, 0.0f, 0.0f, "%.2f",
                               ImGuiInputTextFlags_EnterReturnsTrue );
        s_uiEditing[2] = ImGui::IsItemActive();
        if ( litEnter || ImGui::IsItemDeactivatedAfterEdit() )
        {
            // Sunlight cannot be negative; the compiler owns the ambient floor
            // (cod4map/primarylights.cpp:1406-1410).
            if ( !( s_uiLight >= 0.0f ) )
                s_uiLight = 0.0f;
            char buf[32];
            KiwiFmt_Num( buf, sizeof( buf ), s_uiLight, 6 );
            WriteKey( "sunlight", buf, "sun intensity" );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Sun intensity.  Read by both compilers at BSP + light time." );

        if ( wd && !s_uiEditingColor )
        {
            float c[3] = { 1.0f, 1.0f, 1.0f };
            if ( Entity_GetVec3ForKey( wd, c, "suncolor" ) )
            {
                s_uiColor[0] = c[0];
                s_uiColor[1] = c[1];
                s_uiColor[2] = c[2];
            }
        }
        ImGui::SetNextItemWidth( KSUNUI_FIELD_W * 2.0f );
        ImGui::ColorEdit3( "suncolor", s_uiColor,
                           ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB );
        s_uiEditingColor = ImGui::IsItemActive();
        if ( ImGui::IsItemDeactivatedAfterEdit() )
        {
            char buf[64];
            FormatSunVec3( buf, sizeof( buf ),
                           s_uiColor[0], s_uiColor[1], s_uiColor[2] );
            WriteKey( "suncolor", buf, "sun colour" );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                "Sun colour.  The compiler MAX-NORMALISES it "
                "(PrimaryLight_ColorNormalize,\n"
                "cod4map/primarylights.cpp:1393-1395), so only the RATIO between the\n"
                "three channels matters - brightness is `sunlight` above." );

        ImGui::EndDisabled();

        // ── shadow-preview debug ─────────────────────────────────────────────
        // Per-caster-class kill switches for the stencil shadow-volume preview
        // (shadowvolume.cpp).  Isolates which caster class produces a reported
        // shadow artifact live, without a rebuild.
        {
            extern bool g_svCastPatches, g_svCastModels, g_svCastBrushes;  // shadowvolume.cpp
            if ( ImGui::TreeNode( "Shadow preview debug" ) )
            {
                bool any = false;
                any |= ImGui::Checkbox( "Patches cast",  &g_svCastPatches );
                ImGui::SameLine();
                any |= ImGui::Checkbox( "Models cast",   &g_svCastModels );
                ImGui::SameLine();
                any |= ImGui::Checkbox( "Brushes cast",  &g_svCastBrushes );
                if ( any )
                    g_nUpdateBits = -1;
                extern int g_svCastersDrawn, g_svTrisFed, g_svBatches;     // shadowvolume.cpp
                ImGui::TextDisabled( "casters %d  silhouette tris %d  batches %d",
                                     g_svCastersDrawn, g_svTrisFed, g_svBatches );
                ImGui::TreePop();
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled( "Drag the glyph in the 3D view to orbit the sun "
                             "around the whole map (Ctrl snaps 5 deg)." );
        ImGui::TextDisabled( "sundirection is baked at compile time - recompile "
                             "BSP + light to see it in the lighting." );
    }
    ImGui::End();
}
