#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Axis-aligned, editor-only reference images. Source pixels are copied beside the
// map; renderer assets are generated under raw/ and never referenced by map data.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"

#include <imgui/imgui.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <d3d9.h>
// KIWI: the June 2010 DXSDK include dir precedes the Windows Kit on the include path, so
// wincodec.h's "dxgitype.h" / <dcommon.h> resolve to the 2010 copies, which predate the
// JPEG table structs and D2D1_PIXEL_FORMAT that the Kit's wincodec.h references.  Pull the
// old headers in first, then supply exactly those definitions (copied from the Kit's
// shared/dxgitype.h and um/dcommon.h) so wincodec.h parses against the DXSDK headers.
#include <dxgiformat.h>
#include <dxgitype.h>
#include <dcommon.h>
typedef enum D2D1_ALPHA_MODE        // um/dcommon.h (Kit); absent from the 2010 copy
{
    D2D1_ALPHA_MODE_UNKNOWN       = 0,
    D2D1_ALPHA_MODE_PREMULTIPLIED = 1,
    D2D1_ALPHA_MODE_STRAIGHT      = 2,
    D2D1_ALPHA_MODE_IGNORE        = 3,
    D2D1_ALPHA_MODE_FORCE_DWORD   = 0xffffffff
} D2D1_ALPHA_MODE;
typedef struct D2D1_PIXEL_FORMAT
{
    DXGI_FORMAT     format;
    D2D1_ALPHA_MODE alphaMode;
} D2D1_PIXEL_FORMAT;
typedef struct DXGI_JPEG_DC_HUFFMAN_TABLE
{
    BYTE CodeCounts[12];
    BYTE CodeValues[12];
} DXGI_JPEG_DC_HUFFMAN_TABLE;
typedef struct DXGI_JPEG_AC_HUFFMAN_TABLE
{
    BYTE CodeCounts[16];
    BYTE CodeValues[162];
} DXGI_JPEG_AC_HUFFMAN_TABLE;
typedef struct DXGI_JPEG_QUANTIZATION_TABLE
{
    BYTE Elements[64];
} DXGI_JPEG_QUANTIZATION_TABLE;
#include <wincodec.h>
#include <commdlg.h>
#include <shellapi.h>

#include "kiwi_refimage.h"
#include "kiwi_camera.h"
#include "kiwi_conselect.h"
#include "kiwi_droptrace.h"
#include "kiwi_focus.h"
#include "kiwi_grid.h"
#include "kiwi_iwi.h"
#include "kiwi_lines.h"
#include "kiwi_matwriter.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_undo.h"
#include "kiwi_units.h"
#include "kiwi_windows.h"
#include "radiant_frame.h"
#include "radiant_registry.h"
#include "radiant_rtt.h"
#include "xywnd.h"

#include <algorithm>
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <map>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

extern int       Sys_Printf( const char *fmt, ... );                         // win_qe3.cpp:121
extern int       g_nUpdateBits;                                              // engine_stubs.cpp:773
extern int       modified;                                                   // map.cpp:67
extern camera_s *Ed_Camera();                                                // camwnd.cpp:165
extern char      Byte4PackPixelColor( float *from, GfxColor *out );          // engine_stubs.cpp:827
extern void __cdecl R_AddRenderCmdDrawTris(
    Material *material, MaterialTechniqueType techType, short indexCount,
    const uint16_t *indices, short vertexCount,
    const float ( *xyzw )[4], const float ( *normal )[3], float *color,
    const float ( *st )[2] );                                                // r_rendercmds.cpp:2505
extern bool ImGuiShell_ViewportAtScreen( int screenX, int screenY,
                                         int *rttId, int *imgX, int *imgY );  // imgui_shell.cpp:385
extern bool ImGuiShell_LastViewport( int *rttId, int *imgX, int *imgY );      // imgui_shell.cpp:405
extern void ImGuiShell_AbortViewportInput();                                  // imgui_shell.cpp:730
extern ImGuiID ImGuiShell_DockRoot();                                         // imgui_shell.cpp:974
extern void ImGuiShell_FocusTab( const char *title );                         // imgui_shell.cpp:208

krefImage_t::krefImage_t()
    : axis( 2 ), width( 512.0f ), height( 512.0f ), keepAspect( true ),
      rotation( 0.0f ), flipU( false ), flipV( false ), opacity( 1.0f ),
      locked( false ), hidden( false ), layerOrder( 0 ), material( nullptr ),
      pixW( 0 ), pixH( 0 )
{
    origin[0] = origin[1] = origin[2] = 0.0f;
}

// Public entry points are kept near the top; their implementation state and
// helpers live in the single anonymous namespace below.
namespace
{
    const char *KREF_PROFILE = "KiwiRefImage";
    const float KREF_PI = 3.14159265358979323846f;
    const int   KREF_UNDO_DEPTH = 32;
    const float KREF_MIN_SIZE = 0.125f;

    struct storeSnap_t { std::vector<krefImage_t> images; int selected = -1; };
    enum dragMode_t { KREF_DRAG_NONE = 0, KREF_DRAG_MOVE, KREF_DRAG_SCALE, KREF_DRAG_ROTATE };
    struct drag_t
    {
        bool active = false; int index = -1; dragMode_t mode = KREF_DRAG_NONE; int corner = -1;
        float startPoint[3] = { 0,0,0 }, grabOffset[3] = { 0,0,0 }, startAngle = 0.0f;
        krefImage_t base;
    };
    struct placement_t { int axis; float origin[3]; };
    struct move_t
    {
        bool        active = false;
        int         index = -1;
        krefImage_t base;
    };

    extern std::vector<krefImage_t> s_images;
    extern unsigned s_generation;
    extern int s_selected;
    extern bool s_armed;
    extern std::vector<storeSnap_t> s_undo, s_redo;
    extern storeSnap_t s_pending;
    extern bool s_pendingHave;
    extern const char *s_pendingLabel;
    extern drag_t s_drag;
    extern move_t s_move;
    extern int s_hoverCamera, s_hoverXY;
    extern bool s_xyClickOwned;
    extern bool s_focusPending;

    int ClampAxis( int axis );
    bool SelectionAllowsImages();
    bool NearF( float a, float b );
    void Sanitize( krefImage_t &r );
    void AxisBasis( const krefImage_t &r, float u[3], float v[3], float n[3] );
    void Touch( bool dirty );
    storeSnap_t Snapshot();
    void CommitPending();
    void BeginEdit( const char *label );
    void CancelEdit();
    void ImmediateCommit( const storeSnap_t &before, const char *label );
    void Restore( const storeSnap_t &snap );
    void DrawImages( int axisFilter, float scale, bool camera );
    void Corners( const krefImage_t &r, float out[4][3] );
    bool PointInside( const krefImage_t &r, const float p[3] );
    bool CameraPick( int x, int y, int *outIndex, float *outDistance,
                     float hitPoint[3] );
    bool XYPick( int x, int y, int *outIndex, float hitPoint[3] );
    void ApplyRectInternal( float x0, float y0, float x1, float y1,
                            bool crossing, bool shift, bool ctrl, bool camera );
    int CameraHit( int x, int y, int *corner, float hitPoint[3] );
    int XYHit( int x, int y, int *corner, float hitPoint[3] );
    bool BeginDrag( int index, int corner, bool shift, const float point[3] );
    bool CameraPointOnImagePlane( int x, int y, const krefImage_t &r, float out[3] );
    void DragTo( const float rawPoint[3] );
    void EndDrag( bool cancel );
    void XYPoint( int imgX, int imgY, float out[3] );
    bool AcceptedImage( const char *path );
    bool PlacementForViewport( int id, int imgX, int imgY, placement_t *out );
    placement_t PlacementUnderCursor();
    bool ImportOne( const char *src, const placement_t &place );
    bool NewPastePath( char *absolute, int cap, std::string *rel );
    bool WicMemoryToPng( BYTE *data, DWORD size, const char *outPath, int *pixW, int *pixH );
    bool DibToBgra( const BYTE *dib, SIZE_T bytes, std::vector<BYTE> *out, int *outW, int *outH );
    bool BgraToPng( const std::vector<BYTE> &pixels, int w, int h, const char *outPath );
}

int KiwiRefImage_Count() { return (int)s_images.size(); }

const krefImage_t *KiwiRefImage_At( int index )
{
    return index >= 0 && index < (int)s_images.size() ? &s_images[index] : nullptr;
}

krefImage_t *KiwiRefImage_MutableAt( int index )
{
    return index >= 0 && index < (int)s_images.size() ? &s_images[index] : nullptr;
}

int KiwiRefImage_Add( const krefImage_t &image )
{
    krefImage_t copy = image;
    Sanitize( copy );
    copy.material = nullptr;
    s_images.push_back( copy );
    s_selected = (int)s_images.size() - 1;
    Touch( true );
    return s_selected;
}

bool KiwiRefImage_RemoveAt( int index )
{
    if ( index < 0 || index >= (int)s_images.size() ) return false;
    s_images.erase( s_images.begin() + index );
    if ( s_selected == index ) s_selected = -1;
    else if ( s_selected > index ) --s_selected;
    if ( s_hoverCamera == index ) s_hoverCamera = -1;
    else if ( s_hoverCamera > index ) --s_hoverCamera;
    if ( s_hoverXY == index ) s_hoverXY = -1;
    else if ( s_hoverXY > index ) --s_hoverXY;
    Touch( true );
    return true;
}

unsigned KiwiRefImage_Generation() { return s_generation; }
int KiwiRefImage_Selected() { return s_selected; }

void KiwiRefImage_Select( int index )
{
    const int next = index >= 0 && index < (int)s_images.size() ? index : -1;
    if ( next != s_selected ) { s_selected = next; Touch( false ); }
}

void KiwiRefImage_ApplyClick( int index, bool shift, bool ctrl, bool viewport )
{
    if ( index < 0 || index >= (int)s_images.size() )
    {
        if ( !shift && !ctrl )
            KiwiRefImage_Select( -1 );
        return;
    }

    if ( !shift && !ctrl )
    {
        Sel_Clear( KiwiSel() );
        Sel_SyncToLegacy();
        KiwiConSel_Clear();
    }

    if ( ctrl )
    {
        if ( s_selected == index )
            KiwiRefImage_Select( -1 );
    }
    else
    {
        KiwiRefImage_Select( index );
    }
    if ( viewport && s_selected == index )
        s_focusPending = true;
    g_nUpdateBits = -1;
}

bool KiwiRefImage_PickAt( int imgX, int imgY, int *outIndex, float *outDepthOrDist )
{
    int index = -1;
    float distance = FLT_MAX;
    const bool hit = CameraPick( imgX, imgY, &index, &distance, nullptr );
    if ( outIndex ) *outIndex = hit ? index : -1;
    if ( outDepthOrDist ) *outDepthOrDist = hit ? distance : FLT_MAX;
    return hit;
}

void KiwiRefImage_ApplyRect( float x0, float y0, float x1, float y1,
                             bool crossing, bool shift, bool ctrl )
{
    ApplyRectInternal( x0, y0, x1, y1, crossing, shift, ctrl, true );
}

void KiwiRefImage_ApplyRectXY( float x0, float y0, float x1, float y1,
                               bool crossing, bool shift, bool ctrl )
{
    ApplyRectInternal( x0, y0, x1, y1, crossing, shift, ctrl, false );
}

bool KiwiRefImage_SetHidden( int index, bool hidden )
{
    if ( index < 0 || index >= (int)s_images.size() || s_images[index].hidden == hidden )
        return false;
    CommitPending();
    const storeSnap_t before = Snapshot();
    s_images[index].hidden = hidden;
    Touch( true );
    ImmediateCommit( before, hidden ? "hide reference image" : "show reference image" );
    return true;
}

bool KiwiRefImage_SetLocked( int index, bool locked )
{
    if ( index < 0 || index >= (int)s_images.size() || s_images[index].locked == locked )
        return false;
    CommitPending();
    const storeSnap_t before = Snapshot();
    s_images[index].locked = locked;
    Touch( true );
    ImmediateCommit( before, locked ? "lock reference image" : "unlock reference image" );
    return true;
}

bool KiwiRefImage_SetName( int index, const char *name )
{
    if ( index < 0 || index >= (int)s_images.size() )
        return false;
    const std::string next = name ? name : "";
    if ( s_images[index].name == next )
        return false;
    CommitPending();
    const storeSnap_t before = Snapshot();
    s_images[index].name = next;
    Touch( true );
    ImmediateCommit( before, "rename reference image" );
    return true;
}

bool KiwiRefImage_DeleteAt( int index )
{
    if ( index < 0 || index >= (int)s_images.size() )
        return false;
    CommitPending();
    const storeSnap_t before = Snapshot();
    if ( !KiwiRefImage_RemoveAt( index ) )
        return false;
    ImmediateCommit( before, "remove reference image" );
    return true;
}

bool KiwiRefImage_Bounds( int index, float mins[3], float maxs[3] )
{
    if ( index < 0 || index >= (int)s_images.size() || !mins || !maxs )
        return false;
    float c[4][3];
    Corners( s_images[index], c );
    for ( int k = 0; k < 3; ++k )
        mins[k] = maxs[k] = c[0][k];
    for ( int i = 1; i < 4; ++i )
        for ( int k = 0; k < 3; ++k )
        {
            mins[k] = (std::min)( mins[k], c[i][k] );
            maxs[k] = (std::max)( maxs[k], c[i][k] );
        }
    return true;
}

