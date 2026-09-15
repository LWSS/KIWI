#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Export selected editor reference geometry as STEP brush solids plus OBJ meshes.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_plastbridge.h"
#include "kiwi_command.h"
#include "kiwi_construct.h"        // construction lines -> tiny STEP prisms
#include "kiwi_droptrace.h"
#include "kiwi_shadowcache.h"
#include "kiwi_windows.h"          // KiwiWindows_RevealInExplorer
#include "radiant_frame.h"
#include "radiant_registry.h"

#include <imgui/imgui.h>
#include <shellapi.h>
#include <xanim/xmodel.h>

#include <algorithm>
#include <float.h>
#include <math.h>
#include <map>
#include <new>
#include <process.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <utility>
#include <vector>

extern int Sys_Printf( const char *fmt, ... );
extern float *AnglesToAxis( float *angles, float ( *axisOut )[3] );
extern selbrush_t active_brushes;                       // map.cpp (the unselected list)
extern char FilterBrush( selbrush_t *a1, int a2 );      // filters.cpp:718 - hidden/filtered

namespace
{
    const double METERS_PER_UNIT = 0.0254;
    const float OBJ_METERS_PER_UNIT = 0.0254f;
    const double STEP_WELD_TOLERANCE = 0.001;
    const double STEP_PLANE_TOLERANCE = 0.01;
    const char *PLASTICITY_PROCESS = "Plasticity.exe";
    const char *PLASTICITY_DIALOG_CLASS = "#32770";
    const char *PLASTICITY_PREF_SECTION = "KiwiUX";
    const UINT AUTO_IMPORT_RESULT_MESSAGE = WM_APP + 1;

    int s_autoImport = -1;
    volatile LONG s_autoImportBusy = 0;
    HWND s_autoImportResultWindow = nullptr;

    // KIWI: construction lines ride along inside the brush STEP as tiny square
    // prisms (Plasticity has no construction-line import and the bridge cannot
    // create curves).  Both preferences persist beside PlasticityAutoImport; the
    // thickness is stored in thousandths of a unit because the profile API is
    // integer-only.
    const char *PLASTICITY_DIALOG_TITLE = "Send Selection to Plasticity";
    const int CONSTRUCTION_THICKNESS_DEFAULT_MILLI = 20;   // 0.020 in = 0.5 mm
    const int CONSTRUCTION_THICKNESS_MIN_MILLI = 5;        // 5x the STEP weld tolerance
    const int CONSTRUCTION_THICKNESS_MAX_MILLI = 4000;
    int s_exportConstruction = -1;
    int s_constructionThicknessMilli = -1;
    // KIWI (2026-09-13, user: "terrain isn't exporting when pushing to plasticity"):
    // the push is selection-driven and sculpted terrain is almost never part of the
    // selection, so every visible terrain sheet rides along in the OBJ (one mesh per
    // patch) when this preference is on - the default.
    int s_exportTerrain = -1;
    bool s_dialogRequest = false;   // Ctrl+Shift+P owes the frame an OpenPopup
    bool s_dialogOpen = false;

    struct ObjVertex
    {
        float x, y, z;
    };

    struct ObjObject
    {
        std::string name;
        std::vector<ObjVertex> vertices;
        std::vector<std::vector<unsigned int> > faces;
    };

    struct StepPoint
    {
        double x, y, z;
    };

    struct StepEdgeUse
    {
        unsigned int edge;
        bool forward;
    };

    struct StepFace
    {
        double normal[3];
        std::vector<unsigned int> vertices;
        std::vector<StepEdgeUse> edgeUses;
    };

    struct StepEdge
    {
        unsigned int a;
        unsigned int b;
        unsigned int useCount;
        unsigned int forwardUses;
    };

    struct StepSolid
    {
        std::string name;
        std::vector<StepPoint> vertices;
        std::vector<StepFace> faces;
        std::vector<StepEdge> edges;
    };

    struct ExportStats
    {
        unsigned int skippedFixed;
        unsigned int skippedInvalid;
        unsigned int skippedModelGeo;
        unsigned int stepBrushes;
        unsigned int patches;
        unsigned int models;
        unsigned int brushFallbacks;
        unsigned int constructionObjects;   // visible construction objects visited
        unsigned int constructionPrisms;    // one mitered prism per planar object
        unsigned int constructionBoxes;     // per-segment fallback boxes
        unsigned int constructionSkipped;   // objects/segments that built nothing
        unsigned int terrainPatches;        // unselected terrain sheets added as OBJ meshes
        unsigned int terrainSkipped;
        unsigned __int64 stepFaces;
        unsigned __int64 objFaces;

        ExportStats()
            : skippedFixed( 0 ), skippedInvalid( 0 ), skippedModelGeo( 0 ),
              stepBrushes( 0 ), patches( 0 ), models( 0 ), brushFallbacks( 0 ),
              constructionObjects( 0 ), constructionPrisms( 0 ),
              constructionBoxes( 0 ), constructionSkipped( 0 ),
              terrainPatches( 0 ), terrainSkipped( 0 ),
              stepFaces( 0 ), objFaces( 0 ) {}
    };

    bool FinitePoint( const float *point )
    {
        return point && _finite( point[0] ) && _finite( point[1] ) &&
               _finite( point[2] );
    }

    ObjVertex InMeters( const float *point )
    {
        ObjVertex vertex = { point[0] * OBJ_METERS_PER_UNIT,
                             point[1] * OBJ_METERS_PER_UNIT,
                             point[2] * OBJ_METERS_PER_UNIT };
        return vertex;
    }

    ObjVertex InMeters( const double point[3] )
    {
        ObjVertex vertex = { (float)( point[0] * METERS_PER_UNIT ),
                             (float)( point[1] * METERS_PER_UNIT ),
                             (float)( point[2] * METERS_PER_UNIT ) };
        return vertex;
    }

    unsigned int AddBrushVertex( ObjObject &object, const float *point )
    {
        const ObjVertex vertex = InMeters( point );
        for ( size_t i = 0; i < object.vertices.size(); ++i )
        {
            const ObjVertex &old = object.vertices[i];
            if ( old.x == vertex.x && old.y == vertex.y && old.z == vertex.z )
                return (unsigned int)i;
        }
        object.vertices.push_back( vertex );
        return (unsigned int)( object.vertices.size() - 1 );
    }

    bool BuildBrush( const brush_t *brush, ObjObject &object )
    {
        if ( !brush || !brush->faces || brush->faceCount <= 0 )
            return false;

        object.faces.reserve( (size_t)brush->faceCount );
        for ( int faceIndex = 0; faceIndex < brush->faceCount; ++faceIndex )
        {
            const winding_t *winding = brush->faces[faceIndex].w;
            if ( !winding || winding->numpoints < 3 ||
                 winding->numpoints > MAX_POINTS_ON_WINDING )
                return false;

            std::vector<unsigned int> face;
            face.reserve( (size_t)winding->numpoints );
            for ( int pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex )
            {
                if ( !FinitePoint( winding->p[pointIndex] ) )
                    return false;
                const unsigned int vertex = AddBrushVertex( object, winding->p[pointIndex] );
                for ( size_t i = 0; i < face.size(); ++i )
                    if ( face[i] == vertex )
                        return false;
                face.push_back( vertex );
            }
            object.faces.push_back( face );
        }
        return !object.faces.empty();
    }

    bool BuildPatch( const patchMesh_t *patch, ObjObject &object )
    {
        const curvePatchDef_t *grid = patch ? patch->curveDef : nullptr;
        if ( !grid || !grid->verts || grid->width < 2 || grid->height < 2 ||
             grid->width > 4096 || grid->height > 4096 ||
             (unsigned __int64)grid->width * (unsigned __int64)grid->height >
                 4000000ui64 )
            return false;

        object.vertices.reserve( (size_t)grid->width * (size_t)grid->height );
        for ( int y = 0; y < grid->height; ++y )
        {
            for ( int x = 0; x < grid->width; ++x )
            {
                const float *point = grid->verts[y * grid->width + x].xyz;
                if ( !FinitePoint( point ) )
                    return false;
                object.vertices.push_back( InMeters( point ) );
            }
        }

        object.faces.reserve( (size_t)( grid->width - 1 ) *
                              (size_t)( grid->height - 1 ) );
        for ( int y = 0; y + 1 < grid->height; ++y )
        {
            for ( int x = 0; x + 1 < grid->width; ++x )
            {
                const unsigned int v00 = (unsigned int)( y * grid->width + x );
                const unsigned int v10 = v00 + 1;
                const unsigned int v01 = v00 + (unsigned int)grid->width;
                const unsigned int v11 = v01 + 1;
                std::vector<unsigned int> face;
                face.reserve( 4 );
                face.push_back( v00 );
                face.push_back( v10 );
                face.push_back( v11 );
                face.push_back( v01 );
                object.faces.push_back( face );
            }
        }
        return !object.faces.empty();
    }

    void SanitizeName( std::string &name )
    {
        for ( size_t i = 0; i < name.size(); ++i )
        {
            const unsigned char c = (unsigned char)name[i];
            const bool safe = ( c >= 'a' && c <= 'z' ) ||
                              ( c >= 'A' && c <= 'Z' ) ||
                              ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
            if ( !safe )
                name[i] = '_';
        }
    }

    bool ObjectName( const selbrush_t *selected, char kind, std::string &name,
                     const char *preferredBase = nullptr )
    {
        if ( !selected || !selected->owner || !selected->owner->def ||
             !selected->def )
            return false;

        entity_s *entity = (entity_s *)selected->owner->def;
        if ( ( !preferredBase || !preferredBase[0] ) &&
             ( !entity->eclass || !entity->eclass->name ||
               !entity->eclass->name[0] ) )
            return false;

        int ordinal = 0;
        brush_t *sentinel = (brush_t *)&entity->def;
        brush_t *brush = (brush_t *)entity->brushes.prev;
        for ( ; brush && brush != sentinel; brush = brush->onext, ++ordinal )
            if ( brush == selected->def )
                break;
        if ( !brush || brush == sentinel )
            return false;

        name = preferredBase && preferredBase[0] ? preferredBase
                                                 : entity->eclass->name;
        SanitizeName( name );

        char suffix[32];
        _snprintf( suffix, sizeof( suffix ), "_%c%d", kind, ordinal );
        suffix[sizeof( suffix ) - 1] = '\0';
        name += suffix;
        return true;
    }

    bool FiniteDouble( double value )
    {
        return _finite( value ) != 0;
    }

    double PointDistanceSquared( const StepPoint &a, const StepPoint &b )
    {
        const double x = a.x - b.x;
        const double y = a.y - b.y;
        const double z = a.z - b.z;
        return x * x + y * y + z * z;
    }

    double Dot3( const double a[3], const double b[3] )
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    void Cross3( const double a[3], const double b[3], double out[3] )
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    bool Normalize3( double value[3] )
    {
        const double lengthSquared = Dot3( value, value );
        if ( !FiniteDouble( lengthSquared ) || lengthSquared <= 1.0e-24 )
            return false;
        const double inverseLength = 1.0 / sqrt( lengthSquared );
        value[0] *= inverseLength;
        value[1] *= inverseLength;
        value[2] *= inverseLength;
        return FiniteDouble( value[0] ) && FiniteDouble( value[1] ) &&
               FiniteDouble( value[2] );
    }

    bool RemoveCollinearPoints( std::vector<StepPoint> &points )
    {
        const double toleranceSquared =
            STEP_WELD_TOLERANCE * STEP_WELD_TOLERANCE;
        bool changed = true;
        while ( changed && points.size() >= 3 )
        {
            changed = false;
            for ( size_t i = 0; i < points.size(); ++i )
            {
                const size_t previous = ( i + points.size() - 1 ) % points.size();
                const size_t next = ( i + 1 ) % points.size();
                if ( PointDistanceSquared( points[previous], points[i] ) <=
                         toleranceSquared ||
                     PointDistanceSquared( points[i], points[next] ) <=
                         toleranceSquared )
                {
                    points.erase( points.begin() + i );
                    changed = true;
                    break;
                }

                const double line[3] = { points[next].x - points[previous].x,
                                         points[next].y - points[previous].y,
                                         points[next].z - points[previous].z };
                const double offset[3] = { points[i].x - points[previous].x,
                                           points[i].y - points[previous].y,
                                           points[i].z - points[previous].z };
                const double lineLengthSquared = Dot3( line, line );
                if ( lineLengthSquared <= toleranceSquared )
                {
                    points.erase( points.begin() + i );
                    changed = true;
                    break;
                }

                double cross[3];
                Cross3( offset, line, cross );
                const double projection = Dot3( offset, line );
                const double distanceSquared = Dot3( cross, cross ) /
                                               lineLengthSquared;
                if ( distanceSquared <= toleranceSquared &&
                     projection >= -toleranceSquared &&
                     projection <= lineLengthSquared + toleranceSquared )
                {
                    points.erase( points.begin() + i );
                    changed = true;
                    break;
                }
            }
        }
        return points.size() >= 3;
    }

