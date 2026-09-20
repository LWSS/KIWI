#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-09-16): barbwire strips swept along construction curves.  See the header for
// the design; this file is the strand extraction, the sweep, the v25 xmodel writer and
// the entity plumbing.  Wire formats are the ones cod4rad's loader consumes
// (src/cod4rad/xmodel_load_obj.c, r_xsurface_load_obj.c) - read back by the very same
// code in the editor, the compilers and the game.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"

#include <imgui/imgui.h>
#include <universal/com_files.h>

#include "kiwi_barbwire.h"
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_conselect.h"
#include "kiwi_selection.h"
#include "kiwi_fmt.h"
#include "kiwi_vec.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

// Ported entry points (all verified against their definitions, see the file:line notes).
extern int         Sys_Printf( const char *fmt, ... );                      // win_qe3.cpp:118
extern int         g_nUpdateBits;
extern entity_s    entities;                                                 // entity.cpp:298
extern entity_s   *world_entity;                                             // map.cpp
extern selbrush_t  selected_brushes;                                         // map.cpp
extern selbrush_t  active_brushes;                                           // map.cpp
extern eclass_t   *Eclass_ForName( int has_brushes, const char *name );
extern brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );
extern void        Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls );
extern void        Brush_BuildWindings( brush_t *def, int bFull );
extern void        Select_Deselect( int a1 );                                // select.cpp:1444
extern void        Select_Brush( selbrush_t *brush, char some_overwrite, char bStatus, char center ); // select.cpp:904
extern void        Select_Delete();                                          // select.cpp:1539
extern void        CreateEntityFromName( const char *str );
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:213
extern void        Undo_ClearRedo();                                         // undo.cpp:176
extern void        Undo_GeneralStart( const char *operation );               // undo.cpp:367
extern void        Undo_AddBrushList( selbrush_t *list );                    // undo.cpp:551
extern void        Undo_EndBrushList( selbrush_t *list );                    // undo.cpp:576
extern void        Undo_AddEntity_W( entity_s *e );                          // undo.cpp:633
extern void        Undo_KiwiMarkCreated( brush_t *def );                     // undo.cpp (KIWI tail)
extern void        Undo_End();                                               // undo.cpp:686
extern void        MarkMapModified();                                        // win_qe3.cpp:198
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
extern void        Ed_EnsureCurrentMaterial_Kiwi();
extern selbrush_t *KiwiExtrude_LandDef( brush_t *def );                     // kiwi_extrude.cpp

namespace
{
    // ── the stock sources ────────────────────────────────────────────────────────
    struct bwSource_t { const char *model; const char *label; };
    const bwSource_t KBW_SOURCES[] =
    {
        { "mil_barbedwire7", "Fence strands (mil_barbedwire7) - 4 twin strands, 9..39 up" },
        { "mil_barbedwire6", "Fence strands (mil_barbedwire6) - same strands, single-post source" },
        { "mil_barbedwire2", "Sloped strands + zigzag (mil_barbedwire2)" },
        { "mil_barbedwire4", "Sloped strands + top wire (mil_barbedwire4)" },
        { "mil_barbedwire8", "Coil mesh (mil_barbedwire8)" },
    };
    const int KBW_SOURCE_COUNT = (int)( sizeof( KBW_SOURCES ) / sizeof( KBW_SOURCES[0] ) );

    // A component counts as a strand when it spans at least this much of the longest
    // component's x-extent.  Posts and coil wraps span 3..10 of ~105 units.
    const float KBW_STRAND_FRAC   = 0.8f;
    // Strand ends this close to the tile boundary are pulled onto it so tiles meet
    // without a gap (the 12-tri strands stop ~1 unit short of the 4-tri ones).
    const float KBW_END_SNAP      = 1.5f;
    // Weld radius for the component analysis (positions are exported floats).
    const float KBW_WELD          = 0.01f;
    // Strands whose height centres are closer than this are one physical wire (the twin
    // crossed ribbons sit within a unit; neighbouring wires are 7..9 units apart).
    const float KBW_WIRE_GAP      = 4.0f;
    // Tile stretch window: tiles are scaled so a whole number fits the curve; above
    // this factor one more tile is used instead of stretching the barbs further.
    const float KBW_MAX_STRETCH   = 1.25f;
    const float KBW_MIN_CURVE_LEN = 8.0f;
    const float KBW_PIECE_MIN     = 64.0f;
    const float KBW_PIECE_MAX     = 8192.0f;
    // LOD0 range for the generated pieces; the last LOD gets 0 (= no limit).
    const float KBW_LOD0_DIST     = 900.0f;
    const char *KBW_PROFILE       = "Barbwire";
    const char *KBW_KEY           = "kiwi_barbwire";       // epair naming the source curve
    const char *KBW_PREFIX        = "kiwi_bw_";            // generated xmodel names
    const char *KBW_BONE          = "tag_origin";

    // ── geometry ─────────────────────────────────────────────────────────────────
    struct bwVert_t
    {
        float         n[3];
        unsigned char c[4];
        float         uv[2];
        float         bin[3];
        float         tan[3];
        float         p[3];
    };
    struct bwSurf_t
    {
        std::string                 material;
        unsigned char               tileMode = 0;
        std::vector<bwVert_t>       verts;
        std::vector<unsigned short> tris;      // 3 per triangle
    };
    struct bwLod_t
    {
        std::vector<bwSurf_t> surfs;
    };
    // The strand tile: LOD0 (+ LOD1 when the source has one), x in [minX, maxX].
    struct bwTile_t
    {
        std::string model;
        bwLod_t     lod[2];
        int         lodCount = 0;
        float       minX = 0.0f, maxX = 0.0f;
        float       zShift = 0.0f;             // applied so the top kept wire sits at z 0
        int         strandTris = 0;            // LOD0, for the report
    };

    // ── little-endian readers with bounds checks ─────────────────────────────────
    struct bwReader_t
    {
        const unsigned char *p   = nullptr;
        const unsigned char *end = nullptr;
        bool                 ok  = true;

        bool Need( size_t n )
        {
            if ( !ok || (size_t)( end - p ) < n ) { ok = false; return false; }
            return true;
        }
        unsigned char U8()  { if ( !Need( 1 ) ) return 0; return *p++; }
        unsigned short U16(){ if ( !Need( 2 ) ) return 0; unsigned short v; memcpy( &v, p, 2 ); p += 2; return v; }
        int S32()           { if ( !Need( 4 ) ) return 0; int v; memcpy( &v, p, 4 ); p += 4; return v; }
        float F32()         { if ( !Need( 4 ) ) return 0.0f; float v; memcpy( &v, p, 4 ); p += 4; return v; }
        void Skip( size_t n ) { if ( Need( n ) ) p += n; }
        std::string CStr()
        {
            std::string s;
            while ( ok )
            {
                if ( !Need( 1 ) ) return s;
                const char ch = (char)*p++;
                if ( !ch ) break;
                s.push_back( ch );
            }
            return s;
        }
    };

    struct bwWriter_t
    {
        std::vector<unsigned char> b;
        void U8( unsigned v )        { b.push_back( (unsigned char)v ); }
        void U16( unsigned v )       { unsigned short s = (unsigned short)v; const unsigned char *q = (const unsigned char *)&s; b.insert( b.end(), q, q + 2 ); }
        void S32( int v )            { const unsigned char *q = (const unsigned char *)&v; b.insert( b.end(), q, q + 4 ); }
        void F32( float v )          { const unsigned char *q = (const unsigned char *)&v; b.insert( b.end(), q, q + 4 ); }
        void V3( const float *v )    { F32( v[0] ); F32( v[1] ); F32( v[2] ); }
        void CStr( const char *s )   { const size_t n = strlen( s ); b.insert( b.end(), (const unsigned char *)s, (const unsigned char *)s + n ); b.push_back( 0 ); }
    };

    void SetErr( char *err, size_t errSz, const char *fmt, ... )
    {
        if ( !err || !errSz )
            return;
        va_list ap;
        va_start( ap, fmt );
        _vsnprintf( err, errSz, fmt, ap );
        va_end( ap );
        err[errSz - 1] = '\0';
    }

    bool ReadRaw( const char *qpath, std::vector<unsigned char> *out )
    {
        void *buf = nullptr;
        const int size = FS_ReadFile( qpath, &buf );
        if ( size <= 0 || !buf )
        {
            if ( buf )
                FS_FreeFile( (char *)buf );
            return false;
        }
        out->assign( (const unsigned char *)buf, (const unsigned char *)buf + size );
        FS_FreeFile( (char *)buf );
        return true;
    }