bool KiwiRefImage_Focus( int index )
{
    float mins[3], maxs[3];
    if ( !KiwiRefImage_Bounds( index, mins, maxs ) )
        return false;
    KiwiCam_FrameBounds( mins, maxs );
    return true;
}

bool KiwiRefImage_CanMove()
{
    return s_selected >= 0 && s_selected < (int)s_images.size()
        && !s_images[s_selected].locked && !s_images[s_selected].hidden;
}

bool KiwiRefImage_MoveBegin( float outRef[3] )
{
    if ( !KiwiRefImage_CanMove() || s_drag.active )
        return false;
    BeginEdit( "move reference image" );
    s_move.active = true;
    s_move.index = s_selected;
    s_move.base = s_images[s_selected];
    if ( outRef )
        memcpy( outRef, s_move.base.origin, sizeof( s_move.base.origin ) );
    return true;
}

void KiwiRefImage_MoveApply( const float total[3] )
{
    if ( !s_move.active || !total || s_move.index < 0
      || s_move.index >= (int)s_images.size() )
        return;
    krefImage_t &r = s_images[s_move.index];
    float next[3];
    bool changed = false;
    for ( int k = 0; k < 3; ++k )
    {
        next[k] = s_move.base.origin[k] + total[k];
        if ( !NearF( r.origin[k], next[k] ) ) changed = true;
    }
    if ( !changed )
        return;
    memcpy( r.origin, next, sizeof( next ) );
    Sanitize( r );
    Touch( true );
}

void KiwiRefImage_MoveCommit()
{
    if ( !s_move.active )
        return;
    CommitPending();
    s_move = move_t();
}

void KiwiRefImage_MoveCancel()
{
    if ( !s_move.active )
        return;
    CancelEdit();
    s_move = move_t();
}

namespace
{
    // Rotate `in` about the world axis `axis` through `pivot` (right-hand rule).
    void RotatePointAboutAxis( const float pivot[3], int axis, float c, float sn,
                               const float in[3], float out[3] )
    {
        const int i = ( axis + 1 ) % 3, j = ( axis + 2 ) % 3;
        const float di = in[i] - pivot[i], dj = in[j] - pivot[j];
        out[axis] = in[axis];
        out[i]    = pivot[i] + di * c - dj * sn;
        out[j]    = pivot[j] + di * sn + dj * c;
    }
}

void KiwiRefImage_RotateApply( const float pivot[3], int axis, float degrees )
{
    if ( !s_move.active || !pivot || axis < 0 || axis > 2
      || s_move.index < 0 || s_move.index >= (int)s_images.size() )
        return;
    krefImage_t       &r = s_images[s_move.index];
    const krefImage_t &b = s_move.base;

    // About any axis other than the picture's own normal the plane must stay on a
    // major axis, so the turn snaps to quarter turns and the picture re-maps onto
    // the plane its rotated normal lands on.  About its own normal it spins freely.
    float deg = degrees;
    if ( axis != b.axis )
        deg = 90.0f * floorf( degrees / 90.0f + 0.5f );
    // Negative: the ported Select_RotateAxis turns the other way for the same degrees,
    // and the ring feeds both, so images must spin like brushes do.
    const float rad = -deg * KREF_PI / 180.0f;
    const float c = cosf( rad ), sn = sinf( rad );
    static const float zero[3] = { 0.0f, 0.0f, 0.0f };

    float origin[3];
    RotatePointAboutAxis( pivot, axis, c, sn, b.origin, origin );

    // Rotate the baseline frame and read the result back as (axis, rotation, flip):
    // the rotated normal picks the plane, the rotated u vector the in-plane angle, and
    // a normal that lands negative means the picture is now seen from behind, which
    // one V flip restores (two-sided quad, so it stays visible either way).
    float u[3], v[3], n[3];
    AxisBasis( b, u, v, n );
    float ur[3], nr[3];
    RotatePointAboutAxis( zero, axis, c, sn, u, ur );
    RotatePointAboutAxis( zero, axis, c, sn, n, nr );
    int newAxis = b.axis;
    if ( axis != b.axis )
    {
        newAxis = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabsf( nr[k] ) > fabsf( nr[newAxis] ) ) newAxis = k;
    }
    krefImage_t flat = b;
    flat.axis     = newAxis;
    flat.rotation = 0.0f;
    float bu[3], bv[3], bn[3];
    AxisBasis( flat, bu, bv, bn );
    const float du = ur[0] * bu[0] + ur[1] * bu[1] + ur[2] * bu[2];
    const float dv = ur[0] * bv[0] + ur[1] * bv[1] + ur[2] * bv[2];
    const float rotation = atan2f( dv, du ) * 180.0f / KREF_PI;
    const bool  flipV    = ( nr[newAxis] < 0.0f ) ? !b.flipV : b.flipV;

    bool changed = !NearF( r.rotation, rotation ) || r.axis != newAxis || r.flipV != flipV;
    for ( int k = 0; k < 3 && !changed; ++k )
        changed = !NearF( r.origin[k], origin[k] );
    if ( !changed )
        return;
    memcpy( r.origin, origin, sizeof( origin ) );
    r.axis     = newAxis;
    r.rotation = rotation;
    r.flipV    = flipV;
    Sanitize( r );
    Touch( true );
}

void KiwiRefImage_ScaleApply( const float pivot[3], const float factor[3] )
{
    if ( !s_move.active || !pivot || !factor
      || s_move.index < 0 || s_move.index >= (int)s_images.size() )
        return;
    krefImage_t       &r = s_images[s_move.index];
    const krefImage_t &b = s_move.base;
    float origin[3];
    for ( int k = 0; k < 3; ++k )
        origin[k] = pivot[k] + ( b.origin[k] - pivot[k] ) * factor[k];
    // Width follows the picture's u direction, height its v direction: each takes
    // the world factors weighted by how much of that axis the direction carries
    // (unit vectors, so the weights sum to one and a uniform scale is exact).
    float u[3], v[3], n[3];
    AxisBasis( b, u, v, n );
    float fw = 0.0f, fh = 0.0f;
    for ( int k = 0; k < 3; ++k )
    {
        fw += u[k] * u[k] * factor[k];
        fh += v[k] * v[k] * factor[k];
    }
    fw = fabsf( fw );
    fh = fabsf( fh );
    if ( b.keepAspect )
        fw = fh = 0.5f * ( fw + fh );
    const float width  = b.width  * fw;
    const float height = b.height * fh;
    bool changed = !NearF( r.width, width ) || !NearF( r.height, height );
    for ( int k = 0; k < 3 && !changed; ++k )
        changed = !NearF( r.origin[k], origin[k] );
    if ( !changed )
        return;
    memcpy( r.origin, origin, sizeof( origin ) );
    r.width  = width;
    r.height = height;
    Sanitize( r );
    Touch( true );
}

void KiwiRefImage_DrawWorld() { DrawImages( -1, 1.0f, true ); }
void KiwiRefImage_DrawXY( int viewType, float scale ) { DrawImages( ClampAxis( viewType ), scale, false ); }

bool KiwiRefImage_IsArmed() { return s_armed; }

bool KiwiRefImage_HandleCameraDown( int imgX, int imgY, bool shift )
{
    if ( !s_armed ) return false;
    int corner; float p[3];
    const int hit = CameraHit( imgX, imgY, &corner, p );
    return hit >= 0 && BeginDrag( hit, corner, shift, p );
}

void KiwiRefImage_HandleCameraDrag( int imgX, int imgY )
{
    if ( !s_drag.active ) return;
    float p[3];
    if ( CameraPointOnImagePlane( imgX, imgY, s_drag.base, p ) ) DragTo( p );
}

void KiwiRefImage_HandleCameraUp() { EndDrag( false ); }
void KiwiRefImage_HandleCameraAbort() { EndDrag( true ); }

void KiwiRefImage_HoverCamera( int imgX, int imgY, bool over )
{
    int next = -1;
    if ( !s_armed && over && SelectionAllowsImages() )
        (void)KiwiRefImage_PickAt( imgX, imgY, &next, nullptr );
    if ( next != s_hoverCamera )
    {
        s_hoverCamera = next;
        g_nUpdateBits |= W_CAMERA;
    }
}

void KiwiRefImage_HoverXY( int imgX, int imgY, bool over )
{
    int next = -1;
    if ( !s_armed && over && SelectionAllowsImages() )
        (void)XYPick( imgX, imgY, &next, nullptr );
    if ( next != s_hoverXY )
    {
        s_hoverXY = next;
        g_nUpdateBits |= ( W_XY | W_Z );
    }
}

bool KiwiRefImage_HandleXYDown( int imgX, int imgY, unsigned int flags )
{
    s_xyClickOwned = false;
    if ( s_armed )
    {
        int corner; float p[3];
        const int hit = XYHit( imgX, imgY, &corner, p );
        return hit >= 0 && BeginDrag( hit, corner, ( flags & MK_SHIFT ) != 0, p );
    }
    if ( !SelectionAllowsImages() )
        return false;
    int hit = -1;
    if ( !XYPick( imgX, imgY, &hit, nullptr ) )
        return false;
    KiwiRefImage_ApplyClick( hit, ( flags & MK_SHIFT ) != 0,
                            ( flags & MK_CONTROL ) != 0, true );
    s_xyClickOwned = true;
    return true;
}

bool KiwiRefImage_HandleXYMove( int imgX, int imgY )
{
    if ( s_xyClickOwned ) return true;
    if ( !s_drag.active ) return false;
    float p[3]; XYPoint( imgX, imgY, p ); DragTo( p );
    return true;
}

bool KiwiRefImage_HandleXYUp()
{
    if ( s_xyClickOwned )
    {
        s_xyClickOwned = false;
        return true;
    }
    if ( !s_drag.active ) return false;
    EndDrag( false );
    return true;
}

void KiwiRefImage_HandleXYAbort()
{
    s_xyClickOwned = false;
    EndDrag( true );
}

bool KiwiRefImage_HandleEscape()
{
    if ( !s_armed ) return false;
    if ( s_drag.active )
    {
        ImGuiShell_AbortViewportInput();
        if ( s_drag.active ) EndDrag( true ); // defensive: a non-viewport caller
    }
    s_armed = false;
    Touch( false );
    return true;
}

bool KiwiRefImage_HandleDelete()
{
    if ( !s_armed || s_selected < 0 || s_selected >= (int)s_images.size() ) return false;
    if ( s_drag.active )
    {
        ImGuiShell_AbortViewportInput();
        if ( s_drag.active ) EndDrag( true );
    }
    return KiwiRefImage_DeleteAt( s_selected );
}

bool KiwiRefImage_OwnsDelete()
{
    return s_selected >= 0 && s_selected < (int)s_images.size()
        && KiwiSel().items.empty() && KiwiConSel_Empty();
}

bool KiwiRefImage_HandleDropFiles( void *dropHandle, int screenX, int screenY )
{
    HDROP drop = (HDROP)dropHandle;
    if ( !drop ) return false;
    int id = -1, imgX = 0, imgY = 0;
    if ( !ImGuiShell_ViewportAtScreen( screenX, screenY, &id, &imgX, &imgY ) ) return false;
    const UINT count = ::DragQueryFileA( drop, 0xFFFFFFFFu, nullptr, 0 );
    if ( !count ) return false;
    std::vector<std::string> paths;
    paths.reserve( count );
    for ( UINT i = 0; i < count; ++i )
    {
        const UINT len = ::DragQueryFileA( drop, i, nullptr, 0 );
        std::vector<char> path( (size_t)len + 1u, '\0' );
        ::DragQueryFileA( drop, i, path.data(), len + 1u );
        if ( !AcceptedImage( path.data() ) ) return false; // old texture import keeps ownership
        paths.push_back( path.data() );
    }
    placement_t place;
    if ( !PlacementForViewport( id, imgX, imgY, &place ) ) return false;

    ::DragFinish( drop );
    BeginEdit( paths.size() == 1 ? "add reference image" : "add reference images" );
    int added = 0;
    for ( size_t i = 0; i < paths.size(); ++i ) if ( ImportOne( paths[i].c_str(), place ) ) ++added;
    if ( added ) CommitPending(); else CancelEdit();
    return true;
}

bool KiwiRefImage_PasteClipboard()
{
    if ( !::OpenClipboard( g_qeglobals.d_hwndMain ) ) return false;
    const UINT pngFormat = ::RegisterClipboardFormatA( "PNG" );
    bool recognized = false, wrote = false;
    char pasted[1200] = { 0 }; std::string rel;

    HANDLE h = pngFormat ? ::GetClipboardData( pngFormat ) : nullptr;
    if ( h )
    {
        recognized = true;
        if ( NewPastePath( pasted, sizeof( pasted ), &rel ) )
        {
            BYTE *data = (BYTE *)::GlobalLock( h );
            const SIZE_T size = ::GlobalSize( h );
            wrote = data && size && size <= 0xFFFFFFFFu
                 && WicMemoryToPng( data, (DWORD)size, pasted, nullptr, nullptr );
            if ( data ) ::GlobalUnlock( h );
        }
    }
    if ( !wrote )
    {
        const UINT formats[2] = { CF_DIBV5, CF_DIB };
        for ( int f = 0; f < 2 && !wrote; ++f )
        {
            h = ::GetClipboardData( formats[f] );
            if ( !h ) continue;
            recognized = true;
            if ( NewPastePath( pasted, sizeof( pasted ), &rel ) )
            {
                BYTE *data = (BYTE *)::GlobalLock( h );
                const SIZE_T size = ::GlobalSize( h );
                std::vector<BYTE> pixels; int w = 0, hgt = 0;
                wrote = data && DibToBgra( data, size, &pixels, &w, &hgt )
                     && BgraToPng( pixels, w, hgt, pasted );
                if ( data ) ::GlobalUnlock( h );
            }
        }
    }

    char dropped[1200] = { 0 };
    if ( !wrote )
    {
        HDROP files = (HDROP)::GetClipboardData( CF_HDROP );
        if ( files && ::DragQueryFileA( files, 0, dropped, sizeof( dropped ) ) && AcceptedImage( dropped ) )
            recognized = true;
    }
    ::CloseClipboard();
    if ( !recognized ) return false;

    const placement_t place = PlacementUnderCursor();
    BeginEdit( "paste reference image" );
    bool added = false;
    if ( dropped[0] ) added = ImportOne( dropped, place );
    else if ( wrote ) added = ImportOne( pasted, place );
    if ( added ) CommitPending();
    else
    {
        CancelEdit();
        if ( pasted[0] ) ::DeleteFileA( pasted );
        Sys_Printf( "Reference image paste: clipboard image decode/import failed.\n" );
    }
    return true;
}