    bool NewellNormal( const std::vector<StepPoint> &points, double normal[3] )
    {
        normal[0] = normal[1] = normal[2] = 0.0;
        for ( size_t i = 0; i < points.size(); ++i )
        {
            const StepPoint &current = points[i];
            const StepPoint &next = points[( i + 1 ) % points.size()];
            normal[0] += ( current.y - next.y ) * ( current.z + next.z );
            normal[1] += ( current.z - next.z ) * ( current.x + next.x );
            normal[2] += ( current.x - next.x ) * ( current.y + next.y );
        }
        return Normalize3( normal );
    }

    bool FacePlanePointsOutward( const brush_t *brush, int faceIndex,
                                 const double normal[3] )
    {
        const plane_t &plane = brush->faces[faceIndex].plane;
        const double rawNormal[3] = { (double)plane.normal[0],
                                      (double)plane.normal[1],
                                      (double)plane.normal[2] };
        const double rawLengthSquared = Dot3( rawNormal, rawNormal );
        if ( !FiniteDouble( rawLengthSquared ) || rawLengthSquared <= 1.0e-24 )
            return false;
        const double distance = (double)plane.dist / sqrt( rawLengthSquared );
        if ( !FiniteDouble( distance ) )
            return false;

        for ( int otherIndex = 0; otherIndex < brush->faceCount; ++otherIndex )
        {
            if ( otherIndex == faceIndex )
                continue;
            const winding_t *other = brush->faces[otherIndex].w;
            if ( !other || other->numpoints < 3 ||
                 other->numpoints > MAX_POINTS_ON_WINDING )
                return false;

            double centroid[3] = { 0.0, 0.0, 0.0 };
            for ( int pointIndex = 0; pointIndex < other->numpoints; ++pointIndex )
            {
                if ( !FinitePoint( other->p[pointIndex] ) )
                    return false;
                centroid[0] += (double)other->p[pointIndex][0];
                centroid[1] += (double)other->p[pointIndex][1];
                centroid[2] += (double)other->p[pointIndex][2];
            }
            const double inverseCount = 1.0 / (double)other->numpoints;
            centroid[0] *= inverseCount;
            centroid[1] *= inverseCount;
            centroid[2] *= inverseCount;
            if ( Dot3( normal, centroid ) > distance + STEP_PLANE_TOLERANCE )
                return false;
        }
        return true;
    }

    unsigned int WeldStepVertex( StepSolid &solid, const StepPoint &point )
    {
        const double toleranceSquared =
            STEP_WELD_TOLERANCE * STEP_WELD_TOLERANCE;
        for ( size_t i = 0; i < solid.vertices.size(); ++i )
            if ( PointDistanceSquared( solid.vertices[i], point ) <=
                 toleranceSquared )
                return (unsigned int)i;
        solid.vertices.push_back( point );
        return (unsigned int)( solid.vertices.size() - 1 );
    }

    struct PreparedStepFace
    {
        double normal[3];
        std::vector<StepPoint> points;
    };

    bool AssembleStepSolid( const std::vector<PreparedStepFace> &prepared,
                            const std::string &name, StepSolid &solid );

    bool BuildStepSolid( const brush_t *brush, const std::string &name,
                         StepSolid &solid )
    {
        if ( !brush || !brush->faces || brush->faceCount < 4 )
            return false;

        std::vector<PreparedStepFace> prepared;
        prepared.reserve( (size_t)brush->faceCount );
        for ( int faceIndex = 0; faceIndex < brush->faceCount; ++faceIndex )
        {
            const face_t &sourceFace = brush->faces[faceIndex];
            const winding_t *winding = sourceFace.w;
            if ( !winding || winding->numpoints < 3 ||
                 winding->numpoints > MAX_POINTS_ON_WINDING )
                return false;

            PreparedStepFace face;
            face.normal[0] = (double)sourceFace.plane.normal[0];
            face.normal[1] = (double)sourceFace.plane.normal[1];
            face.normal[2] = (double)sourceFace.plane.normal[2];
            if ( !Normalize3( face.normal ) ||
                 !FacePlanePointsOutward( brush, faceIndex, face.normal ) )
                return false;

            face.points.reserve( (size_t)winding->numpoints );
            for ( int pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex )
            {
                if ( !FinitePoint( winding->p[pointIndex] ) )
                    return false;
                StepPoint point = { (double)winding->p[pointIndex][0],
                                    (double)winding->p[pointIndex][1],
                                    (double)winding->p[pointIndex][2] };
                face.points.push_back( point );
            }
            if ( !RemoveCollinearPoints( face.points ) )
                return false;

            double windingNormal[3];
            if ( !NewellNormal( face.points, windingNormal ) )
                return false;
            const double agreement = Dot3( windingNormal, face.normal );
            if ( !FiniteDouble( agreement ) || fabs( agreement ) <= 1.0e-8 )
                return false;
            if ( agreement < 0.0 )
                std::reverse( face.points.begin(), face.points.end() );
            prepared.push_back( face );
        }
        return AssembleStepSolid( prepared, name, solid );
    }

    // Weld the prepared faces into one manifold solid: every edge must be used
    // exactly twice, once in each direction.  Shared by brushes and the
    // construction-line prisms below.
    bool AssembleStepSolid( const std::vector<PreparedStepFace> &prepared,
                            const std::string &name, StepSolid &solid )
    {
        solid.name = name;
        solid.faces.reserve( prepared.size() );
        for ( size_t faceIndex = 0; faceIndex < prepared.size(); ++faceIndex )
        {
            StepFace face;
            face.normal[0] = prepared[faceIndex].normal[0];
            face.normal[1] = prepared[faceIndex].normal[1];
            face.normal[2] = prepared[faceIndex].normal[2];
            face.vertices.reserve( prepared[faceIndex].points.size() );
            for ( size_t pointIndex = 0;
                  pointIndex < prepared[faceIndex].points.size(); ++pointIndex )
            {
                const unsigned int vertex =
                    WeldStepVertex( solid, prepared[faceIndex].points[pointIndex] );
                if ( face.vertices.empty() || face.vertices.back() != vertex )
                    face.vertices.push_back( vertex );
            }
            if ( face.vertices.size() > 1 &&
                 face.vertices.front() == face.vertices.back() )
                face.vertices.pop_back();
            if ( face.vertices.size() < 3 )
                return false;
            for ( size_t i = 0; i < face.vertices.size(); ++i )
                for ( size_t j = i + 1; j < face.vertices.size(); ++j )
                    if ( face.vertices[i] == face.vertices[j] )
                        return false;
            solid.faces.push_back( face );
        }

        typedef std::pair<unsigned int, unsigned int> EdgeKey;
        std::map<EdgeKey, unsigned int> edgeMap;
        for ( size_t faceIndex = 0; faceIndex < solid.faces.size(); ++faceIndex )
        {
            StepFace &face = solid.faces[faceIndex];
            face.edgeUses.reserve( face.vertices.size() );
            for ( size_t vertexIndex = 0; vertexIndex < face.vertices.size();
                  ++vertexIndex )
            {
                const unsigned int a = face.vertices[vertexIndex];
                const unsigned int b =
                    face.vertices[( vertexIndex + 1 ) % face.vertices.size()];
                if ( a == b )
                    return false;
                const EdgeKey key( (std::min)( a, b ), (std::max)( a, b ) );
                std::map<EdgeKey, unsigned int>::iterator found = edgeMap.find( key );
                unsigned int edgeIndex;
                if ( found == edgeMap.end() )
                {
                    StepEdge edge = { a, b, 0, 0 };
                    solid.edges.push_back( edge );
                    edgeIndex = (unsigned int)( solid.edges.size() - 1 );
                    edgeMap.insert( std::make_pair( key, edgeIndex ) );
                }
                else
                {
                    edgeIndex = found->second;
                }

                StepEdge &edge = solid.edges[edgeIndex];
                const bool forward = edge.a == a && edge.b == b;
                if ( !forward && !( edge.a == b && edge.b == a ) )
                    return false;
                ++edge.useCount;
                if ( forward )
                    ++edge.forwardUses;
                StepEdgeUse use = { edgeIndex, forward };
                face.edgeUses.push_back( use );
            }
        }

        for ( size_t edgeIndex = 0; edgeIndex < solid.edges.size(); ++edgeIndex )
            if ( solid.edges[edgeIndex].useCount != 2 ||
                 solid.edges[edgeIndex].forwardUses != 1 )
                return false;
        return !solid.vertices.empty() && !solid.faces.empty() &&
               !solid.edges.empty();
    }

    bool BuildModel( selbrush_t *selected, ObjObject &object )
    {
        float mins[3], maxs[3], angles[3], scale, origin[3];
        XModel *model = nullptr;
        if ( !KiwiDrop_GetModelInfo( selected, mins, maxs, angles, &scale,
                                     origin, &model ) || !model )
            return false;

        const float *verts = nullptr;
        const unsigned short *indices = nullptr;
        int indexCount = 0;
        if ( !KiwiShadowCache_ModelGeo( model, &verts, &indices, &indexCount ) ||
             !verts || !indices || indexCount < 3 || indexCount % 3 != 0 )
            return false;
        if ( !ObjectName( selected, 'm', object.name, model->name ) )
            return false;

        int maxIndex = -1;
        for ( int i = 0; i < indexCount; ++i )
            if ( (int)indices[i] > maxIndex )
                maxIndex = (int)indices[i];
        if ( maxIndex < 0 || maxIndex >= 0x4000 )
            return false;

        float axis[3][3];
        AnglesToAxis( angles, axis );
        if ( !_finite( scale ) || !( scale > 0.0f ) )
            return false;
        for ( int component = 0; component < 3; ++component )
        {
            if ( !_finite( origin[component] ) )
                return false;
            for ( int basis = 0; basis < 3; ++basis )
                if ( !_finite( axis[basis][component] ) )
                    return false;
        }

        std::vector<int> remap( (size_t)maxIndex + 1, -1 );
        object.faces.reserve( (size_t)indexCount / 3 );
        for ( int index = 0; index < indexCount; index += 3 )
        {
            std::vector<unsigned int> face;
            face.reserve( 3 );
            for ( int corner = 0; corner < 3; ++corner )
            {
                const unsigned short sourceIndex = indices[index + corner];
                int objectIndex = remap[sourceIndex];
                if ( objectIndex < 0 )
                {
                    const float *local = verts + 3 * sourceIndex;
                    if ( !FinitePoint( local ) )
                        return false;
                    const double scaled[3] = { (double)local[0] * (double)scale,
                                               (double)local[1] * (double)scale,
                                               (double)local[2] * (double)scale };
                    const double world[3] = {
                        (double)origin[0] + (double)axis[0][0] * scaled[0] +
                            (double)axis[1][0] * scaled[1] +
                            (double)axis[2][0] * scaled[2],
                        (double)origin[1] + (double)axis[0][1] * scaled[0] +
                            (double)axis[1][1] * scaled[1] +
                            (double)axis[2][1] * scaled[2],
                        (double)origin[2] + (double)axis[0][2] * scaled[0] +
                            (double)axis[1][2] * scaled[1] +
                            (double)axis[2][2] * scaled[2] };
                    if ( !FiniteDouble( world[0] ) || !FiniteDouble( world[1] ) ||
                         !FiniteDouble( world[2] ) )
                        return false;
                    object.vertices.push_back( InMeters( world ) );
                    objectIndex = (int)object.vertices.size() - 1;
                    remap[sourceIndex] = objectIndex;
                }
                face.push_back( (unsigned int)objectIndex );
            }
            object.faces.push_back( face );
        }
        return !object.vertices.empty() && !object.faces.empty();
    }