    // raw/ rather than main/: the map compilers' search paths are raw/raw_shared/devraw
    // only, exactly the reasoning kiwi_matwriter.cpp records for materials.
    bool WriteRaw( const char *qpath, const std::vector<unsigned char> &bytes, char *err, size_t errSz )
    {
        const int h = FS_FOpenFileWriteToDir( qpath, "raw" );
        if ( !h )
        {
            SetErr( err, errSz, "could not open '%s' for writing", qpath );
            return false;
        }
        const unsigned wrote = bytes.empty() ? 0u : FS_Write( (const char *)&bytes[0], (unsigned)bytes.size(), h );
        FS_FCloseFile( h );
        if ( wrote != (unsigned)bytes.size() )
        {
            FS_DeleteInDir( (char *)qpath, (char *)"raw" );
            SetErr( err, errSz, "short write to '%s' (%u of %u bytes)", qpath, wrote, (unsigned)bytes.size() );
            return false;
        }
        return true;
    }

    // ── source parsing ───────────────────────────────────────────────────────────
    struct bwHeader_t
    {
        std::string              lodFile[4];
        std::vector<std::string> lodMaterials[4];
    };

    // XModel_ReadHeader + XModelReadCollSurfs + the first LOD material pass, verbatim
    // order (cod4rad/xmodel_load_obj.c:603-747).
    bool ParseHeader( const std::vector<unsigned char> &d, bwHeader_t *h, char *err, size_t errSz )
    {
        bwReader_t r;
        r.p = d.empty() ? nullptr : &d[0];
        r.end = r.p + d.size();
        if ( r.U16() != 25 ) { SetErr( err, errSz, "xmodel is not version 25" ); return false; }
        r.U8();                                   // flags
        for ( int i = 0; i < 6; ++i ) r.F32();    // radius cube
        r.CStr();                                 // physics preset
        for ( int i = 0; i < 4; ++i )
        {
            r.F32();                              // lod distance
            h->lodFile[i] = r.CStr();
        }
        r.S32();                                  // collLod
        const int numCollSurfs = r.S32();
        for ( int i = 0; i < numCollSurfs && r.ok; ++i )
        {
            const int tris = r.S32();
            if ( tris < 0 ) { r.ok = false; break; }
            r.Skip( (size_t)tris * 48 );          // plane/svec/tvec per tri
            r.Skip( 24 );                         // mins/maxs
            r.Skip( 12 );                         // bone, contents, surfFlags
        }
        for ( int i = 0; i < 4 && r.ok; ++i )
        {
            if ( h->lodFile[i].empty() )
                continue;
            const int n = r.U16();
            for ( int j = 0; j < n && r.ok; ++j )
                h->lodMaterials[i].push_back( r.CStr() );
        }
        if ( !r.ok ) { SetErr( err, errSz, "xmodel header is truncated" ); return false; }
        return true;
    }

    // R_XSurfaceLoadObj's rigid single-list layout (r_xsurface_load_obj.c:40-204).
    bool ParseSurfs( const std::vector<unsigned char> &d, const std::vector<std::string> &materials,
                     bwLod_t *lod, char *err, size_t errSz )
    {
        bwReader_t r;
        r.p = d.empty() ? nullptr : &d[0];
        r.end = r.p + d.size();
        if ( r.U16() != 25 ) { SetErr( err, errSz, "xmodelsurfs is not version 25" ); return false; }
        const int numSurfs = r.U16();
        if ( numSurfs != (int)materials.size() )
        {
            SetErr( err, errSz, "xmodelsurfs surface count (%d) disagrees with the xmodel (%d)",
                    numSurfs, (int)materials.size() );
            return false;
        }
        for ( int s = 0; s < numSurfs && r.ok; ++s )
        {
            bwSurf_t surf;
            surf.material = materials[(size_t)s];
            surf.tileMode = r.U8();
            r.U16();                              // retained by nobody
            const int vertCount = r.U16();
            const int triCount  = r.U16();
            int lists = 0, rigid = 0;
            for ( ;; )
            {
                const int count = r.U16();
                if ( !r.ok ) break;
                if ( !count ) break;
                r.U16();                          // bone index
                rigid += count;
                ++lists;
            }
            if ( !r.ok ) break;
            if ( rigid != vertCount || lists != 1 )
            {
                SetErr( err, errSz, "surface %d is skinned or multi-bone; only rigid sources are supported", s );
                return false;
            }
            surf.verts.resize( (size_t)vertCount );
            for ( int v = 0; v < vertCount && r.ok; ++v )
            {
                bwVert_t &o = surf.verts[(size_t)v];
                o.n[0] = r.F32(); o.n[1] = r.F32(); o.n[2] = r.F32();
                o.c[0] = r.U8(); o.c[1] = r.U8(); o.c[2] = r.U8(); o.c[3] = r.U8();
                o.uv[0] = r.F32(); o.uv[1] = r.F32();
                o.bin[0] = r.F32(); o.bin[1] = r.F32(); o.bin[2] = r.F32();
                o.tan[0] = r.F32(); o.tan[1] = r.F32(); o.tan[2] = r.F32();
                o.p[0] = r.F32(); o.p[1] = r.F32(); o.p[2] = r.F32();
            }
            surf.tris.resize( (size_t)triCount * 3 );
            for ( size_t i = 0; i < surf.tris.size() && r.ok; ++i )
            {
                surf.tris[i] = r.U16();
                if ( surf.tris[i] >= vertCount ) { r.ok = false; }
            }
            lod->surfs.push_back( surf );
        }
        if ( !r.ok ) { SetErr( err, errSz, "xmodelsurfs is truncated or malformed" ); return false; }
        return true;
    }

    // ── strand extraction ────────────────────────────────────────────────────────
    struct bwWeldKey_t
    {
        int x, y, z;
        bool operator<( const bwWeldKey_t &o ) const
        {
            if ( x != o.x ) return x < o.x;
            if ( y != o.y ) return y < o.y;
            return z < o.z;
        }
    };

    int Find( std::vector<int> &parent, int a )
    {
        while ( parent[(size_t)a] != a )
        {
            parent[(size_t)a] = parent[(size_t)parent[(size_t)a]];
            a = parent[(size_t)a];
        }
        return a;
    }

    // A connected component of one surface: which triangles, and its x / z extents.
    struct bwComp_t
    {
        std::vector<int> tris;
        float minX = FLT_MAX, maxX = -FLT_MAX;
        float minZ = FLT_MAX, maxZ = -FLT_MAX;
        float ZCentre() const { return 0.5f * ( minZ + maxZ ); }
    };

    // Components welded by position so split-normal seams do not fragment a strand.
    void Components( const bwSurf_t &surf, std::vector<bwComp_t> *out )
    {
        const int nv = (int)surf.verts.size();
        const int nt = (int)surf.tris.size() / 3;
        std::map<bwWeldKey_t, int> weld;
        std::vector<int> vid( (size_t)nv );
        for ( int v = 0; v < nv; ++v )
        {
            const bwWeldKey_t k = { (int)floorf( surf.verts[(size_t)v].p[0] / KBW_WELD + 0.5f ),
                                    (int)floorf( surf.verts[(size_t)v].p[1] / KBW_WELD + 0.5f ),
                                    (int)floorf( surf.verts[(size_t)v].p[2] / KBW_WELD + 0.5f ) };
            std::map<bwWeldKey_t, int>::iterator it = weld.find( k );
            if ( it == weld.end() )
                it = weld.insert( std::make_pair( k, (int)weld.size() ) ).first;
            vid[(size_t)v] = it->second;
        }
        std::vector<int> parent( weld.size() );
        for ( size_t i = 0; i < parent.size(); ++i ) parent[i] = (int)i;
        for ( int t = 0; t < nt; ++t )
        {
            const int a = Find( parent, vid[surf.tris[(size_t)t * 3 + 0]] );
            const int b = Find( parent, vid[surf.tris[(size_t)t * 3 + 1]] );
            const int c = Find( parent, vid[surf.tris[(size_t)t * 3 + 2]] );
            parent[(size_t)b] = a;
            parent[(size_t)c] = Find( parent, a );
        }
        std::map<int, int> slot;                 // root -> index in *out
        for ( int t = 0; t < nt; ++t )
        {
            const int root = Find( parent, vid[surf.tris[(size_t)t * 3]] );
            std::map<int, int>::iterator it = slot.find( root );
            if ( it == slot.end() )
            {
                it = slot.insert( std::make_pair( root, (int)out->size() ) ).first;
                out->push_back( bwComp_t() );
            }
            bwComp_t &c = ( *out )[(size_t)it->second];
            c.tris.push_back( t );
            for ( int k = 0; k < 3; ++k )
            {
                const bwVert_t &v = surf.verts[surf.tris[(size_t)t * 3 + k]];
                if ( v.p[0] < c.minX ) c.minX = v.p[0];
                if ( v.p[0] > c.maxX ) c.maxX = v.p[0];
                if ( v.p[2] < c.minZ ) c.minZ = v.p[2];
                if ( v.p[2] > c.maxZ ) c.maxZ = v.p[2];
            }
        }
    }