bool KiwiRefImage_UndoPop()
{
    CommitPending();
    if ( s_undo.empty() ) return false;
    storeSnap_t current = Snapshot();
    storeSnap_t target = s_undo.back(); s_undo.pop_back();
    s_redo.push_back( current );
    if ( (int)s_redo.size() > KREF_UNDO_DEPTH ) s_redo.erase( s_redo.begin() );
    Restore( target );
    return true;
}

bool KiwiRefImage_RedoPop()
{
    CommitPending();
    if ( s_redo.empty() ) return false;
    storeSnap_t current = Snapshot();
    storeSnap_t target = s_redo.back(); s_redo.pop_back();
    s_undo.push_back( current );
    if ( (int)s_undo.size() > KREF_UNDO_DEPTH ) s_undo.erase( s_undo.begin() );
    Restore( target );
    return true;
}

void KiwiRefImage_ClearRedo() { s_redo.clear(); }

void KiwiRefImage_UndoReset()
{
    s_undo.clear(); s_redo.clear(); s_pending = storeSnap_t();
    s_pendingHave = false; s_pendingLabel = nullptr; s_drag = drag_t(); s_move = move_t();
}

namespace
{
    std::vector<krefImage_t> s_images;
    unsigned                 s_generation = 1;
    int                      s_selected = -1;
    bool                     s_armed = false;
    bool                     s_profileLoaded = false;
    float                    s_defaultOpacity = 0.75f;

    std::vector<storeSnap_t> s_undo;
    std::vector<storeSnap_t> s_redo;
    storeSnap_t              s_pending;
    const char              *s_pendingLabel = nullptr;
    bool                     s_pendingHave = false;

    struct assetCache_t
    {
        Material *material = nullptr;
        int pixW = 0, pixH = 0;
    };
    std::map<std::string, std::string> s_fileAsset;
    std::map<std::string, assetCache_t> s_assetCache;
    std::set<std::string> s_missingWarned;

    drag_t s_drag;
    move_t s_move;
    int    s_hoverCamera = -1;
    int    s_hoverXY = -1;
    bool   s_xyClickOwned = false;
    bool   s_focusPending = false;

    bool        s_parseActive = false;
    krefImage_t s_parseImage;

    float ClampF( float v, float lo, float hi )
    {
        if ( !_finite( v ) ) return lo;
        return v < lo ? lo : ( v > hi ? hi : v );
    }

    int ClampAxis( int axis ) { return axis < 0 ? 0 : ( axis > 2 ? 2 : axis ); }

    bool SelectionAllowsImages()
    {
        const sel_mask_t mask = KiwiSel_GetModeMask();
        return mask == SEL_MASK_OBJECT || mask == SEL_MASK_EVERYTHING;
    }

    bool NearF( float a, float b )
    {
        const float d = fabsf( a - b );
        const float scale = (std::max)( 1.0f, (std::max)( fabsf( a ), fabsf( b ) ) );
        return d <= 1.0e-5f * scale;
    }

    void LoadProfile()
    {
        if ( s_profileLoaded ) return;
        s_profileLoaded = true;
        const std::string text = Radiant_ProfileGetString( KREF_PROFILE, "DefaultOpacity", "0.75" );
        char *end = nullptr;
        const double v = strtod( text.c_str(), &end );
        s_defaultOpacity = ( end != text.c_str() && _finite( v ) )
                         ? ClampF( (float)v, 0.0f, 1.0f ) : 0.75f;
    }

    void SaveDefaults()
    {
        char value[32];
        _snprintf( value, sizeof( value ), "%.6g", (double)s_defaultOpacity );
        value[sizeof( value ) - 1] = '\0';
        Radiant_ProfileSetString( KREF_PROFILE, "DefaultOpacity", value );
    }

    void Touch( bool dirty )
    {
        ++s_generation;
        g_nUpdateBits = -1;
        if ( dirty ) modified = 1;
    }

    void Sanitize( krefImage_t &r )
    {
        r.axis = ClampAxis( r.axis );
        for ( int i = 0; i < 3; ++i )
            if ( !_finite( r.origin[i] ) ) r.origin[i] = 0.0f;
        r.width = ClampF( r.width, KREF_MIN_SIZE, 131072.0f );
        r.height = ClampF( r.height, KREF_MIN_SIZE, 131072.0f );
        r.rotation = ClampF( r.rotation, -360000.0f, 360000.0f );
        while ( r.rotation > 180.0f ) r.rotation -= 360.0f;
        while ( r.rotation <= -180.0f ) r.rotation += 360.0f;
        r.opacity = ClampF( r.opacity, 0.0f, 1.0f );
    }

    storeSnap_t Snapshot()
    {
        storeSnap_t s;
        s.images = s_images;
        s.selected = s_selected;
        return s;
    }

    bool SameRecord( const krefImage_t &a, const krefImage_t &b )
    {
        if ( a.file != b.file || a.axis != b.axis || a.keepAspect != b.keepAspect
          || a.flipU != b.flipU || a.flipV != b.flipV || a.locked != b.locked
          || a.hidden != b.hidden || a.layerOrder != b.layerOrder || a.name != b.name )
            return false;
        if ( !NearF( a.width, b.width ) || !NearF( a.height, b.height )
          || !NearF( a.rotation, b.rotation ) || !NearF( a.opacity, b.opacity ) )
            return false;
        for ( int i = 0; i < 3; ++i )
            if ( !NearF( a.origin[i], b.origin[i] ) ) return false;
        return true;
    }

    bool SameSnap( const storeSnap_t &a, const storeSnap_t &b )
    {
        if ( a.selected != b.selected || a.images.size() != b.images.size() ) return false;
        for ( size_t i = 0; i < a.images.size(); ++i )
            if ( !SameRecord( a.images[i], b.images[i] ) ) return false;
        return true;
    }

    void Restore( const storeSnap_t &snap )
    {
        s_images = snap.images;
        s_selected = ( snap.selected >= 0 && snap.selected < (int)s_images.size() )
                   ? snap.selected : -1;
        s_drag = drag_t();
        s_move = move_t();
        s_hoverCamera = s_hoverXY = -1;
        s_xyClickOwned = false;
        s_fileAsset.clear(); s_assetCache.clear(); s_missingWarned.clear();
        Touch( true );
    }

    void CommitPending()
    {
        if ( !s_pendingHave ) return;
        s_pendingHave = false;
        const storeSnap_t now = Snapshot();
        if ( SameSnap( s_pending, now ) )
        {
            s_pending = storeSnap_t();
            return;
        }
        s_undo.push_back( storeSnap_t() );
        s_undo.back().images.swap( s_pending.images );
        s_undo.back().selected = s_pending.selected;
        s_pending = storeSnap_t();
        if ( (int)s_undo.size() > KREF_UNDO_DEPTH ) s_undo.erase( s_undo.begin() );
        s_redo.clear();
        KiwiUndo_NoteRefImageRecord( s_pendingLabel ? s_pendingLabel : "reference image edit" );
    }

    void BeginEdit( const char *label )
    {
        CommitPending();
        s_pending = Snapshot();
        s_pendingLabel = label;
        s_pendingHave = true;
    }

    void BeginEditFrom( const storeSnap_t &before, const char *label )
    {
        CommitPending();
        s_pending = before;
        s_pendingLabel = label;
        s_pendingHave = true;
    }

    void CancelEdit()
    {
        if ( !s_pendingHave ) return;
        const storeSnap_t before = s_pending;
        s_pendingHave = false;
        s_pending = storeSnap_t();
        s_images = before.images;
        s_selected = before.selected;
        s_drag = drag_t();
        Touch( false ); // restored state is not a document edit; only request a redraw
    }

    void ImmediateCommit( const storeSnap_t &before, const char *label )
    {
        BeginEditFrom( before, label );
        CommitPending();
    }

