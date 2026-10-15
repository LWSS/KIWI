#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI: Physics Drop's ODE side; see kiwi_physdrop_sim.h.  World and contact parameters follow
// the game's phys_* dvar defaults (phys_ode.cpp) except where an editor placement wants calmer
// settling (noted per value).

#include <universal/q_shared.h>
#include <physics/phys_local.h>          // PhysGeomList / PhysGeomInfo / BrushWrapper
#include <xanim/xmodel.h>
#include <ode/objects.h>
#include <ode/mass.h>
#include <physics/ode/odeext.h>          // ODE_Init
#include <physics/ode/collision_trimesh_KISAK.h>

#include "kiwi_physdrop_sim.h"

#include <float.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <vector>

// collision_std.cpp:646 (no header declares it).  Fills contact pos + depth; normal returned.
int dBoxBox( const dVector3 p1, const dMatrix3 R1, const dVector3 side1, const dVector3 p2,
             const dMatrix3 R2, const dVector3 side2, dVector3 normal, dReal *depth,
             int *return_code, int maxc, dContactGeom *contact, int skip );

namespace
{
    const int   KPD_CLIPMASK    = 0x2806C91;     // qcommon.h PHYS_WORLD_CLIPMASK (KISAK_MP)
    const float KPD_STEP        = 1.0f / 120.0f;
    const int   KPD_MAX_SUB     = 8;             // sub-steps for fast or thin bodies
    const int   KPD_MAX_STEPS   = 32;            // per Advance call
    const float KPD_TIME_CAP    = 20.0f;         // seconds of simulated time
    const int   KPD_MAX_BODIES  = 256;           // the ODE body pool holds 512
    const int   KPD_WORLD_KEEP  = 6;             // contacts kept per body against the world
    const int   KPD_PAIR_KEEP   = 4;             // ...per body pair
    const int   KPD_TRI_BATCH   = 21000;         // 3 * batch < 65536 (uint16 indices)
    const float KPD_MIN_HALF    = 0.25f;

    struct part_t
    {
        float center[3];     // body frame (origin = centre of mass)
        float axis[3][3];    // rows, body frame
        float half[3];
    };

    struct body_t
    {
        dxBody             *id = nullptr;
        std::vector<part_t> parts;
        float               com[3]   = { 0.0f, 0.0f, 0.0f };   // model space
        float               friction = 0.5f;
        float               bounce   = 0.5f;
        float               minHalf  = 1.0f;
        float               radius   = 1.0f;
        float               lo[3], hi[3];                       // this step's world bounds
        bool                lost     = false;
    };

    struct tri_t
    {
        float v[3][3];
        float lo[3], hi[3];
        bool  twoSided;
    };

    struct node_t
    {
        float lo[3], hi[3];
        int   first, count, left, right;
    };

    bool                 s_odeInit = false;
    dxWorld             *s_odeWorld   = nullptr;
    dxJointGroup        *s_group   = nullptr;
    std::vector<body_t>  s_bodies;
    float                s_time    = 0.0f;
    float                s_acc     = 0.0f;
    float                s_floor   = -FLT_MAX;

    std::vector<tri_t>   s_tris;
    std::vector<int>     s_order;
    std::vector<node_t>  s_nodes;
    std::vector<int>     s_hits;
    std::vector<float>   s_batchVerts;
    std::vector<unsigned short> s_batchIdx;

    inline float Dot3( const float *a, const float *b ) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    inline void Cross3( const float *a, const float *b, float *out )
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    // ODE matrix <-> axis rows (Phys_AxisToOdeMatrix3 / Phys_OdeMatrix3ToAxis).
    void AxisToOde( const float axis[3][3], float R[12] )
    {
        for ( int r = 0; r < 3; ++r )
        {
            for ( int c = 0; c < 3; ++c )
                R[4 * r + c] = axis[c][r];
            R[4 * r + 3] = 0.0f;
        }
    }

    void OdeToAxis( const float *R, float axis[3][3] )
    {
        for ( int r = 0; r < 3; ++r )
            for ( int c = 0; c < 3; ++c )
                axis[c][r] = R[4 * r + c];
    }

    bool Finite3( const float *v )
    {
        return _finite( v[0] ) && _finite( v[1] ) && _finite( v[2] );
    }

