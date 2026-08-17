#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_lollipop.cpp — ROUND K: the Plasticity extrude handle.  See
// kiwi_lollipop.h for the user directive, the picture it reproduces, why the
// three-arrow gizmo stands down rather than merely hiding, and the ride/flip rule.
//
// NEW code.  It mutates NO geometry: like kiwi_gizmo.cpp it only AIMS the modal
// command that is already running.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s

#include "kiwi_lollipop.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_ux.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>

// ── ported entry points (each verified against its DEFINITION) ────────────
//   camwnd.cpp:151        camera_s *Ed_Camera()
//   camwnd.cpp:162        void      CamWnd_BuildMatrix()      0x403470
//   engine_stubs.cpp:693  int       g_nUpdateBits = 0         0x25d5a74
extern camera_s *Ed_Camera();
extern void      CamWnd_BuildMatrix();
extern int       g_nUpdateBits;

namespace
{
    // ── KIWI-UX (ROUND U): THE RESTYLE ──────────────────────────────────────
    // USER DIRECTIVE, verbatim: "Same thing goes for the extrusion lollipop, it's
    // a bit ugly." — measured against the Plasticity screenshot the round K header
    // already reproduces: a CLEAN THIN RING lying on the face, a THIN stem, and a
    // small SOLID ball, all in one yellow.
    //
    // WHAT CHANGED AND WHY, size by size:
    //   ring   18 -> 14 px   the old ring was nearly a third of the stem's length
    //                        and read as a halo rather than as a marker.  28 -> 20
    //                        chords with it: 20 chords at 14 px is under a pixel of
    //                        chord error, so it still reads as a circle and the
    //                        batch is a third cheaper.
    //   stem   48 -> 42 px   proportionally shorter now the ring is smaller, so the
    //                        whole glyph keeps the picture's stubby look.
    //   ball    7 ->  5 px   the directive's "solid filled ball ~5px".  10 -> 12
    //                        sides: a SMALLER disc needs MORE sides to stop reading
    //                        as a polygon, and 12 with 6 diagonals is still cheaper
    //                        than the ring it replaced.
    //   pick   12 px         UNCHANGED.  It is a fingertip target, not a drawn
    //                        thing, and shrinking it with the ball would have made
    //                        the handle harder to grab — the one regression the
    //                        directive is definitely not asking for.
    // "THIN" is the whole point and there is no line-width control in kiwi_lines
    // (every segment is one device line), so thinness here is bought by drawing
    // FEWER, better-placed segments: the ring gained no second circle, the stem is
    // still ONE segment, and the ball's diagonals are the minimum that fills it.
    const float KLOL_RING_PIX  = 14.0f;
    const float KLOL_STEM_PIX  = 42.0f;
    const float KLOL_BALL_PIX  = 5.0f;      // ~10 px across, per the directive
    const float KLOL_PICK_PIX  = 12.0f;
    const int   KLOL_RING_SEGS = 20;        // 20 chords read as round at 14 px
    const int   KLOL_BALL_SEGS = 12;