    bool FileExists( const char *path )
    {
        const DWORD a = path && path[0] ? ::GetFileAttributesA( path ) : INVALID_FILE_ATTRIBUTES;
        return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY ) == 0;
    }

    const char *Extension( const char *path )
    {
        if ( !path ) return "";
        const char *base = path;
        for ( const char *p = path; *p; ++p ) if ( *p == '/' || *p == '\\' ) base = p + 1;
        const char *dot = nullptr;
        for ( const char *p = base; *p; ++p ) if ( *p == '.' ) dot = p;
        return dot ? dot : "";
    }

    bool AcceptedImage( const char *path )
    {
        const char *e = Extension( path );
        return !_stricmp( e, ".png" ) || !_stricmp( e, ".jpg" )
            || !_stricmp( e, ".jpeg" ) || !_stricmp( e, ".bmp" )
            || !_stricmp( e, ".tga" ) || !_stricmp( e, ".dds" )
            || !_stricmp( e, ".webp" );
    }

    bool FullPath( const char *in, char *out, int cap )
    {
        if ( !in || !in[0] || !out || cap < 2 ) return false;
        const DWORD n = ::GetFullPathNameA( in, (DWORD)cap, out, nullptr );
        if ( !n || n >= (DWORD)cap ) return false;
        return true;
    }

    void StripFilename( char *path )
    {
        if ( !path ) return;
        char *last = nullptr;
        for ( char *p = path; *p; ++p ) if ( *p == '/' || *p == '\\' ) last = p;
        if ( last ) *last = '\0'; else path[0] = '\0';
    }

    bool MapDirectory( char *out, int cap, bool *unnamed )
    {
        if ( unnamed ) *unnamed = false;
        const char *mapPath = Radiant_CurrentMapPath();
        if ( mapPath && mapPath[0] && _stricmp( mapPath, "unnamed.map" ) )
        {
            if ( !FullPath( mapPath, out, cap ) ) return false;
            StripFilename( out );
            return out[0] != '\0';
        }

        char module[MAX_PATH];
        const DWORD n = ::GetModuleFileNameA( nullptr, module, MAX_PATH );
        if ( !n || n >= MAX_PATH ) return false;
        StripFilename( module );
        _snprintf( out, cap, "%s\\map_source", module );
        out[cap - 1] = '\0';
        if ( unnamed ) *unnamed = true;
        return out[0] != '\0';
    }

    bool RelativeSafe( const std::string &rel )
    {
        if ( rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find( ':' ) != std::string::npos )
            return false;
        size_t start = 0;
        while ( start <= rel.size() )
        {
            size_t end = rel.find_first_of( "/\\", start );
            if ( end == std::string::npos ) end = rel.size();
            if ( rel.substr( start, end - start ) == ".." ) return false;
            if ( end == rel.size() ) break;
            start = end + 1;
        }
        return true;
    }

    bool AbsoluteForRecord( const krefImage_t &r, char *out, int cap )
    {
        if ( !RelativeSafe( r.file ) ) return false;
        char dir[MAX_PATH];
        if ( !MapDirectory( dir, sizeof( dir ), nullptr ) ) return false;
        std::string rel = r.file;
        std::replace( rel.begin(), rel.end(), '/', '\\' );
        _snprintf( out, cap, "%s\\%s", dir, rel.c_str() );
        out[cap - 1] = '\0';
        return true;
    }

    bool EnsureRefDirectory( char *out, int cap, bool *unnamed )
    {
        char dir[MAX_PATH]; bool isUnnamed = false;
        if ( !MapDirectory( dir, sizeof( dir ), &isUnnamed ) ) return false;
        if ( unnamed ) *unnamed = isUnnamed;
        if ( isUnnamed && !::CreateDirectoryA( dir, nullptr )
          && ::GetLastError() != ERROR_ALREADY_EXISTS )
            return false;
        _snprintf( out, cap, "%s\\refimages", dir );
        out[cap - 1] = '\0';
        if ( ::CreateDirectoryA( out, nullptr ) || ::GetLastError() == ERROR_ALREADY_EXISTS )
            return true;
        return false;
    }

    const char *BaseName( const char *path )
    {
        const char *base = path ? path : "";
        for ( const char *p = base; *p; ++p ) if ( *p == '/' || *p == '\\' ) base = p + 1;
        return base;
    }

    void StemName( const char *path, char *out, int cap )
    {
        _snprintf( out, cap, "%s", BaseName( path ) );
        out[cap - 1] = '\0';
        char *dot = strrchr( out, '.' );
        if ( dot ) *dot = '\0';
        if ( !out[0] ) _snprintf( out, cap, "Reference image" );
    }

    bool SamePath( const char *a, const char *b )
    {
        char fa[1200], fb[1200];
        if ( !FullPath( a, fa, sizeof( fa ) ) || !FullPath( b, fb, sizeof( fb ) ) ) return false;
        for ( char *p = fa; *p; ++p ) if ( *p == '/' ) *p = '\\';
        for ( char *p = fb; *p; ++p ) if ( *p == '/' ) *p = '\\';
        return _stricmp( fa, fb ) == 0;
    }

    bool UniqueCopyTarget( const char *src, char *dst, int dstCap, std::string *rel )
    {
        char dir[MAX_PATH]; bool unnamed = false;
        if ( !EnsureRefDirectory( dir, sizeof( dir ), &unnamed ) )
        {
            Sys_Printf( "Reference image '%s': could not create the map's refimages directory.\n", BaseName( src ) );
            return false;
        }
        if ( unnamed )
            Sys_Printf( "Reference image '%s': the map is unnamed; using %s.\n", BaseName( src ), dir );

        const char *base = BaseName( src );
        char stem[MAX_PATH], ext[64];
        _snprintf( stem, sizeof( stem ), "%s", base ); stem[sizeof( stem ) - 1] = '\0';
        char *dot = strrchr( stem, '.' );
        ext[0] = '\0';
        if ( dot ) { _snprintf( ext, sizeof( ext ), "%s", dot ); *dot = '\0'; }
        if ( !stem[0] ) _snprintf( stem, sizeof( stem ), "reference" );

        for ( int i = 0; i < 10000; ++i )
        {
            char name[MAX_PATH];
            if ( i == 0 ) _snprintf( name, sizeof( name ), "%s%s", stem, ext );
            else          _snprintf( name, sizeof( name ), "%s_%i%s", stem, i, ext );
            name[sizeof( name ) - 1] = '\0';
            _snprintf( dst, dstCap, "%s\\%s", dir, name ); dst[dstCap - 1] = '\0';
            if ( SamePath( src, dst ) || !FileExists( dst ) )
            {
                *rel = std::string( "refimages/" ) + name;
                return true;
            }
        }
        Sys_Printf( "Reference image '%s': no free destination filename.\n", BaseName( src ) );
        return false;
    }

    // WIC is used for WebP/fallback decoding and for clipboard PNG/DIB serialization.
    IWICImagingFactory *WicFactory()
    {
        static bool comTried = false;
        static bool comUsable = false;
        if ( !comTried )
        {
            comTried = true;
            const HRESULT hr = ::CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
            comUsable = SUCCEEDED( hr ) || hr == RPC_E_CHANGED_MODE;
        }
        if ( !comUsable ) return nullptr;
        IWICImagingFactory *factory = nullptr;
        if ( FAILED( ::CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS( &factory ) ) ) )
            return nullptr;
        return factory;
    }

    bool WicEncodePng( IWICImagingFactory *factory, IWICBitmapSource *source,
                       UINT width, UINT height, const wchar_t *outPath )
    {
        IWICStream *stream = nullptr;
        IWICBitmapEncoder *encoder = nullptr;
        IWICBitmapFrameEncode *frame = nullptr;
        IPropertyBag2 *bag = nullptr;
        bool ok = false;
        if ( SUCCEEDED( factory->CreateStream( &stream ) )
          && SUCCEEDED( stream->InitializeFromFilename( outPath, GENERIC_WRITE ) )
          && SUCCEEDED( factory->CreateEncoder( GUID_ContainerFormatPng, nullptr, &encoder ) )
          && SUCCEEDED( encoder->Initialize( stream, WICBitmapEncoderNoCache ) )
          && SUCCEEDED( encoder->CreateNewFrame( &frame, &bag ) )
          && SUCCEEDED( frame->Initialize( bag ) )
          && SUCCEEDED( frame->SetSize( width, height ) ) )
        {
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if ( SUCCEEDED( frame->SetPixelFormat( &format ) )
              && IsEqualGUID( format, GUID_WICPixelFormat32bppBGRA )
              && SUCCEEDED( frame->WriteSource( source, nullptr ) )
              && SUCCEEDED( frame->Commit() )
              && SUCCEEDED( encoder->Commit() ) )
                ok = true;
        }
        if ( bag ) bag->Release();
        if ( frame ) frame->Release();
        if ( encoder ) encoder->Release();
        if ( stream ) stream->Release();
        if ( !ok ) ::DeleteFileW( outPath );
        return ok;
    }

    bool WicDecoderToPng( IWICImagingFactory *factory, IWICBitmapDecoder *decoder,
                          const char *outPath, int *pixW, int *pixH )
    {
        IWICBitmapFrameDecode *frame = nullptr;
        IWICFormatConverter *conv = nullptr;
        UINT w = 0, h = 0;
        bool ok = false;
        wchar_t wide[1200];
        if ( ::MultiByteToWideChar( CP_ACP, 0, outPath, -1, wide, 1200 ) <= 0 ) return false;
        if ( SUCCEEDED( decoder->GetFrame( 0, &frame ) )
          && SUCCEEDED( frame->GetSize( &w, &h ) ) && w > 0 && h > 0
          && w <= 4096u && h <= 4096u
          && SUCCEEDED( factory->CreateFormatConverter( &conv ) )
          && SUCCEEDED( conv->Initialize( frame, GUID_WICPixelFormat32bppBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom ) ) )
            ok = WicEncodePng( factory, conv, w, h, wide );
        if ( conv ) conv->Release();
        if ( frame ) frame->Release();
        if ( ok )
        {
            if ( pixW ) *pixW = (int)w;
            if ( pixH ) *pixH = (int)h;
        }
        return ok;
    }

    bool WicFileToPng( const char *srcPath, const char *outPath, int *pixW, int *pixH )
    {
        IWICImagingFactory *factory = WicFactory();
        if ( !factory ) return false;
        wchar_t wide[1200];
        IWICBitmapDecoder *decoder = nullptr;
        bool ok = false;
        if ( ::MultiByteToWideChar( CP_ACP, 0, srcPath, -1, wide, 1200 ) > 0
          && SUCCEEDED( factory->CreateDecoderFromFilename( wide, nullptr, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand,
                                                            &decoder ) ) )
            ok = WicDecoderToPng( factory, decoder, outPath, pixW, pixH );
        if ( decoder ) decoder->Release();
        factory->Release();
        return ok;
    }

    bool WicFileDimensions( const char *srcPath, int *pixW, int *pixH )
    {
        IWICImagingFactory *factory = WicFactory();
        if ( !factory ) return false;
        wchar_t wide[1200];
        IWICBitmapDecoder *decoder = nullptr;
        IWICBitmapFrameDecode *frame = nullptr;
        UINT w = 0, h = 0;
        bool ok = ::MultiByteToWideChar( CP_ACP, 0, srcPath, -1, wide, 1200 ) > 0
               && SUCCEEDED( factory->CreateDecoderFromFilename( wide, nullptr, GENERIC_READ,
                                                                  WICDecodeMetadataCacheOnDemand,
                                                                  &decoder ) )
               && SUCCEEDED( decoder->GetFrame( 0, &frame ) )
               && SUCCEEDED( frame->GetSize( &w, &h ) )
               && w > 0 && h > 0 && w <= 4096u && h <= 4096u;
        if ( frame ) frame->Release();
        if ( decoder ) decoder->Release();
        factory->Release();
        if ( ok )
        {
            if ( pixW ) *pixW = (int)w;
            if ( pixH ) *pixH = (int)h;
        }
        return ok;
    }

    bool WicMemoryToPng( BYTE *data, DWORD size, const char *outPath, int *pixW, int *pixH )
    {
        if ( !data || !size ) return false;
        IWICImagingFactory *factory = WicFactory();
        if ( !factory ) return false;
        IWICStream *stream = nullptr;
        IWICBitmapDecoder *decoder = nullptr;
        bool ok = false;
        if ( SUCCEEDED( factory->CreateStream( &stream ) )
          && SUCCEEDED( stream->InitializeFromMemory( data, size ) )
          && SUCCEEDED( factory->CreateDecoderFromStream( stream, nullptr,
                                                          WICDecodeMetadataCacheOnDemand,
                                                          &decoder ) ) )
            ok = WicDecoderToPng( factory, decoder, outPath, pixW, pixH );
        if ( decoder ) decoder->Release();
        if ( stream ) stream->Release();
        factory->Release();
        return ok;
    }

    unsigned MaskValue( unsigned pixel, unsigned mask )
    {
        if ( !mask ) return 0;
        unsigned shift = 0;
        while ( shift < 32 && ( ( mask >> shift ) & 1u ) == 0 ) ++shift;
        unsigned bits = 0, m = mask >> shift;
        while ( bits < 32 && ( m & 1u ) ) { ++bits; m >>= 1; }
        const unsigned value = ( pixel & mask ) >> shift;
        const unsigned maxv = bits >= 32 ? 0xFFFFFFFFu : ( ( 1u << bits ) - 1u );
        return maxv ? (unsigned)( ( (uint64_t)value * 255u + maxv / 2u ) / maxv ) : 0;
    }

    bool DibToBgra( const BYTE *dib, SIZE_T bytes, std::vector<BYTE> *out,
                    int *outW, int *outH )
    {
        if ( !dib || bytes < sizeof( BITMAPINFOHEADER ) ) return false;
        const BITMAPINFOHEADER *h = (const BITMAPINFOHEADER *)dib;
        if ( h->biSize < sizeof( BITMAPINFOHEADER ) || h->biSize > bytes
          || h->biWidth <= 0 || h->biHeight == 0 || h->biHeight == INT_MIN
          || ( h->biBitCount != 24 && h->biBitCount != 32 )
          || ( h->biCompression != BI_RGB && h->biCompression != BI_BITFIELDS ) )
            return false;
        const int width = h->biWidth;
        const int height = h->biHeight < 0 ? -h->biHeight : h->biHeight;
        if ( width > 4096 || height > 4096 ) return false; // same cap as KiwiIwi_Probe
        SIZE_T offset = h->biSize;
        unsigned masks[4] = { 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0u };
        if ( h->biCompression == BI_BITFIELDS )
        {
            // V2/V3/V4/V5 headers embed the masks at the same offsets; a
            // 40-byte BITMAPINFOHEADER carries three DWORDs immediately after it.
            if ( h->biSize >= 52u )
            {
                memcpy( masks, dib + 40, 12 );
                if ( h->biSize >= 56u ) memcpy( &masks[3], dib + 52, 4 );
            }
            else
            {
                if ( offset + 12 > bytes ) return false;
                memcpy( masks, dib + offset, 12 );
                offset += 12;
            }
        }
        else if ( h->biBitCount == 32 && h->biSize >= 52u )
        {
            unsigned embedded[4] = { 0,0,0,0 };
            memcpy( embedded, dib + 40, 12 );
            if ( h->biSize >= 56u ) memcpy( &embedded[3], dib + 52, 4 );
            if ( embedded[0] && embedded[1] && embedded[2] )
            {
                memcpy( masks, embedded, sizeof( masks ) );
            }
        }
        if ( h->biClrUsed )
        {
            const SIZE_T paletteBytes = (SIZE_T)h->biClrUsed * sizeof( RGBQUAD );
            if ( paletteBytes / sizeof( RGBQUAD ) != h->biClrUsed || paletteBytes > bytes - offset )
                return false;
            offset += paletteBytes;
        }
        if ( h->biSize >= sizeof( BITMAPV5HEADER ) )
        {
            const BITMAPV5HEADER *v5 = (const BITMAPV5HEADER *)dib;
            if ( v5->bV5ProfileData && v5->bV5ProfileSize )
            {
                const SIZE_T profile = (SIZE_T)v5->bV5ProfileData;
                const SIZE_T profileSize = (SIZE_T)v5->bV5ProfileSize;
                if ( profile > bytes || profileSize > bytes - profile ) return false;
                offset = (std::max)( offset, profile + profileSize );
                if ( offset > (SIZE_T)-1 - 3u ) return false;
                offset = ( offset + 3u ) & ~(SIZE_T)3u;
            }
        }
        const SIZE_T srcStride = ( ( (SIZE_T)width * h->biBitCount + 31u ) / 32u ) * 4u;
        if ( offset > bytes || srcStride > bytes || (SIZE_T)height > ( bytes - offset ) / srcStride )
            return false;
        out->assign( (SIZE_T)width * (SIZE_T)height * 4u, 255u );
        for ( int y = 0; y < height; ++y )
        {
            const int sy = h->biHeight < 0 ? y : ( height - 1 - y );
            const BYTE *src = dib + offset + (SIZE_T)sy * srcStride;
            BYTE *dst = out->data() + (SIZE_T)y * (SIZE_T)width * 4u;
            for ( int x = 0; x < width; ++x )
            {
                if ( h->biBitCount == 24 && h->biCompression == BI_RGB )
                {
                    dst[x * 4 + 0] = src[x * 3 + 0];
                    dst[x * 4 + 1] = src[x * 3 + 1];
                    dst[x * 4 + 2] = src[x * 3 + 2];
                }
                else
                {
                    unsigned pixel = 0;
                    const int bpp = h->biBitCount / 8;
                    memcpy( &pixel, src + (SIZE_T)x * bpp, (size_t)bpp );
                    dst[x * 4 + 0] = (BYTE)MaskValue( pixel, masks[2] );
                    dst[x * 4 + 1] = (BYTE)MaskValue( pixel, masks[1] );
                    dst[x * 4 + 2] = (BYTE)MaskValue( pixel, masks[0] );
                    dst[x * 4 + 3] = masks[3] ? (BYTE)MaskValue( pixel, masks[3] ) : 255u;
                }
            }
        }
        *outW = width; *outH = height;
        return true;
    }

    bool BgraToPng( const std::vector<BYTE> &pixels, int w, int h, const char *outPath )
    {
        if ( w <= 0 || h <= 0 || pixels.size() != (size_t)w * (size_t)h * 4u ) return false;
        const UINT bufferSize = (UINT)pixels.size();
        if ( (size_t)bufferSize != pixels.size() ) return false;
        IWICImagingFactory *factory = WicFactory();
        if ( !factory ) return false;
        IWICBitmap *bitmap = nullptr;
        bool ok = false;
        if ( SUCCEEDED( factory->CreateBitmapFromMemory( (UINT)w, (UINT)h,
                                                         GUID_WICPixelFormat32bppBGRA,
                                                         (UINT)w * 4u, bufferSize,
                                                         const_cast<BYTE *>( pixels.data() ),
                                                         &bitmap ) ) )
        {
            wchar_t wide[1200];
            if ( ::MultiByteToWideChar( CP_ACP, 0, outPath, -1, wide, 1200 ) > 0 )
                ok = WicEncodePng( factory, bitmap, (UINT)w, (UINT)h, wide );
        }
        if ( bitmap ) bitmap->Release();
        factory->Release();
        return ok;
    }

    uint32_t HashByte( uint32_t h, unsigned char b ) { return ( h ^ b ) * 16777619u; }

    bool AssetName( const krefImage_t &r, char *out, int cap )
    {
        char absolute[1200];
        if ( !AbsoluteForRecord( r, absolute, sizeof( absolute ) ) ) return false;
        struct _stat64 st;
        if ( _stat64( absolute, &st ) != 0 ) return false;
        uint32_t h = 2166136261u;
        for ( size_t i = 0; i < r.file.size(); ++i )
            h = HashByte( h, (unsigned char)tolower( (unsigned char)r.file[i] ) );
        const uint64_t values[2] = { (uint64_t)st.st_size, (uint64_t)st.st_mtime };
        for ( int v = 0; v < 2; ++v )
            for ( int b = 0; b < 8; ++b ) h = HashByte( h, (unsigned char)( values[v] >> ( b * 8 ) ) );
        _snprintf( out, cap, "kiwi_refimg_%08x", (unsigned)h );
        out[cap - 1] = '\0';
        return true;
    }

    int BlendTemplate()
    {
        static int cached = -2;
        if ( cached != -2 ) return cached;
        cached = -1;
        for ( int i = 0; i < KiwiMat_TemplateCount(); ++i )
        {
            const kiwiMatTemplateInfo_t *info = KiwiMat_TemplateInfo( i );
            if ( info && info->techSet && !_stricmp( info->techSet, "l_sm_b0c0" ) )
            {
                if ( KiwiMat_ResolveTemplate( i ) ) cached = i;
                break;
            }
        }
        return cached;
    }

    bool EnsureMaterial( krefImage_t &r )
    {
        if ( r.material ) return true;
        // A missing sidecar target is checked once, not on every paint.  A panel
        // edit, import, map load, or undo clears this cache and permits a retry.
        if ( s_missingWarned.find( r.file ) != s_missingWarned.end() ) return false;
        char asset[64] = { 0 };
        std::map<std::string, std::string>::const_iterator known = s_fileAsset.find( r.file );
        if ( known != s_fileAsset.end() )
        {
            _snprintf( asset, sizeof( asset ), "%s", known->second.c_str() );
            asset[sizeof( asset ) - 1] = '\0';
        }
        else if ( !AssetName( r, asset, sizeof( asset ) ) )
        {
            char absolute[1200];
            AbsoluteForRecord( r, absolute, sizeof( absolute ) );
            if ( s_missingWarned.insert( r.file ).second )
                Sys_Printf( "Reference image '%s' is missing; the sidecar record was kept.\n", r.file.c_str() );
            return false;
        }
        else s_fileAsset[r.file] = asset;

        std::map<std::string, assetCache_t>::iterator cached = s_assetCache.find( asset );
        if ( cached != s_assetCache.end() )
        {
            r.material = cached->second.material;
            r.pixW = cached->second.pixW; r.pixH = cached->second.pixH;
            return r.material != nullptr;
        }
        assetCache_t &cache = s_assetCache[asset]; // null also caches a failed attempt

        char absolute[1200];
        if ( !AbsoluteForRecord( r, absolute, sizeof( absolute ) ) ) return false;
        char err[512] = { 0 };
        kiwiIwiSource_t probe;
        memset( &probe, 0, sizeof( probe ) );
        bool probed = KiwiIwi_Probe( absolute, &probe, err, sizeof( err ) );
        if ( probed ) { r.pixW = probe.width; r.pixH = probe.height; cache.pixW = r.pixW; cache.pixH = r.pixH; }
        else
        {
            int w = 0, h = 0;
            if ( WicFileDimensions( absolute, &w, &h ) )
            {
                r.pixW = w; r.pixH = h; cache.pixW = w; cache.pixH = h;
            }
        }

        char imageQPath[96];
        _snprintf( imageQPath, sizeof( imageQPath ), "images/%s.iwi", asset );
        imageQPath[sizeof( imageQPath ) - 1] = '\0';
        bool imageOk = KiwiIwi_ExistsOnDisk( asset );
        std::string decodePath = absolute;
        std::string tempPng;
        if ( !imageOk )
        {
            kiwiIwiOptions_t opt;
            opt.compress = true; opt.resampleToPot = true; opt.encoding = KIWI_IWI_ENC_COLOR;
            kiwiIwiResult_t result;
            memset( &result, 0, sizeof( result ) );
            imageOk = KiwiIwi_WriteFromFile( decodePath.c_str(), imageQPath, &opt, &result,
                                              err, sizeof( err ) );
            if ( !imageOk )
            {
                tempPng = std::string( absolute ) + ".kiwi.png";
                int w = 0, h = 0;
                if ( WicFileToPng( absolute, tempPng.c_str(), &w, &h ) )
                {
                    r.pixW = w; r.pixH = h;
                    imageOk = KiwiIwi_WriteFromFile( tempPng.c_str(), imageQPath, &opt, &result,
                                                      err, sizeof( err ) );
                }
                else if ( !_stricmp( Extension( absolute ), ".webp" ) )
                {
                    Sys_Printf( "Reference image '%s': WebP decode failed; install 'WebP Image Extensions' from the Microsoft Store.\n",
                                r.file.c_str() );
                    err[0] = '\0';
                }
                ::DeleteFileA( tempPng.c_str() );
            }
        }
        if ( !imageOk )
        {
            if ( err[0] ) Sys_Printf( "Reference image '%s': image conversion failed (%s).\n", r.file.c_str(), err );
            return false;
        }
        if ( !KiwiIwi_VerifyOnDisk( imageQPath, err, sizeof( err ) ) )
        {
            Sys_Printf( "Reference image '%s': generated image verification failed (%s).\n", r.file.c_str(), err );
            return false;
        }

        const int tpl = BlendTemplate();
        if ( tpl < 0 )
        {
            Sys_Printf( "Reference image '%s': no shipped l_sm_b0c0 material template was found.\n", r.file.c_str() );
            return false;
        }
        if ( !KiwiMat_ExistsOnDisk( asset ) )
        {
            kiwiMatFields_t fields;
            memset( &fields, 0, sizeof( fields ) );
            _snprintf( fields.name, sizeof( fields.name ), "%s", asset );
            _snprintf( fields.imageName, sizeof( fields.imageName ), "%s", asset );
            fields.usage = 1;
            fields.locale = 1u;
            fields.autoTexScaleWidth = (unsigned short)(std::max)( 1, (std::min)( 65535, r.pixW ? r.pixW : 512 ) );
            fields.autoTexScaleHeight = (unsigned short)(std::max)( 1, (std::min)( 65535, r.pixH ? r.pixH : 512 ) );
            fields.surfaceType = 0;
            if ( !KiwiMat_Write( tpl, &fields, err, sizeof( err ) ) )
            {
                Sys_Printf( "Reference image '%s': material creation failed (%s).\n", r.file.c_str(), err );
                return false;
            }
        }
        kiwiMatVerify_t verify;
        memset( &verify, 0, sizeof( verify ) );
        if ( !KiwiMat_Verify( asset, &verify, err, sizeof( err ) ) )
        {
            Sys_Printf( "Reference image '%s': material verification failed (%s).\n", r.file.c_str(), err );
            return false;
        }
        char loadName[96];
        _snprintf( loadName, sizeof( loadName ), "wc/%s", asset );
        loadName[sizeof( loadName ) - 1] = '\0';
        Material *material = Material_Load( loadName, 0 );
        if ( !material || Material_IsDefault( material ) )
        {
            Sys_Printf( "Reference image '%s': Material_Load('%s') did not resolve the generated material.\n",
                        r.file.c_str(), loadName );
            return false;
        }
        r.material = material;
        if ( r.pixW <= 0 ) r.pixW = verify.colorMapWidth;
        if ( r.pixH <= 0 ) r.pixH = verify.colorMapHeight;
        cache.material = material; cache.pixW = r.pixW; cache.pixH = r.pixH;
        return true;
    }

    void XYPoint( int imgX, int imgY, float out[3] )
    {
        const xywndState_t *w = Ed_ActiveXY();
        out[0] = w->m_vOrigin[0]; out[1] = w->m_vOrigin[1]; out[2] = w->m_vOrigin[2];
        const float x = ( (float)imgX - (float)w->m_nWidth * 0.5f ) / w->m_fScale;
        const float y = ( (float)w->m_nHeight * 0.5f - (float)imgY ) / w->m_fScale;
        if ( w->m_nViewType == ED_VIEW_XY ) { out[0] += x; out[1] += y; }
        else if ( w->m_nViewType == ED_VIEW_XZ ) { out[0] += x; out[2] += y; }
        else { out[1] += x; out[2] += y; }
    }

    int CameraDominantAxis()
    {
        const camera_s *c = Ed_Camera();
        int axis = 0;
        if ( fabsf( c->vpn[1] ) > fabsf( c->vpn[axis] ) ) axis = 1;
        if ( fabsf( c->vpn[2] ) > fabsf( c->vpn[axis] ) ) axis = 2;
        return axis;
    }

    bool PlacementForViewport( int id, int imgX, int imgY, placement_t *out )
    {
        out->axis = 2;
        out->origin[0] = out->origin[1] = out->origin[2] = 0.0f;
        if ( id == RTT_XY )
        {
            out->axis = ClampAxis( Ed_ActiveXY()->m_nViewType );
            XYPoint( imgX, imgY, out->origin );
            return true;
        }
        if ( id != RTT_CAMERA ) return false;
        out->axis = CameraDominantAxis();
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) ) return true;
        kiwiDropHit_t hit;
        if ( KiwiDrop_Trace( ray, false, &hit ) )
            memcpy( out->origin, hit.point, sizeof( out->origin ) );
        else
            for ( int i = 0; i < 3; ++i ) out->origin[i] = ray.origin[i] + ray.dir[i] * 512.0f;
        return true;
    }

    placement_t PlacementUnderCursor()
    {
        LoadProfile();
        placement_t p;
        p.axis = 2; p.origin[0] = p.origin[1] = p.origin[2] = 0.0f;
        POINT pt;
        int id = -1, x = 0, y = 0;
        if ( ::GetCursorPos( &pt )
          && ImGuiShell_ViewportAtScreen( pt.x, pt.y, &id, &x, &y )
          && PlacementForViewport( id, x, y, &p ) )
            return p;
        if ( ImGuiShell_LastViewport( &id, &x, &y ) )
            PlacementForViewport( id, x, y, &p );
        return p;
    }

    bool CopySource( const char *src, char *copied, int cap, std::string *rel )
    {
        if ( !src || !FileExists( src ) || !AcceptedImage( src ) ) return false;
        if ( !UniqueCopyTarget( src, copied, cap, rel ) ) return false;
        if ( SamePath( src, copied ) ) return true;
        if ( !::CopyFileA( src, copied, TRUE ) )
        {
            Sys_Printf( "Reference image '%s': copy into the map folder failed.\n", BaseName( src ) );
            return false;
        }
        return true;
    }

    void InitialSize( krefImage_t &r )
    {
        const int w = r.pixW > 0 ? r.pixW : 1;
        const int h = r.pixH > 0 ? r.pixH : 1;
        if ( w >= h ) { r.width = 512.0f; r.height = 512.0f * (float)h / (float)w; }
        else          { r.height = 512.0f; r.width = 512.0f * (float)w / (float)h; }
        r.width = (std::max)( KREF_MIN_SIZE, r.width );
        r.height = (std::max)( KREF_MIN_SIZE, r.height );
    }

    bool ImportOne( const char *src, const placement_t &place )
    {
        LoadProfile();
        char copied[1200]; std::string rel;
        if ( !CopySource( src, copied, sizeof( copied ), &rel ) ) return false;
        krefImage_t r;
        r.file = rel;
        r.axis = ClampAxis( place.axis );
        memcpy( r.origin, place.origin, sizeof( r.origin ) );
        r.opacity = s_defaultOpacity;
        char display[256];
        StemName( copied, display, sizeof( display ) );
        r.name = display;
        if ( !EnsureMaterial( r ) )
        {
            std::map<std::string, std::string>::iterator known = s_fileAsset.find( r.file );
            if ( known != s_fileAsset.end() ) { s_assetCache.erase( known->second ); s_fileAsset.erase( known ); }
            if ( !SamePath( src, copied ) ) ::DeleteFileA( copied );
            return false;
        }
        InitialSize( r );
        s_images.push_back( r );
        s_selected = (int)s_images.size() - 1;
        Touch( true );
        Sys_Printf( "Reference image '%s' added on the %s plane.\n", r.file.c_str(),
                    r.axis == 2 ? "XY" : ( r.axis == 1 ? "XZ" : "YZ" ) );
        return true;
    }

    bool NewPastePath( char *absolute, int cap, std::string *rel )
    {
        char dir[MAX_PATH];
        if ( !EnsureRefDirectory( dir, sizeof( dir ), nullptr ) ) return false;
        SYSTEMTIME st; ::GetLocalTime( &st );
        for ( int i = 0; i < 1000; ++i )
        {
            char name[128];
            if ( i )
                _snprintf( name, sizeof( name ), "pasted_%04u%02u%02u_%02u%02u%02u_%i.png",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, i );
            else
                _snprintf( name, sizeof( name ), "pasted_%04u%02u%02u_%02u%02u%02u.png",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond );
            name[sizeof( name ) - 1] = '\0';
            _snprintf( absolute, cap, "%s\\%s", dir, name ); absolute[cap - 1] = '\0';
            if ( !FileExists( absolute ) )
            {
                *rel = std::string( "refimages/" ) + name;
                return true;
            }
        }
        return false;
    }

    void AxisBasis( const krefImage_t &r, float u[3], float v[3], float n[3] )
    {
        float bu[3] = { 0.0f, 0.0f, 0.0f };
        float bv[3] = { 0.0f, 0.0f, 0.0f };
        n[0] = n[1] = n[2] = 0.0f;
        n[r.axis] = 1.0f;
        if ( r.axis == 2 )      { bu[0] = 1.0f; bv[1] = 1.0f; }
        else if ( r.axis == 1 ) { bu[0] = 1.0f; bv[2] = 1.0f; }
        else                    { bu[1] = 1.0f; bv[2] = 1.0f; }
        const float a = r.rotation * KREF_PI / 180.0f;
        const float c = cosf( a ), s = sinf( a );
        for ( int i = 0; i < 3; ++i )
        {
            u[i] = bu[i] * c + bv[i] * s;
            v[i] = bv[i] * c - bu[i] * s;
        }
    }

    void Corners( const krefImage_t &r, float out[4][3] )
    {
        float u[3], v[3], n[3];
        AxisBasis( r, u, v, n );
        static const float sign[4][2] = { { -1,-1 }, { 1,-1 }, { 1,1 }, { -1,1 } };
        for ( int c = 0; c < 4; ++c )
            for ( int k = 0; k < 3; ++k )
                out[c][k] = r.origin[k] + u[k] * sign[c][0] * r.width * 0.5f
                                         + v[k] * sign[c][1] * r.height * 0.5f;
    }

    void PlaneLocal( const krefImage_t &r, const float p[3], float *uOut, float *vOut )
    {
        float u[3], v[3], n[3];
        AxisBasis( r, u, v, n );
        float rel[3];
        for ( int k = 0; k < 3; ++k ) rel[k] = p[k] - r.origin[k];
        *uOut = rel[0] * u[0] + rel[1] * u[1] + rel[2] * u[2];
        *vOut = rel[0] * v[0] + rel[1] * v[1] + rel[2] * v[2];
    }

    bool RayPlane( const ray_t &ray, int axis, const float origin[3], float out[3], float *outT = nullptr )
    {
        const float den = ray.dir[axis];
        if ( fabsf( den ) < 1.0e-6f ) return false;
        const float t = ( origin[axis] - ray.origin[axis] ) / den;
        if ( t <= 0.0f || t > 1000000.0f ) return false;
        for ( int k = 0; k < 3; ++k ) out[k] = ray.origin[k] + ray.dir[k] * t;
        if ( outT ) *outT = t;
        return true;
    }

    bool CameraPointOnImagePlane( int x, int y, const krefImage_t &r, float out[3] )
    {
        ray_t ray;
        return Pick_RayFromImagePos( x, y, &ray ) && RayPlane( ray, r.axis, r.origin, out );
    }

    bool CameraPick( int x, int y, int *outIndex, float *outDistance,
                     float hitPoint[3] )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( x, y, &ray ) )
            return false;

        int best = -1;
        int bestLayer = INT_MIN;
        float bestDistance = FLT_MAX;
        float bestPoint[3] = { 0.0f, 0.0f, 0.0f };
        for ( int i = 0; i < (int)s_images.size(); ++i )
        {
            const krefImage_t &r = s_images[i];
            float p[3];
            if ( r.hidden || !r.material
              || !RayPlane( ray, r.axis, r.origin, p ) || !PointInside( r, p ) )
                continue;
            const float dx = p[0] - ray.origin[0];
            const float dy = p[1] - ray.origin[1];
            const float dz = p[2] - ray.origin[2];
            const float distance = sqrtf( dx * dx + dy * dy + dz * dz );
            const bool sameDepth = fabsf( distance - bestDistance ) <= 1.0e-3f;
            if ( best < 0 || distance < bestDistance - 1.0e-3f
              || ( sameDepth && r.layerOrder >= bestLayer ) )
            {
                best = i;
                bestLayer = r.layerOrder;
                bestDistance = distance;
                memcpy( bestPoint, p, sizeof( bestPoint ) );
            }
        }
        if ( best < 0 )
            return false;
        if ( outIndex ) *outIndex = best;
        if ( outDistance ) *outDistance = bestDistance;
        if ( hitPoint ) memcpy( hitPoint, bestPoint, sizeof( bestPoint ) );
        return true;
    }

    bool XYPick( int x, int y, int *outIndex, float hitPoint[3] )
    {
        const xywndState_t *w = Ed_ActiveXY();
        if ( !w ) return false;
        float p[3];
        XYPoint( x, y, p );
        int best = -1, bestLayer = INT_MIN;
        for ( int i = 0; i < (int)s_images.size(); ++i )
        {
            const krefImage_t &r = s_images[i];
            if ( r.hidden || !r.material || r.axis != w->m_nViewType || !PointInside( r, p ) )
                continue;
            if ( best < 0 || r.layerOrder >= bestLayer )
            {
                best = i;
                bestLayer = r.layerOrder;
            }
        }
        if ( best < 0 ) return false;
        if ( outIndex ) *outIndex = best;
        if ( hitPoint ) memcpy( hitPoint, p, sizeof( p ) );
        return true;
    }

    bool XYWorldToImage( const float p[3], float *outX, float *outY )
    {
        const xywndState_t *w = Ed_ActiveXY();
        if ( !w || !p || !outX || !outY || !( w->m_fScale > 0.0f ) )
            return false;
        int h = 0, v = 1;
        if ( w->m_nViewType == ED_VIEW_XZ ) { h = 0; v = 2; }
        else if ( w->m_nViewType == ED_VIEW_YZ ) { h = 1; v = 2; }
        *outX = ( p[h] - w->m_vOrigin[h] ) * w->m_fScale + (float)w->m_nWidth * 0.5f;
        *outY = (float)w->m_nHeight * 0.5f - ( p[v] - w->m_vOrigin[v] ) * w->m_fScale;
        return _finite( *outX ) && _finite( *outY );
    }

    bool RectPoint( float x0, float y0, float x1, float y1, float x, float y )
    {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }

    bool RectSegment( float x0, float y0, float x1, float y1,
                      float ax, float ay, float bx, float by )
    {
        float t0 = 0.0f, t1 = 1.0f;
        const float dx = bx - ax, dy = by - ay;
        const float p[4] = { -dx, dx, -dy, dy };
        const float q[4] = { ax - x0, x1 - ax, ay - y0, y1 - ay };
        for ( int i = 0; i < 4; ++i )
        {
            if ( p[i] == 0.0f )
            {
                if ( q[i] < 0.0f ) return false;
                continue;
            }
            const float t = q[i] / p[i];
            if ( p[i] < 0.0f )
            {
                if ( t > t1 ) return false;
                if ( t > t0 ) t0 = t;
            }
            else
            {
                if ( t < t0 ) return false;
                if ( t < t1 ) t1 = t;
            }
        }
        return true;
    }

    bool PointInQuad( const float q[4][2], float x, float y )
    {
        bool inside = false;
        for ( int i = 0, j = 3; i < 4; j = i++ )
        {
            const bool crosses = ( q[i][1] > y ) != ( q[j][1] > y );
            if ( crosses )
            {
                const float at = ( q[j][0] - q[i][0] ) * ( y - q[i][1] )
                               / ( q[j][1] - q[i][1] ) + q[i][0];
                if ( x < at ) inside = !inside;
            }
        }
        return inside;
    }

    bool QuadHitsRect( const float q[4][2], float x0, float y0, float x1, float y1,
                       bool crossing )
    {
        bool all = true, any = false;
        for ( int i = 0; i < 4; ++i )
        {
            const bool in = RectPoint( x0, y0, x1, y1, q[i][0], q[i][1] );
            all = all && in;
            any = any || in;
        }
        if ( !crossing ) return all;
        if ( any ) return true;
        for ( int i = 0; i < 4; ++i )
            if ( RectSegment( x0, y0, x1, y1, q[i][0], q[i][1],
                              q[( i + 1 ) & 3][0], q[( i + 1 ) & 3][1] ) )
                return true;
        return PointInQuad( q, x0, y0 ) || PointInQuad( q, x1, y0 )
            || PointInQuad( q, x1, y1 ) || PointInQuad( q, x0, y1 );
    }

    void ApplyRectInternal( float x0, float y0, float x1, float y1,
                            bool crossing, bool shift, bool ctrl, bool camera )
    {
        if ( !SelectionAllowsImages() )
            return;
        if ( x0 > x1 ) { const float t = x0; x0 = x1; x1 = t; }
        if ( y0 > y1 ) { const float t = y0; y0 = y1; y1 = t; }

        const xywndState_t *w = camera ? nullptr : Ed_ActiveXY();
        int best = -1, bestLayer = INT_MIN;
        for ( int i = 0; i < (int)s_images.size(); ++i )
        {
            const krefImage_t &r = s_images[i];
            if ( r.hidden || !r.material || ( w && r.axis != w->m_nViewType ) )
                continue;
            float world[4][3], q[4][2];
            Corners( r, world );
            bool projected = true;
            for ( int k = 0; k < 4; ++k )
                projected = projected && ( camera
                    ? Pick_WorldToImage( world[k], &q[k][0], &q[k][1] )
                    : XYWorldToImage( world[k], &q[k][0], &q[k][1] ) );
            if ( !projected || !QuadHitsRect( q, x0, y0, x1, y1, crossing ) )
                continue;
            if ( best < 0 || r.layerOrder >= bestLayer )
            {
                best = i;
                bestLayer = r.layerOrder;
            }
        }

        if ( ctrl )
        {
            if ( best >= 0 && s_selected == best )
                KiwiRefImage_Select( -1 );
        }
        else if ( best >= 0 )
        {
            KiwiRefImage_Select( best );
        }
        else if ( !shift )
        {
            KiwiRefImage_Select( -1 );
        }
        if ( best >= 0 && s_selected == best )
            s_focusPending = true;
        g_nUpdateBits = -1;
    }

    void DrawOne( krefImage_t &r )
    {
        if ( r.hidden || !EnsureMaterial( r ) ) return;
        float corners[4][3];
        Corners( r, corners );
        float xyzw[4][4], normal[4][3], st[4][2], color[4];
        const float alpha = ClampF( r.opacity, 0.0f, 1.0f );
        float rgba[4] = { 1.0f, 1.0f, 1.0f, alpha };
        GfxColor packed;
        Byte4PackPixelColor( rgba, &packed );
        float packedFloat;
        memcpy( &packedFloat, &packed.packed, sizeof( packedFloat ) );
        float u[3], v[3], n[3];
        AxisBasis( r, u, v, n );
        static const float uv[4][2] = { { 0,1 }, { 1,1 }, { 1,0 }, { 0,0 } };
        for ( int i = 0; i < 4; ++i )
        {
            xyzw[i][0] = corners[i][0]; xyzw[i][1] = corners[i][1];
            xyzw[i][2] = corners[i][2]; xyzw[i][3] = 1.0f;
            normal[i][0] = n[0]; normal[i][1] = n[1]; normal[i][2] = n[2];
            st[i][0] = r.flipU ? 1.0f - uv[i][0] : uv[i][0];
            st[i][1] = r.flipV ? 1.0f - uv[i][1] : uv[i][1];
            color[i] = packedFloat;
        }
        // The blend material is back-face culled; duplicate the two triangles in
        // reverse order so a construction image is readable from either side.
        static const uint16_t index[12] = { 0,1,2, 0,2,3, 0,2,1, 0,3,2 };
        const float neutral[4] = { 0,0,0,0 };
        R_AddCmdSetMaterialColor( neutral );
        R_AddRenderCmdDrawTris( r.material, TECHNIQUE_UNLIT, 12, index, 4,
                                xyzw, normal, color, st );
    }

    void DrawSelection( int axisFilter, float scale, bool camera )
    {
        if ( s_selected < 0 || s_selected >= (int)s_images.size() ) return;
        const krefImage_t &r = s_images[s_selected];
        if ( r.hidden || !r.material || ( axisFilter >= 0 && r.axis != axisFilter ) ) return;
        float c[4][3];
        Corners( r, c );
        KiwiLines_Begin( 20, 2 );
        KiwiLines_Color( r.locked ? 0.9f : 1.0f, r.locked ? 0.35f : 0.75f, 0.15f );
        for ( int i = 0; i < 4; ++i ) KiwiLines_Add( c[i], c[( i + 1 ) & 3] );
        float u[3], v[3], n[3];
        AxisBasis( r, u, v, n );
        for ( int i = 0; i < 4; ++i )
        {
            float h = camera ? KiwiCam_WorldPerPixel( c[i] ) * 5.0f
                             : ( scale > 0.0f ? 5.0f / scale : 5.0f );
            if ( !( h > 0.0f ) ) h = 1.0f;
            float a[3], b[3];
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = c[i][k] - u[k] * h; b[k] = c[i][k] + u[k] * h;
            }
            KiwiLines_Add( a, b );
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = c[i][k] - v[k] * h; b[k] = c[i][k] + v[k] * h;
            }
            KiwiLines_Add( a, b );
        }
        KiwiLines_Flush();
    }

    void DrawHover( int axisFilter, bool camera )
    {
        const int index = camera ? s_hoverCamera : s_hoverXY;
        if ( index < 0 || index >= (int)s_images.size() || index == s_selected )
            return;
        const krefImage_t &r = s_images[index];
        if ( r.hidden || !r.material || ( axisFilter >= 0 && r.axis != axisFilter ) )
            return;
        float c[4][3];
        Corners( r, c );
        KiwiLines_Begin( 4, 1 );
        KiwiLines_Color( 0.42f, 0.58f, 0.72f );
        for ( int i = 0; i < 4; ++i )
            KiwiLines_Add( c[i], c[( i + 1 ) & 3] );
        KiwiLines_Flush();
    }

    void DrawImages( int axisFilter, float scale, bool camera )
    {
        std::vector<int> order;
        for ( int i = 0; i < (int)s_images.size(); ++i )
            if ( !s_images[i].hidden && ( axisFilter < 0 || s_images[i].axis == axisFilter ) )
                order.push_back( i );
        std::stable_sort( order.begin(), order.end(), []( int a, int b ) {
            return s_images[a].layerOrder < s_images[b].layerOrder;
        } );
        for ( size_t i = 0; i < order.size(); ++i ) DrawOne( s_images[order[i]] );
        static const float white[4] = { 1,1,1,1 };
        if ( !order.empty() ) R_AddCmdSetMaterialColor( white );
        DrawHover( axisFilter, camera );
        DrawSelection( axisFilter, scale, camera );
    }

    bool PointInside( const krefImage_t &r, const float p[3] )
    {
        float u, v;
        PlaneLocal( r, p, &u, &v );
        return fabsf( u ) <= r.width * 0.5f && fabsf( v ) <= r.height * 0.5f;
    }

    int CameraHit( int x, int y, int *corner, float hitPoint[3] )
    {
        *corner = -1;
        if ( s_selected >= 0 && s_selected < (int)s_images.size()
          && !s_images[s_selected].hidden && s_images[s_selected].material )
        {
            float c[4][3]; Corners( s_images[s_selected], c );
            for ( int i = 0; i < 4; ++i )
            {
                float sx, sy;
                if ( Pick_WorldToImage( c[i], &sx, &sy )
                  && hypotf( sx - (float)x, sy - (float)y ) <= 10.0f )
                {
                    *corner = i; memcpy( hitPoint, c[i], sizeof( float ) * 3 );
                    return s_selected;
                }
            }
        }
        // Keep the armed editor's established layer-first pick policy.  Ordinary
        // unarmed object selection uses CameraPick's eye-distance policy instead.
        ray_t ray;
        if ( !Pick_RayFromImagePos( x, y, &ray ) ) return -1;
        int best = -1, bestLayer = INT_MIN;
        float bestT = FLT_MAX, p[3];
        for ( int i = 0; i < (int)s_images.size(); ++i )
        {
            const krefImage_t &r = s_images[i];
            float t;
            if ( r.hidden || !r.material
              || !RayPlane( ray, r.axis, r.origin, p, &t ) || !PointInside( r, p ) )
                continue;
            if ( r.layerOrder > bestLayer || ( r.layerOrder == bestLayer && t <= bestT ) )
            {
                best = i; bestLayer = r.layerOrder; bestT = t;
                memcpy( hitPoint, p, sizeof( p ) );
            }
        }
        return best;
    }

    int XYHit( int x, int y, int *corner, float hitPoint[3] )
    {
        const xywndState_t *w = Ed_ActiveXY();
        XYPoint( x, y, hitPoint );
        *corner = -1;
        if ( s_selected >= 0 && s_selected < (int)s_images.size() )
        {
            const krefImage_t &r = s_images[s_selected];
            if ( !r.hidden && r.material && r.axis == w->m_nViewType )
            {
                float c[4][3]; Corners( r, c );
                const float maxDist = w->m_fScale > 0.0f ? 10.0f / w->m_fScale : 10.0f;
                float best = maxDist;
                for ( int i = 0; i < 4; ++i )
                {
                    float d = 0.0f;
                    for ( int k = 0; k < 3; ++k ) if ( k != r.axis ) d += ( c[i][k] - hitPoint[k] ) * ( c[i][k] - hitPoint[k] );
                    d = sqrtf( d );
                    if ( d <= best ) { best = d; *corner = i; }
                }
                if ( *corner >= 0 ) return s_selected;
            }
        }
        int hit = -1;
        return XYPick( x, y, &hit, hitPoint ) ? hit : -1;
    }

    bool AltDown() { return ( ::GetKeyState( VK_MENU ) & 0x8000 ) != 0; }

    bool BeginDrag( int index, int corner, bool shift, const float point[3] )
    {
        if ( index < 0 || index >= (int)s_images.size() )
        {
            if ( s_selected != -1 ) { s_selected = -1; Touch( false ); }
            return false;
        }
        if ( s_selected != index ) { s_selected = index; Touch( false ); }
        if ( s_images[index].locked )
        {
            // Consume the complete click/drag sequence without mutating either
            // the image or the legacy geometry tools underneath it.
            s_drag = drag_t();
            s_drag.active = true; s_drag.index = index; s_drag.base = s_images[index];
            return true;
        }
        BeginEdit( shift && corner < 0 ? "rotate reference image"
                                      : ( corner >= 0 ? "scale reference image" : "move reference image" ) );
        s_drag.active = true; s_drag.index = index; s_drag.corner = corner;
        s_drag.mode = shift && corner < 0 ? KREF_DRAG_ROTATE
                    : ( corner >= 0 ? KREF_DRAG_SCALE : KREF_DRAG_MOVE );
        s_drag.base = s_images[index];
        memcpy( s_drag.startPoint, point, sizeof( s_drag.startPoint ) );
        for ( int k = 0; k < 3; ++k ) s_drag.grabOffset[k] = s_drag.base.origin[k] - point[k];
        float lu, lv;
        PlaneLocal( s_drag.base, point, &lu, &lv );
        s_drag.startAngle = atan2f( lv, lu ) * 180.0f / KREF_PI;
        return true;
    }

    void DragTo( const float rawPoint[3] )
    {
        if ( !s_drag.active || s_drag.index < 0 || s_drag.index >= (int)s_images.size() ) return;
        if ( s_drag.mode == KREF_DRAG_NONE ) return;
        krefImage_t &r = s_images[s_drag.index];
        float p[3]; memcpy( p, rawPoint, sizeof( p ) );
        if ( s_drag.mode == KREF_DRAG_MOVE )
        {
            float desired[3];
            for ( int k = 0; k < 3; ++k ) desired[k] = p[k] + s_drag.grabOffset[k];
            if ( !AltDown() )
            {
                float snapped[3];
                if ( KiwiGrid_Snap( desired, snapped ) )
                {
                    const float depth = desired[s_drag.base.axis];
                    memcpy( desired, snapped, sizeof( desired ) );
                    desired[s_drag.base.axis] = depth;
                }
            }
            memcpy( r.origin, desired, sizeof( r.origin ) );
        }
        else if ( s_drag.mode == KREF_DRAG_SCALE )
        {
            if ( !AltDown() )
            {
                float snapped[3];
                if ( KiwiGrid_Snap( p, snapped ) )
                {
                    const float depth = p[s_drag.base.axis];
                    memcpy( p, snapped, sizeof( p ) ); p[s_drag.base.axis] = depth;
                }
            }
            float lu, lv; PlaneLocal( s_drag.base, p, &lu, &lv );
            float nw = (std::max)( KREF_MIN_SIZE, fabsf( lu ) * 2.0f );
            float nh = (std::max)( KREF_MIN_SIZE, fabsf( lv ) * 2.0f );
            if ( s_drag.base.keepAspect && s_drag.base.width > 0.0f && s_drag.base.height > 0.0f )
            {
                const float aspect = s_drag.base.width / s_drag.base.height;
                const float sw = nw / s_drag.base.width, sh = nh / s_drag.base.height;
                if ( sw >= sh ) nh = nw / aspect; else nw = nh * aspect;
            }
            r.width = nw; r.height = nh;
        }
        else if ( s_drag.mode == KREF_DRAG_ROTATE )
        {
            float lu, lv; PlaneLocal( s_drag.base, p, &lu, &lv );
            float angle = s_drag.base.rotation + atan2f( lv, lu ) * 180.0f / KREF_PI - s_drag.startAngle;
            if ( !AltDown() )
            {
                const float steps = angle / 15.0f;
                angle = ( steps >= 0.0f ? floorf( steps + 0.5f ) : ceilf( steps - 0.5f ) ) * 15.0f;
            }
            r.rotation = angle;
        }
        Sanitize( r );
        Touch( true );
    }

    void EndDrag( bool cancel )
    {
        if ( !s_drag.active ) return;
        if ( cancel ) CancelEdit(); else CommitPending();
        s_drag = drag_t();
    }
}