    bool GatherObjects( std::vector<StepSolid> &solids,
                        std::vector<ObjObject> &objects, ExportStats &stats,
                        std::string &error )
    {
        for ( selbrush_t *selected = selected_brushes.next;
              selected && selected != &selected_brushes; selected = selected->next )
        {
            if ( !selected->owner || !selected->owner->def || !selected->def )
            {
                ++stats.skippedInvalid;
                continue;
            }

            if ( KiwiDrop_IsModelEntity( selected ) )
            {
                ObjObject object;
                if ( !BuildModel( selected, object ) )
                {
                    ++stats.skippedModelGeo;
                    continue;
                }
                ++stats.models;
                stats.objFaces += (unsigned __int64)object.faces.size();
                objects.push_back( object );
                continue;
            }

            entity_s *entity = (entity_s *)selected->owner->def;
            if ( entity->eclass && entity->eclass->fixedsize )
            {
                ++stats.skippedFixed;
                continue;
            }

            const patchMesh_t *patch = selected->def->patch;
            if ( patch )
            {
                ObjObject object;
                if ( !ObjectName( selected, 'p', object.name ) ||
                     !BuildPatch( patch, object ) )
                {
                    ++stats.skippedInvalid;
                    continue;
                }
                ++stats.patches;
                stats.objFaces += (unsigned __int64)object.faces.size();
                objects.push_back( object );
                continue;
            }

            std::string name;
            if ( !ObjectName( selected, 'b', name ) )
            {
                ++stats.skippedInvalid;
                continue;
            }
            StepSolid solid;
            if ( BuildStepSolid( selected->def, name, solid ) )
            {
                ++stats.stepBrushes;
                stats.stepFaces += (unsigned __int64)solid.faces.size();
                solids.push_back( solid );
                continue;
            }

            ObjObject fallback;
            fallback.name = name;
            if ( !BuildBrush( selected->def, fallback ) )
            {
                ++stats.skippedInvalid;
                continue;
            }
            ++stats.brushFallbacks;
            stats.objFaces += (unsigned __int64)fallback.faces.size();
            objects.push_back( fallback );
        }

        if ( solids.empty() && objects.empty() )
        {
            error = "selection contains no exportable brush, evaluated patch, or "
                    "resident model geometry";
            return false;
        }
        return true;
    }

    // ---- Terrain sheets, selected or not (KIWI 2026-09-13) ----------------------
    // Every visible PATCH_TERRAIN patch in the UNSELECTED list becomes one OBJ mesh
    // (the evaluated render grid, like a selected patch).  Selected terrain is already
    // handled by GatherObjects, so the two never double up.
    bool TerrainNode( const selbrush_t *b )
    {
        return b && b->def && b->def->patch
            && ( b->def->patch->type & PATCH_TERRAIN ) != 0
            && b->owner && b->owner->def;
    }

    unsigned int VisibleTerrainCount()
    {
        unsigned int n = 0;
        for ( selbrush_t *b = active_brushes.next; b && b != &active_brushes; b = b->next )
            if ( TerrainNode( b ) && !FilterBrush( b, 0 ) )
                ++n;
        return n;
    }

    void GatherTerrain( std::vector<ObjObject> &objects, ExportStats &stats )
    {
        for ( selbrush_t *b = active_brushes.next; b && b != &active_brushes; b = b->next )
        {
            if ( !TerrainNode( b ) || FilterBrush( b, 0 ) )
                continue;
            ObjObject object;
            if ( !ObjectName( b, 'p', object.name, "terrain" ) ||
                 !BuildPatch( b->def->patch, object ) )
            {
                ++stats.terrainSkipped;
                continue;
            }
            ++stats.terrainPatches;
            stats.objFaces += (unsigned __int64)object.faces.size();
            objects.push_back( object );
        }
    }

    // ---- Construction lines as tiny STEP prisms --------------------------------
    // Plasticity cannot import construction lines and the bridge cannot create
    // curves, so each visible construction object is swept into a square prism
    // `thickness` units across and appended to the brush STEP.  Profile corner 0
    // sits ON the path: one long edge of every prism IS the construction line, so
    // snapping to that edge in Plasticity snaps to the line exactly.  Planar
    // objects become ONE mitred prism per object (a ring for closed shapes); the
    // mitre keeps every side face planar because all four corners of a side lie
    // at a constant in-plane offset from the segment.  Non-planar polylines, over-
    // sharp turns, and segments shorter than their mitres fall back to one box per
    // segment.  Face orientation is settled by the signed volume, so the winding
    // convention below only has to be topologically consistent.

    const double SWEEP_POINT_WELD = 1.0e-4;      // consecutive-point collapse, units
    const double SWEEP_MIN_MITRE_DENOM = 0.05;   // 1 + left(prev).left(next); ~174 deg turn
    const double SWEEP_PLANAR_DOT = 1.0e-3;      // |segment dir . plane normal| ceiling

    void Scale3( double value[3], double scale )
    {
        value[0] *= scale;
        value[1] *= scale;
        value[2] *= scale;
    }

    StepPoint OffsetPoint( const StepPoint &base, const double *along, double a,
                           const double *normal, double b )
    {
        StepPoint point = { base.x + along[0] * a + normal[0] * b,
                            base.y + along[1] * a + normal[1] * b,
                            base.z + along[2] * a + normal[2] * b };
        return point;
    }

    double SignedVolume( const std::vector<PreparedStepFace> &faces )
    {
        double volume = 0.0;
        for ( size_t f = 0; f < faces.size(); ++f )
        {
            const std::vector<StepPoint> &p = faces[f].points;
            for ( size_t i = 1; i + 1 < p.size(); ++i )
            {
                const double a[3] = { p[0].x, p[0].y, p[0].z };
                const double b[3] = { p[i].x, p[i].y, p[i].z };
                const double c[3] = { p[i + 1].x, p[i + 1].y, p[i + 1].z };
                double cross[3];
                Cross3( b, c, cross );
                volume += Dot3( a, cross );
            }
        }
        return volume / 6.0;
    }