    // ── static triangle BVH ─────────────────────────────────────────────────
    int BuildNode( int first, int count )
    {
        node_t n;
        n.first = first; n.count = count; n.left = n.right = -1;
        for ( int k = 0; k < 3; ++k ) { n.lo[k] = FLT_MAX; n.hi[k] = -FLT_MAX; }
        for ( int i = first; i < first + count; ++i )
        {
            const tri_t &t = s_tris[s_order[i]];
            for ( int k = 0; k < 3; ++k )
            {
                n.lo[k] = (std::min)( n.lo[k], t.lo[k] );
                n.hi[k] = (std::max)( n.hi[k], t.hi[k] );
            }
        }
        const int id = (int)s_nodes.size();
        s_nodes.push_back( n );
        if ( count > 8 )
        {
            int axis = 0;
            for ( int k = 1; k < 3; ++k )
                if ( n.hi[k] - n.lo[k] > n.hi[axis] - n.lo[axis] )
                    axis = k;
            const int middle = first + count / 2;
            std::nth_element( s_order.begin() + first, s_order.begin() + middle, s_order.begin() + first + count,
                [axis]( int a, int b ) { return s_tris[a].lo[axis] + s_tris[a].hi[axis] < s_tris[b].lo[axis] + s_tris[b].hi[axis]; } );
            const int left  = BuildNode( first, middle - first );
            const int right = BuildNode( middle, first + count - middle );
            s_nodes[id].left  = left;
            s_nodes[id].right = right;
        }
        return id;
    }

    void Query( const float lo[3], const float hi[3] )
    {
        s_hits.clear();
        if ( s_nodes.empty() )
            return;
        int stack[128];
        int top = 0;
        stack[top++] = 0;
        while ( top > 0 )
        {
            const node_t &n = s_nodes[stack[--top]];
            if ( n.lo[0] > hi[0] || n.hi[0] < lo[0] || n.lo[1] > hi[1] || n.hi[1] < lo[1]
              || n.lo[2] > hi[2] || n.hi[2] < lo[2] )
                continue;
            if ( n.left >= 0 && top + 2 <= 128 )
            {
                stack[top++] = n.left;
                stack[top++] = n.right;
                continue;
            }
            for ( int i = n.first; i < n.first + n.count; ++i )
            {
                const tri_t &t = s_tris[s_order[i]];
                if ( t.lo[0] > hi[0] || t.hi[0] < lo[0] || t.lo[1] > hi[1] || t.hi[1] < lo[1]
                  || t.lo[2] > hi[2] || t.hi[2] < lo[2] )
                    continue;
                s_hits.push_back( s_order[i] );
            }
        }
        // Geometric order, not gather order: a regathered region gives the same contacts.
        std::sort( s_hits.begin(), s_hits.end(), []( int a, int b )
        {
            const tri_t &ta = s_tris[a], &tb = s_tris[b];
            const int c = memcmp( ta.v, tb.v, sizeof( ta.v ) );
            return c != 0 ? c < 0 : ( ta.twoSided && !tb.twoSided );
        } );
    }

    // ── body helpers ────────────────────────────────────────────────────────
    void BodyFrame( const body_t &b, float pos[3], float axis[3][3] )
    {
        const float *p = dBodyGetPosition( b.id );
        pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
        OdeToAxis( dBodyGetRotation( b.id ), axis );
    }

    // A part in world space: centre (4 floats for ODE), ODE rotation, half extents.
    void PartWorld( const float pos[3], const float axis[3][3], const part_t &p,
                    float center[4], float R[12], float lo[3], float hi[3] )
    {
        float pa[3][3];
        for ( int j = 0; j < 3; ++j )
        {
            center[j] = pos[j] + axis[0][j] * p.center[0] + axis[1][j] * p.center[1] + axis[2][j] * p.center[2];
            for ( int k = 0; k < 3; ++k )
                pa[k][j] = p.axis[k][0] * axis[0][j] + p.axis[k][1] * axis[1][j] + p.axis[k][2] * axis[2][j];
        }
        center[3] = 0.0f;
        AxisToOde( pa, R );
        for ( int j = 0; j < 3; ++j )
        {
            const float ext = fabsf( pa[0][j] ) * p.half[0] + fabsf( pa[1][j] ) * p.half[1] + fabsf( pa[2][j] ) * p.half[2];
            lo[j] = center[j] - ext;
            hi[j] = center[j] + ext;
        }
    }

    void UpdateBounds( body_t &b )
    {
        float pos[3], axis[3][3];
        BodyFrame( b, pos, axis );
        for ( int k = 0; k < 3; ++k ) { b.lo[k] = FLT_MAX; b.hi[k] = -FLT_MAX; }
        for ( const part_t &p : b.parts )
        {
            float c[4], R[12], lo[3], hi[3];
            PartWorld( pos, axis, p, c, R, lo, hi );
            for ( int k = 0; k < 3; ++k )
            {
                b.lo[k] = (std::min)( b.lo[k], lo[k] );
                b.hi[k] = (std::max)( b.hi[k], hi[k] );
            }
        }
    }