namespace
{
    const char *AxisName( int axis ) { return axis == 2 ? "XY" : ( axis == 1 ? "XZ" : "YZ" ); }

    void EditContinuous( const storeSnap_t &before, const char *label )
    {
        if ( !s_pendingHave ) BeginEditFrom( before, label );
        if ( s_selected >= 0 && s_selected < (int)s_images.size() ) Sanitize( s_images[s_selected] );
        Touch( true );
    }

    // Called after EVERY continuous widget, changed or not.  The release frame of a
    // DragFloat / SliderFloat reports no value change, so polling the deactivation
    // only inside the changed branch (the old shape) left the edit pending forever:
    // the journal never received the record and Ctrl+Z skipped straight past it.
    void EditSettle()
    {
        if ( s_pendingHave && !s_move.active && !s_drag.active
          && ImGui::IsItemDeactivatedAfterEdit() )
            CommitPending();
    }

    void EditImmediate( const storeSnap_t &before, const char *label )
    {
        if ( s_selected >= 0 && s_selected < (int)s_images.size() ) Sanitize( s_images[s_selected] );
        Touch( true );
        ImmediateCommit( before, label );
    }

    void AddFileDialog()
    {
        char path[1200] = { 0 };
        OPENFILENAMEA ofn;
        memset( &ofn, 0, sizeof( ofn ) );
        ofn.lStructSize = sizeof( ofn );
        ofn.hwndOwner = g_qeglobals.d_hwndMain;
        ofn.lpstrFile = path;
        ofn.nMaxFile = sizeof( path );
        ofn.lpstrFilter = "Reference images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds;*.webp\0All files\0*.*\0\0";
        ofn.lpstrTitle = "Add reference image";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if ( !::GetOpenFileNameA( &ofn ) ) return;
        const placement_t place = PlacementUnderCursor();
        BeginEdit( "add reference image" );
        if ( ImportOne( path, place ) ) CommitPending(); else CancelEdit();
    }

