#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_extrude.cpp — RADIANT_UX_DESIGN §23 implementation.  See kiwi_extrude.h
// for the creation sequence this mirrors, the two documented divergences from
// it, and the undo reasoning (which was read out of undo.cpp, not assumed).
//
// NEW code over the ported cores.  Every brush that reaches the map goes through
// the ported allocator / linker / list functions in the ported order; this file
// owns the profile math, the cursor mapping and the validity gate.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
// ROUND X, ITEM 4b — the translucent prism preview needs the triangle path.
// Same three headers kiwi_split.cpp:18-20 pulls in for its cut quad.
#include <gfx_d3d/r_gfx.h>          // GfxColor, Byte4PackPixelColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT,
                                    // R_AddRenderCmdDrawTris, R_AddCmdSetMaterialColor

#include "kiwi_extrude.h"
#include "kiwi_boxselect.h"             // ROUND K — the ONE click grammar
#include "kiwi_camera.h"                // ROUND AI, ITEM 2 — KiwiCam_AxisPortrayable
// ROUND AP, ITEM 1: kiwi_boolean.h is GONE from this file.  The only thing it was
// ever pulled in for was round AF's push-in auto-carve (the point probe, the dry
// run and the difference cascade), which the user has now withdrawn by directive —
// see the AUTO-CARVE WITHDRAWN note over Commit().  Nothing else in this file has
// ever performed a boolean.
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_grid.h"
#include "kiwi_hover.h"                 // shakeout G — E's hovered-face arm
#include "kiwi_lines.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_region.h"
#include "kiwi_selection.h"             // shakeout G — E's selected-face arm
#include "kiwi_snap.h"
#include "kiwi_transform.h"             // ROUND Q — KiwiXform_PushFaceOnce (negative E)
#include "kiwi_units.h"
#include "kiwi_validity.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <stdarg.h>
#include <stdint.h>     // ROUND X, ITEM 4b — uint16_t index buffers
#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int         Sys_Printf( const char *fmt, ... );                     // win_qe3.cpp
extern camera_s   *Ed_Camera();                                            // camwnd.cpp
extern int         g_nUpdateBits;                                          // 0x25D5A74 (mainfrm.cpp)

// ── KIWI-UX (ROUND BP, ITEM 2): the shared "is Ctrl making this absolute" read ──
// The physical key, exactly as KiwiCmd_SnapEngaged reads it (kiwi_command.cpp:1554
// — KIWI-UX (ROUND BT): re-cited, round BR shifted it)
// and as every ported drag path does (drag.cpp:308) — polled, never latched, so it
// cannot go stale against a message queue this layer does not run off.  It is the
// SAME key that turns the ranked snap query on for a transform, which is the point:
// one modifier, one meaning (round BO, item 3), now with the depth mapping joining
// the things it changes.  Not a member of any command, because all three one-axis
// gestures ask it and two of them live in another file.
bool KiwiExt_AbsoluteHeld()
{
    return ( ::GetAsyncKeyState( VK_CONTROL ) & 0x8000 ) != 0;
}

// ── KIWI-UX (ROUND BT): THE ONE-AXIS LADDER (kiwi_extrude.h) ────────────────
// The full argument, the autopsy and the ranking are on the declaration.  Every
// rule here is an EXISTING shipped rule, moved rather than invented: the named-
// target exemption is kiwi_transform.cpp's `namedTarget` (round BL, item 4), the
// area magnet is KiwiSnap_AreaMagnet (round BK, items 6a/6c) and the lattice with
// its major preference is KiwiSnap_LatticeAxis (round BL, item 4).  What round BT
// changes is the ORDER they are consulted in: geometry ranks above the grid, which
// is what kiwi_snap.h has said in prose since v1 and what round BP's nearest-value
// comparison quietly inverted for every one-axis gesture.
float KiwiExt_LadderDepth( const snap_result_t &snap, const float *ref,
                           const float *axis, float rawAbs, bool *outMajor )
{
    if ( outMajor )
        *outMajor = false;
    if ( !ref || !axis )
        return rawAbs;

    // Rung 1/2 — the geometry answer, resolved onto THIS gesture's axis so a face
    // gives the same depth everywhere on it (kiwi_snap.h KiwiSnap_AxisDepth), and
    // refused when it is glued to the source plane (KEXT_SELF_SNAP_BAND).
    float sd = 0.0f;
    const bool haveGeom = snap.valid && KiwiSnap_IsGeometry( snap.type )
                       && KiwiSnap_AxisDepth( snap, ref, axis, &sd )
                       && fabsf( sd ) >= KEXT_SELF_SNAP_BAND;
    if ( haveGeom )
    {
        if ( snap.type != SNAP_FACE )
            return sd;                     // a place the user aimed at: never rounded

        float at[3];
        for ( int k = 0; k < 3; ++k )
            at[k] = ref[k] + axis[k] * rawAbs;
        // An area hit is a MAGNET, not a teleport: it takes the depth as the cursor
        // sweeps across the plane and passes through to the lattice outside the band.
        if ( KiwiSnap_AreaMagnet( rawAbs, sd, at ) == sd )
            return sd;
    }

    // Rung 3 — the lattice, absolute on a world axis, majors preferred.
    bool major = false;
    const float ld = KiwiSnap_LatticeAxis( rawAbs, ref, axis, true, &major );
    if ( outMajor )
        *outMajor = major;
    return ld;
}

extern brush_t    *Brush_Alloc( const void *materialDefSrc, eclass_t *ecls ); // brush.cpp 0x4751e0
extern selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner );          // brush.cpp 0x475980
extern void        Brush_AddToList2( selbrush_t *b );                         // brush.cpp 0x4765a0
extern void        Brush_Free_R( brush_t *def );                              // brush.cpp 0x475af0
extern void        Entity_LinkBrush( brush_t *b, entity_s *world_ent );       // entity.cpp 0x484fc0
extern void        Select_Deselect( int bAlsoFreeFaces );                     // select.cpp 0x48E800 (int, NOT char — mangling)
extern entity_s   *world_entity;                                              // entity.cpp 0x25D5B30

// brush.cpp / xywnd.cpp // KIWI-UX forwarders (see kiwi_extrude.h).
extern void Ed_BrushSetFaceCount( brush_t *def, int faceCount );
extern void Ed_EnsureCurrentMaterial_Kiwi();