    // Plasticity's colours: a yellow dot on a stem and a ring on the face.
    // ROUND U: the ring is YELLOW too now, not near-white.  In the picture the
    // ring and the ball are one object in one colour; the white ring read as a
    // separate piece of UI that happened to be nearby, which is most of what made
    // the glyph look assembled rather than designed.  It is the DIM yellow — the
    // ring is context, the ball is the target, and the brightness difference is
    // what says which one to grab.  The bright yellow is still the SAME KGZ_HOT
    // the gizmo uses for a grabbed handle.
    const float KLOL_COL_BALL[3] = { 1.00f, 0.90f, 0.30f };
    const float KLOL_COL_HOT [3] = { 1.00f, 0.98f, 0.72f };   // hovered / held
    const float KLOL_COL_RING[3] = { 0.86f, 0.74f, 0.28f };
    // ── ROUND Y, ITEM 7: THE OUT-OF-RANGE STATE ─────────────────────────────
    // USER DIRECTIVE, verbatim: "Edge bevel mode needs a gizmo/lollipop.  Hard to
    // tell when where it starts/beings and goes out of range."  The second half is
    // a VISIBILITY problem, not a clamp problem: every one-axis command already
    // knows when its value is past what the geometry allows — that is the §19
    // validity gate, surfaced as KiwiEditorCommand::HudInvalid (kiwi_command.h:473)
    // — and it already tints its own preview with it (kiwi_extrude.cpp's
    // KEXT_COL_BAD, kiwi_patchfillet.cpp's KPF_COL_BAD).  The HANDLE did not, so
    // the one piece of UI the eye is actually on stayed cheerfully yellow while the
    // preview went red behind the solid.
    //
    // Same red as both of those, deliberately: KEXT_COL_BAD (kiwi_extrude.cpp:83)
    // and KPF_COL_BAD (kiwi_patchfillet.cpp:68) are already the identical triple,
    // so "this is refused" is ONE colour across the whole layer.  It is asked of
    // the COMMAND rather than plumbed per-tool, so every present and future
    // lollipop host gets it from the predicate it already implements.
    const float KLOL_COL_BAD [3] = { 1.00f, 0.30f, 0.25f };

    bool s_hot     = false;
    bool s_grabbed = false;


    struct lolGeo_t
    {
        float anchor[3];        // the LIVE face / region centroid
        float dir[3];           // unit, sign already folded in by the command
        float ring;             // world radius
        float stem;             // world length
        float ball;             // world radius
        float tip[3];           // anchor + dir * stem
        float e0[3], e1[3];     // an orthonormal pair spanning the face plane
    };

    // Any unit vector perpendicular to `n`.  The world axis LEAST aligned with n
    // is a safe seed — its cross product with n can never collapse.
    void PerpBasis( const float n[3], float e0[3], float e1[3] )
    {
        int least = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabsf( n[k] ) < fabsf( n[least] ) )
                least = k;
        float seed[3] = { 0.0f, 0.0f, 0.0f };
        seed[least] = 1.0f;
        // e0 = normalize( seed - n * (n.seed) )
        const float d = Dot3( n, seed );
        for ( int k = 0; k < 3; ++k )
            e0[k] = seed[k] - n[k] * d;
        float l = sqrtf( Dot3( e0, e0 ) );
        if ( l < 1.0e-5f )
        {
            e0[0] = 1.0f; e0[1] = 0.0f; e0[2] = 0.0f;
            l = 1.0f;
        }
        for ( int k = 0; k < 3; ++k )
            e0[k] /= l;
        // e1 = n x e0
        e1[0] = n[1]*e0[2] - n[2]*e0[1];
        e1[1] = n[2]*e0[0] - n[0]*e0[2];
        e1[2] = n[0]*e0[1] - n[1]*e0[0];
    }

    // The frame's handle geometry.  False = no lollipop this frame, for any of the
    // reasons the whole layer can refuse: the master toggle, no camera, no active
    // command, or an active command that does not want one.
    bool BuildGeo( lolGeo_t *g )
    {
        if ( !KiwiUX_ModernInput() )
            return false;
        const camera_s *c = Ed_Camera();
        if ( c->width < 1 || c->height < 1 )
            return false;
        if ( !KiwiLollipop_Wanted( g->anchor, g->dir ) )
            return false;

        const float wpp = KiwiCam_WorldPerPixel( g->anchor );
        g->ring = KLOL_RING_PIX * wpp;
        g->stem = KLOL_STEM_PIX * wpp;
        g->ball = KLOL_BALL_PIX * wpp;
        Mad3( g->anchor, g->dir, g->stem, g->tip );
        PerpBasis( g->dir, g->e0, g->e1 );
        return true;
    }

    bool BallHit( const lolGeo_t &g, int imgX, int imgY )
    {
        float tx, ty;
        if ( !Pick_WorldToImage( g.tip, &tx, &ty ) )
            return false;                       // behind the eye
        const float dx = tx - (float)imgX;
        const float dy = ty - (float)imgY;
        return sqrtf( dx * dx + dy * dy ) <= KLOL_PICK_PIX;
    }

    // A camera-facing regular polygon, optionally with its long diagonals filled
    // in — the same trick kiwi_snap.cpp's marker uses to make a small polygon read
    // as a SOLID dot with a line renderer and no fill primitive.
    void EmitDisc( const camera_s *c, const float p[3], float r, int segs, bool solid )
    {
        float prev[3], pt[3];
        for ( int i = 0; i <= segs; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % segs ) ) / (float)segs;
            const float cx = cosf( th ) * r;
            const float cy = sinf( th ) * r;
            for ( int k = 0; k < 3; ++k )
                pt[k] = p[k] + c->vright[k] * cx + c->vup[k] * cy;
            if ( i > 0 )
                KiwiLines_Add( prev, pt );
            Copy3( pt, prev );
        }
        if ( !solid )
            return;
        for ( int i = 0; i < segs / 2; ++i )
        {
            const float th0 = ( 6.283185307179586f * (float)i ) / (float)segs;
            const float th1 = th0 + 3.14159265358979f;
            float a[3], b[3];
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = p[k] + c->vright[k] * cosf( th0 ) * r + c->vup[k] * sinf( th0 ) * r;
                b[k] = p[k] + c->vright[k] * cosf( th1 ) * r + c->vup[k] * sinf( th1 ) * r;
            }
            KiwiLines_Add( a, b );
        }
    }
}