    void FitSelectedToBrushSelection()
    {
        if ( s_selected < 0 || s_selected >= (int)s_images.size() ) return;
        float mins[3], maxs[3];
        if ( !KiwiFocus_SelectionBounds( mins, maxs ) )
        {
            Sys_Printf( "Reference image '%s': Fit to selection needs selected geometry.\n",
                        s_images[s_selected].file.c_str() );
            return;
        }
        const storeSnap_t before = Snapshot();
        krefImage_t &r = s_images[s_selected];
        for ( int k = 0; k < 3; ++k ) r.origin[k] = ( mins[k] + maxs[k] ) * 0.5f;
        float u[3], v[3], n[3]; AxisBasis( r, u, v, n );
        float hu = 0.0f, hv = 0.0f;
        for ( int i = 0; i < 8; ++i )
        {
            float d[3];
            for ( int k = 0; k < 3; ++k )
                d[k] = ( ( i & ( 1 << k ) ) ? maxs[k] : mins[k] ) - r.origin[k];
            hu = (std::max)( hu, fabsf( d[0] * u[0] + d[1] * u[1] + d[2] * u[2] ) );
            hv = (std::max)( hv, fabsf( d[0] * v[0] + d[1] * v[1] + d[2] * v[2] ) );
        }
        r.width = (std::max)( KREF_MIN_SIZE, hu * 2.0f );
        r.height = (std::max)( KREF_MIN_SIZE, hv * 2.0f );
        EditImmediate( before, "fit reference image to selection" );
    }