    // Rebuild a surface from the listed triangles (compacting the vertices).
    void KeepTris( bwSurf_t *surf, const std::vector<int> &tris )
    {
        std::vector<unsigned short> keptTris;
        std::vector<int> remap( surf->verts.size(), -1 );
        std::vector<bwVert_t> keptVerts;
        for ( size_t i = 0; i < tris.size(); ++i )
            for ( int k = 0; k < 3; ++k )
            {
                const int v = surf->tris[(size_t)tris[i] * 3 + k];
                if ( remap[(size_t)v] < 0 )
                {
                    remap[(size_t)v] = (int)keptVerts.size();
                    keptVerts.push_back( surf->verts[(size_t)v] );
                }
                keptTris.push_back( (unsigned short)remap[(size_t)v] );
            }
        surf->verts.swap( keptVerts );
        surf->tris.swap( keptTris );
    }

    // Two cuts.  (1) Strands: components spanning most of the longest one along x
    // (posts and coil wraps drop out).  (2) Wires: the strands cluster by height into
    // the physical wires (a wire is a twin pair of crossed ribbons, ~8 units apart
    // from the next); keep the top `wires` of them (0 = all).  KIWI (2026-09-16, user:
    // "It should be ONE strand of barbwire by default, not 4 ... just use the top strand").
    // `outZCentre` is the top kept wire's height centre (the caller re-bases on it).
    void ExtractStrands( bwLod_t *lod, int wires, float *outMinX, float *outMaxX,
                         int *outTris, float *outZCentre )
    {
        std::vector<std::vector<bwComp_t> > comps( lod->surfs.size() );
        float longest = 0.0f;
        for ( size_t s = 0; s < lod->surfs.size(); ++s )
        {
            Components( lod->surfs[s], &comps[s] );
            for ( size_t c = 0; c < comps[s].size(); ++c )
                if ( comps[s][c].maxX - comps[s][c].minX > longest )
                    longest = comps[s][c].maxX - comps[s][c].minX;
        }
        // strand cut, then the height clusters of what survived
        std::vector<float> centres;
        for ( size_t s = 0; s < lod->surfs.size(); ++s )
            for ( size_t c = 0; c < comps[s].size(); ++c )
            {
                bwComp_t &comp = comps[s][c];
                if ( comp.maxX - comp.minX < KBW_STRAND_FRAC * longest )
                    comp.tris.clear();               // not a strand
                else
                    centres.push_back( comp.ZCentre() );
            }
        std::vector<float> clusters;                 // descending wire centres
        {
            std::vector<float> sorted = centres;
            for ( size_t i = 0; i < sorted.size(); ++i )   // descending, small n
                for ( size_t j = i + 1; j < sorted.size(); ++j )
                    if ( sorted[j] > sorted[i] ) { const float t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
            for ( size_t i = 0; i < sorted.size(); ++i )
                if ( clusters.empty() || clusters.back() - sorted[i] > KBW_WIRE_GAP )
                    clusters.push_back( sorted[i] );
        }
        // the cluster a strand belongs to = the nearest cluster centre from above
        float gMin = FLT_MAX, gMax = -FLT_MAX, topCentre = 0.0f, topSum = 0.0f;
        int   kept = 0, topN = 0;
        for ( size_t s = 0; s < lod->surfs.size(); ++s )
        {
            std::vector<int> keep;
            for ( size_t c = 0; c < comps[s].size(); ++c )
            {
                const bwComp_t &comp = comps[s][c];
                if ( comp.tris.empty() )
                    continue;
                int cluster = 0;
                while ( cluster + 1 < (int)clusters.size()
                     && clusters[(size_t)cluster] - comp.ZCentre() > KBW_WIRE_GAP )
                    ++cluster;
                if ( wires > 0 && cluster >= wires )
                    continue;
                if ( cluster == 0 ) { topSum += comp.ZCentre(); ++topN; }
                keep.insert( keep.end(), comp.tris.begin(), comp.tris.end() );
                if ( comp.minX < gMin ) gMin = comp.minX;
                if ( comp.maxX > gMax ) gMax = comp.maxX;
                kept += (int)comp.tris.size();
            }
            KeepTris( &lod->surfs[s], keep );
        }
        if ( topN )
            topCentre = topSum / (float)topN;
        // drop surfaces the extraction emptied (the loader refuses a 0-triangle surface)
        for ( size_t s = 0; s < lod->surfs.size(); )
        {
            if ( lod->surfs[s].tris.empty() ) lod->surfs.erase( lod->surfs.begin() + (ptrdiff_t)s );
            else ++s;
        }
        *outMinX = gMin;
        *outMaxX = gMax;
        *outTris = kept;
        *outZCentre = topCentre;
    }

    void ShiftZ( bwLod_t *lod, float dz )
    {
        for ( size_t s = 0; s < lod->surfs.size(); ++s )
            for ( size_t v = 0; v < lod->surfs[s].verts.size(); ++v )
                lod->surfs[s].verts[v].p[2] += dz;
    }

    void SnapEnds( bwLod_t *lod, float minX, float maxX )
    {
        for ( size_t s = 0; s < lod->surfs.size(); ++s )
            for ( size_t v = 0; v < lod->surfs[s].verts.size(); ++v )
            {
                float &x = lod->surfs[s].verts[v].p[0];
                if ( x < minX + KBW_END_SNAP )      x = minX;
                else if ( x > maxX - KBW_END_SNAP ) x = maxX;
            }
    }

    // Read the stock model and reduce it to its strand tile (cached per source + wires).
    // With `wires` > 0 the top kept wire is re-based onto z = 0, so the curve IS that
    // wire; 0 keeps every strand at its post height (the curve is the ground line).
    bool LoadTile( int source, int wires, bwTile_t *out, char *err, size_t errSz )
    {
        static std::map<int, bwTile_t> s_cache;
        const int key = source * 64 + ( wires < 0 ? 0 : ( wires > 63 ? 63 : wires ) );
        std::map<int, bwTile_t>::iterator hit = s_cache.find( key );
        if ( hit != s_cache.end() )
        {
            *out = hit->second;
            return true;
        }
        if ( source < 0 || source >= KBW_SOURCE_COUNT )
        {
            SetErr( err, errSz, "no such barbwire source" );
            return false;
        }
        const char *model = KBW_SOURCES[source].model;
        std::vector<unsigned char> bytes;
        char qpath[128];
        _snprintf( qpath, sizeof( qpath ), "xmodel/%s", model );
        qpath[sizeof( qpath ) - 1] = '\0';
        if ( !ReadRaw( qpath, &bytes ) )
        {
            SetErr( err, errSz, "'%s' is not on the search path (raw/ or the IWDs)", qpath );
            return false;
        }
        bwHeader_t h;
        if ( !ParseHeader( bytes, &h, err, errSz ) )
            return false;

        bwTile_t t;
        t.model = model;
        for ( int l = 0; l < 2; ++l )
        {
            if ( h.lodFile[l].empty() )
                break;
            _snprintf( qpath, sizeof( qpath ), "xmodelsurfs/%s", h.lodFile[l].c_str() );
            qpath[sizeof( qpath ) - 1] = '\0';
            if ( !ReadRaw( qpath, &bytes ) )
            {
                SetErr( err, errSz, "'%s' is not on the search path", qpath );
                return false;
            }
            if ( !ParseSurfs( bytes, h.lodMaterials[l], &t.lod[l], err, errSz ) )
                return false;
            float mn = 0.0f, mx = 0.0f, zc = 0.0f;
            int tris = 0;
            ExtractStrands( &t.lod[l], wires, &mn, &mx, &tris, &zc );
            if ( l == 0 )
            {
                if ( tris <= 0 || !( mx - mn > 1.0f ) )
                {
                    SetErr( err, errSz, "'%s' has no strand geometry to lift", model );
                    return false;
                }
                t.minX = mn;
                t.maxX = mx;
                t.strandTris = tris;
                t.lodCount = 1;
                t.zShift = ( wires > 0 ) ? -zc : 0.0f;
            }
            else if ( tris > 0 )
                t.lodCount = 2;
            else
                break;
        }
        for ( int l = 0; l < t.lodCount; ++l )
        {
            SnapEnds( &t.lod[l], t.minX, t.maxX );
            ShiftZ( &t.lod[l], t.zShift );           // LOD1 follows LOD0's re-base exactly
        }
        s_cache[key] = t;
        *out = t;
        return true;
    }

    // ── the curve ────────────────────────────────────────────────────────────────
    struct bwCurve_t
    {
        std::vector<float> pts;      // xyz, duplicates removed, closed curves repeat P0
        std::vector<float> cum;      // arc length at each point
        std::vector<float> tan;      // unit tangent at each point (averaged at corners)
        float              length = 0.0f;
    };

    bool BuildCurve( const kconObject_t &o, bwCurve_t *c )
    {
        const int n = KiwiCon_VertCount( o );
        if ( n < 2 )
            return false;
        const int total = o.closed ? n + 1 : n;
        for ( int i = 0; i < total; ++i )
        {
            float p[3];
            if ( !KiwiCon_VertWorld( o, i % n, p ) )
                return false;
            if ( !c->pts.empty() )
            {
                const float *q = &c->pts[c->pts.size() - 3];
                const float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
                if ( Len3( d ) < 0.01f )
                    continue;                 // a repeated point makes no segment
            }
            c->pts.push_back( p[0] ); c->pts.push_back( p[1] ); c->pts.push_back( p[2] );
        }
        const int m = (int)c->pts.size() / 3;
        if ( m < 2 )
            return false;
        c->cum.resize( (size_t)m );
        c->tan.resize( (size_t)m * 3 );
        std::vector<float> seg( (size_t)( m - 1 ) * 3 );
        c->cum[0] = 0.0f;
        for ( int i = 0; i + 1 < m; ++i )
        {
            float d[3];
            Sub3( &c->pts[(size_t)( i + 1 ) * 3], &c->pts[(size_t)i * 3], d );
            const float len = Len3( d );
            seg[(size_t)i * 3 + 0] = d[0] / len;
            seg[(size_t)i * 3 + 1] = d[1] / len;
            seg[(size_t)i * 3 + 2] = d[2] / len;
            c->cum[(size_t)i + 1] = c->cum[(size_t)i] + len;
        }
        c->length = c->cum[(size_t)m - 1];
        for ( int i = 0; i < m; ++i )
        {
            float t[3] = { 0.0f, 0.0f, 0.0f };
            if ( i > 0 )     { t[0] += seg[(size_t)( i - 1 ) * 3]; t[1] += seg[(size_t)( i - 1 ) * 3 + 1]; t[2] += seg[(size_t)( i - 1 ) * 3 + 2]; }
            if ( i + 1 < m ) { t[0] += seg[(size_t)i * 3];         t[1] += seg[(size_t)i * 3 + 1];         t[2] += seg[(size_t)i * 3 + 2]; }
            float len = Len3( t );
            if ( !( len > 1e-4f ) )       // a hairpin: fall back to the outgoing/incoming segment
            {
                const float *s = ( i + 1 < m ) ? &seg[(size_t)i * 3] : &seg[(size_t)( i - 1 ) * 3];
                t[0] = s[0]; t[1] = s[1]; t[2] = s[2];
                len = 1.0f;
            }
            c->tan[(size_t)i * 3 + 0] = t[0] / len;
            c->tan[(size_t)i * 3 + 1] = t[1] / len;
            c->tan[(size_t)i * 3 + 2] = t[2] / len;
        }
        return c->length > KBW_MIN_CURVE_LEN;
    }

    // Point and the (along, side, up) frame at arc length s.  `up` follows world Z
    // projected off the tangent so the strands hang vertically however the curve bends.
    void FrameAt( const bwCurve_t &c, float s, float outP[3], float T[3], float B[3], float N[3] )
    {
        const int m = (int)c.cum.size();
        int j = 0;
        while ( j + 2 < m && s > c.cum[(size_t)j + 1] )
            ++j;
        const float segLen = c.cum[(size_t)j + 1] - c.cum[(size_t)j];
        float t = segLen > 0.0f ? ( s - c.cum[(size_t)j] ) / segLen : 0.0f;
        if ( t < 0.0f ) t = 0.0f;
        if ( t > 1.0f ) t = 1.0f;
        const float *p0 = &c.pts[(size_t)j * 3], *p1 = &c.pts[(size_t)( j + 1 ) * 3];
        const float *t0 = &c.tan[(size_t)j * 3], *t1 = &c.tan[(size_t)( j + 1 ) * 3];
        for ( int k = 0; k < 3; ++k )
        {
            outP[k] = p0[k] + ( p1[k] - p0[k] ) * t;
            T[k]    = t0[k] + ( t1[k] - t0[k] ) * t;
        }
        float len = Len3( T );
        if ( !( len > 1e-4f ) ) { T[0] = p1[0] - p0[0]; T[1] = p1[1] - p0[1]; T[2] = p1[2] - p0[2]; len = Len3( T ); }
        if ( !( len > 1e-6f ) ) { T[0] = 1.0f; T[1] = 0.0f; T[2] = 0.0f; len = 1.0f; }
        T[0] /= len; T[1] /= len; T[2] /= len;

        // up = Z off the tangent; a vertical run falls back to Y so the frame stays defined
        N[0] = -T[2] * T[0];
        N[1] = -T[2] * T[1];
        N[2] = 1.0f - T[2] * T[2];
        len = Len3( N );
        if ( !( len > 1e-3f ) )
        {
            N[0] = -T[1] * T[0];
            N[1] = 1.0f - T[1] * T[1];
            N[2] = -T[1] * T[2];
            len = Len3( N );
        }
        N[0] /= len; N[1] /= len; N[2] /= len;
        Cross3( N, T, B );                     // T x B == N: x along, y side, z up
    }

    // ── sweep + write ────────────────────────────────────────────────────────────
    unsigned Fnv1a( unsigned h, const void *data, size_t n )
    {
        const unsigned char *p = (const unsigned char *)data;
        for ( size_t i = 0; i < n; ++i )
        {
            h ^= p[i];
            h *= 16777619u;
        }
        return h;
    }

    struct bwPiece_t
    {
        std::string name;               // kiwi_bw_xxxxxxxx
        float       origin[3];
        bwLod_t     lod[2];
        int         lodCount = 0;
        float       mins[3], maxs[3];
        int         tris = 0;
    };

    // The meander: three sines of random wavelength (60..220 units) and phase, summed
    // with decreasing weight, one set sideways and a weaker one up.  A function of arc
    // length only, so tile joins stay continuous.  KIWI (2026-09-16, user: "I want the
    // strands randomized in the side direction so that the wavyness isn't perfectly
    // articulating").
    struct bwMeander_t
    {
        float freq[3], phase[3];
        float upFreq[3], upPhase[3];
        float amp = 0.0f;
    };

    unsigned Lcg( unsigned *state )
    {
        *state = *state * 1664525u + 1013904223u;
        return *state >> 8;
    }
    float Rand01( unsigned *state ) { return (float)( Lcg( state ) & 0xFFFF ) / 65535.0f; }

    void SeedMeander( unsigned seed, float amp, bwMeander_t *m )
    {
        unsigned st = seed * 2654435761u + 12345u;
        for ( int i = 0; i < 3; ++i )
        {
            const float wl   = 60.0f + Rand01( &st ) * 160.0f;
            m->freq[i]       = KCON_TWO_PI / wl;
            m->phase[i]      = Rand01( &st ) * KCON_TWO_PI;
            const float wlUp = 60.0f + Rand01( &st ) * 160.0f;
            m->upFreq[i]     = KCON_TWO_PI / wlUp;
            m->upPhase[i]    = Rand01( &st ) * KCON_TWO_PI;
        }
        m->amp = amp;
    }

    void MeanderAt( const bwMeander_t &m, float s, float *side, float *up )
    {
        static const float W[3] = { 0.55f, 0.30f, 0.15f };
        float a = 0.0f, b = 0.0f;
        for ( int i = 0; i < 3; ++i )
        {
            a += W[i] * sinf( s * m.freq[i]   + m.phase[i] );
            b += W[i] * sinf( s * m.upFreq[i] + m.upPhase[i] );
        }
        *side = m.amp * a;
        *up   = m.amp * 0.4f * b;
    }

    // One piece = tiles k0 .. k1-1 of the curve, geometry relative to P(k0 * tileLen).
    bool SweepPiece( const bwTile_t &tile, const bwCurve_t &curve, const kiwiBarbwireOpts_t &opts,
                     const bwMeander_t &meander, int k0, int k1, float tileLen, unsigned hashSeed,
                     bwPiece_t *out, char *err, size_t errSz )
    {
        const float L = tile.maxX - tile.minX;
        const float stretch = tileLen / L;
        float T[3], B[3], N[3];
        FrameAt( curve, (float)k0 * tileLen, out->origin, T, B, N );
        out->lodCount = tile.lodCount;
        out->mins[0] = out->mins[1] = out->mins[2] = FLT_MAX;
        out->maxs[0] = out->maxs[1] = out->maxs[2] = -FLT_MAX;

        for ( int l = 0; l < tile.lodCount; ++l )
        {
            const bwLod_t &src = tile.lod[l];
            bwLod_t &dst = out->lod[l];
            dst.surfs.resize( src.surfs.size() );
            for ( size_t s = 0; s < src.surfs.size(); ++s )
            {
                const bwSurf_t &ss = src.surfs[s];
                bwSurf_t &ds = dst.surfs[s];
                ds.material = ss.material;
                ds.tileMode = ss.tileMode;
                ds.verts.reserve( ss.verts.size() * (size_t)( k1 - k0 ) );
                ds.tris.reserve( ss.tris.size() * (size_t)( k1 - k0 ) );
                for ( int k = k0; k < k1; ++k )
                {
                    const bool mirror = opts.mirrorTiles && ( k & 1 );
                    const size_t base = ds.verts.size();
                    if ( base + ss.verts.size() > 65535 )
                    {
                        SetErr( err, errSz, "a piece would exceed 65535 vertices; shorten the piece length" );
                        return false;
                    }
                    for ( size_t v = 0; v < ss.verts.size(); ++v )
                    {
                        const bwVert_t &sv = ss.verts[v];
                        const float xl  = mirror ? ( tile.maxX - sv.p[0] ) : ( sv.p[0] - tile.minX );
                        const float arc = (float)k * tileLen + xl * stretch;
                        float P[3];
                        FrameAt( curve, arc, P, T, B, N );
                        float side = 0.0f, up = 0.0f;
                        if ( meander.amp > 0.0f )
                            MeanderAt( meander, arc, &side, &up );
                        const float y = sv.p[1] + side;
                        const float z = sv.p[2] + opts.zOffset + up;
                        bwVert_t o = sv;
                        for ( int a = 0; a < 3; ++a )
                        {
                            o.p[a] = P[a] + B[a] * y + N[a] * z - out->origin[a];
                            const float nx = mirror ? -sv.n[0]   : sv.n[0];
                            const float tx = mirror ? -sv.tan[0] : sv.tan[0];
                            const float bx = mirror ? -sv.bin[0] : sv.bin[0];
                            o.n[a]   = T[a] * nx + B[a] * sv.n[1]   + N[a] * sv.n[2];
                            o.tan[a] = T[a] * tx + B[a] * sv.tan[1] + N[a] * sv.tan[2];
                            o.bin[a] = T[a] * bx + B[a] * sv.bin[1] + N[a] * sv.bin[2];
                        }
                        if ( l == 0 )
                        {
                            for ( int a = 0; a < 3; ++a )
                            {
                                if ( o.p[a] < out->mins[a] ) out->mins[a] = o.p[a];
                                if ( o.p[a] > out->maxs[a] ) out->maxs[a] = o.p[a];
                            }
                        }
                        ds.verts.push_back( o );
                    }
                    for ( size_t t = 0; t + 2 < ss.tris.size(); t += 3 )
                    {
                        const unsigned short a = (unsigned short)( base + ss.tris[t] );
                        const unsigned short b = (unsigned short)( base + ss.tris[t + 1] );
                        const unsigned short c = (unsigned short)( base + ss.tris[t + 2] );
                        // a mirrored tile flips handedness: restore the winding
                        ds.tris.push_back( a );
                        ds.tris.push_back( mirror ? c : b );
                        ds.tris.push_back( mirror ? b : c );
                        if ( l == 0 )
                            ++out->tris;
                    }
                }
            }
        }

        // name: deterministic in the inputs so a re-run of the same curve reuses its names
        unsigned h = 2166136261u;
        h = Fnv1a( h, &hashSeed, sizeof( hashSeed ) );
        h = Fnv1a( h, tile.model.c_str(), tile.model.size() );
        h = Fnv1a( h, &curve.pts[0], curve.pts.size() * sizeof( float ) );
        h = Fnv1a( h, &opts.zOffset, sizeof( opts.zOffset ) );
        h = Fnv1a( h, &opts.pieceLen, sizeof( opts.pieceLen ) );
        h = Fnv1a( h, &opts.mirrorTiles, sizeof( opts.mirrorTiles ) );
        h = Fnv1a( h, &opts.strands, sizeof( opts.strands ) );
        h = Fnv1a( h, &opts.wobble, sizeof( opts.wobble ) );
        h = Fnv1a( h, &opts.seed, sizeof( opts.seed ) );
        h = Fnv1a( h, &k0, sizeof( k0 ) );
        char name[64];
        _snprintf( name, sizeof( name ), "%s%08x", KBW_PREFIX, h );
        name[sizeof( name ) - 1] = '\0';
        out->name = name;
        return true;
    }

    bool WritePiece( const bwPiece_t &p, char *err, size_t errSz )
    {
        // radius cube for the header, real bounds for the bone
        float r = 0.0f;
        for ( int a = 0; a < 3; ++a )
        {
            r = ( fabsf( p.mins[a] ) > r ) ? fabsf( p.mins[a] ) : r;
            r = ( fabsf( p.maxs[a] ) > r ) ? fabsf( p.maxs[a] ) : r;
        }
        r *= 1.7320508f;                            // corner of the box is the far point
        const std::string lod0 = p.name + "_lod0";
        const std::string lod1 = p.name + "_lod1";

        // ── xmodel (XModel_ReadHeader order, then the LOD material pass, then bone bounds)
        {
            bwWriter_t w;
            w.U16( 25 );
            w.U8( 0 );                              // flags
            const float mn[3] = { -r, -r, -r }, mx[3] = { r, r, r };
            w.V3( mn ); w.V3( mx );
            w.CStr( "" );                           // physics preset
            for ( int l = 0; l < 4; ++l )
            {
                if ( l < p.lodCount )
                {
                    w.F32( ( l + 1 < p.lodCount ) ? KBW_LOD0_DIST : 0.0f );
                    w.CStr( l == 0 ? lod0.c_str() : lod1.c_str() );
                }
                else
                {
                    w.F32( 0.0f );
                    w.CStr( "" );
                }
            }
            w.S32( -1 );                            // collLod: no collision
            w.S32( 0 );                             // numCollSurfs
            for ( int l = 0; l < p.lodCount; ++l )
            {
                w.U16( (unsigned)p.lod[l].surfs.size() );
                for ( size_t s = 0; s < p.lod[l].surfs.size(); ++s )
                    w.CStr( p.lod[l].surfs[s].material.c_str() );
            }
            w.V3( p.mins ); w.V3( p.maxs );         // per-bone bounds (one bone)
            if ( !WriteRaw( ( "xmodel/" + p.name ).c_str(), w.b, err, errSz ) )
                return false;
        }
        // ── xmodelparts (named after LOD0's surfs file, as the loader expects)
        {
            bwWriter_t w;
            w.U16( 25 );
            w.U16( 0 );                             // child bones
            w.U16( 1 );                             // root bones
            w.CStr( KBW_BONE );
            w.U8( 0 );                              // partClassification
            w.U8( 1 );                              // useBones
            if ( !WriteRaw( ( "xmodelparts/" + lod0 ).c_str(), w.b, err, errSz ) )
                return false;
        }
        // ── xmodelsurfs per LOD (R_XSurfaceLoadObj's rigid single-list layout)
        for ( int l = 0; l < p.lodCount; ++l )
        {
            bwWriter_t w;
            w.U16( 25 );
            w.U16( (unsigned)p.lod[l].surfs.size() );
            for ( size_t s = 0; s < p.lod[l].surfs.size(); ++s )
            {
                const bwSurf_t &surf = p.lod[l].surfs[s];
                w.U8( surf.tileMode );
                w.U16( 0 );                         // the opaque value nobody retains
                w.U16( (unsigned)surf.verts.size() );
                w.U16( (unsigned)( surf.tris.size() / 3 ) );
                w.U16( (unsigned)surf.verts.size() ); w.U16( 0 );   // one rigid list, bone 0
                w.U16( 0 );                         // terminator
                for ( size_t v = 0; v < surf.verts.size(); ++v )
                {
                    const bwVert_t &o = surf.verts[v];
                    w.V3( o.n );
                    w.U8( o.c[0] ); w.U8( o.c[1] ); w.U8( o.c[2] ); w.U8( o.c[3] );
                    w.F32( o.uv[0] ); w.F32( o.uv[1] );
                    w.V3( o.bin );
                    w.V3( o.tan );
                    w.V3( o.p );
                }
                for ( size_t i = 0; i < surf.tris.size(); ++i )
                    w.U16( surf.tris[i] );
            }
            if ( !WriteRaw( ( "xmodelsurfs/" + ( l == 0 ? lod0 : lod1 ) ).c_str(), w.b, err, errSz ) )
                return false;
        }
        return true;
    }

    void DeleteModelFiles( const std::string &name )
    {
        if ( name.compare( 0, strlen( KBW_PREFIX ), KBW_PREFIX ) != 0 )
            return;                                 // never touch anything we did not write
        const std::string paths[4] = { "xmodel/" + name, "xmodelparts/" + name + "_lod0",
                                       "xmodelsurfs/" + name + "_lod0", "xmodelsurfs/" + name + "_lod1" };
        for ( int i = 0; i < 4; ++i )
            FS_DeleteInDir( (char *)paths[i].c_str(), (char *)"raw" );
    }

    // ── entities ─────────────────────────────────────────────────────────────────
    const char *Key( const entity_s_def *def, const char *key )
    {
        for ( const epair_t *ep = def ? def->epairs : nullptr; ep; ep = ep->next )
            if ( ep->key && !_stricmp( ep->key, key ) )
                return ep->value ? ep->value : "";
        return nullptr;
    }

    bool ModelInUse( const std::string &model )
    {
        for ( const entity_s *e = entities.next; e && e != &entities; e = e->next )
        {
            const char *m = Key( (const entity_s_def *)e, "model" );
            if ( m && !_stricmp( m, model.c_str() ) )
                return true;
        }
        return false;
    }

    // The placeholder + CreateEntityFromName sequence the Models browser lands drops with
    // (kiwi_modelbrowser.cpp DropPlaceholder / PerformDrop), minus its undo bracket - the
    // caller holds ONE record for the whole run.
    entity_s_def *CreateModelEntity( eclass_t *miscModel, const char *modelName, const float origin[3] )
    {
        Select_Deselect( 1 );
        float mins[3], maxs[3];
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = origin[i] + miscModel->mins[i];
            maxs[i] = origin[i] + miscModel->maxs[i];
            if ( maxs[i] - mins[i] < 1.0f )
                maxs[i] = mins[i] + 1.0f;
        }
        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
        if ( !def )
            return nullptr;
        Brush_Create( mins, maxs, def, nullptr );
        Brush_BuildWindings( def, 1 );
        KiwiExtrude_LandDef( def );
        // Born inside the record: stamp it so CreateEntityFromName's Undo_AddBrushList
        // does not clone it (the clone came back as a stray box on undo).
        Undo_KiwiMarkCreated( def );
        CreateEntityFromName( "misc_model" );

        entity_s_def *created = nullptr;
        selbrush_t *selected = selected_brushes.next;
        if ( selected && selected != &selected_brushes && selected->next == &selected_brushes
          && selected->owner && selected->owner != world_entity )
        {
            entity_s_def *candidate = (entity_s_def *)selected->owner->def;
            if ( candidate && candidate->eclass == miscModel )
                created = candidate;
        }
        if ( created )
            SetKeyValue( created, "model", modelName );   // Checkkey_Model + EntityAssignModel
        return created;
    }