// ─── the predicate everything asks ───────────────────────────────────────────
bool KiwiLollipop_Wanted( float outAnchor[3], float outDir[3] )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return false;
    float a[3] = { 0.0f, 0.0f, 0.0f };
    float d[3] = { 0.0f, 0.0f, 1.0f };
    if ( !cmd->LollipopHandle( a, d ) )
        return false;
    // A zero direction is a command that answered true without a usable normal —
    // refuse rather than draw a stem of length zero the user cannot grab.
    const float l = sqrtf( d[0]*d[0] + d[1]*d[1] + d[2]*d[2] );
    if ( l < 1.0e-4f )
        return false;
    if ( outAnchor )
        Copy3( a, outAnchor );
    if ( outDir )
        for ( int k = 0; k < 3; ++k )
            outDir[k] = d[k] / l;
    return true;
}

bool KiwiLollipop_Active()
{
    return KiwiLollipop_Wanted( 0, 0 );
}

// ─── hover ───────────────────────────────────────────────────────────────────
void KiwiLollipop_Hover( int imgX, int imgY, bool over )
{
    const bool was = s_hot;
    s_hot = false;
    if ( over && !s_grabbed )
    {
        CamWnd_BuildMatrix();
        lolGeo_t g;
        if ( BuildGeo( &g ) )
            s_hot = BallHit( g, imgX, imgY );
    }
    if ( s_hot != was )
        g_nUpdateBits |= 1;                     // repaint so the brighten shows
}

// ─── grab ────────────────────────────────────────────────────────────────────
bool KiwiLollipop_MouseDown( int imgX, int imgY )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return false;

    CamWnd_BuildMatrix();
    lolGeo_t g;
    if ( !BuildGeo( &g ) )
        return false;
    if ( !BallHit( g, imgX, imgY ) )
        return false;

    // KIWI-UX (ROUND L): THE ARM ORDER LIVES IN ONE PLACE NOW.  Round K wrote it
    // out here (latch the press pixel → resume-or-rebase → open the gate → feed)
    // and left kiwi_gizmo.cpp's older, one-rung-shorter copy alone, which is
    // precisely why the ball never jumped and the arrows did.  Both call
    // KiwiCmd_HandleGrab; the five numbered steps are documented on it
    // (kiwi_command.h THE ONE HANDLE-GRAB ENTRY) and nothing here re-states them.
    if ( !KiwiCmd_HandleGrab( imgX, imgY ) )
        return false;
    s_grabbed = true;
    s_hot     = true;
    g_nUpdateBits |= 1;
    return true;
}