    // `path` must already lie in the plane with unit `planeNormal`.  Closed paths
    // need three points and get no caps; open paths get a cap at each end.
    bool BuildSweepSolid( const std::vector<StepPoint> &path, bool closed,
                          const double planeNormal[3], double thickness,
                          const std::string &name, StepSolid &solid,
                          std::string &reason )
    {
        const size_t count = path.size();
        if ( count < 2 || ( closed && count < 3 ) )
        {
            reason = "too few points";
            return false;
        }
        if ( !FiniteDouble( thickness ) || thickness <= STEP_WELD_TOLERANCE * 2.0 )
        {
            reason = "thickness below the STEP weld tolerance";
            return false;
        }
        const size_t segments = closed ? count : count - 1;

        // Unit direction, in-plane left offset (n x d) and length per segment.
        std::vector<double> dir( segments * 3 ), left( segments * 3 ), length( segments );
        for ( size_t i = 0; i < segments; ++i )
        {
            const StepPoint &a = path[i];
            const StepPoint &b = path[( i + 1 ) % count];
            double d[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
            const double len = sqrt( Dot3( d, d ) );
            if ( !FiniteDouble( len ) || len <= SWEEP_POINT_WELD )
            {
                reason = "degenerate segment";
                return false;
            }
            Scale3( d, 1.0 / len );
            if ( fabs( Dot3( d, planeNormal ) ) > SWEEP_PLANAR_DOT )
            {
                reason = "path leaves its plane";
                return false;
            }
            double u[3];
            Cross3( planeNormal, d, u );
            if ( !Normalize3( u ) )
            {
                reason = "segment parallel to the plane normal";
                return false;
            }
            for ( int k = 0; k < 3; ++k )
            {
                dir[i * 3 + k] = d[k];
                left[i * 3 + k] = u[k];
            }
            length[i] = len;
        }

        // Per-joint mitre direction w, scaled so w.left == 1 for BOTH adjacent
        // segments: the corner sits at in-plane distance `thickness` from each.
        std::vector<double> mitre( count * 3 );
        for ( size_t j = 0; j < count; ++j )
        {
            const bool interior = closed || ( j > 0 && j + 1 < count );
            double w[3];
            if ( !interior )
            {
                const size_t seg = j == 0 ? 0 : segments - 1;
                for ( int k = 0; k < 3; ++k )
                    w[k] = left[seg * 3 + k];
            }
            else
            {
                const size_t prev = ( j + segments - 1 ) % segments;
                const size_t next = j % segments;
                const double denominator =
                    1.0 + Dot3( &left[prev * 3], &left[next * 3] );
                if ( !FiniteDouble( denominator ) || denominator < SWEEP_MIN_MITRE_DENOM )
                {
                    reason = "turn too sharp for a mitre";
                    return false;
                }
                for ( int k = 0; k < 3; ++k )
                    w[k] = ( left[prev * 3 + k] + left[next * 3 + k] ) / denominator;
            }
            for ( int k = 0; k < 3; ++k )
                mitre[j * 3 + k] = w[k];
        }

        // The offset corners must not cross: the mitred side of a segment keeps a
        // positive length after both end mitres are pushed along it.
        for ( size_t i = 0; i < segments; ++i )
        {
            const size_t j0 = i;
            const size_t j1 = ( i + 1 ) % count;
            const double startPush = Dot3( &mitre[j0 * 3], &dir[i * 3] );
            const double endPush = Dot3( &mitre[j1 * 3], &dir[i * 3] );
            const double remaining = length[i] - thickness * ( startPush - endPush );
            if ( remaining <= STEP_WELD_TOLERANCE * 4.0 )
            {
                reason = "segment shorter than its mitres";
                return false;
            }
        }

        // Profile corners as (along mitre, along normal) multiples of thickness.
        // Corner 0 is the construction line itself.
        static const double CORNER_A[4] = { 0.0, 1.0, 1.0, 0.0 };
        static const double CORNER_B[4] = { 0.0, 0.0, 1.0, 1.0 };
        std::vector<StepPoint> corner( count * 4 );
        for ( size_t j = 0; j < count; ++j )
            for ( int k = 0; k < 4; ++k )
                corner[j * 4 + k] = OffsetPoint( path[j], &mitre[j * 3],
                                                 thickness * CORNER_A[k],
                                                 planeNormal, thickness * CORNER_B[k] );

        std::vector<PreparedStepFace> faces;
        faces.reserve( segments * 4 + 2 );
        for ( size_t i = 0; i < segments; ++i )
        {
            const size_t j0 = i;
            const size_t j1 = ( i + 1 ) % count;
            for ( int k = 0; k < 4; ++k )
            {
                const int k1 = ( k + 1 ) & 3;
                PreparedStepFace face;
                face.points.push_back( corner[j0 * 4 + k] );
                face.points.push_back( corner[j0 * 4 + k1] );
                face.points.push_back( corner[j1 * 4 + k1] );
                face.points.push_back( corner[j1 * 4 + k] );
                faces.push_back( face );
            }
        }
        if ( !closed )
        {
            PreparedStepFace start;
            for ( int k = 3; k >= 0; --k )
                start.points.push_back( corner[k] );
            faces.push_back( start );
            PreparedStepFace end;
            for ( int k = 0; k < 4; ++k )
                end.points.push_back( corner[( count - 1 ) * 4 + k] );
            faces.push_back( end );
        }

        const double volume = SignedVolume( faces );
        if ( !FiniteDouble( volume ) || fabs( volume ) <= 1.0e-12 )
        {
            reason = "prism has no volume";
            return false;
        }
        for ( size_t f = 0; f < faces.size(); ++f )
        {
            PreparedStepFace &face = faces[f];
            if ( volume < 0.0 )
                std::reverse( face.points.begin(), face.points.end() );
            if ( !NewellNormal( face.points, face.normal ) ||
                 !Normalize3( face.normal ) )
            {
                reason = "degenerate prism face";
                return false;
            }
        }
        return AssembleStepSolid( faces, name, solid );
    }

    // Any unit vector perpendicular to `d`, for the per-segment box fallback.
    void PerpendicularTo( const double d[3], double out[3] )
    {
        int smallest = 0;
        if ( fabs( d[1] ) < fabs( d[smallest] ) )
            smallest = 1;
        if ( fabs( d[2] ) < fabs( d[smallest] ) )
            smallest = 2;
        double axis[3] = { 0.0, 0.0, 0.0 };
        axis[smallest] = 1.0;
        Cross3( axis, d, out );
        if ( !Normalize3( out ) )
        {
            out[0] = 0.0;
            out[1] = 0.0;
            out[2] = 1.0;
        }
    }

    bool BuildSegmentBox( const StepPoint &a, const StepPoint &b, double thickness,
                          const std::string &name, StepSolid &solid,
                          std::string &reason )
    {
        double d[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
        if ( !Normalize3( d ) )
        {
            reason = "degenerate segment";
            return false;
        }
        double normal[3];
        PerpendicularTo( d, normal );
        std::vector<StepPoint> path;
        path.push_back( a );
        path.push_back( b );
        return BuildSweepSolid( path, false, normal, thickness, name, solid, reason );
    }

    std::string ConstructionName( int index, const kconObject_t &object )
    {
        static const char *TYPE_NAMES[KCON_TYPE_COUNT] =
            { "line", "polyline", "rect", "circle", "arc" };
        const int type = (int)object.type;
        const char *typeName = type >= 0 && type < (int)KCON_TYPE_COUNT
                             ? TYPE_NAMES[type] : "construction";
        char base[64];
        _snprintf( base, sizeof( base ), "con%d_%s", index, typeName );
        base[sizeof( base ) - 1] = '\0';
        std::string name = base;
        const char *label = KiwiCon_Name( index );
        if ( label && label[0] )
        {
            std::string suffix = label;
            SanitizeName( suffix );
            name += "_" + suffix;
        }
        return name;
    }

    // Tessellated world-space path with consecutive duplicates welded away.
    bool ConstructionPath( const kconObject_t &object, std::vector<StepPoint> &path,
                           bool &closed )
    {
        const int vertCount = KiwiCon_VertCount( object );
        const int segmentCount = KiwiCon_SegmentCount( object );
        if ( vertCount < 2 || segmentCount < 1 )
            return false;
        closed = segmentCount == vertCount;

        path.clear();
        path.reserve( (size_t)vertCount );
        const double weldSquared = SWEEP_POINT_WELD * SWEEP_POINT_WELD;
        for ( int v = 0; v < vertCount; ++v )
        {
            float world[3];
            if ( !KiwiCon_VertWorld( object, v, world ) || !FinitePoint( world ) )
                return false;
            StepPoint point = { (double)world[0], (double)world[1], (double)world[2] };
            if ( !path.empty() &&
                 PointDistanceSquared( path.back(), point ) <= weldSquared )
                continue;
            path.push_back( point );
        }
        while ( closed && path.size() > 1 &&
                PointDistanceSquared( path.front(), path.back() ) <= weldSquared )
            path.pop_back();
        if ( closed && path.size() < 3 )
            closed = false;
        return path.size() >= 2;
    }

    void CountVisibleConstruction( int &objects, int &segments )
    {
        objects = 0;
        segments = 0;
        const int count = KiwiCon_Count();
        for ( int index = 0; index < count; ++index )
        {
            const kconObject_t *object = KiwiCon_At( index );
            if ( !object || object->hidden )
                continue;
            const int objectSegments = KiwiCon_SegmentCount( *object );
            if ( objectSegments < 1 )
                continue;
            ++objects;
            segments += objectSegments;
        }
    }

    void GatherConstruction( double thickness, std::vector<StepSolid> &solids,
                             ExportStats &stats )
    {
        const int count = KiwiCon_Count();
        for ( int index = 0; index < count; ++index )
        {
            const kconObject_t *object = KiwiCon_At( index );
            if ( !object || object->hidden )
                continue;

            std::vector<StepPoint> path;
            bool closed = false;
            if ( !ConstructionPath( *object, path, closed ) )
            {
                if ( KiwiCon_SegmentCount( *object ) > 0 )
                    ++stats.constructionSkipped;
                continue;
            }
            ++stats.constructionObjects;
            const std::string name = ConstructionName( index, *object );

            kconPlane_t plane;
            std::string reason;
            if ( KiwiCon_ObjectPlane( *object, &plane ) )
            {
                double normal[3] = { (double)plane.normal[0], (double)plane.normal[1],
                                     (double)plane.normal[2] };
                if ( Normalize3( normal ) )
                {
                    // Flatten float noise so every side face is exactly planar.
                    const double origin[3] = { (double)plane.origin[0],
                                               (double)plane.origin[1],
                                               (double)plane.origin[2] };
                    for ( size_t p = 0; p < path.size(); ++p )
                    {
                        const double offset[3] = { path[p].x - origin[0],
                                                   path[p].y - origin[1],
                                                   path[p].z - origin[2] };
                        const double height = Dot3( offset, normal );
                        path[p].x -= normal[0] * height;
                        path[p].y -= normal[1] * height;
                        path[p].z -= normal[2] * height;
                    }
                    StepSolid solid;
                    if ( BuildSweepSolid( path, closed, normal, thickness, name,
                                          solid, reason ) )
                    {
                        ++stats.constructionPrisms;
                        stats.stepFaces += (unsigned __int64)solid.faces.size();
                        solids.push_back( solid );
                        continue;
                    }
                    Sys_Printf( "Plasticity export: construction \"%s\": %s; "
                                "exporting one box per segment instead.\n",
                                name.c_str(), reason.c_str() );
                }
            }

            const size_t segments = closed ? path.size() : path.size() - 1;
            unsigned int boxes = 0;
            for ( size_t s = 0; s < segments; ++s )
            {
                char suffix[32];
                _snprintf( suffix, sizeof( suffix ), "_s%u", (unsigned int)s );
                suffix[sizeof( suffix ) - 1] = '\0';
                StepSolid solid;
                if ( !BuildSegmentBox( path[s], path[( s + 1 ) % path.size()], thickness,
                                       name + suffix, solid, reason ) )
                {
                    ++stats.constructionSkipped;
                    Sys_Printf( "Plasticity export: construction \"%s%s\": %s; skipped.\n",
                                name.c_str(), suffix, reason.c_str() );
                    continue;
                }
                ++boxes;
                stats.stepFaces += (unsigned __int64)solid.faces.size();
                solids.push_back( solid );
            }
            stats.constructionBoxes += boxes;
        }
    }

    void PrintExportStats( const ExportStats &stats )
    {
        Sys_Printf( "Plasticity export: %u brush(es) -> STEP; %u patch(es), "
                    "%u model instance(s), %u degenerate brush fallback(s) -> OBJ; "
                    "skipped %u model selection(s) without resident/valid mesh "
                    "geometry, %u fixed-size selection(s), %u invalid selection(s).\n",
                    stats.stepBrushes, stats.patches, stats.models,
                    stats.brushFallbacks, stats.skippedModelGeo,
                    stats.skippedFixed, stats.skippedInvalid );
        if ( stats.constructionObjects || stats.constructionSkipped )
            Sys_Printf( "Plasticity export: %u construction object(s) -> %u prism(s) "
                        "+ %u per-segment box(es) in the STEP; %u skipped.\n",
                        stats.constructionObjects, stats.constructionPrisms,
                        stats.constructionBoxes, stats.constructionSkipped );
        if ( stats.terrainPatches || stats.terrainSkipped )
            Sys_Printf( "Plasticity export: %u unselected terrain sheet(s) -> OBJ meshes; "
                        "%u skipped (no evaluated grid).\n",
                        stats.terrainPatches, stats.terrainSkipped );
    }

    bool ExportPaths( std::string &stepPath, std::string &objPath,
                      std::string &groupName, std::string &error )
    {
        const char *mapPath = Radiant_CurrentMapPath();
        if ( !mapPath || !mapPath[0] )
        {
            error = "save the map before exporting the selection";
            return false;
        }

        char fullPath[MAX_PATH * 4];
        const DWORD length = ::GetFullPathNameA( mapPath, (DWORD)sizeof( fullPath ),
                                                 fullPath, nullptr );
        if ( !length || length >= (DWORD)sizeof( fullPath ) )
        {
            error = "could not resolve the map source path";
            return false;
        }

        const std::string source = fullPath;
        const size_t slash = source.find_last_of( "\\/" );
        const size_t filename = slash == std::string::npos ? 0 : slash + 1;
        const size_t dot = source.find_last_of( '.' );
        const size_t end = dot != std::string::npos && dot > filename ? dot : source.size();
        if ( end <= filename )
        {
            error = "map filename has no usable basename";
            return false;
        }

        groupName = source.substr( filename, end - filename ) + "_ref";
        stepPath = source.substr( 0, filename ) + groupName + ".step";
        objPath = source.substr( 0, filename ) + groupName + ".obj";
        return true;
    }

    std::string StepDouble( double value )
    {
        char text[64];
        _snprintf( text, sizeof( text ), "%.15g", value );
        text[sizeof( text ) - 1] = '\0';
        for ( char *scan = text; *scan; ++scan )
            if ( *scan == ',' )
                *scan = '.';
        return text;
    }

    std::string StepReference( int identifier )
    {
        char text[32];
        _snprintf( text, sizeof( text ), "#%d", identifier );
        text[sizeof( text ) - 1] = '\0';
        return text;
    }

    std::string StepReferenceList( const std::vector<int> &identifiers )
    {
        std::string list = "(";
        for ( size_t i = 0; i < identifiers.size(); ++i )
        {
            if ( i )
                list += ",";
            list += StepReference( identifiers[i] );
        }
        list += ")";
        return list;
    }

    std::string StepQuote( const std::string &value )
    {
        std::string quoted = "'";
        for ( size_t i = 0; i < value.size(); ++i )
        {
            quoted += value[i];
            if ( value[i] == '\'' )
                quoted += '\'';
        }
        quoted += "'";
        return quoted;
    }

    std::string StepTriple( const double value[3] )
    {
        return "(" + StepDouble( value[0] ) + "," + StepDouble( value[1] ) +
               "," + StepDouble( value[2] ) + ")";
    }

    struct StepEntityTable
    {
        std::vector<std::string> definitions;

        int Add( const std::string &definition )
        {
            definitions.push_back( definition );
            return (int)definitions.size();
        }
    };

    bool WriteStep( const std::string &path, const std::string &sourceGroupName,
                    const std::vector<StepSolid> &solids, std::string &error )
    {
        if ( solids.empty() )
        {
            error = "STEP export contains no brush solids";
            return false;
        }

        std::string groupName = sourceGroupName;
        SanitizeName( groupName );
        StepEntityTable table;
        const int application = table.Add( "APPLICATION_CONTEXT('automotive design')" );
        table.Add( "APPLICATION_PROTOCOL_DEFINITION('international standard',"
                   "'automotive_design',2010," + StepReference( application ) + ")" );
        const int productContext = table.Add(
            "PRODUCT_CONTEXT(''," + StepReference( application ) + ",'mechanical')" );
        const int product = table.Add(
            "PRODUCT(" + StepQuote( groupName ) + "," + StepQuote( groupName ) +
            ",'',(" + StepReference( productContext ) + "))" );
        const int formation = table.Add(
            "PRODUCT_DEFINITION_FORMATION('',''," + StepReference( product ) + ")" );
        const int definitionContext = table.Add(
            "PRODUCT_DEFINITION_CONTEXT('part definition'," +
            StepReference( application ) + ",'design')" );
        const int definition = table.Add(
            "PRODUCT_DEFINITION('design',''," + StepReference( formation ) + "," +
            StepReference( definitionContext ) + ")" );
        const int definitionShape = table.Add(
            "PRODUCT_DEFINITION_SHAPE('',''," + StepReference( definition ) + ")" );
        const int lengthUnit = table.Add(
            "(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT($,.METRE.))" );
        const int angleUnit = table.Add(
            "(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.))" );
        const int solidAngleUnit = table.Add(
            "(NAMED_UNIT(*)SI_UNIT($,.STERADIAN.)SOLID_ANGLE_UNIT())" );
        const int uncertainty = table.Add(
            "UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.0E-7)," +
            StepReference( lengthUnit ) + ",'distance_accuracy_value','')" );
        const int representationContext = table.Add(
            "(GEOMETRIC_REPRESENTATION_CONTEXT(3)"
            "GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((" + StepReference( uncertainty ) +
            "))GLOBAL_UNIT_ASSIGNED_CONTEXT((" + StepReference( lengthUnit ) + "," +
            StepReference( angleUnit ) + "," + StepReference( solidAngleUnit ) +
            "))REPRESENTATION_CONTEXT('',''))" );

        const double identityOriginValue[3] = { 0.0, 0.0, 0.0 };
        const double identityAxisValue[3] = { 0.0, 0.0, 1.0 };
        const double identityRefValue[3] = { 1.0, 0.0, 0.0 };
        const int identityOrigin = table.Add(
            "CARTESIAN_POINT(''," + StepTriple( identityOriginValue ) + ")" );
        const int identityAxis = table.Add(
            "DIRECTION(''," + StepTriple( identityAxisValue ) + ")" );
        const int identityRef = table.Add(
            "DIRECTION(''," + StepTriple( identityRefValue ) + ")" );
        const int identityPlacement = table.Add(
            "AXIS2_PLACEMENT_3D(''," + StepReference( identityOrigin ) + "," +
            StepReference( identityAxis ) + "," + StepReference( identityRef ) + ")" );

        std::vector<int> solidIdentifiers;
        solidIdentifiers.reserve( solids.size() );
        for ( size_t solidIndex = 0; solidIndex < solids.size(); ++solidIndex )
        {
            const StepSolid &solid = solids[solidIndex];
            std::vector<int> pointIdentifiers;
            std::vector<int> vertexIdentifiers;
            pointIdentifiers.reserve( solid.vertices.size() );
            vertexIdentifiers.reserve( solid.vertices.size() );
            for ( size_t vertexIndex = 0; vertexIndex < solid.vertices.size();
                  ++vertexIndex )
            {
                const double meters[3] = {
                    solid.vertices[vertexIndex].x * METERS_PER_UNIT,
                    solid.vertices[vertexIndex].y * METERS_PER_UNIT,
                    solid.vertices[vertexIndex].z * METERS_PER_UNIT };
                const int point = table.Add(
                    "CARTESIAN_POINT(''," + StepTriple( meters ) + ")" );
                pointIdentifiers.push_back( point );
                vertexIdentifiers.push_back( table.Add(
                    "VERTEX_POINT(''," + StepReference( point ) + ")" ) );
            }

            std::vector<int> edgeCurveIdentifiers;
            edgeCurveIdentifiers.reserve( solid.edges.size() );
            for ( size_t edgeIndex = 0; edgeIndex < solid.edges.size(); ++edgeIndex )
            {
                const StepEdge &edge = solid.edges[edgeIndex];
                const StepPoint &a = solid.vertices[edge.a];
                const StepPoint &b = solid.vertices[edge.b];
                double directionValue[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
                if ( !Normalize3( directionValue ) )
                {
                    error = "STEP edge direction became degenerate";
                    return false;
                }
                const int direction = table.Add(
                    "DIRECTION(''," + StepTriple( directionValue ) + ")" );
                const int vector = table.Add(
                    "VECTOR(''," + StepReference( direction ) + ",1.)" );
                const int line = table.Add(
                    "LINE(''," + StepReference( pointIdentifiers[edge.a] ) + "," +
                    StepReference( vector ) + ")" );
                edgeCurveIdentifiers.push_back( table.Add(
                    "EDGE_CURVE(''," + StepReference( vertexIdentifiers[edge.a] ) +
                    "," + StepReference( vertexIdentifiers[edge.b] ) + "," +
                    StepReference( line ) + ",.T.)" ) );
            }

            std::vector<int> faceIdentifiers;
            faceIdentifiers.reserve( solid.faces.size() );
            for ( size_t faceIndex = 0; faceIndex < solid.faces.size(); ++faceIndex )
            {
                const StepFace &face = solid.faces[faceIndex];
                std::vector<int> orientedEdges;
                orientedEdges.reserve( face.edgeUses.size() );
                for ( size_t useIndex = 0; useIndex < face.edgeUses.size(); ++useIndex )
                {
                    const StepEdgeUse &use = face.edgeUses[useIndex];
                    orientedEdges.push_back( table.Add(
                        "ORIENTED_EDGE('',*,*," +
                        StepReference( edgeCurveIdentifiers[use.edge] ) + "," +
                        ( use.forward ? ".T.)" : ".F.)" ) ) );
                }
                const int loop = table.Add(
                    "EDGE_LOOP(''," + StepReferenceList( orientedEdges ) + ")" );
                const int bound = table.Add(
                    "FACE_OUTER_BOUND(''," + StepReference( loop ) + ",.T.)" );
                const int axisDirection = table.Add(
                    "DIRECTION(''," + StepTriple( face.normal ) + ")" );

                int smallestComponent = 0;
                if ( fabs( face.normal[1] ) < fabs( face.normal[smallestComponent] ) )
                    smallestComponent = 1;
                if ( fabs( face.normal[2] ) < fabs( face.normal[smallestComponent] ) )
                    smallestComponent = 2;
                double referenceDirectionValue[3] = { 0.0, 0.0, 0.0 };
                referenceDirectionValue[smallestComponent] = 1.0;
                const double projection = face.normal[smallestComponent];
                referenceDirectionValue[0] -= projection * face.normal[0];
                referenceDirectionValue[1] -= projection * face.normal[1];
                referenceDirectionValue[2] -= projection * face.normal[2];
                if ( !Normalize3( referenceDirectionValue ) )
                {
                    error = "STEP plane reference direction became degenerate";
                    return false;
                }
                const int referenceDirection = table.Add(
                    "DIRECTION(''," + StepTriple( referenceDirectionValue ) + ")" );
                const int placement = table.Add(
                    "AXIS2_PLACEMENT_3D(''," +
                    StepReference( pointIdentifiers[face.vertices[0]] ) + "," +
                    StepReference( axisDirection ) + "," +
                    StepReference( referenceDirection ) + ")" );
                const int plane = table.Add(
                    "PLANE(''," + StepReference( placement ) + ")" );
                faceIdentifiers.push_back( table.Add(
                    "ADVANCED_FACE('',(" + StepReference( bound ) + ")," +
                    StepReference( plane ) + ",.T.)" ) );
            }
            const int shell = table.Add(
                "CLOSED_SHELL(''," + StepReferenceList( faceIdentifiers ) + ")" );
            solidIdentifiers.push_back( table.Add(
                "MANIFOLD_SOLID_BREP(" + StepQuote( solid.name ) + "," +
                StepReference( shell ) + ")" ) );
        }

        std::vector<int> representationItems = solidIdentifiers;
        representationItems.push_back( identityPlacement );
        const int representation = table.Add(
            "ADVANCED_BREP_SHAPE_REPRESENTATION(''," +
            StepReferenceList( representationItems ) + "," +
            StepReference( representationContext ) + ")" );
        table.Add( "SHAPE_DEFINITION_REPRESENTATION(" +
                   StepReference( definitionShape ) + "," +
                   StepReference( representation ) + ")" );

        FILE *file = fopen( path.c_str(), "wb" );
        if ( !file )
        {
            error = "could not create the STEP export";
            return false;
        }
        fprintf( file, "ISO-10303-21;\nHEADER;\n" );
        fprintf( file, "FILE_DESCRIPTION(('KIWI reference geometry'),'2;1');\n" );
        fprintf( file, "FILE_NAME(%s,'',('KIWI'),('KIWI Radiant'),'','','');\n",
                 StepQuote( groupName ).c_str() );
        fprintf( file, "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\n" );
        fprintf( file, "ENDSEC;\nDATA;\n" );
        for ( size_t i = 0; i < table.definitions.size(); ++i )
            fprintf( file, "#%u=%s;\n", (unsigned int)i + 1,
                     table.definitions[i].c_str() );
        fprintf( file, "ENDSEC;\nEND-ISO-10303-21;\n" );

        const bool writeFailed = ferror( file ) != 0;
        const bool closeFailed = fclose( file ) != 0;
        if ( writeFailed || closeFailed )
        {
            error = "could not finish the STEP export";
            return false;
        }
        return true;
    }

    bool WriteObj( const std::string &path, const std::vector<ObjObject> &objects,
                   std::string &error )
    {
        unsigned __int64 vertexCount = 0;
        for ( size_t i = 0; i < objects.size(); ++i )
            vertexCount += (unsigned __int64)objects[i].vertices.size();
        if ( vertexCount >= 0xFFFFFFFFui64 )
        {
            error = "OBJ vertex count exceeds the 32-bit index limit";
            return false;
        }

        FILE *file = fopen( path.c_str(), "wb" );
        if ( !file )
        {
            error = "could not create the OBJ export";
            return false;
        }

        fprintf( file, "# KIWI reference geometry\n" );
        fprintf( file, "# Units: meters; axes: X Y Z (Z-up)\n" );
        unsigned int vertexBase = 1;
        for ( size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex )
        {
            const ObjObject &object = objects[objectIndex];
            fprintf( file, "\no %s\n", object.name.c_str() );
            for ( size_t i = 0; i < object.vertices.size(); ++i )
            {
                const ObjVertex &vertex = object.vertices[i];
                fprintf( file, "v %.6f %.6f %.6f\n", vertex.x, vertex.y, vertex.z );
            }
            for ( size_t faceIndex = 0; faceIndex < object.faces.size(); ++faceIndex )
            {
                const std::vector<unsigned int> &face = object.faces[faceIndex];
                fputc( 'f', file );
                for ( size_t i = 0; i < face.size(); ++i )
                    fprintf( file, " %u", vertexBase + face[i] );
                fputc( '\n', file );
            }
            vertexBase += (unsigned int)object.vertices.size();
        }

        const bool writeFailed = ferror( file ) != 0;
        const bool closeFailed = fclose( file ) != 0;
        if ( writeFailed || closeFailed )
        {
            error = "could not finish the OBJ export";
            return false;
        }
        return true;
    }

    bool CopyPathToClipboard( const std::string &path )
    {
        if ( !::OpenClipboard( g_qeglobals.d_hwndMain ) )
            return false;
        if ( !::EmptyClipboard() )
        {
            ::CloseClipboard();
            return false;
        }

        HGLOBAL memory = ::GlobalAlloc( GMEM_MOVEABLE, path.size() + 1 );
        if ( !memory )
        {
            ::CloseClipboard();
            return false;
        }
        void *text = ::GlobalLock( memory );
        if ( !text )
        {
            ::GlobalFree( memory );
            ::CloseClipboard();
            return false;
        }
        memcpy( text, path.c_str(), path.size() + 1 );
        ::GlobalUnlock( memory );

        if ( !::SetClipboardData( CF_TEXT, memory ) )
        {
            ::GlobalFree( memory );
            ::CloseClipboard();
            return false;
        }
        ::CloseClipboard();
        return true;
    }

    // Explorer reveal is single-sourced in kiwi_windows.cpp (KiwiWindows_RevealInExplorer)
    // so the Models browser and this bridge cannot drift apart.

    bool AutoImportEnabled()
    {
        if ( s_autoImport < 0 )
            s_autoImport = Radiant_ProfileGetInt( PLASTICITY_PREF_SECTION,
                                                  "PlasticityAutoImport", 1 ) ? 1 : 0;
        return s_autoImport != 0;
    }

    void SetAutoImportEnabled( bool enabled )
    {
        const int value = enabled ? 1 : 0;
        if ( s_autoImport == value )
            return;
        s_autoImport = value;
        Radiant_ProfileSetInt( PLASTICITY_PREF_SECTION, "PlasticityAutoImport", value );
    }

    bool ExportConstructionEnabled()
    {
        if ( s_exportConstruction < 0 )
            s_exportConstruction = Radiant_ProfileGetInt( PLASTICITY_PREF_SECTION,
                                                          "PlasticityExportConstruction",
                                                          0 ) ? 1 : 0;
        return s_exportConstruction != 0;
    }

    void SetExportConstructionEnabled( bool enabled )
    {
        const int value = enabled ? 1 : 0;
        if ( s_exportConstruction == value )
            return;
        s_exportConstruction = value;
        Radiant_ProfileSetInt( PLASTICITY_PREF_SECTION, "PlasticityExportConstruction",
                               value );
    }

    bool ExportTerrainEnabled()
    {
        if ( s_exportTerrain < 0 )
            s_exportTerrain = Radiant_ProfileGetInt( PLASTICITY_PREF_SECTION,
                                                     "PlasticityExportTerrain", 1 ) ? 1 : 0;
        return s_exportTerrain != 0;
    }

    void SetExportTerrainEnabled( bool enabled )
    {
        const int value = enabled ? 1 : 0;
        if ( s_exportTerrain == value )
            return;
        s_exportTerrain = value;
        Radiant_ProfileSetInt( PLASTICITY_PREF_SECTION, "PlasticityExportTerrain", value );
    }

    int ClampThicknessMilli( int milli )
    {
        if ( milli < CONSTRUCTION_THICKNESS_MIN_MILLI )
            return CONSTRUCTION_THICKNESS_MIN_MILLI;
        if ( milli > CONSTRUCTION_THICKNESS_MAX_MILLI )
            return CONSTRUCTION_THICKNESS_MAX_MILLI;
        return milli;
    }

    // Prism cross-section width in Radiant units.
    double ConstructionThickness()
    {
        if ( s_constructionThicknessMilli < 0 )
            s_constructionThicknessMilli = ClampThicknessMilli(
                Radiant_ProfileGetInt( PLASTICITY_PREF_SECTION,
                                       "PlasticityConstructionThicknessMilli",
                                       CONSTRUCTION_THICKNESS_DEFAULT_MILLI ) );
        return s_constructionThicknessMilli / 1000.0;
    }

    void SetConstructionThickness( double units )
    {
        if ( !FiniteDouble( units ) )
            return;
        const int value = ClampThicknessMilli( (int)floor( units * 1000.0 + 0.5 ) );
        if ( s_constructionThicknessMilli == value )
            return;
        s_constructionThicknessMilli = value;
        Radiant_ProfileSetInt( PLASTICITY_PREF_SECTION,
                               "PlasticityConstructionThicknessMilli", value );
    }

    enum ExportFileKind
    {
        EXPORT_FILE_STEP,
        EXPORT_FILE_OBJ
    };

    struct ExportFile
    {
        ExportFileKind kind;
        std::string path;
        unsigned int objectCount;
        unsigned __int64 faceCount;
        std::string dialogRoute;
        bool acceptedOptions;

        ExportFile()
            : kind( EXPORT_FILE_OBJ ), objectCount( 0 ), faceCount( 0 ),
              acceptedOptions( false ) {}
    };

    struct AutoImportJob
    {
        std::vector<ExportFile> files;
        std::string groupName;
        bool windowChosen;
        bool usedForeground;
        HWND chosenWindow;
        DWORD processId;
        std::string windowTitle;
        size_t failedFile;
        bool success;
        std::string failure;

        AutoImportJob()
            : windowChosen( false ), usedForeground( false ),
              chosenWindow( nullptr ), processId( 0 ), failedFile( 0 ),
              success( false ) {}
    };

    void FallbackToExplorer( const AutoImportJob &job, const char *reason )
    {
        if ( job.files.empty() )
            return;
        const size_t failed = job.failedFile < job.files.size() ? job.failedFile : 0;
        const std::string &failedPath = job.files[failed].path;
        Sys_Printf( "Plasticity auto-import: failed on \"%s\": %s\n",
                    failedPath.c_str(), reason );
        Sys_Printf( "Plasticity auto-import: fell back to Explorer for all exported files.\n" );
        for ( size_t i = 0; i < job.files.size(); ++i )
        {
            const ExportFile &exported = job.files[i];
            if ( exported.kind == EXPORT_FILE_STEP )
                Sys_Printf( "Plasticity STEP export: wrote \"%s\" (%u brush solids, "
                            "%I64u faces). Drag the file into the Plasticity viewport "
                            "to import (delete the previous \"%s\" product there first "
                            "if re-importing).\n", exported.path.c_str(),
                            exported.objectCount, exported.faceCount,
                            job.groupName.c_str() );
            else
                Sys_Printf( "Plasticity OBJ export: wrote \"%s\" (%u objects, "
                            "%I64u faces). Drag the file into the Plasticity viewport "
                            "to import (delete the previous \"%s\" group there first "
                            "if re-importing).\n", exported.path.c_str(),
                            exported.objectCount, exported.faceCount,
                            job.groupName.c_str() );
        }

        const bool copied = CopyPathToClipboard( failedPath );
        if ( !copied )
            Sys_Printf( "Plasticity export: could not copy failed path \"%s\" to "
                        "the clipboard.\n", failedPath.c_str() );
        for ( size_t i = 0; i < job.files.size(); ++i )
            if ( !KiwiWindows_RevealInExplorer( job.files[i].path.c_str() ) )
                Sys_Printf( "Plasticity export: could not open Explorer for \"%s\".\n",
                            job.files[i].path.c_str() );
    }

    LRESULT CALLBACK AutoImportResultWindowProc( HWND window, UINT message,
                                                  WPARAM wParam, LPARAM lParam )
    {
        if ( message != AUTO_IMPORT_RESULT_MESSAGE )
            return ::DefWindowProcA( window, message, wParam, lParam );

        AutoImportJob *job = (AutoImportJob *)lParam;
        if ( !job )
        {
            ::InterlockedExchange( &s_autoImportBusy, 0 );
            return 0;
        }

        if ( job->windowChosen )
        {
            Sys_Printf( "Plasticity auto-import: chose %s window %p (PID %lu, title \"%s\").\n",
                        job->usedForeground ? "foreground" : "topmost Z-order",
                        (void *)job->chosenWindow, (unsigned long)job->processId,
                        job->windowTitle.empty() ? "<untitled>" : job->windowTitle.c_str() );
        }
        for ( size_t i = 0; i < job->files.size(); ++i )
        {
            const ExportFile &exported = job->files[i];
            if ( !exported.dialogRoute.empty() )
                Sys_Printf( "Plasticity auto-import: submitted \"%s\" via %s.\n",
                            exported.path.c_str(), exported.dialogRoute.c_str() );
            if ( exported.acceptedOptions )
                Sys_Printf( "Plasticity auto-import: \"%s\" import-options window "
                            "appeared; pressed Enter once to accept defaults.\n",
                            exported.path.c_str() );
        }

        if ( job->success )
        {
            std::string paths;
            for ( size_t i = 0; i < job->files.size(); ++i )
            {
                if ( i )
                    paths += ", ";
                paths += job->files[i].path;
            }
            Sys_Printf( "Plasticity export: sent to Plasticity: %s\n", paths.c_str() );
        }
        else
            FallbackToExplorer( *job, job->failure.c_str() );
        delete job;
        ::InterlockedExchange( &s_autoImportBusy, 0 );
        return 0;
    }

    bool EnsureAutoImportResultWindow()
    {
        if ( ::IsWindow( s_autoImportResultWindow ) )
            return true;

        const char *className = "KiwiPlasticityAutoImportResult";
        const HINSTANCE instance = ::GetModuleHandleA( nullptr );
        WNDCLASSA windowClass;
        memset( &windowClass, 0, sizeof( windowClass ) );
        windowClass.lpfnWndProc = AutoImportResultWindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = className;
        if ( !::RegisterClassA( &windowClass ) &&
             ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
            return false;

        // KIWI: The message-only window returns worker results to Radiant's UI thread.
        s_autoImportResultWindow = ::CreateWindowExA(
            0, className, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr );
        return s_autoImportResultWindow != nullptr;
    }

    typedef bool ( *PollCondition )( void *context );

    // KIWI: All automation waits yield in 50 ms slices so the worker stays bounded.
    bool PollFor( PollCondition condition, void *context, DWORD timeoutMs )
    {
        const DWORD started = ::GetTickCount();
        for ( ;; )
        {
            if ( condition( context ) )
                return true;
            if ( ::GetTickCount() - started >= timeoutMs )
                return condition( context );
            ::Sleep( 50 );
        }
    }

    struct DelayPoll
    {
        DWORD started;
        DWORD delayMs;
    };

    bool HasDelayElapsed( void *rawContext )
    {
        DelayPoll *context = (DelayPoll *)rawContext;
        return ::GetTickCount() - context->started >= context->delayMs;
    }

    void PollDelay( DWORD delayMs )
    {
        DelayPoll context = { ::GetTickCount(), delayMs };
        PollFor( HasDelayElapsed, &context, delayMs );
    }

    bool HasWindowClass( HWND window, const char *wanted )
    {
        char className[64] = { 0 };
        return ::GetClassNameA( window, className, sizeof( className ) ) > 0 &&
               strcmp( className, wanted ) == 0;
    }

    bool IsPlasticityProcess( DWORD processId )
    {
        HANDLE process = ::OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                        processId );
        if ( !process )
            process = ::OpenProcess( PROCESS_QUERY_INFORMATION, FALSE, processId );
        if ( !process )
            return false;

        char imagePath[MAX_PATH * 4] = { 0 };
        DWORD imagePathLength = (DWORD)sizeof( imagePath );
        const BOOL queried = ::QueryFullProcessImageNameA( process, 0, imagePath,
                                                           &imagePathLength );
        ::CloseHandle( process );
        if ( !queried || !imagePathLength )
            return false;

        const char *baseName = imagePath;
        for ( const char *scan = imagePath; *scan; ++scan )
            if ( *scan == '\\' || *scan == '/' )
                baseName = scan + 1;
        return _stricmp( baseName, PLASTICITY_PROCESS ) == 0;
    }

    struct PlasticityWindow
    {
        HWND window;
        DWORD processId;
        std::string title;
    };

    struct PlasticityWindowSearch
    {
        std::vector<PlasticityWindow> candidates;
    };

    BOOL CALLBACK CollectPlasticityWindows( HWND window, LPARAM parameter )
    {
        PlasticityWindowSearch *search = (PlasticityWindowSearch *)parameter;
        if ( !::IsWindowVisible( window ) ||
             HasWindowClass( window, PLASTICITY_DIALOG_CLASS ) )
            return TRUE;

        DWORD processId = 0;
        ::GetWindowThreadProcessId( window, &processId );
        if ( !processId || !IsPlasticityProcess( processId ) )
            return TRUE;

        char title[256] = { 0 };
        ::GetWindowTextA( window, title, sizeof( title ) );
        PlasticityWindow candidate;
        candidate.window = window;
        candidate.processId = processId;
        candidate.title = title;
        search->candidates.push_back( candidate );
        return TRUE;
    }

    bool FindPlasticityWindow( PlasticityWindow &chosen, bool &usedForeground )
    {
        PlasticityWindowSearch search;
        ::EnumWindows( CollectPlasticityWindows, (LPARAM)&search );
        if ( search.candidates.empty() )
            return false;

        const HWND foreground = ::GetForegroundWindow();
        for ( size_t i = 0; i < search.candidates.size(); ++i )
        {
            if ( search.candidates[i].window == foreground )
            {
                chosen = search.candidates[i];
                usedForeground = true;
                return true;
            }
        }

        // KIWI: EnumWindows is top-to-bottom Z-order, so its first match is topmost.
        chosen = search.candidates[0];
        usedForeground = false;
        return true;
    }

    void SetKeyInput( INPUT &input, WORD key, DWORD flags )
    {
        memset( &input, 0, sizeof( input ) );
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = flags;
    }

    bool SendAltPulse()
    {
        INPUT inputs[2];
        SetKeyInput( inputs[0], VK_MENU, 0 );
        SetKeyInput( inputs[1], VK_MENU, KEYEVENTF_KEYUP );
        const UINT sent = ::SendInput( 2, inputs, sizeof( INPUT ) );
        if ( sent == 2 )
            return true;
        INPUT release;
        SetKeyInput( release, VK_MENU, KEYEVENTF_KEYUP );
        ::SendInput( 1, &release, sizeof( INPUT ) );
        return false;
    }

    struct ForegroundPoll
    {
        HWND window;
    };

    bool IsForegroundWindow( void *rawContext )
    {
        ForegroundPoll *context = (ForegroundPoll *)rawContext;
        return ::GetForegroundWindow() == context->window;
    }

    // KIWI: AttachThreadInput plus the ALT pulse cover Windows' foreground lock.
    bool ForceForegroundWindow( HWND window )
    {
        if ( !::IsWindow( window ) )
            return false;
        if ( ::IsIconic( window ) )
            ::ShowWindow( window, SW_RESTORE );

        ForegroundPoll poll = { window };
        ::SetForegroundWindow( window );
        if ( PollFor( IsForegroundWindow, &poll, 100 ) )
            return true;

        const DWORD currentThread = ::GetCurrentThreadId();
        const DWORD targetThread = ::GetWindowThreadProcessId( window, nullptr );
        const HWND oldForeground = ::GetForegroundWindow();
        const DWORD foregroundThread = oldForeground
                                           ? ::GetWindowThreadProcessId( oldForeground, nullptr )
                                           : 0;
        const bool attachedForeground = foregroundThread &&
                                        foregroundThread != currentThread &&
                                        ::AttachThreadInput( currentThread,
                                                             foregroundThread, TRUE ) != FALSE;
        const bool attachedTarget = targetThread && targetThread != currentThread &&
                                    targetThread != foregroundThread &&
                                    ::AttachThreadInput( currentThread,
                                                         targetThread, TRUE ) != FALSE;

        ::BringWindowToTop( window );
        ::SetForegroundWindow( window );

        if ( attachedTarget )
            ::AttachThreadInput( currentThread, targetThread, FALSE );
        if ( attachedForeground )
            ::AttachThreadInput( currentThread, foregroundThread, FALSE );
        if ( PollFor( IsForegroundWindow, &poll, 100 ) )
            return true;

        if ( !SendAltPulse() )
            return false;
        ::SetForegroundWindow( window );
        ::BringWindowToTop( window );
        return PollFor( IsForegroundWindow, &poll, 100 );
    }

    void ReleaseChordModifiers()
    {
        INPUT releases[3];
        SetKeyInput( releases[0], VK_SHIFT, KEYEVENTF_KEYUP );
        SetKeyInput( releases[1], VK_CONTROL, KEYEVENTF_KEYUP );
        SetKeyInput( releases[2], VK_MENU, KEYEVENTF_KEYUP );
        ::SendInput( 3, releases, sizeof( INPUT ) );
    }

    bool SendCtrlShiftO()
    {
        INPUT inputs[6];
        SetKeyInput( inputs[0], VK_CONTROL, 0 );
        SetKeyInput( inputs[1], VK_SHIFT, 0 );
        SetKeyInput( inputs[2], 'O', 0 );
        SetKeyInput( inputs[3], 'O', KEYEVENTF_KEYUP );
        SetKeyInput( inputs[4], VK_SHIFT, KEYEVENTF_KEYUP );
        SetKeyInput( inputs[5], VK_CONTROL, KEYEVENTF_KEYUP );
        if ( ::SendInput( 6, inputs, sizeof( INPUT ) ) == 6 )
            return true;
        ReleaseChordModifiers();
        return false;
    }

    bool SendCtrlA()
    {
        INPUT inputs[4];
        SetKeyInput( inputs[0], VK_CONTROL, 0 );
        SetKeyInput( inputs[1], 'A', 0 );
        SetKeyInput( inputs[2], 'A', KEYEVENTF_KEYUP );
        SetKeyInput( inputs[3], VK_CONTROL, KEYEVENTF_KEYUP );
        if ( ::SendInput( 4, inputs, sizeof( INPUT ) ) == 4 )
            return true;
        ReleaseChordModifiers();
        return false;
    }

    bool SendEnter()
    {
        INPUT inputs[2];
        SetKeyInput( inputs[0], VK_RETURN, 0 );
        SetKeyInput( inputs[1], VK_RETURN, KEYEVENTF_KEYUP );
        const UINT sent = ::SendInput( 2, inputs, sizeof( INPUT ) );
        if ( sent == 2 )
            return true;
        INPUT release;
        SetKeyInput( release, VK_RETURN, KEYEVENTF_KEYUP );
        ::SendInput( 1, &release, sizeof( INPUT ) );
        return false;
    }

    bool SendUnicodeText( const std::string &text )
    {
        const int wideLength = ::MultiByteToWideChar( CP_ACP, 0, text.c_str(), -1,
                                                       nullptr, 0 );
        if ( wideLength <= 1 )
            return false;
        std::vector<wchar_t> wide( (size_t)wideLength );
        if ( !::MultiByteToWideChar( CP_ACP, 0, text.c_str(), -1, &wide[0],
                                     wideLength ) )
            return false;

        for ( int i = 0; i + 1 < wideLength; ++i )
        {
            INPUT inputs[2];
            SetKeyInput( inputs[0], 0, KEYEVENTF_UNICODE );
            SetKeyInput( inputs[1], 0, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP );
            inputs[0].ki.wScan = wide[(size_t)i];
            inputs[1].ki.wScan = wide[(size_t)i];
            if ( ::SendInput( 2, inputs, sizeof( INPUT ) ) != 2 )
            {
                ::SendInput( 1, &inputs[1], sizeof( INPUT ) );
                return false;
            }
        }
        return true;
    }

    struct ProcessWindowCollector
    {
        DWORD processId;
        bool dialogsOnly;
        std::vector<HWND> windows;
    };

    BOOL CALLBACK CollectProcessWindows( HWND window, LPARAM parameter )
    {
        ProcessWindowCollector *collector = (ProcessWindowCollector *)parameter;
        DWORD processId = 0;
        ::GetWindowThreadProcessId( window, &processId );
        if ( processId == collector->processId && ::IsWindowVisible( window ) &&
             ( !collector->dialogsOnly ||
               HasWindowClass( window, PLASTICITY_DIALOG_CLASS ) ) )
            collector->windows.push_back( window );
        return TRUE;
    }

    void CaptureProcessWindows( DWORD processId, bool dialogsOnly,
                                std::vector<HWND> &windows )
    {
        ProcessWindowCollector collector;
        collector.processId = processId;
        collector.dialogsOnly = dialogsOnly;
        ::EnumWindows( CollectProcessWindows, (LPARAM)&collector );
        windows.swap( collector.windows );
    }

    bool ContainsWindow( const std::vector<HWND> &windows, HWND wanted )
    {
        for ( size_t i = 0; i < windows.size(); ++i )
            if ( windows[i] == wanted )
                return true;
        return false;
    }

    struct NewProcessWindowPoll
    {
        DWORD processId;
        bool dialogsOnly;
        const std::vector<HWND> *baseline;
        HWND found;
    };

    BOOL CALLBACK FindNewProcessWindow( HWND window, LPARAM parameter )
    {
        NewProcessWindowPoll *poll = (NewProcessWindowPoll *)parameter;
        DWORD processId = 0;
        ::GetWindowThreadProcessId( window, &processId );
        if ( processId != poll->processId || !::IsWindowVisible( window ) ||
             ( poll->dialogsOnly &&
               !HasWindowClass( window, PLASTICITY_DIALOG_CLASS ) ) ||
             ContainsWindow( *poll->baseline, window ) )
            return TRUE;
        poll->found = window;
        return FALSE;
    }

    bool HasNewProcessWindow( void *rawContext )
    {
        NewProcessWindowPoll *context = (NewProcessWindowPoll *)rawContext;
        context->found = nullptr;
        ::EnumWindows( FindNewProcessWindow, (LPARAM)context );
        return context->found != nullptr;
    }

    struct WindowClosedPoll
    {
        HWND window;
    };

    bool IsWindowClosed( void *rawContext )
    {
        WindowClosedPoll *context = (WindowClosedPoll *)rawContext;
        return !::IsWindow( context->window ) || !::IsWindowVisible( context->window );
    }

    HWND FindFilenameEdit( HWND dialog )
    {
        for ( HWND comboEx = nullptr;
              ( comboEx = ::FindWindowExA( dialog, comboEx, "ComboBoxEx32", nullptr ) ) != nullptr; )
        {
            for ( HWND combo = nullptr;
                  ( combo = ::FindWindowExA( comboEx, combo, "ComboBox", nullptr ) ) != nullptr; )
            {
                HWND edit = ::FindWindowExA( combo, nullptr, "Edit", nullptr );
                if ( edit )
                    return edit;
            }
        }
        return nullptr;
    }

    // KIWI: Primary dialog route is filename Edit/WM_SETTEXT plus posted IDOK.
    bool TryDialogMessageRoute( HWND dialog, const std::string &path )
    {
        HWND edit = FindFilenameEdit( dialog );
        if ( !edit )
            return false;

        DWORD_PTR messageResult = 0;
        if ( !::SendMessageTimeoutA( edit, WM_SETTEXT, 0, (LPARAM)path.c_str(),
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 500,
                                     &messageResult ) || !messageResult )
            return false;
        return ::PostMessageA( dialog, WM_COMMAND, IDOK, 0 ) != FALSE;
    }

    enum DialogDriveRoute
    {
        DIALOG_DRIVE_NONE,
        DIALOG_DRIVE_MESSAGE,
        DIALOG_DRIVE_UNICODE
    };

    bool DriveImportDialog( HWND dialog, const std::string &path,
                            DialogDriveRoute &route, const char *&failure )
    {
        WindowClosedPoll closed = { dialog };
        if ( TryDialogMessageRoute( dialog, path ) )
        {
            if ( PollFor( IsWindowClosed, &closed, 500 ) )
            {
                route = DIALOG_DRIVE_MESSAGE;
                return true;
            }
        }

        if ( IsWindowClosed( &closed ) )
        {
            route = DIALOG_DRIVE_MESSAGE;
            return true;
        }
        if ( !ForceForegroundWindow( dialog ) )
        {
            failure = "could not bring the import dialog to the foreground.";
            return false;
        }
        PollDelay( 100 );
        if ( ::GetForegroundWindow() != dialog )
        {
            failure = "import dialog lost the foreground before the file path was typed.";
            return false;
        }
        if ( !SendCtrlA() )
        {
            failure = "could not select the import dialog filename field.";
            return false;
        }
        if ( !SendUnicodeText( path ) )
        {
            failure = "could not type the file path into the import dialog.";
            return false;
        }
        if ( !SendEnter() )
        {
            failure = "could not submit the file path to the import dialog.";
            return false;
        }
        if ( !PollFor( IsWindowClosed, &closed, 1000 ) )
        {
            failure = "import dialog did not close after submitting the file path.";
            return false;
        }

        route = DIALOG_DRIVE_UNICODE;
        return true;
    }

    bool RunAutoImportFile( const PlasticityWindow &mainWindow,
                            ExportFile &exported, std::string &failureText )
    {
        std::vector<HWND> dialogBaseline;
        CaptureProcessWindows( mainWindow.processId, true, dialogBaseline );
        if ( !ForceForegroundWindow( mainWindow.window ) )
        {
            failureText = "could not bring the Plasticity window to the foreground.";
            return false;
        }
        PollDelay( 200 );
        if ( ::GetForegroundWindow() != mainWindow.window )
        {
            failureText = "Plasticity lost the foreground before Ctrl+Shift+O was sent.";
            return false;
        }
        if ( !SendCtrlShiftO() )
        {
            failureText = "could not send Ctrl+Shift+O.";
            return false;
        }

        NewProcessWindowPoll importDialogPoll;
        importDialogPoll.processId = mainWindow.processId;
        importDialogPoll.dialogsOnly = true;
        importDialogPoll.baseline = &dialogBaseline;
        importDialogPoll.found = nullptr;
        if ( !PollFor( HasNewProcessWindow, &importDialogPoll, 3000 ) )
        {
            failureText =
                "import dialog did not open - expected Ctrl+Shift+O = file:import; "
                "is that still the Plasticity default?";
            return false;
        }

        const HWND importDialog = importDialogPoll.found;
        std::vector<HWND> optionsBaseline;
        CaptureProcessWindows( mainWindow.processId, false, optionsBaseline );
        for ( size_t i = 0; i < optionsBaseline.size(); ++i )
        {
            if ( optionsBaseline[i] == importDialog )
            {
                optionsBaseline.erase( optionsBaseline.begin() + i );
                break;
            }
        }

        DialogDriveRoute route = DIALOG_DRIVE_NONE;
        const char *failure = nullptr;
        if ( !DriveImportDialog( importDialog, exported.path, route, failure ) )
        {
            failureText = failure ? failure : "could not drive the import dialog.";
            return false;
        }
        exported.dialogRoute = route == DIALOG_DRIVE_MESSAGE
                                   ? "filename Edit/WM_SETTEXT + IDOK"
                                   : "Unicode SendInput + Enter";

        NewProcessWindowPoll optionsPoll;
        optionsPoll.processId = mainWindow.processId;
        optionsPoll.dialogsOnly = false;
        optionsPoll.baseline = &optionsBaseline;
        optionsPoll.found = nullptr;
        if ( PollFor( HasNewProcessWindow, &optionsPoll, 2000 ) )
        {
            if ( !ForceForegroundWindow( optionsPoll.found ) )
            {
                failureText =
                    "could not bring the import-options window to the foreground.";
                return false;
            }
            PollDelay( 100 );
            if ( ::GetForegroundWindow() != optionsPoll.found )
            {
                failureText =
                    "import-options window lost the foreground before defaults were accepted.";
                return false;
            }
            if ( !SendEnter() )
            {
                failureText = "could not send Enter to the import-options window.";
                return false;
            }
            exported.acceptedOptions = true;
            WindowClosedPoll optionsClosed = { optionsPoll.found };
            if ( !PollFor( IsWindowClosed, &optionsClosed, 1000 ) )
            {
                failureText =
                    "import-options window did not close after defaults were accepted.";
                return false;
            }
        }
        return true;
    }

    void RunAutoImport( AutoImportJob &job )
    {
        PlasticityWindow mainWindow;
        bool usedForeground = false;
        if ( !FindPlasticityWindow( mainWindow, usedForeground ) )
        {
            job.failure = "Plasticity not running.";
            return;
        }

        job.windowChosen = true;
        job.usedForeground = usedForeground;
        job.chosenWindow = mainWindow.window;
        job.processId = mainWindow.processId;
        job.windowTitle = mainWindow.title;

        for ( size_t fileIndex = 0; fileIndex < job.files.size(); ++fileIndex )
        {
            job.failedFile = fileIndex;
            if ( !RunAutoImportFile( mainWindow, job.files[fileIndex], job.failure ) )
                return;
        }

        job.success = true;
    }

    unsigned __stdcall AutoImportThread( void *rawJob )
    {
        AutoImportJob *job = (AutoImportJob *)rawJob;
        RunAutoImport( *job );
        if ( !::PostMessageA( s_autoImportResultWindow, AUTO_IMPORT_RESULT_MESSAGE,
                              0, (LPARAM)job ) )
        {
            delete job;
            ::InterlockedExchange( &s_autoImportBusy, 0 );
        }
        return 0;
    }

    void StartAutoImport( const AutoImportJob &source )
    {
        if ( source.files.empty() )
            return;
        if ( !AutoImportEnabled() )
        {
            FallbackToExplorer( source, "PlasticityAutoImport is OFF." );
            return;
        }
        if ( !EnsureAutoImportResultWindow() )
        {
            FallbackToExplorer(
                source, "could not create the auto-import UI result window." );
            return;
        }
        if ( ::InterlockedCompareExchange( &s_autoImportBusy, 1, 0 ) != 0 )
        {
            FallbackToExplorer( source, "another Plasticity auto-import is already in progress." );
            return;
        }

        AutoImportJob *job = new ( std::nothrow ) AutoImportJob( source );
        if ( !job )
        {
            ::InterlockedExchange( &s_autoImportBusy, 0 );
            FallbackToExplorer( source, "could not allocate the auto-import worker job." );
            return;
        }

        HANDLE thread = (HANDLE)::_beginthreadex( nullptr, 0, AutoImportThread,
                                                   job, 0, nullptr );
        if ( !thread )
        {
            delete job;
            ::InterlockedExchange( &s_autoImportBusy, 0 );
            FallbackToExplorer( source, "could not start the auto-import worker thread." );
            return;
        }
        ::CloseHandle( thread );
    }

    // `handOff` false writes the files and only prints their paths (test mode);
    // true runs the auto-import, which itself falls back to Explorer when the
    // PlasticityAutoImport preference is off.
    bool ExecuteExport( bool includeConstruction, bool includeTerrain, bool handOff )
    {
        std::string stepPath;
        std::string objPath;
        std::string groupName;
        std::string error;
        if ( !ExportPaths( stepPath, objPath, groupName, error ) )
        {
            Sys_Printf( "Plasticity export: %s.\n", error.c_str() );
            return false;
        }

        std::vector<StepSolid> solids;
        std::vector<ObjObject> objects;
        ExportStats stats;
        const bool gathered = GatherObjects( solids, objects, stats, error );
        if ( includeConstruction )
            GatherConstruction( ConstructionThickness(), solids, stats );
        if ( includeTerrain )
            GatherTerrain( objects, stats );
        if ( !gathered && solids.empty() && objects.empty() )
        {
            Sys_Printf( "Plasticity export: %s.\n", error.c_str() );
            PrintExportStats( stats );
            return false;
        }
        PrintExportStats( stats );

        AutoImportJob job;
        job.groupName = groupName;
        if ( !solids.empty() )
        {
            if ( !WriteStep( stepPath, groupName, solids, error ) )
            {
                Sys_Printf( "Plasticity STEP export: \"%s\": %s.\n",
                            stepPath.c_str(), error.c_str() );
                return false;
            }
            ExportFile exported;
            exported.kind = EXPORT_FILE_STEP;
            exported.path = stepPath;
            exported.objectCount = (unsigned int)solids.size();
            exported.faceCount = stats.stepFaces;
            job.files.push_back( exported );
        }
        if ( !objects.empty() )
        {
            if ( !WriteObj( objPath, objects, error ) )
            {
                Sys_Printf( "Plasticity OBJ export: \"%s\": %s.\n",
                            objPath.c_str(), error.c_str() );
                return false;
            }
            ExportFile exported;
            exported.kind = EXPORT_FILE_OBJ;
            exported.path = objPath;
            exported.objectCount = (unsigned int)objects.size();
            exported.faceCount = stats.objFaces;
            job.files.push_back( exported );
        }
        if ( !handOff )
        {
            for ( size_t i = 0; i < job.files.size(); ++i )
                Sys_Printf( "Plasticity export: wrote \"%s\" (%u object(s)).\n",
                            job.files[i].path.c_str(), job.files[i].objectCount );
            return true;
        }
        StartAutoImport( job );
        return true;
    }

    unsigned int SelectedObjectCount()
    {
        unsigned int count = 0;
        for ( selbrush_t *selected = selected_brushes.next;
              selected && selected != &selected_brushes; selected = selected->next )
            ++count;
        return count;
    }

    void DrawConstructionOptions( int conObjects, int conSegments )
    {
        bool includeConstruction = ExportConstructionEnabled();
        char label[160];
        _snprintf( label, sizeof( label ),
                   "Include construction lines (%d visible object(s), %d segment(s))",
                   conObjects, conSegments );
        label[sizeof( label ) - 1] = '\0';
        if ( ImGui::Checkbox( label, &includeConstruction ) )
            SetExportConstructionEnabled( includeConstruction );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Plasticity cannot import construction lines, so each visible\n"
                               "one is written into the brush STEP as a tiny square prism.\n"
                               "One long edge of every prism lies EXACTLY on the line:\n"
                               "snap to that edge in Plasticity.  Planar objects become one\n"
                               "mitred prism (a ring for circles/rects); non-planar polylines\n"
                               "fall back to one box per segment.  Hidden objects are skipped." );
        if ( !includeConstruction )
            return;
        float thickness = (float)ConstructionThickness();
        ImGui::SetNextItemWidth( 140.0f );
        if ( ImGui::InputFloat( "Prism thickness (units)", &thickness, 0.005f, 0.05f, "%.3f" ) )
            SetConstructionThickness( (double)thickness );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Cross-section width of the construction prisms, in Radiant\n"
                               "units (inches).  0.020 = 0.5 mm.  Range %.3f .. %.1f.",
                               CONSTRUCTION_THICKNESS_MIN_MILLI / 1000.0,
                               CONSTRUCTION_THICKNESS_MAX_MILLI / 1000.0 );
    }
}

// Top-level window scope: the modal popup must be opened/drawn outside any other
// window's Begin/End pair (imgui_shell.cpp calls this beside KiwiImport_Draw).
void KiwiPlastBridge_Draw()
{
    if ( s_dialogRequest )
    {
        ImGui::OpenPopup( PLASTICITY_DIALOG_TITLE );
        s_dialogRequest = false;
        s_dialogOpen = true;
    }
    if ( !s_dialogOpen )
        return;
    if ( !ImGui::BeginPopupModal( PLASTICITY_DIALOG_TITLE, nullptr,
                                  ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        s_dialogOpen = false;           // dismissed externally: nothing was written
        return;
    }

    std::string stepPath;
    std::string objPath;
    std::string groupName;
    std::string error;
    const bool pathsOk = ExportPaths( stepPath, objPath, groupName, error );
    const unsigned int selectedCount = SelectedObjectCount();
    int conObjects = 0;
    int conSegments = 0;
    CountVisibleConstruction( conObjects, conSegments );

    if ( pathsOk )
    {
        ImGui::Text( "Brush STEP:        %s", stepPath.c_str() );
        ImGui::Text( "Patch/model OBJ:   %s", objPath.c_str() );
    }
    else
        ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.3f, 1.0f ), "%s", error.c_str() );
    ImGui::Text( "%u selected object(s) -> STEP brushes, OBJ patches/models", selectedCount );
    ImGui::Separator();