    // Select every instance node of the def entities that name `curveName`; returns
    // their model names for the file cleanup.
    int SelectPreviousRun( const char *curveName, std::vector<std::string> *models )
    {
        Select_Deselect( 1 );
        int n = 0;
        selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
        std::vector<selbrush_t *> nodes;
        for ( int li = 0; li < 2; ++li )
            for ( selbrush_t *b = lists[li]->next; b && b != lists[li]; b = b->next )
            {
                if ( !b->owner || !b->owner->def )
                    continue;
                const entity_s_def *def = (const entity_s_def *)b->owner->def;
                const char *src = Key( def, KBW_KEY );
                if ( !src || strcmp( src, curveName ) )
                    continue;
                nodes.push_back( b );
                const char *m = Key( def, "model" );
                if ( m && m[0] )
                    models->push_back( m );
            }
        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            Select_Brush( nodes[i], 0, 0, 0 );
            ++n;
        }
        return n;
    }

    std::string UniqueCurveName()
    {
        for ( int k = 1; ; ++k )
        {
            char buf[32];
            _snprintf( buf, sizeof( buf ), "barbwire_%d", k );
            buf[sizeof( buf ) - 1] = '\0';
            bool taken = false;
            for ( int i = 0; i < KiwiCon_Count() && !taken; ++i )
                taken = !strcmp( KiwiCon_Name( i ), buf );
            if ( !taken )
                return buf;
        }
    }

    // ── options persistence ──────────────────────────────────────────────────────
    kiwiBarbwireOpts_t s_opts;
    bool s_optsLoaded = false;
    bool s_dialogRequest = false;
    bool s_dialogOpen = false;
    const char *KBW_DIALOG_TITLE = "Barbwire along construction";

    void LoadOpts()
    {
        if ( s_optsLoaded )
            return;
        s_optsLoaded = true;
        kiwiBarbwireOpts_t d;
        s_opts.source      = Radiant_ProfileGetInt( KBW_PROFILE, "Source", d.source );
        s_opts.zOffset     = (float)atof( Radiant_ProfileGetString( KBW_PROFILE, "ZOffset", "0" ).c_str() );
        s_opts.pieceLen    = (float)Radiant_ProfileGetInt( KBW_PROFILE, "SplitAbove", (int)d.pieceLen );
        s_opts.wobble      = (float)atof( Radiant_ProfileGetString( KBW_PROFILE, "Wobble", "2" ).c_str() );
        s_opts.seed        = Radiant_ProfileGetInt( KBW_PROFILE, "Seed", d.seed );
        s_opts.mirrorTiles = Radiant_ProfileGetInt( KBW_PROFILE, "MirrorTiles", d.mirrorTiles ? 1 : 0 ) != 0;
        s_opts.replace     = Radiant_ProfileGetInt( KBW_PROFILE, "Replace", d.replace ? 1 : 0 ) != 0;
        s_opts.strands     = Radiant_ProfileGetInt( KBW_PROFILE, "Strands", d.strands );
        if ( s_opts.strands < 0 || s_opts.strands > 16 ) s_opts.strands = d.strands;
        if ( s_opts.source < 0 || s_opts.source >= KBW_SOURCE_COUNT ) s_opts.source = 0;
        if ( s_opts.pieceLen < 0.0f ) s_opts.pieceLen = 0.0f;
        if ( s_opts.pieceLen > 0.0f && s_opts.pieceLen < KBW_PIECE_MIN ) s_opts.pieceLen = KBW_PIECE_MIN;
        if ( s_opts.pieceLen > KBW_PIECE_MAX ) s_opts.pieceLen = KBW_PIECE_MAX;
        if ( !( s_opts.wobble >= 0.0f ) || s_opts.wobble > 64.0f ) s_opts.wobble = d.wobble;
    }

    int SelectedObjects( std::vector<int> *out )
    {
        out->clear();
        for ( int i = 0; i < KiwiConSel_Count(); ++i )
        {
            const kconSelItem_t *it = KiwiConSel_At( i );
            if ( !it || it->object < 0 || it->object >= KiwiCon_Count() )
                continue;
            bool dup = false;
            for ( size_t k = 0; k < out->size() && !dup; ++k )
                dup = ( ( *out )[k] == it->object );
            if ( !dup )
                out->push_back( it->object );
        }
        return (int)out->size();
    }
}