    // Keep the deepest contact, then the ones farthest from those kept (a stable support set).
    int Reduce( dContactGeom *c, int n, int keep )
    {
        if ( n <= keep )
            return n;
        int best = 0;
        for ( int i = 1; i < n; ++i )
            if ( c[i].depth > c[best].depth )
                best = i;
        std::swap( c[0], c[best] );
        for ( int k = 1; k < keep; ++k )
        {
            int   pick  = k;
            float farD2 = -1.0f;
            for ( int i = k; i < n; ++i )
            {
                float d2 = FLT_MAX;
                for ( int j = 0; j < k; ++j )
                {
                    const float d[3] = { c[i].pos[0] - c[j].pos[0], c[i].pos[1] - c[j].pos[1], c[i].pos[2] - c[j].pos[2] };
                    d2 = (std::min)( d2, Dot3( d, d ) );
                }
                if ( d2 > farD2 )
                {
                    farD2 = d2;
                    pick  = i;
                }
            }
            std::swap( c[k], c[pick] );
        }
        return keep;
    }

    // Body against the static triangles: the game's collider (phys_coll_boxbrush.cpp
    // Phys_CollideBoxWithTriangleList) per part.  Contact normals point into the world, so the
    // joint takes the body as body 2 (dCollideWorldGeom's order).
    int WorldContacts( const body_t &b, dContactGeom *out, int maxOut )
    {
        static std::vector<dContactGeom> all;
        all.clear();
        float pos[3], axis[3][3];
        BodyFrame( b, pos, axis );
        const float bodyCenter[3] = { pos[0], pos[1], pos[2] };
        for ( const part_t &p : b.parts )
        {
            float c[4], R[12], lo[3], hi[3];
            PartWorld( pos, axis, p, c, R, lo, hi );
            for ( int k = 0; k < 3; ++k ) { lo[k] -= 0.5f; hi[k] += 0.5f; }
            Query( lo, hi );
            for ( size_t start = 0; start < s_hits.size(); start += KPD_TRI_BATCH )
            {
                const int count = (int)(std::min)( s_hits.size() - start, (size_t)KPD_TRI_BATCH );
                s_batchVerts.resize( (size_t)count * 9 );
                s_batchIdx.resize( (size_t)count * 3 );
                for ( int i = 0; i < count; ++i )
                {
                    const tri_t &t = s_tris[s_hits[start + i]];
                    int order[3] = { 0, 1, 2 };
                    if ( t.twoSided )
                    {
                        // Collider normal = -(b - a) x (c - a): turn it toward this box.
                        float e0[3], e1[3], s[3], toBox[3];
                        for ( int k = 0; k < 3; ++k )
                        {
                            e0[k] = t.v[1][k] - t.v[0][k];
                            e1[k] = t.v[2][k] - t.v[0][k];
                            toBox[k] = c[k] - t.v[0][k];
                        }
                        Cross3( e0, e1, s );
                        if ( Dot3( s, toBox ) > 0.0f )
                        {
                            order[1] = 2;
                            order[2] = 1;
                        }
                    }
                    for ( int v = 0; v < 3; ++v )
                    {
                        memcpy( &s_batchVerts[( (size_t)i * 3 + v ) * 3], t.v[order[v]], sizeof( float ) * 3 );
                        s_batchIdx[(size_t)i * 3 + v] = (unsigned short)( i * 3 + v );
                    }
                }
                dContactGeom tmp[24];
                const int n = dCollideBoxTriangleList( s_batchIdx.data(), (const float ( * )[3])s_batchVerts.data(),
                                                       count, R, c, p.half, bodyCenter, 24, tmp, sizeof( dContactGeom ) );
                for ( int i = 0; i < n; ++i )
                {
                    tmp[i].g1 = tmp[i].g2 = nullptr;
                    if ( Finite3( tmp[i].pos ) && Finite3( tmp[i].normal ) && _finite( tmp[i].depth ) )
                        all.push_back( tmp[i] );
                }
            }
        }
        const int n = Reduce( all.data(), (int)all.size(), (std::min)( KPD_WORLD_KEEP, maxOut ) );
        for ( int i = 0; i < n; ++i )
            out[i] = all[i];
        return n;
    }