    DrawConstructionOptions( conObjects, conSegments );
    const bool includeConstruction = ExportConstructionEnabled();

    // KIWI (2026-09-13): terrain sheets, selected or not, as OBJ meshes.
    const unsigned int terrainCount = VisibleTerrainCount();
    {
        bool includeTerrainOpt = ExportTerrainEnabled();
        char tlabel[160];
        _snprintf( tlabel, sizeof( tlabel ),
                   "Include all terrain as meshes (%u visible unselected sheet(s))", terrainCount );
        tlabel[sizeof( tlabel ) - 1] = '\0';
        if ( ImGui::Checkbox( tlabel, &includeTerrainOpt ) )
            SetExportTerrainEnabled( includeTerrainOpt );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Every visible terrain patch goes into the OBJ as one mesh\n"
                               "(its evaluated render grid), whether or not it is selected.\n"
                               "Selected terrain is exported either way." );
    }
    const bool includeTerrain = ExportTerrainEnabled();

    bool autoImport = AutoImportEnabled();
    if ( ImGui::Checkbox( "Auto-import into Plasticity", &autoImport ) )
        SetAutoImportEnabled( autoImport );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Drives Plasticity's Ctrl+Shift+O file:import for each file.\n"
                           "Off: the files are written and revealed in Explorer." );
    ImGui::Separator();

    const bool canSend = pathsOk &&
                         ( selectedCount > 0 || ( includeConstruction && conObjects > 0 )
                           || ( includeTerrain && terrainCount > 0 ) );
    const bool enter = ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
                       ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false );
    ImGui::BeginDisabled( !canSend );
    const bool send = ImGui::Button( "Send", ImVec2( 140.0f, 0.0f ) ) || ( canSend && enter );
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button( "Cancel", ImVec2( 140.0f, 0.0f ) ) ||
                        ImGui::IsKeyPressed( ImGuiKey_Escape, false );
    if ( send || cancel )
    {
        ImGui::CloseCurrentPopup();
        s_dialogOpen = false;
    }
    ImGui::EndPopup();

    if ( send )
        ExecuteExport( includeConstruction, includeTerrain, true );
}