// ── public ───────────────────────────────────────────────────────────────────────
int         KiwiBarbwire_SourceCount()          { return KBW_SOURCE_COUNT; }
const char *KiwiBarbwire_SourceName( int i )    { return ( i >= 0 && i < KBW_SOURCE_COUNT ) ? KBW_SOURCES[i].model : ""; }
const char *KiwiBarbwire_SourceLabel( int i )   { return ( i >= 0 && i < KBW_SOURCE_COUNT ) ? KBW_SOURCES[i].label : ""; }

void KiwiBarbwire_GetOpts( kiwiBarbwireOpts_t *out )
{
    LoadOpts();
    if ( out )
        *out = s_opts;
}

void KiwiBarbwire_SetOpts( const kiwiBarbwireOpts_t &o )
{
    LoadOpts();
    s_opts = o;
    char z[32];
    Radiant_ProfileSetInt( KBW_PROFILE, "Source", o.source );
    Radiant_ProfileSetString( KBW_PROFILE, "ZOffset", KiwiFmt_Num( z, sizeof( z ), o.zOffset ) );
    Radiant_ProfileSetInt( KBW_PROFILE, "SplitAbove", (int)o.pieceLen );
    Radiant_ProfileSetString( KBW_PROFILE, "Wobble", KiwiFmt_Num( z, sizeof( z ), o.wobble ) );
    Radiant_ProfileSetInt( KBW_PROFILE, "Seed", o.seed );
    Radiant_ProfileSetInt( KBW_PROFILE, "MirrorTiles", o.mirrorTiles ? 1 : 0 );
    Radiant_ProfileSetInt( KBW_PROFILE, "Replace", o.replace ? 1 : 0 );
    Radiant_ProfileSetInt( KBW_PROFILE, "Strands", o.strands );
}