// ROUND X, ITEM 4b — the translucent fill pair, declared exactly as
// kiwi_region.cpp:92-98 and kiwi_split.cpp:68-72 declare it (one spelling in three
// files; a drift here is a link failure, which is why it is copied verbatim).
// R_AddCmdSetMaterialColor comes from r_rendercmds.h (declared __cdecl there).
extern char        Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void        __cdecl R_AddRenderCmdDrawTris(
                       Material *material, MaterialTechniqueType techType, short indexCount,
                       const uint16_t *indices, short vertexCount,
                       const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                       const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    // KIWI-UX (shakeout E): the numeric field table (kiwi_command.h
    // NumericFields).  STATIC storage — the numeric layer copies the struct but
    // never the label string.
    const kiwiNumField_t KEXT_FIELDS[1] = { { "length", KNUM_LENGTH, false } };

    const float KEXT_COL_PREVIEW[3] = { 0.55f, 0.85f, 1.00f };   // the prism outline
    const float KEXT_COL_BAD[3]     = { 1.00f, 0.30f, 0.25f };
    // ROUND Q — the un-extrude's two previews.  DOOMED is the SAME dim red the
    // interactive push-through delete already draws with (kiwi_transform.cpp
    // DrawWorld), so "this brush is about to go" looks the same wherever it is
    // said.  CARVE is the ordinary preview blue, warmed, because a carve is an
    // ordinary reshape and must not read as a warning.
    const float KEXT_COL_CARVE[3]   = { 1.00f, 0.72f, 0.35f };
    const float KEXT_COL_DOOMED[3]  = { 0.60f, 0.13f, 0.13f };

    // ROUND Q — the slack on the push-through test.  KIWI-UX (CLEANUP, A-26):
    // this WAS a private 1.0e-4f "numerically identical to kiwi_transform.cpp's
    // KX_EPS".  The HUD's correctness depends on the two being the SAME number,
    // not on two literals agreeing, so it is now the exported KXPUSH_EPS
    // (kiwi_transform.h) that KiwiXform_PushFaceOnce itself compares with.
    const float KEXT_PUSH_EPS = KXPUSH_EPS;

    // ROUND Q — which of the three things E is about to do (KiwiExtrudeFaceCommand).
    enum kiwiExtrudeFaceMode_t
    {
        KEXTF_IDLE = 0,   // |distance| too small to mean anything
        KEXTF_GROW,       // OUT: a new body off the face winding (the original verb)
        KEXTF_CARVE,      // IN : push the SOURCE face inward — the brush shrinks
        KEXTF_DESTROY,    // IN, all the way through: the brush is removed
    };


    // KIWI-UX (CLEANUP, RayAxis): the local copy is gone — it was one of four
    // byte-identical bodies.  It is KiwiCam_RayAxis (kiwi_camera.h) now, beside
    // the KCAM_RAYAXIS_MIN_DEN gate every copy already cited.

    // ── the prism builder ───────────────────────────────────────────────────
    // `loop` is a CCW plane-space ring, `dist` the signed travel along
    // plane.normal.  Every face is written as THREE PLANEPTS wound so
    // Face_MakePlane's cross(p0 - p1, p2 - p1) comes out OUTWARD.
    //
    // The two cap heights are chosen by SIGN so the "bottom" cap is always the
    // one at the smaller offset: that keeps the side-face winding rule a single
    // expression instead of two mirrored ones.
    struct prismBuild_t
    {
        kconPlane_t plane;
        float       lo, hi;              // offsets along plane.normal
    };

    void PrismVertex( const prismBuild_t &pb, const float uv[2], bool high, float out[3] )
    {
        float base[3];
        KiwiCon_PlaneToWorld( pb.plane, uv, base );
        Mad3( base, pb.plane.normal, high ? pb.hi : pb.lo, out );
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND X, ITEM 4b) — THE PREVIEW IS A SOLID, NOT A SET OF WIRES.
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "The Preview is still not 3d either.  It's just lines
    // from each vertex going up."
    //
    // Literally true of what shipped: DrawWorld drew the TOP ring plus one vertical
    // per profile vertex, and nothing else — no bottom ring, no faces.  A ring
    // floating over a fan of verticals reads as scaffolding, not as the body the
    // command is about to make.
    //
    // What ships now, per gesture: the SIDE QUADS and the MOVING CAP, filled and
    // translucent, with the full wireframe (bottom ring, top ring, verticals) drawn
    // over them so the silhouette stays readable against dark geometry.
    //
    // It is drawn with the SAME machinery every other translucent overlay in this
    // layer uses — R_AddRenderCmdDrawTris on g_qeglobals.d_white inside a neutral
    // MATERIAL_COLOR bracket, per-vertex packed RGBA — because kiwi_lines pins alpha
    // to 1 (kiwi_lines.h TRAP 2) and cannot express a translucent surface at all.
    // kiwi_region.cpp's region fills (KiwiRegion_DrawFills) and kiwi_split.cpp's cut
    // quad are the two existing users and this is their idiom, unchanged.  Emitting
    // draw commands from inside the framework's open kiwi_lines batch is likewise
    // already established (the cut tool does exactly this from its own DrawWorld).
    // KIWI-UX (ROUND AA, ITEM 2): `idx` is now MUTABLE — this is the one choke
    // point both the cap and the sides go through, so the eye-orient lives here
    // and covers the whole prism in one place.
    void EmitFilledTris( float *xyzw, int vertCount,
                         uint16_t *idx, int idxCount, const float rgba[4] )
    {
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };

        const camera_s *cam = Ed_Camera();
        if ( !cam || vertCount < 3 || idxCount < 3 )
            return;

        enum { KEXT_PREVIEW_MAX_VERTS = 4 * ( KEXT_MAX_PROFILE + 1 ) };
        if ( vertCount > KEXT_PREVIEW_MAX_VERTS )
            return;

        static float nrm[KEXT_PREVIEW_MAX_VERTS][3];
        static float st [KEXT_PREVIEW_MAX_VERTS][2];
        static float col[KEXT_PREVIEW_MAX_VERTS];

        float rgbaCopy[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( rgbaCopy, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        for ( int i = 0; i < vertCount; ++i )
        {
            // KIWI-UX (ROUND AL, ITEM 1): a CONSTANT world normal, not `-vpn`.
            // kiwi_lines.h TRAP 4 — the fill's normal feeds a fixed-direction
            // shading term, so a camera vector here makes the preview's
            // brightness a function of where the camera is pointing.
            KiwiTris_FillNormal( nrm[i] );
            st[i][0]  = 0.0f;
            st[i][1]  = 0.0f;
            col[i]    = packedAsFloat;
        }

        // KIWI-UX (ROUND AA, ITEM 2) — see the (now corrected) claim below and
        // kiwi_lines.h TRAP 3.  Per-triangle, so a closed prism gets its near
        // half AND its far half: each side quad and each cap triangle resolves
        // against the eye on its own.
        KiwiTris_OrientToEye( xyzw, 4, idx, idxCount, cam->origin );

        R_AddCmdSetMaterialColor( s_neutral );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)idxCount, idx, (short)vertCount,
                                (float (*)[4])xyzw, nrm, col, st );
        R_AddCmdSetMaterialColor( s_white );
    }

    // The solid half of a prism preview: `loopUV` is the profile in `pb.plane`'s own
    // (u,v), `capTris` its triangulation (indices into the profile), and both the
    // moving cap and every side quad come out of the same two rings.
    //
    // TWO DRAW CALLS, not one: the cap is drawn a shade stronger than the sides so
    // the face the drag is actually moving is the one the eye lands on, which is the
    // whole point of previewing a push.
    //
    // KIWI-UX (ROUND AA, ITEM 2) — THE PARAGRAPH THAT USED TO BE HERE WAS WRONG.
    // It read "backface culling is not available on this path (the state comes from
    // d_white's material), so both are drawn once and the far side simply shows
    // through".  Backface culling is available on this path and it comes from
    // EXACTLY that material: white_tools' refStateBits[0] decode is GFXS0_CULL_BACK
    // (kiwi_lines.h TRAP 3, and round Y's decode on the cut disc).  The far side did
    // NOT show through — it was culled, and which half of the prism you lost
    // depended on which side of it you stood.  EmitFilledTris now orients every
    // triangle at the eye, which makes the sentence true for the first time.
    void EmitPrismSolid( const prismBuild_t &pb, const std::vector<float> &loopUV,
                         const std::vector<int> &capTris, const float rgb[3] )
    {
        const int n = (int)( loopUV.size() / 2 );
        if ( n < 3 || n > KEXT_MAX_PROFILE )
            return;
        if ( !( pb.hi - pb.lo > 1.0e-4f ) )
            return;                          // no depth: there is no solid to show

        // ── the moving CAP ──────────────────────────────────────────────────
        // Which end moves is the sign of the push: a positive distance grows the
        // HIGH cap away from the source, a negative one drives the LOW cap.
        // MakeBuild puts the source plane at whichever of lo/hi is zero, so the
        // moving end is simply the other one.
        const bool capHigh = !( pb.lo < 0.0f );
        {
            static float xyzw[KEXT_MAX_PROFILE][4];
            static uint16_t idx[( KEXT_MAX_PROFILE - 2 ) * 3];
            if ( (int)capTris.size() >= 3
              && (int)capTris.size() <= ( KEXT_MAX_PROFILE - 2 ) * 3 )
            {
                for ( int i = 0; i < n; ++i )
                {
                    float w[3];
                    PrismVertex( pb, &loopUV[(size_t)i * 2], capHigh, w );
                    xyzw[i][0] = w[0]; xyzw[i][1] = w[1]; xyzw[i][2] = w[2];
                    xyzw[i][3] = 1.0f;
                }
                for ( size_t k = 0; k < capTris.size(); ++k )
                    idx[k] = (uint16_t)capTris[k];
                const float capRgba[4] = { rgb[0], rgb[1], rgb[2], 0.34f };
                EmitFilledTris( &xyzw[0][0], n, idx, (int)capTris.size(), capRgba );
            }
        }

        // ── the SIDES, all of them in one batch ─────────────────────────────
        // 4 verts and 6 indices per profile edge.  One call rather than one per
        // quad: every side shares a colour, and a per-quad call would open and
        // close the MATERIAL_COLOR bracket n times a frame.
        {
            static float    xyzw[4 * KEXT_MAX_PROFILE][4];
            static uint16_t idx [6 * KEXT_MAX_PROFILE];
            int v = 0, t = 0;
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float p[4][3];
                PrismVertex( pb, &loopUV[(size_t)i * 2], false, p[0] );
                PrismVertex( pb, &loopUV[(size_t)j * 2], false, p[1] );
                PrismVertex( pb, &loopUV[(size_t)j * 2], true,  p[2] );
                PrismVertex( pb, &loopUV[(size_t)i * 2], true,  p[3] );
                const int base = v;
                for ( int k = 0; k < 4; ++k, ++v )
                {
                    xyzw[v][0] = p[k][0]; xyzw[v][1] = p[k][1]; xyzw[v][2] = p[k][2];
                    xyzw[v][3] = 1.0f;
                }
                idx[t++] = (uint16_t)( base + 0 );
                idx[t++] = (uint16_t)( base + 1 );
                idx[t++] = (uint16_t)( base + 2 );
                idx[t++] = (uint16_t)( base + 0 );
                idx[t++] = (uint16_t)( base + 2 );
                idx[t++] = (uint16_t)( base + 3 );
            }
            const float sideRgba[4] = { rgb[0], rgb[1], rgb[2], 0.20f };
            EmitFilledTris( &xyzw[0][0], v, idx, t, sideRgba );
        }
    }

    // ROUND AG, ITEM 1: the prism WIREFRAME — top ring, bottom ring and the
    // verticals, into the currently open kiwi_lines batch at the caller's colour.
    // Lifted out of the two previews' inline copies because a MULTI-SOURCE preview
    // needs to draw it once per source and a third copy would have drifted.
    void EmitPrismWire( const prismBuild_t &pb, const std::vector<float> &loopUV )
    {
        const int n = (int)( loopUV.size() / 2 );
        for ( int i = 0; i < n; ++i )
        {
            const int j = ( i + 1 ) % n;
            float a[3], b[3];
            PrismVertex( pb, &loopUV[(size_t)i * 2], true,  a );
            PrismVertex( pb, &loopUV[(size_t)j * 2], true,  b );
            if ( !KiwiLines_Add( a, b ) )
                return;
            PrismVertex( pb, &loopUV[(size_t)i * 2], false, a );
            PrismVertex( pb, &loopUV[(size_t)j * 2], false, b );
            if ( !KiwiLines_Add( a, b ) )
                return;
            PrismVertex( pb, &loopUV[(size_t)i * 2], false, a );
            PrismVertex( pb, &loopUV[(size_t)i * 2], true,  b );
            if ( !KiwiLines_Add( a, b ) )
                return;
        }
    }

    inline float Cross2( const float *o, const float *a, const float *b )
    {
        return ( a[0] - o[0] ) * ( b[1] - o[1] ) - ( a[1] - o[1] ) * ( b[0] - o[0] );
    }

    // Three WELL-SPREAD ring positions for the cap planepts, returned in ASCENDING
    // ring order.  Order matters: on a convex CCW ring, any three vertices taken in
    // increasing index order are themselves CCW, which is what makes the cap
    // winding rule below a single expression.  Spread matters because three
    // ADJACENT vertices of a 64-gon are nearly collinear, and a near-collinear
    // planept triple is exactly what the §19 V2 check rejects.
    bool CapTriple( const std::vector<float> &loopUV, const std::vector<int> &ring,
                    int *o0, int *o1, int *o2 )
    {
        const int n = (int)ring.size();
        if ( n < 3 )
            return false;
        const float *p0 = &loopUV[(size_t)ring[0] * 2];

        int   i1 = -1;
        float best = 0.0f;
        for ( int k = 1; k < n; ++k )
        {
            const float *p = &loopUV[(size_t)ring[k] * 2];
            const float d = ( p[0] - p0[0] ) * ( p[0] - p0[0] )
                          + ( p[1] - p0[1] ) * ( p[1] - p0[1] );
            if ( i1 < 0 || d > best ) { best = d; i1 = k; }
        }
        if ( i1 < 0 || best < 1.0e-6f )
            return false;

        int   i2 = -1;
        float bestArea = 0.0f;
        const float *p1 = &loopUV[(size_t)ring[i1] * 2];
        for ( int k = 1; k < n; ++k )
        {
            if ( k == i1 )
                continue;
            const float a = fabsf( Cross2( p0, p1, &loopUV[(size_t)ring[k] * 2] ) );
            if ( i2 < 0 || a > bestArea ) { bestArea = a; i2 = k; }
        }
        if ( i2 < 0 || bestArea < 1.0e-4f )
            return false;                    // every vertex is collinear — no plane

        int t[3] = { 0, i1, i2 };
        for ( int a = 0; a < 2; ++a )
            for ( int b = a + 1; b < 3; ++b )
                if ( t[b] < t[a] ) { const int s = t[a]; t[a] = t[b]; t[b] = s; }
        *o0 = t[0]; *o1 = t[1]; *o2 = t[2];
        return true;
    }

    // ── KIWI-UX (ROUND R): THE PROFILE RING'S OWN DEFENCE ───────────────────
    // USER REPORT: "Extrude: rejected — duplicate plane", repeatedly, on a square
    // drawn out of five lines.  Every side face this builder writes is "through
    // profile edge e, parallel to the extrusion axis", so TWO CONSECUTIVE EDGES
    // THAT LIE ON ONE LINE produce TWO IDENTICAL PLANES — and §19's V5 gate
    // (kiwi_validity.cpp, dot > KVALID_PLANE_DOT and |d0-d1| < KVALID_PLANE_DIST)
    // reports exactly that.  A ZERO-LENGTH edge is the same defect one step worse:
    // its plane has no normal at all and trips V2 instead.
    //
    // kiwi_region.cpp's AcceptLoop now removes both shapes from every region loop,
    // which is where the FIX belongs.  This is the SECOND line of defence, and it is
    // worth its twenty lines because this builder has a second caller that never
    // sees a region at all — KiwiExtrude_BuildPrismDef, which is handed a profile by
    // kiwi_primitive.cpp / kiwi_bevel.cpp — and because a def that reaches
    // Brush_Alloc and then fails the gate costs an allocation and a free for
    // something that was decidable from the ring.
    //
    // Same tolerance as the gate it is protecting, for the same reason AcceptLoop
    // uses it: the test that decides acceptance and the test that decides
    // preparation must be one test.  Runs to a fixed point (removing one vertex can
    // leave its neighbours collinear).
    void CleanRing( const std::vector<float> &loopUV, std::vector<int> *ring )
    {
        bool changed = true;
        while ( changed )
        {
            changed = false;
            const int n = (int)ring->size();
            if ( n < 4 )
                return;
            for ( int i = 0; i < n; ++i )
            {
                const float *a = &loopUV[(size_t)( *ring )[( i + n - 1 ) % n] * 2];
                const float *b = &loopUV[(size_t)( *ring )[i] * 2];
                const float *c = &loopUV[(size_t)( *ring )[( i + 1 ) % n] * 2];
                const float e0[2] = { b[0] - a[0], b[1] - a[1] };
                const float e1[2] = { c[0] - b[0], c[1] - b[1] };
                const float l0 = sqrtf( e0[0] * e0[0] + e0[1] * e0[1] );
                const float l1 = sqrtf( e1[0] * e1[0] + e1[1] * e1[1] );
                bool drop = false;
                if ( !( l1 > 1.0e-4f ) )
                    drop = true;             // b == c: a zero-length side face
                else if ( l0 > 1.0e-4f )
                    drop = ( ( e0[0] * e1[0] + e0[1] * e1[1] ) / ( l0 * l1 ) )
                           > KVALID_PLANE_DOT;
                if ( !drop )
                    continue;
                ring->erase( ring->begin() + i );
                changed = true;
                break;
            }
        }
    }

    // Write one convex piece as a brush def.  Returns the def, or NULL when the
    // piece is unusable.  The def is NOT linked into the map — the caller lands it
    // only after every piece has passed the §19 gate.
    brush_t *BuildPieceDef( const prismBuild_t &pb, const std::vector<float> &loopUV,
                            const std::vector<int> &ringIn, const char **why )
    {
        // KIWI-UX (ROUND R): work on a CLEANED copy — see CleanRing above.  A copy
        // rather than a mutation of the caller's ring: the caller (Extrude) keeps
        // its decomposition to draw the preview from, and a builder that quietly
        // rewrote it would make the preview and the result disagree.
        std::vector<int> ring = ringIn;
        CleanRing( loopUV, &ring );
        const int n = (int)ring.size();
        if ( n < 3 )
        {
            *why = "piece has fewer than 3 vertices";
            return 0;
        }
        if ( n > KEXT_MAX_PROFILE )
        {
            *why = "piece over the profile cap";
            return 0;
        }

        int i0, i1, i2;
        if ( !CapTriple( loopUV, ring, &i0, &i1, &i2 ) )
        {
            *why = "piece is degenerate (collinear)";
            return 0;
        }

        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, 0 );
        if ( !def )
        {
            *why = "brush allocation failed";
            return 0;
        }
        Ed_BrushSetFaceCount( def, n + 2 );
        if ( !def->faces || def->faceCount != n + 2 )
        {
            Brush_Free_R( def );              // still unlinked, refCount 0
            *why = "face array allocation failed";
            return 0;
        }

        // face 0 — the HIGH cap.  Face_MakePlane's normal is cross(p0 - p1, p2 - p1);
        // feeding the CCW triple in REVERSE (i2, i1, i0) makes that come out
        // +plane.normal, which is outward for the high cap.
        {
            face_t *f = &def->faces[0];
            PrismVertex( pb, &loopUV[(size_t)ring[i2] * 2], true, f->planepts[0] );
            PrismVertex( pb, &loopUV[(size_t)ring[i1] * 2], true, f->planepts[1] );
            PrismVertex( pb, &loopUV[(size_t)ring[i0] * 2], true, f->planepts[2] );
        }
        // face 1 — the LOW cap, the same triple in CCW order, giving -plane.normal.
        {
            face_t *f = &def->faces[1];
            PrismVertex( pb, &loopUV[(size_t)ring[i0] * 2], false, f->planepts[0] );
            PrismVertex( pb, &loopUV[(size_t)ring[i1] * 2], false, f->planepts[1] );
            PrismVertex( pb, &loopUV[(size_t)ring[i2] * 2], false, f->planepts[2] );
        }

        // faces 2..n+1 — one side per profile edge, through the edge and parallel
        // to the extrusion axis.  Three WELL-SPREAD points: both edge ends at the
        // low cap plus one of them at the high cap, so the triangle always has the
        // full edge length in one direction and the full extrusion depth in the
        // other.
        //
        // ORIENTATION, worked out rather than guessed.  With the right-handed basis
        // (u × v == normal) and a CCW ring, the interior lies to the LEFT of a→b in
        // plane space, so OUTWARD is to its right: u*dv - v*du.  Feeding
        // (b_lo, a_lo, a_hi) makes Face_MakePlane compute
        //     cross(p0 - p1, p2 - p1) = cross(D, normal * H),  H = hi - lo > 0
        // and D × normal = (u*du + v*dv) × (u × v) = u*dv - v*du — exactly outward.
        for ( int e = 0; e < n; ++e )
        {
            const float *a = &loopUV[(size_t)ring[e] * 2];
            const float *b = &loopUV[(size_t)ring[( e + 1 ) % n] * 2];
            face_t *f = &def->faces[2 + e];
            PrismVertex( pb, b, false, f->planepts[0] );
            PrismVertex( pb, a, false, f->planepts[1] );
            PrismVertex( pb, a, true,  f->planepts[2] );
        }
        return def;
    }

    // Land a finished def: the ported Ed_NewBrushDrag tail, in its order.
    selbrush_t *LandDef( brush_t *def )
    {
        Entity_LinkBrush( def, (entity_s *)world_entity->def );
        selbrush_t *inst = Brush_AddToList( def, world_entity );
        Brush_AddToList2( inst );                 // → selected_brushes
        return inst;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  §23 the modal command.
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiExtrudeCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Extrude Region"; }
        bool CanExecute() override { return KiwiExtrude_CanExecute(); }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        // ── shakeout E: one LENGTH field, the live distance, and the resume ──
        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KEXT_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_dist;                     // SIGNED: which way the prism grew
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || m_region < 0 )
                return false;
            // The profile centroid, lifted to the extrusion's current top — the
            // middle of the prism face the drag is moving.
            for ( int k = 0; k < 3; ++k )
                out3[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            return true;
        }

        void Rebase() override
        {
            // Re-latch the axis origin so the CURRENT distance is what the cursor
            // now reproduces.  ROUND AI, ITEM 2: the `m_start -= m_dist` correction
            // that used to live HERE moved INTO LatchStart, because the view gate
            // needs the same preservation and there must be exactly one copy of it.
            // (Leaving it here as well would subtract the distance twice.)
            LatchStart();
        }

        // ── ROUND K: THE LOLLIPOP (kiwi_lollipop.h) ───────────────────
        // A region extrude is the same one-degree-of-freedom gesture a face
        // push/pull is, so it gets the same handle — and the same two rules: the
        // anchor is the LIVE top of the prism (so the ball rides the surface being
        // pulled instead of being swallowed by it) and the direction carries the
        // distance's SIGN (so a region pulled the other way keeps its stem on the
        // side the user is dragging toward).
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( m_region < 0 || !outAnchor || !outDir )
                return false;
            for ( int k = 0; k < 3; ++k )
            {
                outAnchor[k] = m_ref[k] + m_plane.normal[k] * m_dist;
                outDir[k]    = ( m_dist < 0.0f ) ? -m_plane.normal[k] : m_plane.normal[k];
            }
            return true;
        }

        bool Begin() override
        {
            m_region  = -1;
            m_dist    = 0.0f;
            m_hasNum  = false;
            m_invalid = false;
            m_hud[0]  = '\0';

            const std::vector<kregion_t> &regions = KiwiRegion_All();
            if ( regions.empty() )
            {
                Sys_Printf( "Extrude: no closed construction region.\n" );
                // ── KIWI-UX (ROUND AF, ITEM 5): …AND WHY NOT ────────────────
                // USER REPORT, verbatim: "Curve closed loop detection still needs a
                // bit more work.  I find myself having to re-trace the line points
                // myself to get it to detect a construction face."
                //
                // This line was the ONLY user-visible sign that detection had
                // failed, and it named no reason — which is exactly what leaves
                // re-tracing the points as the user's best move.  The gap report
                // names the nearest unjoined end, the distance, and the weld it
                // missed by (kiwi_region.h ITEM 5(c)).  `force` because a refusal
                // the user is looking at should always answer, even if the store
                // has not changed since the last time it did.
                KiwiRegion_ReportGaps( true );
                return false;
            }

            // ── KIWI-UX (ROUND K): A SELECTED REGION IS THE ONE, first ──────
            // Round K makes a region CLICKABLE (kiwi_region.h), and a selection is
            // a state the user deliberately put the editor into — so E with a region
            // selected extrudes THAT region wherever the cursor happens to be, which
            // is the directive's "E with a region selected does the same".  The
            // cursor arm below is unchanged and still covers the no-selection case.
            m_region = KiwiRegion_SelectedIndex();
            m_extra.clear();

            ray_t ray;
            if ( m_region < 0 && Pick_RayFromCursor( &ray ) )
                m_region = KiwiRegion_PickAt( ray );
            if ( m_region < 0 && regions.size() == 1 )
                m_region = 0;                     // the documented "only one" shortcut
            if ( m_region < 0 )
            {
                Sys_Printf( "Extrude: put the cursor over a region first "
                            "(%i available).\n", (int)regions.size() );
                return false;
            }

            // ── ROUND AG, ITEM 1: EVERY SELECTED REGION COMES ALONG ─────────
            // USER DIRECTIVE: "I should be able to shift click construction faces"
            // and have one E extrude all of them.  The set is latched HERE, once,
            // into indices: nothing edits the construction store during a drag, and
            // re-resolving centroids per frame would put a scan in the hot path for
            // an answer that cannot change.  The PRIMARY stays m_region and still
            // drives the axis, the reference point and the numeric field.
            //
            // A HOVERED (not selected) region is single-source by construction —
            // there is no set to join — which is the pre-round-AG behaviour intact.
            {
                const int selN = KiwiRegion_SelectedCount();
                for ( int s = 0; s < selN; ++s )
                {
                    const int idx = KiwiRegion_SelectedAt( s );
                    if ( idx >= 0 && idx != m_region )
                        m_extra.push_back( idx );
                }
            }

            const kregion_t &reg = regions[m_region];
            if ( (int)( reg.pts.size() / 2 ) > KEXT_MAX_PROFILE )
            {
                Sys_Printf( "Extrude: profile has %i vertices, the v1 limit is %i.\n",
                            (int)( reg.pts.size() / 2 ), KEXT_MAX_PROFILE );
                return false;
            }

            // The reference point the drag maps against: the profile centroid,
            // lifted onto the region plane.
            m_plane = reg.plane;
            float c[2] = { 0.0f, 0.0f };
            const int n = (int)( reg.pts.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                c[0] += reg.pts[i * 2 + 0];
                c[1] += reg.pts[i * 2 + 1];
            }
            c[0] /= (float)n;
            c[1] /= (float)n;
            KiwiCon_PlaneToWorld( m_plane, c, m_ref );

            m_dist  = 0.0f;
            m_axisBlocked = false;  // ROUND AI, ITEM 2 — recomputed on the first move
            LatchStart();
            UpdateHud();
            // ROUND AP, ITEM 1: no cavity sentence.  An extrude lands a prism, in
            // both directions, whatever it overlaps; Q is the verb for taking
            // material away.
            Sys_Printf( "Extrude: drag along the region normal, or type a distance.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        // ── ROUND K: a plain click while PARKED AND UNMOVED re-selects ──────
        // Same rung, same gate and the same delegation as the face push/pull's
        // (kiwi_transform.cpp writes the rule table out in full).  A region extrude
        // is auto-entered by a region CLICK, so without this the very next click
        // would be swallowed as a resume and the user could not move to another
        // region — the identical complaint, on the identical mechanism.
        //
        // "Nothing applied" is provable here too, and more cheaply than for the
        // face: this command mutates nothing at all until Commit, and its undo
        // bracket is opened INSIDE Extrude() (kiwi_extrude.h's ordering note), so
        // while the distance is still under KEXT_MIN_DIST there is neither geometry
        // to restore nor a record to close.
        // ROUND N: the same question without a press, so a face/solid context verb
        // can take over a region extrude that has done nothing — the identical rung
        // the face push/pull gets (kiwi_command.h PreemptIdle).  A region extrude is
        // auto-entered by a region CLICK, so it is in exactly the same class of
        // "you are in a modal command you never asked for" state.
        bool PreemptIdle() const override
        {
            return m_region >= 0 && !m_hasNum && !( fabsf( m_dist ) >= KEXT_MIN_DIST );
        }

        // KIWI-UX (ROUND BO, ITEM 3): round Z's `SnapOptIn() { return true; }` is
        // GONE — not because the behaviour changed (an extrude is still raw until
        // Ctrl is held: *"with extruding, same thing, no snapping unless ctrl"*) but
        // because it is no longer a per-command exception.  This command WantsClicks
        // false, so kiwi_command.h SnapContext already puts it in KSNAPCTX_TRANSFORM,
        // which IS "raw, Ctrl snaps".  One rule, nothing to remember.

        bool IdlePressReselect( int imgX, int imgY, bool shift ) override
        {
            if ( !PreemptIdle() )
                return false;
            KiwiCmd_Cancel();
            KiwiBox_ClickSelectAt( imgX, imgY, shift, false );
            return true;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        void Commit() override
        {
            if ( fabsf( m_dist ) < KEXT_MIN_DIST )
            {
                Sys_Printf( "Extrude: zero distance — nothing created.\n" );
                return;
            }
            // ═══════════════════════════════════════════════════════════════
            //  KIWI-UX (ROUND AP, ITEM 1) — THE AUTO-CARVE IS WITHDRAWN
            // ═══════════════════════════════════════════════════════════════
            // USER DIRECTIVE, verbatim: "when extruding, dont automatically bool
            // diff the solid.  It can be done by the user with a boolean after."
            //
            // Round AF, item 2 forked HERE on a point probe (DragsIntoSolid: is
            // there solid one KEXT_MIN_DIST along the drag direction from the
            // profile centroid?), and when it said yes the prism pieces became
            // boolean TOOLS instead of bodies.  The probe was
            // deliberately not the sign of the distance, so an extrude drawn on a
            // wall silently changed verb depending on which side of that wall the
            // centroid was — which is exactly the surprise the user is refusing.
            //
            // WHAT REPLACES IT: nothing.  An extrude LANDS THE PRISM, in either
            // direction, and it is allowed to interpenetrate whatever is there.
            // Carving is Q (kiwi_boolean.h), which is one keypress, is explicit, has
            // its own preview and its own undo record, and reaches the SAME
            // CarveTarget subtract the cascade did — so the geometry a user gets by
            // extruding and then pressing Q is the geometry the auto-carve produced.
            // KIWI-UX (CLEANUP, A-16): kiwi_boolean's by-def entry points were
            // deleted once this caller went; Q's own path is the whole surface now.
            //
            // (The FACE command's round-Q KEXTF_CARVE/KEXTF_DESTROY is a DIFFERENT
            // feature and STAYS: it moves one face PLANE of one brush through
            // KiwiXform_PushFaceOnce — a reshape of the brush the user grabbed, not
            // a boolean against bystanders, and the sign of the drag is its whole
            // trigger.  See the AP note over KiwiExtrudeFaceCommand.)
            Extrude();
            // ROUND K, "After confirming an action, the part should be de-selected
            // as well": the REGION drops out of the selection.  The new BRUSH stays
            // selected (Extrude's LandDef tail put it there) because that is the
            // result of the flow — which is Plasticity's own tail too: it adds the
            // results to the selection and removes the source faces and regions from
            // it (ExtrudeCommand.ts:65-68).  The source LINES are untouched: they are
            // scaffolding, and deleting them would be a second, unasked-for edit.
            KiwiRegion_ClearSelection();
        }

        void Cancel() override
        {
            m_region = -1;
            m_extra.clear();            // ROUND AG, ITEM 1
            g_nUpdateBits |= 1;
        }

        // ROUND AG, ITEM 1: the preview costs 3 segments per profile vertex per
        // SOURCE now, so the command names what it needs and the framework scales
        // its batch (kiwi_command.h LineBudget, round AF's own mechanism).  Without
        // this a two-region extrude would silently lose the second prism's
        // wireframe at exactly the cliff round AF removed for the boolean.
        int LineBudget() const override
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            int segs = 0;
            for ( size_t i = 0; i < srcs.size(); ++i )
                segs += 3 * (int)( regions[(size_t)srcs[i]].pts.size() / 2 );
            return segs + 96;          // …plus the snap marker's own headroom
        }

        void DrawWorld() override
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            for ( size_t si = 0; si < srcs.size(); ++si )
            {
            const kregion_t &reg = regions[(size_t)srcs[si]];

            prismBuild_t pb;
            MakeBuildFor( reg, &pb );

            // ROUND AP, ITEM 1: the amber CARVE preview went with the carve.  Two
            // colours again: blue is the prism, red is a refusal.
            const float *col = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;

            // ── KIWI-UX (ROUND X, ITEM 4b): THE SOLID, FIRST ────────────────
            // USER REPORT: "The Preview is still not 3d either.  It's just lines
            // from each vertex going up."  Side quads + the moving cap, translucent
            // (EmitPrismSolid).  Drawn BEFORE the wireframe so the edges read on top
            // of the fill rather than being lost inside it; nothing is drawn at all
            // while the prism has no depth, which is EmitPrismSolid's own guard.
            {
                std::vector<int> capTris;
                KiwiRegion_Triangulate( reg.pts, &capTris );
                EmitPrismSolid( pb, reg.pts, capTris, col );
            }

            KiwiLines_Color( col[0], col[1], col[2] );

            const int n = (int)( reg.pts.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float a[3], b[3];
                // top ring
                PrismVertex( pb, &reg.pts[(size_t)i * 2], true, a );
                PrismVertex( pb, &reg.pts[(size_t)j * 2], true, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                // ROUND X, ITEM 4b: the BOTTOM ring too.  It was simply missing —
                // "lines from each vertex going up" with a ring only at the top is
                // the report, verbatim.
                PrismVertex( pb, &reg.pts[(size_t)i * 2], false, a );
                PrismVertex( pb, &reg.pts[(size_t)j * 2], false, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                // the vertical at this vertex
                PrismVertex( pb, &reg.pts[(size_t)i * 2], false, a );
                PrismVertex( pb, &reg.pts[(size_t)i * 2], true,  b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
            }   // ROUND AG, ITEM 1 — end of the per-source loop
        }

    private:
        // KIWI-UX (CLEANUP, A-35): RULE — every consumer builds through
        // MakeBuildFor with the source it is working on; none of them may read a
        // per-command plane, or two sources on two walls diverge.

        // ═══════════════════════════════════════════════════════════════════
        //  ROUND AG, ITEM 1 — ONE GESTURE, MANY SOURCES
        // ═══════════════════════════════════════════════════════════════════
        // USER DIRECTIVE, verbatim: "you fixed shift-clicking face extending
        // (good job).  But I want it on all extrusions.  I should be able to
        // shift click construction faces.  and shift click extrude from solid
        // faces."
        //
        // EACH SOURCE KEEPS ITS OWN PLANE and grows along ITS OWN NORMAL by the
        // SAME distance.  That is the "active drives, the rest follow" rule §20's
        // multi-face push/pull already uses, and it is the only reading that makes
        // sense for two regions on two different walls: a shared world direction
        // would push one of them sideways through its own wall.
        void MakeBuildFor( const kregion_t &reg, prismBuild_t *pb ) const
        {
            pb->plane = reg.plane;
            pb->lo    = ( m_dist >= 0.0f ) ? 0.0f  : m_dist;
            pb->hi    = ( m_dist >= 0.0f ) ? m_dist : 0.0f;
        }

        // Every region this gesture is about, DRIVE FIRST.  Resolved fresh from the
        // live list because the members were latched at Begin as indices and a
        // region can stop existing (nothing edits the store mid-drag, but the array
        // can still be shorter after a re-derive).
        void EachSource( std::vector<int> *out ) const
        {
            out->clear();
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            if ( m_region >= 0 && m_region < (int)regions.size() )
                out->push_back( m_region );
            for ( size_t i = 0; i < m_extra.size(); ++i )
            {
                const int idx = m_extra[i];
                if ( idx < 0 || idx >= (int)regions.size() || idx == m_region )
                    continue;
                out->push_back( idx );
            }
        }

        // The convex decomposition of ONE region into unlinked, VALIDATED defs.
        // Extracted (round AF) because the grow and the carve built the identical
        // geometry and differed only in what they then did with it; round AP removed
        // the carve, and it stays factored because the "build and gate EVERYTHING
        // before anything is linked" rule spans more than one region.  Appends on success;
        // on failure the caller frees the whole accumulated list.
        bool BuildRegionDefs( const kregion_t &reg, std::vector<brush_t *> *out,
                              const char **why ) const
        {
            const int n = (int)( reg.pts.size() / 2 );
            if ( n < 3 || n > KEXT_MAX_PROFILE )
            {
                *why = "profile out of range";
                return false;
            }

            std::vector< std::vector<int> > pieces;
            if ( KiwiRegion_IsConvex( reg.pts ) )
            {
                std::vector<int> whole;
                for ( int i = 0; i < n; ++i )
                    whole.push_back( i );
                pieces.push_back( whole );
            }
            else if ( !KiwiRegion_ConvexPieces( reg.pts, &pieces ) )
            {
                *why = "could not decompose the profile (self-intersecting or degenerate)";
                return false;
            }
            if ( pieces.empty() || (int)pieces.size() > KEXT_MAX_PIECES )
            {
                *why = "the decomposition produced too many pieces";
                return false;
            }

            prismBuild_t pb;
            MakeBuildFor( reg, &pb );
            for ( size_t p = 0; p < pieces.size(); ++p )
            {
                brush_t *def = BuildPieceDef( pb, reg.pts, pieces[p], why );
                if ( !def )
                    return false;
                out->push_back( def );
                KiwiValid_Rebuild( def );
                if ( !KiwiValid_CheckBrush( def, why ) )
                    return false;
            }
            return true;
        }

        // ── KIWI-UX (ROUND BP, ITEM 2): what the ABSOLUTE bubble prints ─────────
        // On a world-aligned axis the extruded end has a world coordinate and that
        // is the honest "absolute" — "Z 10 ft" is a number the user can check
        // against the grid.  On a slanted normal there is no single axis to name,
        // so it falls back to the distance from the source plane, prefixed so the
        // two readings can never be confused.
        void AbsoluteLabel( char *buf, int bufSize ) const
        {
            int worldAxis = -1;
            for ( int k = 0; k < 3; ++k )
                if ( fabsf( m_plane.normal[k] ) > 0.999f )
                    worldAxis = k;
            char v[32];
            if ( worldAxis >= 0 )
            {
                const float pos = m_ref[worldAxis] + m_plane.normal[worldAxis] * m_dist;
                KiwiUnits_Format( v, sizeof( v ), pos );
                _snprintf( buf, bufSize, "%c %s", "XYZ"[worldAxis], v );
            }
            else
            {
                KiwiUnits_Format( v, sizeof( v ), m_dist );
                _snprintf( buf, bufSize, "out %s", v );
            }
            buf[bufSize - 1] = '\0';
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        // ROUND AI, ITEM 2 — the latch is a REBASE: `m_start` is biased by the
        // current `m_dist`, so the mapping evaluates to exactly `m_dist` at the
        // instant of the latch.  At Begin() `m_dist` is 0, so this is identical to
        // what it always did; mid-gesture (the view gate re-opening) it is what
        // keeps the distance where the user left it.
        void LatchStart()
        {
            m_haveStart = false;
            if ( !KiwiCam_AxisPortrayable( m_plane.normal ) )
                return;
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                return;
            float rel[3];
            Sub3( p, m_ref, rel );
            m_start     = Dot3( rel, m_plane.normal ) - m_dist;
            m_haveStart = true;
        }

        void Recompute()
        {
            float d = m_dist;
            m_gridMajor = false;               // ROUND BN, ITEM 6 — one frame's answer

            // ── KIWI-UX (ROUND AI, ITEM 2): THE VIEW GATE ───────────────────
            // Same degeneracy the box's height stage hit, same fix — a region
            // extruded from the XY plane while looking straight down is the
            // IDENTICAL geometry ("It's impossible to portray Z movement while at
            // top/bottom camera lock").  See KiwiCam_AxisPortrayable, kiwi_camera.h.
            // The distance is HELD while the view cannot express it, and the
            // mapping rebases when it can.  Typing and the SNAP arms below are
            // unaffected: neither goes through the cursor→axis solve.
            const bool canAxis = KiwiCam_AxisPortrayable( m_plane.normal );
            if ( !canAxis )
                m_haveStart = false;
            else if ( !m_haveStart )
                LatchStart();
            m_axisBlocked = !canAxis;

            // ── KIWI-UX (ROUND BP, ITEM 2): CTRL = ABSOLUTE (kiwi_extrude.h) ──
            // The CTRL-UP edge is the only one that rebases: relative has to resume
            // from wherever absolute left the face, so `m_start` is re-latched to
            // make the mapping evaluate to the current `d`.  CTRL-DOWN deliberately
            // does not — "match the mouse absolutely" IS the jump, and it is exactly
            // `m_start`, which is small for a gesture grabbed at its own lollipop.
            const bool absNow = KiwiExt_AbsoluteHeld();
            if ( !absNow && m_absPrev )
                LatchStart();
            m_absPrev  = absNow;
            m_absolute = absNow && canAxis && m_haveStart;

            ray_t ray;
            float p[3];
            float rawAbs  = d;                 // the cursor's own axis position
            bool  haveRaw = false;
            if ( canAxis && m_haveStart && CursorRay( &ray )
              && KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
            {
                float rel[3];
                Sub3( p, m_ref, rel );
                rawAbs  = Dot3( rel, m_plane.normal );
                haveRaw = true;
                d = absNow ? rawAbs : ( rawAbs - m_start );
            }

            if ( m_hasNum )
            {
                d = m_numWorld;
            }
            else if ( m_absolute && haveRaw )
            {
                // ── KIWI-UX (ROUND BT): THE FULL LADDER (kiwi_extrude.h) ──────
                // Round BP's nearest-value contest between the geometry answer and
                // the hard lattice is GONE: a hard lattice answer is never further
                // than half a cell from the cursor, so on the user's 6-inch grid it
                // won every comparison and the extrude *"only snapped to the grid"*.
                // Geometry ranks first (aim-gated), the face plane is a magnet, and
                // the lattice with its majors catches everything else — one function,
                // shared by all four one-axis gestures.
                d = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, rawAbs,
                                         &m_gridMajor );
            }
            else if ( m_snap.valid && m_snap.type != SNAP_NONE )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // Extrude UP TO the thing under the cursor.
                    // ── KIWI-UX (ROUND Z, ITEM 3): A FACE IS A PLANE ────────
                    // Was `dot( snapPos - ref, planeNormal )`, and for arm 6's
                    // sliding ray-surface hit that gave a different depth for every
                    // pixel of the same flat roof.  KiwiSnap_AxisDepth (kiwi_snap.h)
                    // intersects the target face's PLANE with this gesture's axis
                    // instead and leaves every POINT candidate exactly where it is.
                    // False = an edge-on face: no answer, leave `d` alone.
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_plane.normal, &sd ) )
                    {
                        // ROUND X, ITEM 4a: …unless it is the SOURCE.  The region's
                        // own boundary lines and their endpoints all sit at scalar
                        // 0, which is the one answer this gesture can never use.
                        // See KEXT_SELF_SNAP_BAND in kiwi_extrude.h.  ROUND Z,
                        // ITEM 2: still applies — Ctrl turns the ranked query ON,
                        // it does not turn this rule off.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            d = sd;
                    }
                }
                else
                {
                    // §6 numeric hygiene: quantise the SCALAR, never the cursor
                    // point, so an axis-aligned region extrudes exactly on-grid.
                    //
                    // ── KIWI-UX (ROUND BN, ITEM 6): …THROUGH THE MAJOR-AWARE ────
                    //    LATTICE, WHICH IS THE ONE THE MOVE ALREADY USES.
                    // USER DIRECTIVE, verbatim: *"Holding Ctrl should allow snapping
                    // to the major lines of the grid as well… (in this case, while
                    // extruding)."*
                    //
                    // This branch IS "Ctrl is held" for an extrude: an extrude is a
                    // TRANSFORM, so the ranked query only answers at all with Ctrl
                    // down (kiwi_command.h SnapContext), and SNAP_GRID lands here.
                    // KIWI-UX (ROUND BO, ITEM 3): that sentence was true under round
                    // Z's per-command opt-in and it is still true under the one
                    // context rule — only the reason changed.  The hand-
                    // rolled `floorf(d/g + 0.5)*g` it replaces had never heard of a
                    // MAJOR line and quantised the DELTA rather than the absolute
                    // world coordinate — both of which round BL had already fixed
                    // ONCE, in KiwiSnap_LatticeAxis, for the object move.  Same
                    // function, `hard` = true (Ctrl means quantise, not nudge), so a
                    // major within KSNAP_MAJOR_BAND_MUL cells takes the value and an
                    // off-grid start does not drag its offset along.
                    //
                    // THE GATE IS UNTOUCHED.  This decides WHAT the lattice answers,
                    // never WHEN it is consulted: the gate above is still
                    // `m_snap.valid && type != SNAP_NONE`, which is arm 0's answer.
                    bool major = false;
                    d = KiwiSnap_LatticeAxis( d, m_ref, m_plane.normal, true, &major );
                    m_gridMajor = major;
                }
            }
            // KIWI-UX (ROUND BO, ITEM 3): ROUND AG'S LIGHT GRID MAGNET IS GONE —
            // see the twin note in kiwi_transform.cpp RecomputeFace.  *"no
            // snapping unless ctrl"* has to mean no snapping, so the raw mapped
            // `d` stands and fine detail is reachable again.

            m_dist    = d;
            m_invalid = ( fabsf( d ) < KEXT_MIN_DIST );
            // ROUND AP, ITEM 1: the into-solid probe is gone — there is no fork left
            // for it to feed.
            UpdateHud();
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_dist );
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            const int verts = ( m_region >= 0 && m_region < (int)regions.size() )
                            ? (int)( regions[m_region].pts.size() / 2 ) : 0;
            // ROUND AG, ITEM 1: the source COUNT is in the strip, because "am I
            // about to extrude one region or four" is the one thing a shift-click
            // set can be wrong about and the preview alone can be off screen.
            char many[24];
            many[0] = '\0';
            if ( !m_extra.empty() )
                _snprintf( many, sizeof( many ), " x%i", (int)m_extra.size() + 1 );
            many[sizeof( many ) - 1] = '\0';

            // ROUND AI, ITEM 2: the view gate names its own remedy, and it takes
            // precedence over TOO THIN — a held distance is of course too thin, and
            // saying so instead would bury the sentence that tells the user what to do.
            if ( m_axisBlocked && !m_hasNum )
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude  %i-gon%s  %s  ·  ORBIT to pull — this view looks "
                           "straight along the extrude axis  ·  or type a distance",
                           verts, many, b );
            else if ( m_invalid )
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s  TOO THIN",
                           verts, many, b );
            // ROUND AP, ITEM 1: the CARVE (cavity) rung is gone with the fork it
            // was advertising — there is only one outcome now.
            // KIWI-UX (ROUND BN, ITEM 6): a MAJOR lock says so, exactly as the object
            // move's HUD does (kiwi_transform.cpp m_majorLock) — a lattice preference
            // the user cannot see is a lattice preference they will not trust.
            // ── KIWI-UX (ROUND BP, ITEM 2): ABSOLUTE SAYS SO, AND SHOWS BOTH ──
            // The bubble reads the ABSOLUTE position first because that is the number
            // the user is aiming at while Ctrl is down, with the delta in parentheses
            // so the familiar reading is not taken away.  "Absolute" here is the
            // extruded end's own world coordinate along the gesture axis when that
            // axis is a world one — which is exactly when a lattice line is a plane
            // it can land on — and the distance from the source otherwise.
            else if ( m_absolute )
            {
                char abso[32];
                AbsoluteLabel( abso, sizeof( abso ) );
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s  (%s)%s  CTRL",
                           verts, many, abso, b, m_gridMajor ? "  [MAJOR]" : "" );
            }
            else
                _snprintf( m_hud, sizeof( m_hud ), "extrude  %i-gon%s  %s%s",
                           verts, many, b, m_gridMajor ? "  [MAJOR]" : "" );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // KIWI-UX (CLEANUP, A-35/A-16): RULE — an extrude never carves.  It lands
        // its prism even inside a solid (overlapping brushes are legal in a .map);
        // carving is the user's own Q boolean.  See the note over Commit().

        // ── the commit ──────────────────────────────────────────────────────
        void Extrude()
        {
            const std::vector<kregion_t> &regions = KiwiRegion_All();
            std::vector<int> srcs;
            EachSource( &srcs );
            if ( srcs.empty() )
                return;

            // ── BUILD AND VALIDATE EVERYTHING BEFORE ANYTHING IS LINKED ──────
            // Brush_BuildWindings and the §19 checks read only planepts / faces /
            // windings — never def->owner — so a def can be built and gated while it
            // is still unlinked.  Doing it this way means a rejection touches
            // neither the map nor the selection nor the undo stack: no half-landed
            // prism, and no empty undo record for the user to step over.
            //
            // ROUND AG, ITEM 1: the rule now spans ALL the sources.  One bad region
            // takes the whole gesture with it, deliberately — "one gesture, one
            // undo record" means the record is all-or-nothing, and landing three
            // prisms out of four and then reporting the fourth would leave the user
            // with a half-done edit that one Ctrl+Z cannot describe.
            std::vector<brush_t *> defs;
            const char *why = "unknown";
            bool  ok    = true;
            int   verts = 0;
            for ( size_t s = 0; s < srcs.size() && ok; ++s )
            {
                const kregion_t &reg = regions[(size_t)srcs[s]];
                verts += (int)( reg.pts.size() / 2 );
                ok = BuildRegionDefs( reg, &defs, &why );
            }

            if ( !ok )
            {
                // Nothing was linked, so every def still has refCount 0 and no
                // owner-chain links — exactly Brush_Free_R's precondition.
                for ( size_t i = 0; i < defs.size(); ++i )
                    Brush_Free_R( defs[i] );
                Sys_Printf( "Extrude: rejected — %s.\n", why ? why : "invalid geometry" );
                return;
            }

            // ── LAND THEM ────────────────────────────────────────────────────
            // The undo ordering kiwi_extrude.h justifies: deselect FIRST so the
            // bracket clones nothing, then open it, then create.  Everything the
            // bracket clones must still be on selected_brushes at commit, or its
            // clone would be restored alongside the live original.
            // ROUND AG, ITEM 1: ONE bracket for every source — the whole point.
            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "extrude region" );
            for ( size_t i = 0; i < defs.size(); ++i )
                LandDef( defs[i] );

            Sys_Printf( "Extruded %i region(s): %i brush(es) from %i profile vertices.\n",
                        (int)srcs.size(), (int)defs.size(), verts );
            g_nUpdateBits = -1;
        }

        int              m_region  = -1;
        // ROUND AG, ITEM 1: the OTHER selected regions this gesture also extrudes,
        // latched at Begin.  Empty is the ordinary single-source case and every
        // path below then behaves exactly as it did before this round.
        std::vector<int> m_extra;
        kconPlane_t   m_plane;
        float         m_ref[3]  = { 0.0f, 0.0f, 0.0f };
        float         m_start   = 0.0f;
        bool          m_haveStart = false;
        float         m_dist    = 0.0f;
        bool          m_axisBlocked = false;  // ROUND AI, ITEM 2 — the view gate
        bool          m_hasNum  = false;
        float         m_numWorld = 0.0f;
        bool          m_invalid = false;
        // KIWI-UX (ROUND BN, ITEM 6): did a MAJOR grid line take the distance?  Set by
        // the Ctrl (hard-lattice) arm of Recompute, read by UpdateHud, and cleared on
        // every pass through the other arms — it is one frame's answer, not a mode.
        bool          m_gridMajor = false;
        // ROUND BP, ITEM 2: the Ctrl-absolute pair.  m_absPrev is the edge detector
        // (the rebase happens on CTRL UP only) and m_absolute is this frame's mode,
        // read by the HUD.
        bool          m_absPrev   = false;
        bool          m_absolute  = false;
        snap_result_t m_snap;
        char          m_hud[192] = { 0 };
    };

    // ═════════════════════════════════════════════════════════════════════════
    //  SHAKEOUT G — E, EXTRUDE FACE: a NEW BODY grown off a face winding.
    //
    //  USER DIRECTIVE (owed from an earlier round), verbatim: "extrude (E) […]
    //  new body from extrusion".
    //
    //  ── PLASTICITY (read, not assumed) ─────────────────────────────────────
    //  `e` -> `command:extrude` (default-keymap.ts:264).  ExtrudeCommand's factory
    //  builder (ExtrudeCommand.ts:69-95) fans the SELECTION out by kind — a
    //  RegionExtrudeFactory per selected region, a FaceExtrudeFactory per selected
    //  face, a CurveExtrudeFactory per selected curve — and the results are ADDED
    //  to the selection when it commits (:65 `selected.add(results)`), with the
    //  source faces and regions dropped from it (:67-68).  That is exactly the
    //  context grammar KIWI needs and exactly the tail behaviour: the new body
    //  ends up selected.
    //
    //  KIWI'S ONE STRUCTURAL DEVIATION: Plasticity's extrude is a BOOLEAN by
    //  default (`PossiblyBooleanExtrudeFactory`, ExtrudeCommand.ts:93, whose
    //  `targets` include the face's own parent solid, :80/:94) — i.e. extruding a
    //  face of a solid normally UNIONS the new material back into it.  KIWI leaves
    //  the original brush completely untouched and lands a SEPARATE brush, because
    //  a classic brush is convex: unioning a prism onto a solid is only expressible
    //  as two brushes anyway, and CSG merge (J, kiwi_join.h) is the explicit verb
    //  for asking for one where one is possible.
    //
    //  ── WHY IT REUSES THE REGION PRISM WRITER ──────────────────────────────
    //  A face winding IS a convex plane profile, which is precisely what
    //  KiwiExtrude_BuildPrismDef takes.  The only work here is turning the face
    //  into (plane, CCW plane-space loop):
    //    * the plane is KiwiCon_MakePlane( centroid, face normal, longest edge ),
    //      which returns an ORTHONORMAL RIGHT-HANDED basis with u × v == normal;
    //    * the loop is the winding projected with KiwiCon_WorldToPlane, REVERSED
    //      when its signed area comes out negative.  The prism writer's whole
    //      orientation argument is written against a CCW ring (see the ORIENTATION
    //      note above), and brush windings are NOT guaranteed to wind that way
    //      about their own outward normal — so the area sign is tested rather than
    //      assumed.
    //  lo = 0 and hi = distance, so the prism grows OUTWARD from the face and its
    //  low cap is coincident with it.  Negative and near-zero are refused: a body
    //  grown INTO the solid it came from is not "a new body", it is a hidden
    //  interpenetration, and it has a verb of its own (G push/pull).
    // ═════════════════════════════════════════════════════════════════════════
    class KiwiExtrudeFaceCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Extrude Face"; }
        // Deliberately unconditional: Begin() is what refuses, WITH A MESSAGE.
        // KiwiCmd_Start's CanExecute gate returns false silently, and "E did
        // nothing and said nothing" is the failure this round exists to remove.
        bool CanExecute() override { return true; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }
        bool        HudInvalid() const override { return m_invalid; }

        int NumericFields( const kiwiNumField_t **out ) const override
        { *out = KEXT_FIELDS; return 1; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( field != 0 || !out )
                return false;
            *out = m_dist;
            return true;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || !m_have )
                return false;
            for ( int k = 0; k < 3; ++k )
                out3[k] = m_ref[k] + m_plane.normal[k] * m_dist;
            return true;
        }

        // KIWI-UX (ROUND BO, ITEM 3): round Z's opt-in flag is gone here too — the
        // face extrude / un-extrude is a TRANSFORM (kiwi_command.h SnapContext), so
        // both directions are raw until Ctrl is held, which is what it already did.

        // ROUND AG, ITEM 1: the preview costs 3 segments per profile vertex per
        // SOURCE, so the command names what it needs and the framework scales its
        // batch (round AF's LineBudget).  Without it a five-face extrude would lose
        // its later prisms at exactly the cliff round AF removed for the boolean.
        int LineBudget() const override
        {
            int segs = 3 * (int)( m_loop.size() / 2 );
            for ( size_t i = 0; i < m_extraFaces.size(); ++i )
                segs += 3 * (int)( m_extraFaces[i].loop.size() / 2 );
            return segs + 96;
        }

        // A transform-shaped gesture: never snap or pick the geometry it grows off.
        unsigned PickFlags() const override { return PICKF_EXCLUDE_SELECTED; }

        void Rebase() override
        {
            // ROUND AI, ITEM 2: the distance preservation moved into LatchStart —
            // see the region sibling's Rebase for why.
            LatchStart();
        }

        // ROUND K: the lollipop, on the live top of the growing body.
        // ROUND Q: …and the sign fold is no longer theoretical.  A negative
        // distance is now the UN-EXTRUDE (carve / destroy), so the stem really does
        // flip to the inward side and the ball stays out in front of the face the
        // user is dragging toward instead of ending up buried in the solid.
        bool LollipopHandle( float outAnchor[3], float outDir[3] ) const override
        {
            if ( !m_have || !outAnchor || !outDir )
                return false;
            for ( int k = 0; k < 3; ++k )
            {
                outAnchor[k] = m_ref[k] + m_plane.normal[k] * m_dist;
                outDir[k]    = ( m_dist < 0.0f ) ? -m_plane.normal[k] : m_plane.normal[k];
            }
            return true;
        }

        bool Begin() override
        {
            m_have      = false;
            m_dist      = 0.0f;
            m_axisBlocked = false;      // ROUND AI, ITEM 2
            m_hasNum    = false;
            m_invalid   = true;
            m_mode      = KEXTF_IDLE;
            m_node      = 0;
            m_faceIndex = -1;
            m_depth     = 0.0f;
            m_haveDepth = false;
            m_hud[0]    = '\0';
            m_loop.clear();
            m_extraFaces.clear();       // ROUND AG, ITEM 1

            selbrush_t *node = 0;
            int         face = -1;
            if ( !KiwiExtrudeFace_Pick( &node, &face ) )
            {
                Sys_Printf( "Extrude: select (or hover) a brush face, or put the "
                            "cursor over a construction region.\n" );
                return false;
            }
            if ( !BuildProfile( node, face ) )
                return false;

            // KIWI-UX (ROUND Q): the SOURCE face is remembered now, and so is how
            // far it can be pushed IN before the brush stops existing.  Measured
            // ONCE, here, from the untouched geometry — measuring it per frame
            // would read a brush a previous frame had already deformed, which is
            // the same reason the interactive push measures it at Begin.
            m_node      = node;
            m_faceIndex = face;
            m_haveDepth = ( KiwiXform_FacePushDepth( node, face, &m_depth ) != 0 );

            // ── ROUND AG, ITEM 1: EVERY OTHER SELECTED FACE COMES ALONG ─────
            // USER DIRECTIVE: "shift click extrude from solid faces."  The typed
            // selection is already a multi-face set (round U); until this round E
            // read the ACTIVE face out of it and ignored the rest, which is why
            // shift-clicking faces "worked" for the push/pull and did nothing for
            // the extrude.  Every OTHER live, non-patch SEL_FACE item is profiled
            // here, once, and each keeps its own plane.
            //
            // A face that will not profile (a >KEXT_MAX_PROFILE winding, a
            // degenerate plane, a non-convex winding) is DROPPED from the extras
            // rather than refusing the gesture — the drive face is the one the user
            // aimed at, and losing a whole gesture to a bad neighbour would be
            // worse than losing that neighbour.  It is reported by count.
            {
                const selection_t &sel = KiwiSel();
                int dropped = 0;
                for ( size_t i = 0; i < sel.items.size(); ++i )
                {
                    const sel_item_t &it = sel.items[i];
                    // NOT "any face of the drive brush": two selected faces on ONE
                    // solid are two legitimate sources.  Only the drive FACE itself
                    // is excluded.
                    if ( it.kind != SEL_FACE || !it.brush )
                        continue;
                    if ( it.brush == m_node && it.faceIndex == m_faceIndex )
                        continue;
                    if ( !Sel_BrushLive( it.brush ) || it.brush->patch )
                        continue;
                    const brush_t *d = it.brush->def;
                    if ( !d || !d->faces || it.faceIndex < 0 || it.faceIndex >= d->faceCount )
                        continue;

                    faceSrc_t fs;
                    fs.node = it.brush;
                    fs.face = it.faceIndex;
                    if ( !BuildProfileInto( it.brush, it.faceIndex, false,
                                            &fs.plane, &fs.loop, 0 ) )
                    {
                        ++dropped;
                        continue;
                    }
                    m_extraFaces.push_back( fs );
                }
                if ( dropped > 0 )
                    Sys_Printf( "Extrude Face: %i selected face(s) could not be "
                                "profiled and are not in this extrusion.\n", dropped );
            }

            m_have = true;
            LatchStart();
            UpdateHud();
            Sys_Printf( "Extrude Face: drag OUT along the face normal for a new "
                        "body, or IN to carve this brush (all the way through "
                        "destroys it).  RMB / Enter confirms.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            Recompute();
            g_nUpdateBits |= 1;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            Recompute();
        }

        void Commit() override
        {
            if ( !m_have || m_mode == KEXTF_IDLE )
            {
                Sys_Printf( "Extrude Face: zero distance — nothing created.\n" );
                return;
            }
            if ( m_mode == KEXTF_GROW )
            {
                Grow();
                return;
            }
            // ── ROUND AG, ITEM 1: THE CARVE STAYS SINGLE-SOURCE, AND SAYS SO ──
            // The negative arm is round Q's un-extrude: it PUSHES ONE FACE PLANE of
            // ONE brush (KiwiXform_PushFaceOnce), with its own destroy threshold
            // measured from that brush's thickness.  There is no reading of "push
            // five unrelated face planes inward by the same number" that is one
            // act — each would have its own destroy depth, some would delete their
            // brush and some would not, and a single Ctrl+Z would then undo a
            // mixture the user never described.  Multi-source is a GROW feature;
            // the refusal is stated rather than silently ignored.
            if ( !m_extraFaces.empty() )
                Sys_Printf( "Extrude Face: carving IN applies to the ACTIVE face only "
                            "— the other %i selected face(s) are left alone.  Drag OUT "
                            "to grow all of them at once.\n", (int)m_extraFaces.size() );
            Carve();
        }

        void Cancel() override
        {
            m_have = false;
            m_extraFaces.clear();       // ROUND AG, ITEM 1
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( !m_have )
                return;

            // ── KIWI-UX (ROUND Q): THE UN-EXTRUDE PREVIEW ────────────────────
            // This command mutates NOTHING until Commit (its own contract, see
            // PreemptIdle's note on the region sibling), and the negative arm keeps
            // that promise rather than half-carving live geometry.  So the picture
            // is drawn instead: where the face WILL land, and — once the carve goes
            // all the way through — the whole doomed brush in the delete red, which
            // is the same red-outline-over-untouched-geometry language the
            // interactive push-through delete already speaks (kiwi_transform.cpp
            // DrawWorld).  With the HUD line saying it in words too, there is no
            // way to reach the destroy without having been told.
            if ( m_mode == KEXTF_CARVE || m_mode == KEXTF_DESTROY )
            {
                DrawCarve();
                return;
            }

            // ROUND AG, ITEM 1: the extras preview too, each on its own plane.
            // Drawn FIRST so the drive face's prism — the one the numeric field and
            // the lollipop are about — lands on top of them.
            for ( size_t i = 0; i < m_extraFaces.size(); ++i )
            {
                const faceSrc_t &fs = m_extraFaces[i];
                if ( fs.loop.size() < 6 )
                    continue;
                prismBuild_t xb;
                xb.plane = fs.plane;
                xb.lo    = 0.0f;
                xb.hi    = ( m_dist > 0.0f ) ? m_dist : 0.0f;
                const float *xc = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;
                std::vector<int> xtris;
                KiwiRegion_Triangulate( fs.loop, &xtris );
                EmitPrismSolid( xb, fs.loop, xtris, xc );
                KiwiLines_Color( xc[0], xc[1], xc[2] );
                EmitPrismWire( xb, fs.loop );
            }

            prismBuild_t pb;
            pb.plane = m_plane;
            pb.lo    = 0.0f;
            pb.hi    = ( m_dist > 0.0f ) ? m_dist : 0.0f;

            const float *col = m_invalid ? KEXT_COL_BAD : KEXT_COL_PREVIEW;
            KiwiLines_Color( col[0], col[1], col[2] );

            // ── KIWI-UX (ROUND K): NO PREVIEW AT ZERO DISTANCE ───────────────
            // USER DIRECTIVE: "When Selecting the face of a solid (brush), it still
            // creates a pink outline on the face.  Stop doing that."  The magenta
            // pass in camwnd.cpp was the main culprit; THIS is the second one, and
            // it is this command's own doing — at distance 0 the prism collapses onto
            // the source face and the top ring below traces its winding EXACTLY, i.e.
            // an outline in all but name, drawn the instant E starts.  Below
            // KEXT_MIN_DIST there is no body to preview, so nothing is drawn.
            if ( fabsf( m_dist ) < KEXT_MIN_DIST )
                return;

            // ROUND X, ITEM 4b: the solid, then the wire — the same treatment the
            // region extrude's preview gets, and for the same report.
            {
                std::vector<int> capTris;
                KiwiRegion_Triangulate( m_loop, &capTris );
                EmitPrismSolid( pb, m_loop, capTris, col );
                KiwiLines_Color( col[0], col[1], col[2] );
            }

            const int n = (int)( m_loop.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                float a[3], b[3];
                PrismVertex( pb, &m_loop[(size_t)i * 2], true, a );
                PrismVertex( pb, &m_loop[(size_t)j * 2], true, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                // ROUND X, ITEM 4b: the bottom ring (it was missing here too).
                PrismVertex( pb, &m_loop[(size_t)i * 2], false, a );
                PrismVertex( pb, &m_loop[(size_t)j * 2], false, b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
                PrismVertex( pb, &m_loop[(size_t)i * 2], false, a );
                PrismVertex( pb, &m_loop[(size_t)i * 2], true,  b );
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

    private:
        // ── KIWI-UX (ROUND Q): the negative arm's picture ────────────────────
        // CARVE  — the source winding, translated inward along the face normal by
        //          |dist|: exactly where the plane is going.
        // DESTROY— every winding of the doomed brush, in the delete red.
        void DrawCarve()
        {
            if ( !m_node || !Sel_BrushLive( m_node ) )
                return;
            const brush_t *def = m_node->def;
            if ( !def || !def->faces )
                return;

            if ( m_mode == KEXTF_DESTROY )
            {
                KiwiLines_Color( KEXT_COL_DOOMED[0], KEXT_COL_DOOMED[1],
                                 KEXT_COL_DOOMED[2] );
                for ( int f = 0; f < def->faceCount; ++f )
                {
                    const winding_t *w = def->faces[f].w;
                    if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                        continue;
                    for ( int p = 0; p < w->numpoints; ++p )
                        if ( !KiwiLines_Add( w->p[p], w->p[( p + 1 ) % w->numpoints] ) )
                            return;
                }
                return;
            }

            if ( m_faceIndex < 0 || m_faceIndex >= def->faceCount )
                return;
            const winding_t *w = def->faces[m_faceIndex].w;
            if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                return;

            KiwiLines_Color( KEXT_COL_CARVE[0], KEXT_COL_CARVE[1], KEXT_COL_CARVE[2] );
            for ( int p = 0; p < w->numpoints; ++p )
            {
                const int q = ( p + 1 ) % w->numpoints;
                float a[3], b[3];
                for ( int k = 0; k < 3; ++k )
                {
                    a[k] = w->p[p][k] + m_plane.normal[k] * m_dist;   // m_dist < 0
                    b[k] = w->p[q][k] + m_plane.normal[k] * m_dist;
                }
                if ( !KiwiLines_Add( a, b ) )
                    return;
            }
        }

        // ── KIWI-UX (ROUND Q): NEGATIVE E IS PUSH/PULL ───────────────────────
        // Delegated whole to KiwiXform_PushFaceOnce (kiwi_transform.h §Q), which
        // IS the interactive push's code: the same FaceDepthAlong delete rule, the
        // same texture-lock bracket, the same §19 gate, and the same
        // Select_Deselect → head → Undo_AddEntity_W → Select_Brush → Select_Delete
        // ordering for the removal.  This file contributes the DISTANCE and
        // nothing else, which is why there is no second delete rule to keep in step.
        //
        // The undo bracket it opens is left OPEN and closed by the framework's
        // KiwiCmd_UndoCommit after this returns — one gesture, one record — and a
        // rejected carve cancels the record itself, so a refusal costs nothing.
        void Carve()
        {
            const char *why = 0;
            const int r = KiwiXform_PushFaceOnce( m_node, m_faceIndex, m_dist,
                                                  "un-extrude face", &why );
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), -m_dist );

            if ( r == KXPUSH_DELETED )
            {
                Sel_Clear( KiwiSel() );
                Sel_RebuildFromLegacy();
                Sys_Printf( "Un-extrude: pushed through — the brush was destroyed.\n" );
            }
            else if ( r == KXPUSH_PUSHED )
            {
                // The typed selection still names a face of a brush whose windings
                // have just been re-solved; the face index survives (the push does
                // not change faceCount), but the legacy side is resynced anyway so
                // nothing downstream reads a stale winding pointer.
                Sel_RebuildFromLegacy();
                Sys_Printf( "Un-extrude: carved %s into the brush.\n", b );
            }
            else
            {
                Sys_Printf( "Un-extrude: rejected — %s.\n", why ? why : "invalid geometry" );
            }
            g_nUpdateBits = -1;
        }

        // ROUND AG, ITEM 1: the body took the RESULT by pointer so that the extra
        // source faces can be built with the identical code.  BuildProfile below
        // is the one-line member-writing wrapper it always was.
        bool BuildProfileInto( selbrush_t *node, int faceIndex, bool verbose,
                               kconPlane_t *outPlane, std::vector<float> *outLoop,
                               float *outRef ) const
        {
            const brush_t *def = node->def;
            const face_t  *f   = &def->faces[faceIndex];
            const winding_t *w = f->w;
            if ( !w || w->numpoints < 3 || w->numpoints > KEXT_MAX_PROFILE )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that face has %i vertices (the v1 limit "
                                "is %i).\n", w ? w->numpoints : 0, KEXT_MAX_PROFILE );
                return false;
            }

            // Centroid + the longest edge, which seeds the plane basis so `u` runs
            // along the face's own dominant direction rather than a world axis.
            float c[3] = { 0.0f, 0.0f, 0.0f };
            for ( int i = 0; i < w->numpoints; ++i )
                for ( int k = 0; k < 3; ++k )
                    c[k] += w->p[i][k];
            const float inv = 1.0f / (float)w->numpoints;
            for ( int k = 0; k < 3; ++k )
                c[k] *= inv;

            float hint[3] = { 0.0f, 0.0f, 0.0f };
            float bestLen = 0.0f;
            for ( int i = 0; i < w->numpoints; ++i )
            {
                float e[3];
                Sub3( w->p[( i + 1 ) % w->numpoints], w->p[i], e );
                const float l = sqrtf( Dot3( e, e ) );
                if ( l > bestLen ) { bestLen = l; Copy3( e, hint ); }
            }

            if ( !KiwiCon_MakePlane( c, f->plane.normal, ( bestLen > 1.0e-3f ) ? hint : 0,
                                     outPlane ) )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that face has a degenerate plane.\n" );
                return false;
            }
            if ( outRef )
                Copy3( c, outRef );

            std::vector<float> &loop = *outLoop;
            loop.resize( (size_t)w->numpoints * 2 );
            for ( int i = 0; i < w->numpoints; ++i )
                KiwiCon_WorldToPlane( *outPlane, w->p[i], &loop[(size_t)i * 2] );

            // ── THE WINDING SENSE ────────────────────────────────────────────
            // The prism writer's orientation argument assumes a CCW ring in (u, v).
            // A brush winding's order about its own outward normal is whatever
            // Brush_BuildWindings' clipping left, so the SIGNED AREA decides:
            // negative = clockwise = reverse it.  (KiwiRegion_IsConvex is then a
            // pure sanity check — a brush face is convex by construction, which is
            // exactly what §19 relies on too.)
            float area2 = 0.0f;
            const int n = (int)( loop.size() / 2 );
            for ( int i = 0; i < n; ++i )
            {
                const int j = ( i + 1 ) % n;
                area2 += loop[(size_t)i * 2 + 0] * loop[(size_t)j * 2 + 1]
                       - loop[(size_t)j * 2 + 0] * loop[(size_t)i * 2 + 1];
            }
            if ( area2 < 0.0f )
            {
                for ( int i = 0; i < n / 2; ++i )
                {
                    const int j = n - 1 - i;
                    float t0 = loop[(size_t)i * 2 + 0], t1 = loop[(size_t)i * 2 + 1];
                    loop[(size_t)i * 2 + 0] = loop[(size_t)j * 2 + 0];
                    loop[(size_t)i * 2 + 1] = loop[(size_t)j * 2 + 1];
                    loop[(size_t)j * 2 + 0] = t0;
                    loop[(size_t)j * 2 + 1] = t1;
                }
            }
            if ( !KiwiRegion_IsConvex( loop ) )
            {
                if ( verbose )
                    Sys_Printf( "Extrude Face: that winding is not convex — it cannot "
                                "be one brush.\n" );
                return false;
            }
            return true;
        }

        // The member-writing wrapper — the DRIVE face, and the only one that is
        // allowed to complain out loud (an EXTRA face that cannot be profiled is
        // reported once, by count, at Begin).
        bool BuildProfile( selbrush_t *node, int faceIndex )
        {
            return BuildProfileInto( node, faceIndex, true, &m_plane, &m_loop, m_ref );
        }

        // ── KIWI-UX (ROUND BP, ITEM 2): what the ABSOLUTE bubble prints ─────────
        // On a world-aligned axis the extruded end has a world coordinate and that
        // is the honest "absolute" — "Z 10 ft" is a number the user can check
        // against the grid.  On a slanted normal there is no single axis to name,
        // so it falls back to the distance from the source plane, prefixed so the
        // two readings can never be confused.
        void AbsoluteLabel( char *buf, int bufSize ) const
        {
            int worldAxis = -1;
            for ( int k = 0; k < 3; ++k )
                if ( fabsf( m_plane.normal[k] ) > 0.999f )
                    worldAxis = k;
            char v[32];
            if ( worldAxis >= 0 )
            {
                const float pos = m_ref[worldAxis] + m_plane.normal[worldAxis] * m_dist;
                KiwiUnits_Format( v, sizeof( v ), pos );
                _snprintf( buf, bufSize, "%c %s", "XYZ"[worldAxis], v );
            }
            else
            {
                KiwiUnits_Format( v, sizeof( v ), m_dist );
                _snprintf( buf, bufSize, "out %s", v );
            }
            buf[bufSize - 1] = '\0';
        }

        bool CursorRay( ray_t *out ) const
        {
            int x, y;
            if ( !KiwiCmd_LastCursor( &x, &y ) )
                return false;
            return Pick_RayFromImagePos( x, y, out );
        }

        // ROUND AI, ITEM 2 — a REBASE (see the region sibling's LatchStart).
        void LatchStart()
        {
            m_haveStart = false;
            if ( !KiwiCam_AxisPortrayable( m_plane.normal ) )
                return;
            ray_t ray;
            float p[3];
            if ( !CursorRay( &ray ) || !KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
                return;
            float rel[3];
            Sub3( p, m_ref, rel );
            m_start     = Dot3( rel, m_plane.normal ) - m_dist;
            m_haveStart = true;
        }

        void Recompute()
        {
            float d = m_dist;
            m_gridMajor = false;               // ROUND BN, ITEM 6 — one frame's answer

            // ROUND AI, ITEM 2 — the view gate.  A FACE extrude hits this harder
            // than the region one does, because the axis is the FACE NORMAL: aim
            // the camera at a wall head-on (which is the natural thing to do before
            // pushing it) and the push axis points straight at the eye.  Held +
            // rebased exactly as the region sibling; see kiwi_camera.h.
            const bool canAxis = KiwiCam_AxisPortrayable( m_plane.normal );
            if ( !canAxis )
                m_haveStart = false;
            else if ( !m_haveStart )
                LatchStart();
            m_axisBlocked = !canAxis;

            // ── KIWI-UX (ROUND BP, ITEM 2): CTRL = ABSOLUTE (kiwi_extrude.h) ──
            // The CTRL-UP edge is the only one that rebases: relative has to resume
            // from wherever absolute left the face, so `m_start` is re-latched to
            // make the mapping evaluate to the current `d`.  CTRL-DOWN deliberately
            // does not — "match the mouse absolutely" IS the jump, and it is exactly
            // `m_start`, which is small for a gesture grabbed at its own lollipop.
            const bool absNow = KiwiExt_AbsoluteHeld();
            if ( !absNow && m_absPrev )
                LatchStart();
            m_absPrev  = absNow;
            m_absolute = absNow && canAxis && m_haveStart;

            ray_t ray;
            float p[3];
            float rawAbs  = d;                 // the cursor's own axis position
            bool  haveRaw = false;
            if ( canAxis && m_haveStart && CursorRay( &ray )
              && KiwiCam_RayAxis( ray, m_ref, m_plane.normal, p ) )
            {
                float rel[3];
                Sub3( p, m_ref, rel );
                rawAbs  = Dot3( rel, m_plane.normal );
                haveRaw = true;
                d = absNow ? rawAbs : ( rawAbs - m_start );
            }

            if ( m_hasNum )
            {
                d = m_numWorld;
            }
            else if ( m_absolute && haveRaw )
            {
                // ── KIWI-UX (ROUND BT): THE FULL LADDER (kiwi_extrude.h) ──────
                // Round BP's nearest-value contest between the geometry answer and
                // the hard lattice is GONE: a hard lattice answer is never further
                // than half a cell from the cursor, so on the user's 6-inch grid it
                // won every comparison and the extrude *"only snapped to the grid"*.
                // Geometry ranks first (aim-gated), the face plane is a magnet, and
                // the lattice with its majors catches everything else — one function,
                // shared by all four one-axis gestures.
                d = KiwiExt_LadderDepth( m_snap, m_ref, m_plane.normal, rawAbs,
                                         &m_gridMajor );
            }
            else if ( m_snap.valid && m_snap.type != SNAP_NONE )
            {
                if ( KiwiSnap_IsGeometry( m_snap.type ) )
                {
                    // ROUND Z, ITEM 3: the planar-consistent resolve — see the
                    // region sibling above and KiwiSnap_AxisDepth (kiwi_snap.h).
                    float sd = 0.0f;
                    if ( KiwiSnap_AxisDepth( m_snap, m_ref, m_plane.normal, &sd ) )
                    {
                        // ROUND X, ITEM 4a: refuse the SOURCE PLANE.  The face being
                        // extruded, its winding corners and its midpoints are all at
                        // scalar 0 and are the nearest candidates for the whole first
                        // part of the drag.  PICKF_EXCLUDE_SELECTED (this command's
                        // own PickFlags) covers the brush arms; it cannot cover the
                        // construction or axis-guide arms, and this rule covers all
                        // of them at once.  See KEXT_SELF_SNAP_BAND in
                        // kiwi_extrude.h.  It applies to the UN-EXTRUDE (negative)
                        // arm too, which shares this scalar.
                        if ( fabsf( sd ) >= KEXT_SELF_SNAP_BAND )
                            d = sd;
                    }
                }
                else
                {
                    // KIWI-UX (ROUND BN, ITEM 6): the major-aware HARD lattice, same
                    // as the region sibling above — the whole argument is there.  This
                    // is the FACE extrude / un-extrude, which is the gesture the user's
                    // screenshot was taken during.
                    bool major = false;
                    d = KiwiSnap_LatticeAxis( d, m_ref, m_plane.normal, true, &major );
                    m_gridMajor = major;
                }
            }
            // KIWI-UX (ROUND BO, ITEM 3): round AG's light grid magnet is gone,
            // same as the region sibling above — the raw mapped `d` stands.

            m_dist = d;

            // ── KIWI-UX (ROUND Q): WHICH OF THE THREE THINGS E IS DOING ──────
            // Recomputed from scratch every frame off the CURRENT distance, so
            // dragging back and forth across zero moves cleanly between "grow a
            // body", "carve into the brush" and "destroy it" with no latching.
            // The DESTROY test carries the same KEXT_PUSH_EPS slack
            // KiwiXform_PushFaceOnce's own test does (its KX_EPS), so the HUD can
            // never promise a carve the helper then turns into a delete or the
            // other way round — the two comparisons are the same comparison.
            // KIWI-UX (CLEANUP, A-26): and they are now literally the same
            // constant, KXPUSH_EPS, rather than two literals that agreed.
            m_mode = KEXTF_IDLE;
            if ( d >= KEXT_MIN_DIST )
                m_mode = KEXTF_GROW;
            else if ( d <= -KEXT_MIN_DIST )
                m_mode = ( m_haveDepth && m_depth > KEXT_PUSH_EPS
                        && d <= -m_depth + KEXT_PUSH_EPS )
                       ? KEXTF_DESTROY : KEXTF_CARVE;

            // INVALID now means only "too small to mean anything".  A negative
            // distance is a legitimate request (see the header) and must NOT paint
            // the HUD red — HudInvalid() drives "commit takes the cancel path",
            // which is the exact behaviour the directive is complaining about.
            m_invalid = ( m_mode == KEXTF_IDLE );
            UpdateHud();
        }

        void UpdateHud()
        {
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), ( m_dist < 0.0f ) ? -m_dist : m_dist );
            const int verts = (int)( m_loop.size() / 2 );

            // ROUND AI, ITEM 2: the view gate outranks all three, because while it
            // is closed NONE of them can happen — the distance is held.
            if ( m_axisBlocked && !m_hasNum )
            {
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude face  %i-gon  %s  ·  ORBIT to pull — this view "
                           "looks straight along the face normal  ·  or type a distance",
                           verts, b );
                m_hud[sizeof( m_hud ) - 1] = '\0';
                return;
            }

            // KIWI-UX (ROUND Q): THE HUD SAYS WHAT CONFIRM WILL DO, before it does
            // it.  Three outcomes on one key needs three sentences, not one number.
            switch ( m_mode )
            {
            case KEXTF_GROW:
                // ROUND AG, ITEM 1: the source COUNT, because a shift-click set is
                // the one thing the strip can be wrong about off screen.
                if ( !m_extraFaces.empty() )
                    _snprintf( m_hud, sizeof( m_hud ),
                               "extrude face  %i-gon x%i  %s  ->  %i NEW BODIES",
                               verts, (int)m_extraFaces.size() + 1, b,
                               (int)m_extraFaces.size() + 1 );
                else
                    _snprintf( m_hud, sizeof( m_hud ),
                               "extrude face  %i-gon  %s  ->  NEW BODY", verts, b );
                break;
            case KEXTF_CARVE:
                _snprintf( m_hud, sizeof( m_hud ),
                           "un-extrude  %s IN  ->  CARVES the brush  (%s to destroy)",
                           b, m_haveDepth ? "keep pulling" : "no depth" );
                break;
            case KEXTF_DESTROY:
                _snprintf( m_hud, sizeof( m_hud ),
                           "un-extrude  %s IN  ->  PUSHED THROUGH, confirm DESTROYS "
                           "this brush", b );
                break;
            default:
                _snprintf( m_hud, sizeof( m_hud ),
                           "extrude face  %i-gon  %s  (pull OUT for a new body, "
                           "IN to carve)", verts, b );
                break;
            }
            // KIWI-UX (ROUND BN, ITEM 6): the MAJOR-lock badge, appended rather than
            // woven into four format strings — the lock is a property of the DISTANCE,
            // which every arm above has already printed.
            if ( m_gridMajor )
            {
                const size_t n = strlen( m_hud );
                if ( n + 9 < sizeof( m_hud ) )
                    _snprintf( m_hud + n, sizeof( m_hud ) - n, "  [MAJOR]" );
            }
            // ── KIWI-UX (ROUND BP, ITEM 2): the ABSOLUTE badge, appended the same
            // way and for the same reason: the mode is a property of the DISTANCE
            // every arm above has already printed, so it does not need four more
            // format strings.  It carries the absolute position, which is the number
            // the user is aiming at while Ctrl is held.
            if ( m_absolute )
            {
                char abso[32];
                AbsoluteLabel( abso, sizeof( abso ) );
                const size_t n = strlen( m_hud );
                if ( n + 24 < sizeof( m_hud ) )
                    _snprintf( m_hud + n, sizeof( m_hud ) - n, "  CTRL @ %s", abso );
            }
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // The commit: ONE brush, built and gated UNLINKED (rejection is free — the
        // same rule kiwi_extrude.h states for the region path), then landed with
        // the same deselect-first bracket ordering, then handed to Move.
        void Grow()
        {
            // ── ROUND AG, ITEM 1: EVERY SELECTED FACE GROWS ITS OWN PRISM ────
            // USER DIRECTIVE, verbatim: "shift click extrude from solid faces."
            // One gesture, one depth, ONE undo bracket, N prisms.  Each face grows
            // along ITS OWN normal (m_extraFaces carries a per-face plane and
            // profile) rather than along the drive face's, for the same reason the
            // region sibling does: two faces at 90 degrees pushed along one shared
            // world direction would send one of them sideways through its own
            // solid.  That is also §20's own "active drives, the rest follow" rule
            // for the multi-face push/pull, so the two multi-face gestures agree.
            //
            // BUILD AND GATE EVERYTHING BEFORE ANYTHING IS LINKED, exactly as the
            // region path does: one rejected face refuses the whole gesture rather
            // than landing a partial edit that one Ctrl+Z cannot describe.
            const char *why = "unknown";
            std::vector<brush_t *> defs;
            bool ok = true;
            const int n = (int)( m_loop.size() / 2 );
            {
                brush_t *def = KiwiExtrude_BuildPrismDef( m_plane, &m_loop[0], n,
                                                          0.0f, m_dist, &why );
                if ( !def )
                {
                    ok = false;
                }
                else
                {
                    defs.push_back( def );
                    KiwiValid_Rebuild( def );
                    if ( !KiwiValid_CheckBrush( def, &why ) )
                        ok = false;
                }
            }
            for ( size_t i = 0; i < m_extraFaces.size() && ok; ++i )
            {
                const faceSrc_t &fs = m_extraFaces[i];
                const int fn = (int)( fs.loop.size() / 2 );
                if ( fn < 3 )
                    continue;
                brush_t *def = KiwiExtrude_BuildPrismDef( fs.plane, &fs.loop[0], fn,
                                                          0.0f, m_dist, &why );
                if ( !def )
                {
                    ok = false;
                    break;
                }
                defs.push_back( def );
                KiwiValid_Rebuild( def );
                if ( !KiwiValid_CheckBrush( def, &why ) )
                    ok = false;
            }
            if ( !ok )
            {
                for ( size_t i = 0; i < defs.size(); ++i )
                    Brush_Free_R( defs[i] );   // never linked: refCount 0, no owner chain
                Sys_Printf( "Extrude Face: rejected — %s.\n",
                            why ? why : "invalid geometry" );
                return;
            }

            Select_Deselect( 1 );
            KiwiCmd_UndoBegin( "extrude face" );
            for ( size_t i = 0; i < defs.size(); ++i )
                LandDef( defs[i] );            // lands SELECTED

            // The ORIGINAL BRUSH IS UNTOUCHED — it was never in the bracket, never
            // rebuilt and never re-selected.  The FACE selection that started this
            // is gone, though, because the face it named is no longer what is
            // selected legacy-side.
            Sel_Clear( KiwiSel() );
            Sel_RebuildFromLegacy();

            Sys_Printf( "Extrude Face: %i new brush(es) from %i face(s) (%i-gon drive).\n",
                        (int)defs.size(), (int)m_extraFaces.size() + 1, n );
            g_nUpdateBits = -1;

            // Consistency with paste / clone (kiwi_command.cpp KiwiCmd_AfterPaste):
            // a freshly created body lands selected AND in a paused Move, so it can
            // be placed with the gizmo without pressing anything.
            KiwiCmd_StartDeferred( KIWI_CMD_MOVE, true );
        }

        // ROUND AG, ITEM 1: one EXTRA source face.  It carries its own plane and
        // its own plane-space profile, because it grows along its OWN normal.
        struct faceSrc_t
        {
            selbrush_t        *node;
            int                face;
            kconPlane_t        plane;
            std::vector<float> loop;
        };
        std::vector<faceSrc_t> m_extraFaces;

        kconPlane_t        m_plane;
        std::vector<float> m_loop;                       // CCW, plane space
        float              m_ref[3]  = { 0.0f, 0.0f, 0.0f };
        float              m_start   = 0.0f;
        bool               m_haveStart = false;
        bool               m_have    = false;
        float              m_dist    = 0.0f;
        bool               m_axisBlocked = false;   // ROUND AI, ITEM 2 — the view gate
        bool               m_hasNum  = false;
        float              m_numWorld = 0.0f;
        bool               m_invalid = true;
        // KIWI-UX (ROUND BN, ITEM 6): the face twin of the region command's flag —
        // did a MAJOR line take the distance?  One frame's answer, cleared in
        // Recompute and read by UpdateHud.
        bool               m_gridMajor = false;
        // ROUND BP, ITEM 2: the Ctrl-absolute pair (see the region sibling).
        bool               m_absPrev   = false;
        bool               m_absolute  = false;
        snap_result_t      m_snap;
        char               m_hud[192] = { 0 };

        // ── ROUND Q: the SOURCE face, kept, so a negative distance can carve it ──
        selbrush_t        *m_node      = 0;
        int                m_faceIndex = -1;
        float              m_depth     = 0.0f;   // thickness along the face normal
        bool               m_haveDepth = false;
        int                m_mode      = KEXTF_IDLE;
    };

    KiwiExtrudeCommand     s_extrude;
    KiwiExtrudeFaceCommand s_extrudeFace;
}