bool KiwiPlastBridge_ExportNow( bool includeConstruction, bool includeTerrain, bool handOff )
{
    return ExecuteExport( includeConstruction, includeTerrain, handOff );
}

bool KiwiPlastBridge_CanPush()
{
    const char *mapPath = Radiant_CurrentMapPath();
    if ( !mapPath || !mapPath[0] )
        return false;

    for ( selbrush_t *selected = selected_brushes.next;
          selected && selected != &selected_brushes; selected = selected->next )
    {
        if ( !selected->owner || !selected->owner->def || !selected->def )
            continue;
        if ( KiwiDrop_IsModelEntity( selected ) )
            return true;
        const entity_s *entity = (const entity_s *)selected->owner->def;
        if ( entity->eclass && entity->eclass->fixedsize )
            continue;
        if ( selected->def->patch )
        {
            const curvePatchDef_t *grid = selected->def->patch->curveDef;
            if ( grid && grid->verts && grid->width >= 2 && grid->height >= 2 )
                return true;
        }
        else if ( selected->def->faces && selected->def->faceCount > 0 )
            return true;
    }

    // Construction lines alone are worth sending when that option is on.
    if ( ExportConstructionEnabled() )
    {
        int conObjects = 0;
        int conSegments = 0;
        CountVisibleConstruction( conObjects, conSegments );
        if ( conObjects > 0 )
            return true;
    }
    // So is a map whose terrain is the reference (KIWI 2026-09-13).
    if ( ExportTerrainEnabled() && VisibleTerrainCount() > 0 )
        return true;
    return false;
}