bool KiwiBarbwire_CanExecute()
{
    std::vector<int> objs;
    return SelectedObjects( &objs ) > 0;
}

bool KiwiBarbwire_LayNow( const kiwiBarbwireOpts_t &optsIn, char *err, size_t errSz )
{
    if ( err && errSz ) err[0] = '\0';
    kiwiBarbwireOpts_t opts = optsIn;
    if ( opts.pieceLen < 0.0f ) opts.pieceLen = 0.0f;
    if ( opts.pieceLen > 0.0f && opts.pieceLen < KBW_PIECE_MIN ) opts.pieceLen = KBW_PIECE_MIN;
    if ( opts.pieceLen > KBW_PIECE_MAX ) opts.pieceLen = KBW_PIECE_MAX;
    if ( opts.strands < 0 || opts.strands > 16 ) opts.strands = 1;
    if ( !( opts.wobble >= 0.0f ) ) opts.wobble = 0.0f;
    if ( opts.wobble > 64.0f ) opts.wobble = 64.0f;

    std::vector<int> objs;
    if ( SelectedObjects( &objs ) == 0 )
    {
        SetErr( err, errSz, "select a construction line, polyline, spline, arc or circle first" );
        return false;
    }
    bwTile_t tile;
    if ( !LoadTile( opts.source, opts.strands, &tile, err, errSz ) )
        return false;
    eclass_t *miscModel = Eclass_ForName( 0, "misc_model" );
    if ( !miscModel || *(int *)&miscModel->fixedsize == 0 )
    {
        SetErr( err, errSz, "misc_model is not available in this entity definition set" );
        return false;
    }

    // Plan every curve before touching the map or the disk.
    struct run_t
    {
        int                    object;
        std::string            name;
        bool                   needsName;
        bwCurve_t              curve;
        std::vector<bwPiece_t> pieces;
    };
    std::vector<run_t> runs;
    for ( size_t i = 0; i < objs.size(); ++i )
    {
        const kconObject_t *o = KiwiCon_At( objs[i] );
        if ( !o || o->hidden )
            continue;
        run_t run;
        run.object = objs[i];
        if ( !BuildCurve( *o, &run.curve ) )
        {
            Sys_Printf( "Barbwire: construction #%d is too short (under %g units); skipped.\n",
                        objs[i], KBW_MIN_CURVE_LEN );
            continue;
        }
        run.name = KiwiCon_Name( objs[i] );
        run.needsName = run.name.empty();

        const float L = tile.maxX - tile.minX;
        int tiles = (int)floorf( run.curve.length / L + 0.5f );
        if ( tiles < 1 ) tiles = 1;
        if ( run.curve.length / ( (float)tiles * L ) > KBW_MAX_STRETCH )
            ++tiles;
        // A closed loop with mirrored tiles needs an even count so the closing join is
        // a strand meeting itself too.
        if ( o->closed && opts.mirrorTiles && ( tiles & 1 ) )
            ++tiles;
        const float tileLen = run.curve.length / (float)tiles;
        int perPiece = tiles;                     // 0 = the whole curve is one xmodel
        if ( opts.pieceLen > 0.0f )
        {
            perPiece = (int)floorf( opts.pieceLen / tileLen );
            if ( perPiece < 1 ) perPiece = 1;
        }
        // The meander is seeded from the curve (its points) and the user's seed, so a
        // re-run reproduces it and two parallel lines of an array meander differently.
        bwMeander_t meander;
        {
            unsigned hs = Fnv1a( 2166136261u, &run.curve.pts[0], run.curve.pts.size() * sizeof( float ) );
            hs = Fnv1a( hs, &opts.seed, sizeof( opts.seed ) );
            SeedMeander( hs, opts.wobble, &meander );
        }
        for ( int k0 = 0; k0 < tiles; k0 += perPiece )
        {
            const int k1 = ( k0 + perPiece < tiles ) ? k0 + perPiece : tiles;
            bwPiece_t piece;
            if ( !SweepPiece( tile, run.curve, opts, meander, k0, k1, tileLen, (unsigned)objs[i],
                              &piece, err, errSz ) )
                return false;
            run.pieces.push_back( piece );
        }
        runs.push_back( run );
    }
    if ( runs.empty() )
    {
        SetErr( err, errSz, "none of the selected construction objects can carry barbwire" );
        return false;
    }

    // Names first (their own construction undo ticket; the sidecar carries them).
    for ( size_t r = 0; r < runs.size(); ++r )
        if ( runs[r].needsName )
        {
            KiwiCon_UndoPush();
            runs[r].name = UniqueCurveName();
            KiwiCon_SetName( runs[r].object, runs[r].name.c_str() );
        }

    // Files before entities: a write failure leaves the map untouched.
    for ( size_t r = 0; r < runs.size(); ++r )
        for ( size_t p = 0; p < runs[r].pieces.size(); ++p )
            if ( !WritePiece( runs[r].pieces[p], err, errSz ) )
                return false;

    // One classic record: the previous run's pieces out, the new ones in.
    Undo_ClearRedo();
    Undo_GeneralStart( "lay barbwire" );
    std::vector<std::string> staleModels;
    int removed = 0;
    if ( opts.replace )
    {
        for ( size_t r = 0; r < runs.size(); ++r )
        {
            if ( SelectPreviousRun( runs[r].name.c_str(), &staleModels ) == 0 )
                continue;
            Undo_AddBrushList( &selected_brushes );
            for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
                Undo_AddEntity_W( (entity_s *)i->owner->def );
            for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
                ++removed;
            Select_Delete();
            Undo_EndBrushList( &selected_brushes );
        }
    }
    std::vector<selbrush_t *> made;
    int created = 0, tris = 0;
    for ( size_t r = 0; r < runs.size(); ++r )
        for ( size_t p = 0; p < runs[r].pieces.size(); ++p )
        {
            const bwPiece_t &piece = runs[r].pieces[p];
            entity_s_def *def = CreateModelEntity( miscModel, piece.name.c_str(), piece.origin );
            if ( !def )
            {
                Sys_Printf( "Barbwire: misc_model creation failed for '%s'.\n", piece.name.c_str() );
                continue;
            }
            SetKeyValue( def, KBW_KEY, runs[r].name.c_str() );
            char idx[16];
            _snprintf( idx, sizeof( idx ), "%d", (int)p );
            idx[sizeof( idx ) - 1] = '\0';
            SetKeyValue( def, "kiwi_barbwire_piece", idx );
            if ( selected_brushes.next != &selected_brushes )
                made.push_back( selected_brushes.next );
            ++created;
            tris += piece.tris;
        }
    Undo_End();

    // Files of replaced pieces nobody references any more.
    for ( size_t i = 0; i < staleModels.size(); ++i )
        if ( !ModelInUse( staleModels[i] ) )
            DeleteModelFiles( staleModels[i] );

    Select_Deselect( 1 );
    for ( size_t i = 0; i < made.size(); ++i )
        Select_Brush( made[i], 0, 0, 0 );
    Sel_InvalidateFromLegacy();
    MarkMapModified();
    g_nUpdateBits = -1;

    float totalLen = 0.0f;
    for ( size_t r = 0; r < runs.size(); ++r )
        totalLen += runs[r].curve.length;
    char lenBuf[32];
    Sys_Printf( "Barbwire: %d piece(s) along %d curve(s), %s units, %d LOD0 triangles, from %s"
                " (%s, %d strand tris per %.0f-unit tile)%s%s.\n",
                created, (int)runs.size(), KiwiFmt_Num( lenBuf, sizeof( lenBuf ), totalLen ), tris,
                tile.model.c_str(), opts.strands ? "top wire(s) on the curve" : "all wires at post height",
                tile.strandTris, tile.maxX - tile.minX,
                removed ? ", replacing " : "", removed ? "the previous run" : "" );
    if ( created == 0 )
    {
        SetErr( err, errSz, "no piece could be placed" );
        return false;
    }
    return true;
}