    void ResetSelectedSize()
    {
        if ( s_selected < 0 || s_selected >= (int)s_images.size() ) return;
        const storeSnap_t before = Snapshot();
        EnsureMaterial( s_images[s_selected] );
        InitialSize( s_images[s_selected] );
        EditImmediate( before, "reset reference image size" );
    }

    void DrawImageList()
    {
        if ( !ImGui::BeginChild( "##refimages", ImVec2( 0.0f, 180.0f ), ImGuiChildFlags_Borders ) )
        {
            ImGui::EndChild(); return;
        }
        for ( int i = 0; i < (int)s_images.size(); ++i )
        {
            krefImage_t &r = s_images[i];
            ImGui::PushID( i );
            storeSnap_t before = Snapshot();
            if ( ImGui::SmallButton( r.hidden ? "Show" : "Hide" ) )
            {
                r.hidden = !r.hidden; EditImmediate( before, r.hidden ? "hide reference image" : "show reference image" );
            }
            ImGui::SameLine();
            before = Snapshot();
            if ( ImGui::SmallButton( r.locked ? "Unlock" : "Lock" ) )
            {
                r.locked = !r.locked; EditImmediate( before, r.locked ? "lock reference image" : "unlock reference image" );
            }
            ImGui::SameLine();
            const bool missing = s_missingWarned.find( r.file ) != s_missingWarned.end();
            if ( missing ) ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 0.25f, 0.2f, 1.0f ) );
            const std::string label = ( r.name.empty() ? r.file : r.name ) + "  [" + AxisName( r.axis ) + "]";
            if ( ImGui::Selectable( label.c_str(), i == s_selected ) )
            {
                const ImGuiIO &io = ImGui::GetIO();
                KiwiRefImage_ApplyClick( i, io.KeyShift, io.KeyCtrl, false );
            }
            if ( missing ) ImGui::PopStyleColor();
            if ( missing && ImGui::IsItemHovered() ) ImGui::SetTooltip( "Missing: %s", r.file.c_str() );
            ImGui::PopID();
        }
        if ( s_images.empty() ) ImGui::TextDisabled( "No reference images in this map." );
        ImGui::EndChild();
    }

    void DrawSelectedEditor()
    {
        if ( s_selected < 0 || s_selected >= (int)s_images.size() ) return;
        krefImage_t &r = s_images[s_selected];
        ImGui::SeparatorText( "Selected image" );
        ImGui::TextDisabled( "%s", r.file.c_str() );

        char name[512];
        _snprintf( name, sizeof( name ), "%s", r.name.c_str() ); name[sizeof( name ) - 1] = '\0';
        storeSnap_t before = Snapshot();
        if ( ImGui::InputText( "Name", name, sizeof( name ) ) )
        {
            r.name = name; EditContinuous( before, "rename reference image" );
        }
        EditSettle();

        static const char *axisItems[] = { "YZ (X normal)", "XZ (Y normal)", "XY (Z normal)" };
        before = Snapshot(); int axis = r.axis;
        if ( ImGui::Combo( "Axis", &axis, axisItems, 3 ) )
        {
            r.axis = axis; EditImmediate( before, "change reference image axis" );
        }

        before = Snapshot();
        if ( ImGui::InputFloat3( "Origin", r.origin, "%.3f" ) ) EditContinuous( before, "move reference image" );
        EditSettle();

        before = Snapshot(); const float oldW = r.width, oldH = r.height;
        if ( ImGui::DragFloat( "Width", &r.width, 1.0f, KREF_MIN_SIZE, 131072.0f, "%.3f" ) )
        {
            if ( r.keepAspect && oldW > 0.0f ) r.height = oldH * r.width / oldW;
            EditContinuous( before, "resize reference image" );
        }
        EditSettle();
        before = Snapshot(); const float priorW = r.width, priorH = r.height;
        if ( ImGui::DragFloat( "Height", &r.height, 1.0f, KREF_MIN_SIZE, 131072.0f, "%.3f" ) )
        {
            if ( r.keepAspect && priorH > 0.0f ) r.width = priorW * r.height / priorH;
            EditContinuous( before, "resize reference image" );
        }
        EditSettle();
        before = Snapshot();
        if ( ImGui::Checkbox( "Keep aspect", &r.keepAspect ) ) EditImmediate( before, "change reference image aspect lock" );

        before = Snapshot();
        if ( ImGui::DragFloat( "Rotation", &r.rotation, 1.0f, -180.0f, 180.0f, "%.1f deg" ) )
            EditContinuous( before, "rotate reference image" );
        EditSettle();
        before = Snapshot();
        if ( ImGui::Checkbox( "Flip U", &r.flipU ) ) EditImmediate( before, "flip reference image" );
        ImGui::SameLine(); before = Snapshot();
        if ( ImGui::Checkbox( "Flip V", &r.flipV ) ) EditImmediate( before, "flip reference image" );

        before = Snapshot();
        if ( ImGui::SliderFloat( "Opacity", &r.opacity, 0.0f, 1.0f, "%.2f" ) )
            EditContinuous( before, "change reference image opacity" );
        EditSettle();
        before = Snapshot();
        if ( ImGui::InputInt( "Layer order", &r.layerOrder ) ) EditContinuous( before, "reorder reference image" );
        EditSettle();

        // Safety net: a panel edit whose widget is no longer active (focus moved to
        // another window, the panel was hidden mid-drag) is finished here so the
        // record can never stay pending across the next gesture.
        if ( s_pendingHave && !s_move.active && !s_drag.active && !ImGui::IsAnyItemActive() )
            CommitPending();

        if ( ImGui::Button( "Fit to selection bounds" ) ) FitSelectedToBrushSelection();
        ImGui::SameLine();
        if ( ImGui::Button( "Reset size" ) ) ResetSelectedSize();
        if ( ImGui::Button( "Remove" ) )
            KiwiRefImage_DeleteAt( s_selected );
    }

    void WriteQuoted( FILE *f, const char *key, const std::string &value )
    {
        fprintf( f, "%s \"", key );
        for ( size_t i = 0; i < value.size(); ++i )
        {
            const char c = value[i];
            if ( c == '\\' || c == '"' ) fputc( '\\', f );
            if ( c != '\r' && c != '\n' ) fputc( c, f );
        }
        fprintf( f, "\"\n" );
    }

    const char *SkipSpace( const char *p )
    {
        while ( p && ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) ) ++p;
        return p ? p : "";
    }

    bool Keyword( const char *line, const char *word, const char **rest )
    {
        line = SkipSpace( line );
        const size_t n = strlen( word );
        if ( _strnicmp( line, word, n ) || ( line[n] && line[n] != ' ' && line[n] != '\t' && line[n] != '\r' && line[n] != '\n' ) )
            return false;
        if ( rest ) *rest = SkipSpace( line + n );
        return true;
    }

    bool ReadString( const char *text, std::string *out )
    {
        text = SkipSpace( text ); out->clear();
        if ( *text != '"' )
        {
            const char *end = text + strlen( text );
            while ( end > text && ( end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t' ) ) --end;
            out->assign( text, end ); return !out->empty();
        }
        ++text;
        while ( *text && *text != '"' )
        {
            if ( *text == '\\' && text[1] ) ++text;
            out->push_back( *text++ );
        }
        return *text == '"';
    }

    void FinishParsedImage()
    {
        Sanitize( s_parseImage );
        if ( s_parseImage.name.empty() )
        {
            char stem[256]; StemName( s_parseImage.file.c_str(), stem, sizeof( stem ) );
            s_parseImage.name = stem;
        }
        if ( !RelativeSafe( s_parseImage.file ) )
        {
            Sys_Printf( "Reference image '%s': unsafe sidecar path ignored.\n", s_parseImage.file.c_str() );
        }
        else
        {
            s_parseImage.material = nullptr;
            char absolute[1200];
            if ( !AbsoluteForRecord( s_parseImage, absolute, sizeof( absolute ) ) || !FileExists( absolute ) )
            {
                if ( s_missingWarned.insert( s_parseImage.file ).second )
                    Sys_Printf( "Reference image '%s' is missing; the sidecar record was kept.\n",
                                s_parseImage.file.c_str() );
            }
            s_images.push_back( s_parseImage );
            ++s_generation; g_nUpdateBits = -1;
        }
        s_parseImage = krefImage_t();
        s_parseActive = false;
    }
}

void KiwiRefImage_Draw()
{
    LoadProfile();
    if ( s_focusPending )
    {
        s_focusPending = false;
        KiwiWindows_Set( KIWI_WIN_REFIMAGES, true );
        ImGuiShell_FocusTab( "Reference Images" );
    }
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_REFIMAGES );
    if ( !open || !*open ) return;
    if ( KiwiWindows_JustOpened( KIWI_WIN_REFIMAGES ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );
    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_REFIMAGES ), open ) )
    {
        if ( ImGui::Button( "Add image..." ) ) AddFileDialog();
        ImGui::SameLine();
        if ( ImGui::Button( "Paste" ) ) KiwiRefImage_PasteClipboard();
        ImGui::SameLine();
        if ( ImGui::Button( s_armed ? "Stop editing" : "Edit images" ) )
        {
            if ( s_drag.active )
            {
                ImGuiShell_AbortViewportInput();
                if ( s_drag.active ) EndDrag( true );
            }
            s_armed = !s_armed; Touch( false );
        }
        if ( s_armed ) ImGui::TextDisabled( "LMB move/scale; Shift+body rotates; Alt disables snapping/free-rotates; Delete removes; Esc disarms." );

        ImGui::SetNextItemWidth( 150.0f );
        if ( ImGui::SliderFloat( "New opacity", &s_defaultOpacity, 0.0f, 1.0f, "%.2f" ) ) SaveDefaults();
        DrawImageList();
        DrawSelectedEditor();
        if ( s_pendingHave && !s_drag.active && !ImGui::IsAnyItemActive() ) CommitPending();
    }
    ImGui::End();
}