void KiwiPlastBridge_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiSendSelectionToPlasticity", 0x50, 5,
                             KIWI_CMD_PLASTICITY_PUSH );
}

bool KiwiPlastBridge_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_PLASTICITY_PUSH )
        return false;
    s_dialogRequest = true;             // KiwiPlastBridge_Draw opens the options dialog
    return true;
}

void KiwiPlastBridge_BuildMenu( void *frameMenu )
{
    HMENU menu = (HMENU)frameMenu;
    HMENU selection = menu ? ::GetSubMenu( menu, 3 ) : nullptr;
    if ( !selection || ::GetMenuState( selection, KIWI_CMD_PLASTICITY_PUSH,
                                       MF_BYCOMMAND ) != 0xFFFFFFFFu )
        return;
    ::AppendMenuA( selection, MF_SEPARATOR, 0, nullptr );
    ::AppendMenuA( selection, MF_STRING, KIWI_CMD_PLASTICITY_PUSH,
                   "Send Selection to Plasticity\tCtrl+Shift+P" );
}

void KiwiPlastBridge_DrawSettings()
{
    ImGui::SeparatorText( "PLASTICITY REFERENCE STEP / OBJ" );
    bool autoImport = AutoImportEnabled();
    if ( ImGui::Checkbox( "Auto-import into Plasticity", &autoImport ) )
        SetAutoImportEnabled( autoImport );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Uses Plasticity's Ctrl+Shift+O file:import binding.\n"
                           "Imports brush STEP first, then patch/model OBJ.\n"
                           "Failures print and reveal every exported path; the failed "
                           "path is copied to the clipboard." );
    {
        int conObjects = 0;
        int conSegments = 0;
        CountVisibleConstruction( conObjects, conSegments );
        DrawConstructionOptions( conObjects, conSegments );
    }

    std::string stepPath;
    std::string objPath;
    std::string groupName;
    std::string error;
    if ( ExportPaths( stepPath, objPath, groupName, error ) )
    {
        ImGui::TextWrapped( "Brush STEP path: %s", stepPath.c_str() );
        ImGui::TextWrapped( "Patch/model OBJ path: %s", objPath.c_str() );
    }
    else
        ImGui::TextDisabled( "Export paths: %s", error.c_str() );
    ImGui::TextDisabled( "Meters per Radiant unit: %.4f", METERS_PER_UNIT );
}