// ── commands / UI ────────────────────────────────────────────────────────────────
void KiwiBarbwire_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiBarbwire", 0, 0, KIWI_CMD_BARBWIRE );
}

bool KiwiBarbwire_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_BARBWIRE )
        return false;
    if ( !KiwiBarbwire_CanExecute() )
    {
        Sys_Printf( "Barbwire: select a construction line, polyline, spline, arc or circle first.\n" );
        return true;
    }
    s_dialogRequest = true;
    return true;
}

void KiwiBarbwire_BuildMenu( void *frameMenu )
{
    HMENU menu = (HMENU)frameMenu;
    HMENU selection = menu ? ::GetSubMenu( menu, 3 ) : nullptr;
    if ( !selection || ::GetMenuState( selection, KIWI_CMD_BARBWIRE, MF_BYCOMMAND ) != 0xFFFFFFFFu )
        return;
    ::AppendMenuA( selection, MF_STRING, KIWI_CMD_BARBWIRE, "Barbwire Along Construction..." );
}

void KiwiBarbwire_Draw()
{
    if ( s_dialogRequest )
    {
        ImGui::OpenPopup( KBW_DIALOG_TITLE );
        s_dialogRequest = false;
        s_dialogOpen = true;
        LoadOpts();
    }
    if ( !s_dialogOpen )
        return;
    if ( !ImGui::BeginPopupModal( KBW_DIALOG_TITLE, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        s_dialogOpen = false;
        return;
    }

    std::vector<int> objs;
    SelectedObjects( &objs );
    float total = 0.0f;
    for ( size_t i = 0; i < objs.size(); ++i )
    {
        const kconObject_t *o = KiwiCon_At( objs[i] );
        bwCurve_t c;
        if ( o && !o->hidden && BuildCurve( *o, &c ) )
            total += c.length;
    }
    char lenBuf[32];
    ImGui::Text( "%d selected construction object(s), %s units of wire",
                 (int)objs.size(), KiwiFmt_Num( lenBuf, sizeof( lenBuf ), total ) );
    ImGui::Separator();

    kiwiBarbwireOpts_t o = s_opts;
    const char *preview = KiwiBarbwire_SourceLabel( o.source );
    ImGui::SetNextItemWidth( 420.0f );
    if ( ImGui::BeginCombo( "Strands from", preview ) )
    {
        for ( int i = 0; i < KBW_SOURCE_COUNT; ++i )
            if ( ImGui::Selectable( KBW_SOURCES[i].label, i == o.source ) )
                o.source = i;
        ImGui::EndCombo();
    }
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "The stock CoD4 model the strands are lifted from.  Only the parts\n"
                           "that run the model's whole length are kept (posts and coil wraps\n"
                           "drop out), so what you get is the plain wire, on its own materials." );
    ImGui::SetNextItemWidth( 160.0f );
    ImGui::InputInt( "Strands (0 = all)", &o.strands );
    if ( o.strands < 0 )  o.strands = 0;
    if ( o.strands > 16 ) o.strands = 16;
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "How many of the source's wires to keep, counted from the top.\n"
                           "1 = the top strand alone, centred ON the curve.  0 keeps every\n"
                           "wire at its post height, with the curve as the ground line." );
    ImGui::SetNextItemWidth( 160.0f );
    ImGui::InputFloat( "Height offset", &o.zOffset, 1.0f, 8.0f, "%.1f" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Raises (+) or lowers (-) the strands against the curve." );
    ImGui::SetNextItemWidth( 160.0f );
    ImGui::InputFloat( "Wobble", &o.wobble, 0.5f, 2.0f, "%.1f" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Sideways meander in units (and a weaker up/down one) layered on\n"
                           "the tile's own waviness, so the repeat never reads as a pattern.\n"
                           "0 = the bare tile.  Different for every curve." );
    ImGui::SameLine();
    ImGui::SetNextItemWidth( 90.0f );
    ImGui::InputInt( "Seed", &o.seed, 0, 0 );
    ImGui::SameLine();
    if ( ImGui::Button( "Reseed" ) )
        o.seed = (int)( GetTickCount() & 0x7FFF );
    ImGui::SetNextItemWidth( 160.0f );
    ImGui::InputFloat( "Split above (0 = one model)", &o.pieceLen, 64.0f, 256.0f, "%.0f" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "0 makes the whole curve ONE xmodel (one entity).  A value splits the\n"
                           "run into misc_models no longer than this (%g..%g) - only worth it for\n"
                           "very long runs, since a static model is lit from one light-grid sample\n"
                           "and picks its LOD by one distance.", KBW_PIECE_MIN, KBW_PIECE_MAX );
    ImGui::Checkbox( "Mirror alternate tiles (seamless joins)", &o.mirrorTiles );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "The strand tile's two ends are different cross-sections; flipping\n"
                           "every other tile makes each join a strand meeting itself." );
    ImGui::Checkbox( "Replace this curve's previous barbwire", &o.replace );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Pieces carry `kiwi_barbwire \"<curve name>\"`; a re-run on the same\n"
                           "curve deletes the old pieces (and their files) first." );
    if ( o.pieceLen < 0.0f ) o.pieceLen = 0.0f;
    if ( o.pieceLen > 0.0f && o.pieceLen < KBW_PIECE_MIN ) o.pieceLen = KBW_PIECE_MIN;
    if ( o.pieceLen > KBW_PIECE_MAX ) o.pieceLen = KBW_PIECE_MAX;
    if ( o.wobble < 0.0f )  o.wobble = 0.0f;
    if ( o.wobble > 64.0f ) o.wobble = 64.0f;
    if ( o.seed < 0 ) o.seed = 0;
    s_opts = o;

    ImGui::TextDisabled( "Wire only, no collision: clip it with brushes like the stock barbwire." );
    ImGui::Separator();
    const bool can = !objs.empty();
    ImGui::BeginDisabled( !can );
    if ( ImGui::Button( "Lay barbwire", ImVec2( 140.0f, 0.0f ) ) )
    {
        KiwiBarbwire_SetOpts( s_opts );
        char err[256] = { 0 };
        if ( !KiwiBarbwire_LayNow( s_opts, err, sizeof( err ) ) )
            Sys_Printf( "Barbwire: %s.\n", err );
        s_dialogOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if ( ImGui::Button( "Cancel", ImVec2( 100.0f, 0.0f ) ) )
    {
        KiwiBarbwire_SetOpts( s_opts );
        s_dialogOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