    // Body against body: ODE's box-box per part pair, normals as dCollideBoxBox sets them.
    int PairContacts( const body_t &a, const body_t &b, dContactGeom *out, int maxOut )
    {
        static std::vector<dContactGeom> all;
        all.clear();
        float pa[3], aa[3][3], pb[3], ab[3][3];
        BodyFrame( a, pa, aa );
        BodyFrame( b, pb, ab );
        for ( const part_t &p : a.parts )
        {
            float c1[4], R1[12], lo1[3], hi1[3];
            PartWorld( pa, aa, p, c1, R1, lo1, hi1 );
            const float side1[4] = { 2.0f * p.half[0], 2.0f * p.half[1], 2.0f * p.half[2], 0.0f };
            for ( const part_t &q : b.parts )
            {
                float c2[4], R2[12], lo2[3], hi2[3];
                PartWorld( pb, ab, q, c2, R2, lo2, hi2 );
                if ( lo1[0] > hi2[0] || hi1[0] < lo2[0] || lo1[1] > hi2[1] || hi1[1] < lo2[1]
                  || lo1[2] > hi2[2] || hi1[2] < lo2[2] )
                    continue;
                const float side2[4] = { 2.0f * q.half[0], 2.0f * q.half[1], 2.0f * q.half[2], 0.0f };
                float normal[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                float depth = 0.0f;
                int   code  = 0;
                dContactGeom tmp[8];
                const int n = dBoxBox( c1, R1, side1, c2, R2, side2, normal, &depth, &code, 8, tmp, sizeof( dContactGeom ) );
                for ( int i = 0; i < n; ++i )
                {
                    tmp[i].normal[0] = -normal[0];
                    tmp[i].normal[1] = -normal[1];
                    tmp[i].normal[2] = -normal[2];
                    tmp[i].normal[3] = 0.0f;
                    tmp[i].g1 = tmp[i].g2 = nullptr;
                    if ( Finite3( tmp[i].pos ) && Finite3( tmp[i].normal ) && _finite( tmp[i].depth ) )
                        all.push_back( tmp[i] );
                }
            }
        }
        const int n = Reduce( all.data(), (int)all.size(), (std::min)( KPD_PAIR_KEEP, maxOut ) );
        for ( int i = 0; i < n; ++i )
            out[i] = all[i];
        return n;
    }

    // Phys_NearCallback's surface: soft ERP/CFM (phys_contact_erp / _cfm), summed friction,
    // the smaller bounce.  bounce_vel 30 (the game: 0.1) so resting contacts stop chattering.
    void SurfaceFor( const body_t *a, const body_t *b, dSurfaceParameters *s )
    {
        memset( s, 0, sizeof( *s ) );
        s->mode     = dContactBounce | dContactSoftERP | dContactSoftCFM | dContactApprox1;
        s->mu       = ( a ? a->friction : 0.0f ) + ( b ? b->friction : 0.0f );
        float bounce = 1.0f;
        if ( a ) bounce = (std::min)( bounce, a->bounce );
        if ( b ) bounce = (std::min)( bounce, b->bounce );
        s->bounce     = bounce;
        s->bounce_vel = 30.0f;
        s->soft_erp   = 0.8f;
        s->soft_cfm   = 0.00001f;
    }

    // False when the joint pool is full.
    bool AddContact( const dSurfaceParameters &surf, const dContactGeom &c, dxBody *b1, dxBody *b2 )
    {
        dxJoint *j = dJointCreateContact( s_odeWorld, s_group, &surf, &c );
        if ( !j )
            return false;
        dJointAttach( j, b1, b2 );
        return true;
    }

    bool Resting( const body_t &b )
    {
        return b.lost || !dBodyIsEnabled( b.id );
    }

    bool AllResting()
    {
        for ( const body_t &b : s_bodies )
            if ( !Resting( b ) )
                return false;
        return true;
    }

    void Stop( body_t &b )
    {
        dBodySetLinearVel( b.id, 0.0f, 0.0f, 0.0f );
        dBodySetAngularVel( b.id, 0.0f, 0.0f, 0.0f );
        dBodyDisable( b.id );
    }

    bool BoundsMeet( const body_t &a, const body_t &b, float pad )
    {
        return a.lo[0] - pad <= b.hi[0] && a.hi[0] + pad >= b.lo[0] && a.lo[1] - pad <= b.hi[1]
            && a.hi[1] + pad >= b.lo[1] && a.lo[2] - pad <= b.hi[2] && a.hi[2] + pad >= b.lo[2];
    }

    // An awake body overlaps this one's bounds (this step's, padded), so it may wake it.
    bool NearAwake( size_t i )
    {
        for ( size_t j = 0; j < s_bodies.size(); ++j )
            if ( j != i && !Resting( s_bodies[j] ) && BoundsMeet( s_bodies[i], s_bodies[j], 1.0f ) )
                return true;
        return false;
    }

    // Enough sub-steps that no awake body moves more than ~half its thinnest box per step:
    // the box-triangle collider only sees overlaps, so a thin fast box would pass a floor.
    int SubSteps()
    {
        float worst = 0.0f;
        for ( const body_t &b : s_bodies )
        {
            if ( Resting( b ) )
                continue;
            const float *v = dBodyGetLinearVel( b.id );
            const float *w = dBodyGetAngularVel( b.id );
            const float speed = sqrtf( Dot3( v, v ) ) + sqrtf( Dot3( w, w ) ) * b.radius;
            worst = (std::max)( worst, speed * KPD_STEP / ( 0.5f * b.minHalf ) );
        }
        int sub = (int)ceilf( worst );
        if ( sub < 1 )
            sub = 1;
        return sub > KPD_MAX_SUB ? KPD_MAX_SUB : sub;
    }

    void Step( float h )
    {
        dContactGeom contacts[KPD_WORLD_KEEP > KPD_PAIR_KEEP ? KPD_WORLD_KEEP : KPD_PAIR_KEEP];
        dSurfaceParameters surf;
        bool room = true;
        for ( body_t &b : s_bodies )
            if ( !b.lost )
                UpdateBounds( b );
        for ( size_t i = 0; i < s_bodies.size() && room; ++i )
        {
            body_t &b = s_bodies[i];
            if ( b.lost || ( Resting( b ) && !NearAwake( i ) ) )
                continue;                         // asleep and untouched: nothing can move it
            const int n = WorldContacts( b, contacts, KPD_WORLD_KEEP );
            SurfaceFor( &b, nullptr, &surf );
            for ( int k = 0; k < n && room; ++k )
                room = AddContact( surf, contacts[k], nullptr, b.id );
        }
        for ( size_t i = 0; i < s_bodies.size() && room; ++i )
        {
            const body_t &a = s_bodies[i];
            if ( a.lost )
                continue;
            for ( size_t j = i + 1; j < s_bodies.size() && room; ++j )
            {
                const body_t &b = s_bodies[j];
                if ( b.lost || ( Resting( a ) && Resting( b ) ) )
                    continue;
                if ( a.lo[0] > b.hi[0] || a.hi[0] < b.lo[0] || a.lo[1] > b.hi[1] || a.hi[1] < b.lo[1]
                  || a.lo[2] > b.hi[2] || a.hi[2] < b.lo[2] )
                    continue;
                const int n = PairContacts( a, b, contacts, KPD_PAIR_KEEP );
                SurfaceFor( &a, &b, &surf );
                for ( int k = 0; k < n && room; ++k )
                    room = AddContact( surf, contacts[k], a.id, b.id );
            }
        }

        dWorldQuickStep( s_odeWorld, h );
        dJointGroupEmpty( s_group );
        // KIWI: this ODE's step keeps the force accumulators (gravity included); the game
        // clears them in dxPostProcessIslands.  Left alone, gravity grew every step and a model
        // hit the floor at thousands of units / s and passed through it.
        for ( body_t &b : s_bodies )
        {
            for ( int k = 0; k < 3; ++k )
            {
                b.id->facc[k] = 0.0f;
                b.id->tacc[k] = 0.0f;
            }
        }

        // Phys_DoBodyOncePerFrame: phys_dragLinear 0.03, phys_dragAngular 0.5 per second.
        const float linDrag = (std::max)( 0.0f, 1.0f - 0.03f * h );
        const float angDrag = (std::max)( 0.0f, 1.0f - 0.5f * h );
        for ( body_t &b : s_bodies )
        {
            if ( Resting( b ) )
                continue;
            const float *v = dBodyGetLinearVel( b.id );
            const float *w = dBodyGetAngularVel( b.id );
            dBodySetLinearVel( b.id, v[0] * linDrag, v[1] * linDrag, v[2] * linDrag );
            dBodySetAngularVel( b.id, w[0] * angDrag, w[1] * angDrag, w[2] * angDrag );
            const float *p = dBodyGetPosition( b.id );
            if ( !Finite3( p ) || p[2] < s_floor )
            {
                b.lost = true;
                Stop( b );
            }
        }
    }
}

bool KiwiPhysSim_ModelShape( const XModel *model, float scale, std::vector<kpdPart_t> &parts,
                             float *mass, float *friction, float *bounce )
{
    parts.clear();
    if ( !model )
        return false;
    if ( !( scale > 0.0f ) || !_finite( scale ) )
        scale = 1.0f;
    if ( const PhysPreset *preset = model->physPreset )
    {
        if ( preset->mass > 0.0f && _finite( preset->mass ) && mass )
            *mass = preset->mass;
        if ( preset->friction >= 0.0f && _finite( preset->friction ) && friction )
            *friction = preset->friction;
        if ( preset->bounce >= 0.0f && _finite( preset->bounce ) && bounce )
            *bounce = preset->bounce;
    }
    const PhysGeomList *list = model->physGeoms;
    if ( !list || !list->geoms )
        return false;
    for ( unsigned int i = 0; i < list->count; ++i )
    {
        const PhysGeomInfo &g = list->geoms[i];
        kpdPart_t p;
        memset( &p, 0, sizeof( p ) );
        if ( g.brush )
        {
            for ( int k = 0; k < 3; ++k )
            {
                p.center[k]  = 0.5f * ( g.brush->mins[k] + g.brush->maxs[k] );
                p.half[k]    = 0.5f * ( g.brush->maxs[k] - g.brush->mins[k] );
                p.axis[k][k] = 1.0f;
            }
        }
        else
        {
            for ( int k = 0; k < 3; ++k )
            {
                p.center[k] = g.offset[k];
                for ( int j = 0; j < 3; ++j )
                    p.axis[k][j] = g.orientation[k][j];
            }
            if ( g.type == PHYS_GEOM_BOX )
            {
                p.half[0] = g.halfLengths[0];
                p.half[1] = g.halfLengths[1];
                p.half[2] = g.halfLengths[2];
            }
            else
            {
                // Phys_ObjAddGeomCylinderRotated: axis 0, halfHeight [0], radius [1].
                p.half[0] = g.halfLengths[0];
                p.half[1] = g.halfLengths[1];
                p.half[2] = g.halfLengths[1];
            }
        }
        bool ok = true;
        for ( int k = 0; k < 3; ++k )
        {
            p.center[k] *= scale;
            p.half[k]   *= scale;
            ok = ok && _finite( p.center[k] ) && _finite( p.half[k] ) && p.half[k] > 0.0f && Finite3( p.axis[k] );
        }
        if ( ok )
            parts.push_back( p );
    }
    return !parts.empty();
}

bool KiwiPhysSim_ModelCanBeDynamic( const XModel *model, bool *hasPreset )
{
    if ( hasPreset )
        *hasPreset = model && model->physPreset;
    return model && model->collLod >= 0;
}

int KiwiPhysSim_ClipMask()
{
    return KPD_CLIPMASK;
}

void KiwiPhysSim_Begin()
{
    KiwiPhysSim_End();
    if ( !s_odeInit )
    {
        ODE_Init();                       // the body / geom pools; once per process
        s_odeInit = true;
    }
    s_odeWorld = dWorldCreate( PHYS_WORLD_DYNENT );
    s_group = dGetContactJointGroup( PHYS_WORLD_DYNENT );
    // Phys_RunFrame's world setup (phys_cfm, phys_erp, phys_mcv, phys_gravity); surface layer
    // 0.25 (phys_csl 1.0 sinks resting props a whole unit) and 20 iterations (phys_qsi 15).
    dWorldSetGravity( s_odeWorld, 0.0f, 0.0f, -800.0f );
    dWorldSetCFM( s_odeWorld, 0.0001f );
    dWorldSetERP( s_odeWorld, 0.8f );
    dWorldSetContactMaxCorrectingVel( s_odeWorld, 20.0f );
    dWorldSetContactSurfaceLayer( s_odeWorld, 0.25f );
    dWorldSetQuickStepNumIterations( s_odeWorld, 20 );
    // Rest = ODE auto-disable; stricter than phys_autoDisable* (20 / 1 / 0.9) so slow slides finish.
    dWorldSetAutoDisableFlag( s_odeWorld, 1 );
    dWorldSetAutoDisableSteps( s_odeWorld, 0 );
    dWorldSetAutoDisableLinearThreshold( s_odeWorld, 3.0f );
    dWorldSetAutoDisableAngularThreshold( s_odeWorld, 0.3f );
    dWorldSetAutoDisableTime( s_odeWorld, 0.5f );
    s_time = 0.0f;
    s_acc  = 0.0f;
}

int KiwiPhysSim_AddBody( const kpdBodyDesc_t &d )
{
    if ( !s_odeWorld || d.parts.empty() || (int)s_bodies.size() >= KPD_MAX_BODIES )
        return -1;

    // Centre of mass: the boxes' centres weighted by volume.
    double vol = 0.0, com[3] = { 0.0, 0.0, 0.0 };
    for ( const kpdPart_t &p : d.parts )
    {
        const double v = 8.0 * (std::max)( p.half[0], KPD_MIN_HALF ) * (std::max)( p.half[1], KPD_MIN_HALF )
                             * (std::max)( p.half[2], KPD_MIN_HALF );
        vol += v;
        for ( int k = 0; k < 3; ++k )
            com[k] += v * p.center[k];
    }
    if ( !( vol > 0.0 ) )
        return -1;

    body_t b;
    b.friction = d.friction;
    b.bounce   = d.bounce;
    b.minHalf  = FLT_MAX;
    b.radius   = 0.0f;
    for ( int k = 0; k < 3; ++k )
        b.com[k] = (float)( com[k] / vol );

    dMass total;
    dMassSetZero( &total );
    for ( const kpdPart_t &dp : d.parts )
    {
        part_t p;
        for ( int k = 0; k < 3; ++k )
        {
            p.center[k] = dp.center[k] - b.com[k];
            p.half[k]   = (std::max)( dp.half[k], KPD_MIN_HALF );
            for ( int j = 0; j < 3; ++j )
                p.axis[k][j] = dp.axis[k][j];
            b.minHalf = (std::min)( b.minHalf, p.half[k] );
        }
        b.radius = (std::max)( b.radius, sqrtf( Dot3( p.center, p.center ) ) + sqrtf( Dot3( p.half, p.half ) ) );
        b.parts.push_back( p );

        dMass m;
        float R[12];
        dMassSetBox( &m, 1.0f, 2.0f * p.half[0], 2.0f * p.half[1], 2.0f * p.half[2] );
        AxisToOde( p.axis, R );
        dMassRotate( &m, R );
        dMassTranslate( &m, p.center[0], p.center[1], p.center[2] );
        dMassAdd( &total, &m );
    }
    dMassAdjust( &total, d.mass > 0.0f ? d.mass : 1.0f );
    total.c[0] = total.c[1] = total.c[2] = 0.0f;   // the body origin IS the centre of mass

    float pos[3], R[12];
    for ( int k = 0; k < 3; ++k )
        pos[k] = d.origin[k] + d.axis[0][k] * b.com[0] + d.axis[1][k] * b.com[1] + d.axis[2][k] * b.com[2];
    AxisToOde( d.axis, R );
    if ( !Finite3( pos ) )
        return -1;

    b.id = dBodyCreate( s_odeWorld );
    if ( !b.id )
        return -1;
    dBodySetPosition( b.id, pos[0], pos[1], pos[2] );
    dBodySetRotation( b.id, R );
    dBodySetMass( b.id, &total );
    dBodySetLinearVel( b.id, d.linVel[0], d.linVel[1], d.linVel[2] );
    dBodySetAngularVel( b.id, d.angVel[0], d.angVel[1], d.angVel[2] );
    s_bodies.push_back( b );
    return (int)s_bodies.size() - 1;
}

void KiwiPhysSim_End()
{
    if ( s_group )
        dJointGroupEmpty( s_group );
    for ( body_t &b : s_bodies )
        if ( b.id )
            dBodyDestroy( b.id );
    s_bodies.clear();
    s_odeWorld = nullptr;
    s_group = nullptr;
    s_time  = 0.0f;
    s_acc   = 0.0f;
}

void KiwiPhysSim_SetFloor( float z )
{
    s_floor = z;
}

void KiwiPhysSim_ClearStatic()
{
    s_tris.clear();
    s_order.clear();
    s_nodes.clear();
}

void KiwiPhysSim_AddStaticTri( const float *a, const float *b, const float *c, bool twoSided )
{
    if ( !a || !b || !c || !Finite3( a ) || !Finite3( b ) || !Finite3( c ) )
        return;
    tri_t t;
    // One-sided: stored as (a, c, b) so the collider's -(v1 - v0) x (v2 - v0) is the front.
    memcpy( t.v[0], a, sizeof( float ) * 3 );
    memcpy( t.v[1], twoSided ? b : c, sizeof( float ) * 3 );
    memcpy( t.v[2], twoSided ? c : b, sizeof( float ) * 3 );
    t.twoSided = twoSided;
    for ( int k = 0; k < 3; ++k )
    {
        t.lo[k] = (std::min)( (std::min)( a[k], b[k] ), c[k] );
        t.hi[k] = (std::max)( (std::max)( a[k], b[k] ), c[k] );
    }
    s_tris.push_back( t );
}

void KiwiPhysSim_BuildStatic()
{
    s_order.resize( s_tris.size() );
    for ( size_t i = 0; i < s_tris.size(); ++i )
        s_order[i] = (int)i;
    s_nodes.clear();
    if ( !s_tris.empty() )
        BuildNode( 0, (int)s_tris.size() );
}

int KiwiPhysSim_StaticTriCount()
{
    return (int)s_tris.size();
}

bool KiwiPhysSim_Advance( float dt )
{
    if ( !s_odeWorld || s_bodies.empty() )
        return false;
    if ( dt > 0.0f && _finite( dt ) )
        s_acc += dt;
    int steps = 0;
    while ( s_acc >= KPD_STEP && steps < KPD_MAX_STEPS )
    {
        if ( AllResting() )
        {
            s_acc = 0.0f;
            return false;
        }
        if ( s_time >= KPD_TIME_CAP )
        {
            for ( body_t &b : s_bodies )
                if ( !b.lost )
                    Stop( b );
            s_acc = 0.0f;
            return false;
        }
        const int sub = SubSteps();
        for ( int k = 0; k < sub; ++k )
            Step( KPD_STEP / (float)sub );
        s_time += KPD_STEP;
        s_acc  -= KPD_STEP;
        ++steps;
    }
    if ( steps >= KPD_MAX_STEPS )
        s_acc = 0.0f;                    // a slow frame drops its backlog instead of spiralling
    return !AllResting() && s_time < KPD_TIME_CAP;
}

float KiwiPhysSim_Time()
{
    return s_time;
}

int KiwiPhysSim_RestingCount()
{
    int n = 0;
    for ( const body_t &b : s_bodies )
        if ( Resting( b ) )
            ++n;
    return n;
}

bool KiwiPhysSim_BodyLost( int body )
{
    return body >= 0 && body < (int)s_bodies.size() && s_bodies[body].lost;
}

bool KiwiPhysSim_BodyPose( int body, float origin[3], float axis[3][3] )
{
    if ( body < 0 || body >= (int)s_bodies.size() )
        return false;
    const body_t &b = s_bodies[body];
    float pos[3];
    BodyFrame( b, pos, axis );
    for ( int k = 0; k < 3; ++k )
        origin[k] = pos[k] - ( axis[0][k] * b.com[0] + axis[1][k] * b.com[1] + axis[2][k] * b.com[2] );
    return Finite3( origin ) && Finite3( axis[0] ) && Finite3( axis[1] ) && Finite3( axis[2] );
}

bool KiwiPhysSim_BodyBounds( int body, float mins[3], float maxs[3] )
{
    if ( body < 0 || body >= (int)s_bodies.size() )
        return false;
    body_t &b = s_bodies[body];
    UpdateBounds( b );
    for ( int k = 0; k < 3; ++k )
    {
        mins[k] = b.lo[k];
        maxs[k] = b.hi[k];
    }
    return true;
}

int KiwiPhysSim_PartCount( int body )
{
    return ( body >= 0 && body < (int)s_bodies.size() ) ? (int)s_bodies[body].parts.size() : 0;
}

int KiwiPhysSim_PartBoxes( int body, float ( *corners )[8][3], int maxBoxes )
{
    if ( body < 0 || body >= (int)s_bodies.size() || !corners )
        return 0;
    const body_t &b = s_bodies[body];
    float pos[3], axis[3][3];
    BodyFrame( b, pos, axis );
    int n = 0;
    for ( const part_t &p : b.parts )
    {
        if ( n >= maxBoxes )
            break;
        float c[4], R[12], lo[3], hi[3];
        PartWorld( pos, axis, p, c, R, lo, hi );
        float pa[3][3];
        OdeToAxis( R, pa );
        for ( int k = 0; k < 8; ++k )
            for ( int j = 0; j < 3; ++j )
                corners[n][k][j] = c[j] + pa[0][j] * ( ( k & 1 ) ? p.half[0] : -p.half[0] )
                                        + pa[1][j] * ( ( k & 2 ) ? p.half[1] : -p.half[1] )
                                        + pa[2][j] * ( ( k & 4 ) ? p.half[2] : -p.half[2] );
        ++n;
    }
    return n;
}