// ─── shakeout G: which face E acts on ────────────────────────────────────────
// The SELECTED face first (the active one when several are selected — the same
// "active drives, the rest follow" rule §20's push/pull uses), then the HOVERED
// one.  Hover is included because the directive that produced the sibling verbs
// says "selected/hovered face": with mode 3's auto-push-pull (kiwi_boxselect.cpp)
// a user often has a face under the cursor and no time to have selected it.
bool KiwiExtrudeFace_Pick( selbrush_t **outNode, int *outFace )
{
    const selection_t &sel = KiwiSel();
    const sel_item_t  &act = sel.active;

    const sel_item_t *chosen = 0;
    if ( act.kind == SEL_FACE && Sel_BrushLive( act.brush ) && !act.brush->patch )
        chosen = &act;
    for ( size_t i = 0; i < sel.items.size() && !chosen; ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( it.kind == SEL_FACE && Sel_BrushLive( it.brush ) && !it.brush->patch )
            chosen = &sel.items[i];
    }

    pick_result_t hov;
    if ( !chosen )
    {
        hov = KiwiHover_Get();
        if ( hov.valid && hov.item.kind == SEL_FACE && Sel_BrushLive( hov.item.brush )
          && !hov.item.brush->patch )
            chosen = &hov.item;
    }
    if ( !chosen )
        return false;

    const brush_t *def = chosen->brush->def;
    if ( !def || !def->faces || chosen->faceIndex < 0 || chosen->faceIndex >= def->faceCount )
        return false;
    if ( outNode ) *outNode = chosen->brush;
    if ( outFace ) *outFace = chosen->faceIndex;
    return true;
}

