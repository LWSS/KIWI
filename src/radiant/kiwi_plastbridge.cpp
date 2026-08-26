#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Export selected editor reference geometry as STEP brush solids plus OBJ meshes.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_plastbridge.h"
#include "kiwi_command.h"
#include "kiwi_droptrace.h"
#include "kiwi_shadowcache.h"
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
        unsigned __int64 stepFaces;
        unsigned __int64 objFaces;

        ExportStats()
            : skippedFixed( 0 ), skippedInvalid( 0 ), skippedModelGeo( 0 ),
              stepBrushes( 0 ), patches( 0 ), models( 0 ), brushFallbacks( 0 ),
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

    void PrintExportStats( const ExportStats &stats )
    {
        Sys_Printf( "Plasticity export: %u brush(es) -> STEP; %u patch(es), "
                    "%u model instance(s), %u degenerate brush fallback(s) -> OBJ; "
                    "skipped %u model selection(s) without resident/valid mesh "
                    "geometry, %u fixed-size selection(s), %u invalid selection(s).\n",
                    stats.stepBrushes, stats.patches, stats.models,
                    stats.brushFallbacks, stats.skippedModelGeo,
                    stats.skippedFixed, stats.skippedInvalid );
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

    bool RevealInExplorer( const std::string &path )
    {
        const std::string arguments = "/select,\"" + path + "\"";
        const HINSTANCE result = ::ShellExecuteA( g_qeglobals.d_hwndMain, "open",
                                                   "explorer.exe", arguments.c_str(),
                                                   nullptr, SW_SHOWNORMAL );
        return (INT_PTR)result > 32;
    }

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
            if ( !RevealInExplorer( job.files[i].path ) )
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

    void ExecuteExport()
    {
        std::string stepPath;
        std::string objPath;
        std::string groupName;
        std::string error;
        if ( !ExportPaths( stepPath, objPath, groupName, error ) )
        {
            Sys_Printf( "Plasticity export: %s.\n", error.c_str() );
            return;
        }

        std::vector<StepSolid> solids;
        std::vector<ObjObject> objects;
        ExportStats stats;
        if ( !GatherObjects( solids, objects, stats, error ) )
        {
            Sys_Printf( "Plasticity export: %s.\n", error.c_str() );
            PrintExportStats( stats );
            return;
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
                return;
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
                return;
            }
            ExportFile exported;
            exported.kind = EXPORT_FILE_OBJ;
            exported.path = objPath;
            exported.objectCount = (unsigned int)objects.size();
            exported.faceCount = stats.objFaces;
            job.files.push_back( exported );
        }
        StartAutoImport( job );
    }
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
    ExecuteExport();
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