void KiwiRefImage_WriteSidecar( FILE *f )
{
    if ( !f ) return;
    CommitPending();
    for ( size_t i = 0; i < s_images.size(); ++i )
    {
        const krefImage_t &r = s_images[i];
        WriteQuoted( f, "refimage", r.file );
        fprintf( f, "axis %i\n", r.axis );
        fprintf( f, "origin %.9g %.9g %.9g\n", r.origin[0], r.origin[1], r.origin[2] );
        fprintf( f, "size %.9g %.9g\n", r.width, r.height );
        if ( !r.keepAspect ) fprintf( f, "keepaspect 0\n" );
        fprintf( f, "rotation %.9g\n", r.rotation );
        fprintf( f, "flip %i %i\n", r.flipU ? 1 : 0, r.flipV ? 1 : 0 );
        fprintf( f, "opacity %.9g\n", r.opacity );
        fprintf( f, "locked %i\n", r.locked ? 1 : 0 );
        fprintf( f, "hidden %i\n", r.hidden ? 1 : 0 );
        fprintf( f, "order %i\n", r.layerOrder );
        WriteQuoted( f, "name", r.name );
        fprintf( f, "end\n" );
    }
}

bool KiwiRefImage_ParseSidecarLine( const char *line )
{
    const char *rest = nullptr;
    if ( !s_parseActive )
    {
        if ( !Keyword( line, "refimage", &rest ) ) return false;
        s_parseImage = krefImage_t();
        ReadString( rest, &s_parseImage.file );
        s_parseActive = true;
        return true;
    }
    if ( Keyword( line, "end", &rest ) ) { FinishParsedImage(); return true; }
    if ( Keyword( line, "axis", &rest ) ) { sscanf( rest, "%i", &s_parseImage.axis ); return true; }
    if ( Keyword( line, "origin", &rest ) ) { sscanf( rest, "%f %f %f", &s_parseImage.origin[0], &s_parseImage.origin[1], &s_parseImage.origin[2] ); return true; }
    if ( Keyword( line, "size", &rest ) ) { sscanf( rest, "%f %f", &s_parseImage.width, &s_parseImage.height ); return true; }
    if ( Keyword( line, "keepaspect", &rest ) ) { int v = 1; sscanf( rest, "%i", &v ); s_parseImage.keepAspect = v != 0; return true; }
    if ( Keyword( line, "rotation", &rest ) ) { sscanf( rest, "%f", &s_parseImage.rotation ); return true; }
    if ( Keyword( line, "flip", &rest ) ) { int u = 0, v = 0; sscanf( rest, "%i %i", &u, &v ); s_parseImage.flipU = u != 0; s_parseImage.flipV = v != 0; return true; }
    if ( Keyword( line, "opacity", &rest ) ) { sscanf( rest, "%f", &s_parseImage.opacity ); return true; }
    if ( Keyword( line, "locked", &rest ) ) { int v = 0; sscanf( rest, "%i", &v ); s_parseImage.locked = v != 0; return true; }
    if ( Keyword( line, "hidden", &rest ) ) { int v = 0; sscanf( rest, "%i", &v ); s_parseImage.hidden = v != 0; return true; }
    if ( Keyword( line, "order", &rest ) ) { sscanf( rest, "%i", &s_parseImage.layerOrder ); return true; }
    if ( Keyword( line, "name", &rest ) ) { ReadString( rest, &s_parseImage.name ); return true; }
    return true; // unknown extension inside a refimage block belongs to that block
}

void KiwiRefImage_ResetForNewMap()
{
    s_images.clear(); s_selected = -1; s_armed = false; s_drag = drag_t(); s_move = move_t();
    s_hoverCamera = s_hoverXY = -1; s_xyClickOwned = false; s_focusPending = false;
    s_parseActive = false; s_parseImage = krefImage_t();
    s_fileAsset.clear(); s_assetCache.clear(); s_missingWarned.clear();
    KiwiRefImage_UndoReset();
    ++s_generation; g_nUpdateBits = -1;
}