bool KiwiExtrudeFace_CanExecute()
{
    selbrush_t *node = 0;
    int         face = -1;
    return KiwiExtrudeFace_Pick( &node, &face );
}

// ─── §16b the exported prism writer (see kiwi_extrude.h) ─────────────────────
brush_t *KiwiExtrude_BuildPrismDef( const kconPlane_t &plane, const float *loopUV, int n,
                                    float lo, float hi, const char **why )
{
    const char *localWhy = "unknown";
    if ( !why )
        why = &localWhy;
    if ( !loopUV || n < 3 || n > KEXT_MAX_PROFILE )
    {
        *why = "profile out of range";
        return 0;
    }

    prismBuild_t pb;
    pb.plane = plane;
    pb.lo    = lo;
    pb.hi    = hi;

    std::vector<float> uv( loopUV, loopUV + (size_t)n * 2 );
    std::vector<int>   ring;
    ring.reserve( (size_t)n );
    for ( int i = 0; i < n; ++i )
        ring.push_back( i );

    return BuildPieceDef( pb, uv, ring, why );
}

selbrush_t *KiwiExtrude_LandDef( brush_t *def )
{
    return LandDef( def );
}

// ─── §3 canExecute ───────────────────────────────────────────────────────────
bool KiwiExtrude_CanExecute()
{
    return !KiwiRegion_All().empty();
}