void KiwiLollipop_Release()
{
    if ( !s_grabbed )
        return;
    s_grabbed = false;
    KiwiCmd_HandleRelease();                    // ROUND L — the shared gate edge
    // Shakeout E's rule, unchanged: a handle release PAUSES, it does not commit.
    if ( KiwiCmd_Active() )
        KiwiCmd_Pause();
    g_nUpdateBits |= 1;
}

void KiwiLollipop_Abort()
{
    if ( !s_grabbed )
        return;
    s_grabbed = false;
    KiwiCmd_HandleRelease();                    // ROUND L — the shared gate edge
    if ( KiwiCmd_Active() )
        KiwiCmd_Cancel();
    g_nUpdateBits |= 1;
}

// ─── Cam_Draw tail ───────────────────────────────────────────────────────────
void KiwiLollipop_DrawWorld()
{
    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    lolGeo_t g;
    if ( !BuildGeo( &g ) )
        return;

    // Budget: ring + stem + ball outline + ball diagonals, with slack.  One batch,
    // two colour runs (kiwi_lines.h — a colour change opens a new run, so the ring
    // is emitted whole before the stem/ball rather than alternating).
    KiwiLines_Begin( KLOL_RING_SEGS + KLOL_BALL_SEGS * 2 + 8, 2 );

    // ── the circle ON the face ──────────────────────────────────────────────
    // In the FACE PLANE, not camera-facing: the picture shows a ring lying on the
    // surface, and a camera-facing ring would read as a halo floating in front of
    // it.  It is NOT a face outline — it is a fixed KLOL_RING_PIX (14 px, KIWI-UX
    // (CLEANUP, C-46): this line said 18, round U's number) circle at the centroid,
    // with nothing to do with the winding (ROUND K's "no face outlines" audit).
    // ROUND Y, ITEM 7: out-of-range paints the WHOLE glyph, ring included — a red
    // ball on a yellow ring would read as a hover state, not as a refusal.
    KiwiEditorCommand *lolCmd = KiwiCmd_Active();
    const bool bad = lolCmd && lolCmd->HudInvalid();
    if ( bad )
        KiwiLines_Color( KLOL_COL_BAD[0], KLOL_COL_BAD[1], KLOL_COL_BAD[2] );
    else
        KiwiLines_Color( KLOL_COL_RING[0], KLOL_COL_RING[1], KLOL_COL_RING[2] );
    {
        float prev[3];
        for ( int i = 0; i <= KLOL_RING_SEGS; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % KLOL_RING_SEGS ) )
                           / (float)KLOL_RING_SEGS;
            float p[3];
            for ( int k = 0; k < 3; ++k )
                p[k] = g.anchor[k] + g.e0[k] * cosf( th ) * g.ring
                                   + g.e1[k] * sinf( th ) * g.ring;
            if ( i > 0 && !KiwiLines_Add( prev, p ) )
                break;
            Copy3( p, prev );
        }
    }

    // ── the stem and the ball ───────────────────────────────────────────────
    const float *col = bad ? KLOL_COL_BAD                                  // ROUND Y, ITEM 7
                           : ( ( s_hot || s_grabbed ) ? KLOL_COL_HOT : KLOL_COL_BALL );
    KiwiLines_Color( col[0], col[1], col[2] );
    // The stem starts at the ring's edge rather than at the centroid, so the two
    // pieces read as separate objects the way the picture has them.
    // ROUND U: 0.35 -> 0.55 of the ring radius.  With the ring at 14 px the old
    // factor started the stem barely 5 px out and it visibly crossed the ring's
    // near side; over half the radius clears it and gives the picture's small gap.
    float base[3];
    Mad3( g.anchor, g.dir, g.ring * 0.55f, base );
    KiwiLines_Add( base, g.tip );
    EmitDisc( c, g.tip, g.ball, KLOL_BALL_SEGS, true );

    KiwiLines_Flush();
}