// ─── registration + lookup ───────────────────────────────────────────────────
void KiwiExtrude_RegisterCommands()
{
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    Radiant_RegisterCommand( "KiwiExtrudeRegion", 0, 0, KIWI_CMD_EXTRUDE_REGION );
    Radiant_RegisterCommand( "KiwiExtrudeFace",   0, 0, KIWI_CMD_EXTRUDE_FACE );
}

// KIWI-UX (shakeout G): E IS A CONTEXT VERB, and the context is resolved HERE.
//
// KiwiCmd_Start calls this before CanExecute and before Begin, so returning a
// different command object for the same id IS the dispatch — no framework change,
// no third command, and the HUD / hint rows automatically describe whichever one
// actually ran (they read KiwiCmd_Active()->Name()).
//
//   a FACE is selected or hovered  ->  Extrude Face  (a NEW BODY, kiwi_extrude.h)
//   otherwise                      ->  Extrude Region (§23, which decides for
//                                      itself whether a region is under the cursor
//                                      and says so if not)
//
// The fallback when NEITHER exists is the face command, whose CanExecute is
// deliberately unconditional so its Begin() runs and prints the message naming
// both arms.  Returning the region command there would refuse silently
// (KiwiExtrude_CanExecute is false with no regions), which is the exact failure
// this round is removing everywhere else.
KiwiEditorCommand *KiwiExtrude_CommandForId( int commandId )
{
    if ( commandId == KIWI_CMD_EXTRUDE_REGION )
        return &s_extrude;
    if ( commandId == KIWI_CMD_EXTRUDE_FACE )
    {
        if ( KiwiExtrudeFace_CanExecute() )
            return &s_extrudeFace;
        if ( KiwiExtrude_CanExecute() )
            return &s_extrude;
        return &s_extrudeFace;
    }
    return 0;
}
