#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Editor-only, axis-aligned construction/reference images. Records live in the
// adjacent .kiwi sidecar; neither the .map nor the BSP compiler sees them.

#include <stdio.h>
#include <string>

struct Material;

struct krefImage_t
{
    std::string file;          // map-directory relative, normally refimages/<file>
    int         axis;          // plane normal: 2=XY, 1=XZ, 0=YZ
    float       origin[3];
    float       width, height;
    bool        keepAspect;
    float       rotation;      // degrees around the plane normal
    bool        flipU, flipV;
    float       opacity;
    bool        locked;
    bool        hidden;
    int         layerOrder;
    std::string name;

    // Runtime only. Material resolution is lazy and attempted once per asset key.
    Material   *material;
    int         pixW, pixH;

    krefImage_t();
};

// Index API. Indices remain valid until a preceding item is removed or the store
// is replaced by map load/undo.
int                KiwiRefImage_Count();
const krefImage_t *KiwiRefImage_At( int index );
krefImage_t       *KiwiRefImage_MutableAt( int index );
int                KiwiRefImage_Add( const krefImage_t &image );
bool               KiwiRefImage_RemoveAt( int index );
unsigned           KiwiRefImage_Generation();
int                KiwiRefImage_Selected();
void               KiwiRefImage_Select( int index );

// Shared viewport/panel/outliner selection funnel.  Plain replaces every scene
// selection domain, Shift preserves brush/construction selection, and Ctrl only
// removes the already-selected image.  `viewport` requests the Reference Images tab.
void KiwiRefImage_ApplyClick( int index, bool shift, bool ctrl, bool viewport );

// Camera-image picking returns the nearest visible image body hit.  Distance is
// measured from the camera ray origin to the plane hit and is used to arbitrate
// against brush area hits.
bool KiwiRefImage_PickAt( int imgX, int imgY, int *outIndex, float *outDepthOrDist );

// Directional marquee selection.  The first form consumes camera-image pixels;
// the second consumes active-2D-view image pixels.
void KiwiRefImage_ApplyRect( float x0, float y0, float x1, float y1,
                             bool crossing, bool shift, bool ctrl );
void KiwiRefImage_ApplyRectXY( float x0, float y0, float x1, float y1,
                               bool crossing, bool shift, bool ctrl );

// Store mutations used by the Outliner.  Each successful edit is one ref-image
// undo record.  Focus is view-only and frames the image's world bounds.
bool KiwiRefImage_SetHidden( int index, bool hidden );
bool KiwiRefImage_SetLocked( int index, bool locked );
bool KiwiRefImage_SetName( int index, const char *name );
bool KiwiRefImage_DeleteAt( int index );
bool KiwiRefImage_Focus( int index );
// World-space box of the picture quad with its rotation applied; false for a bad index.
bool KiwiRefImage_Bounds( int index, float mins[3], float maxs[3] );

// Dock window and scene rendering.
void KiwiRefImage_Draw();
void KiwiRefImage_DrawWorld();
void KiwiRefImage_DrawXY( int viewType, float scale );

// Armed viewport editor. Camera coordinates are relative to the RTT image;
// XY coordinates use the active orthographic view's RTT image.
bool KiwiRefImage_IsArmed();
bool KiwiRefImage_HandleCameraDown( int imgX, int imgY, bool shift );
void KiwiRefImage_HandleCameraDrag( int imgX, int imgY );
void KiwiRefImage_HandleCameraUp();
void KiwiRefImage_HandleCameraAbort();
void KiwiRefImage_HoverCamera( int imgX, int imgY, bool over );
void KiwiRefImage_HoverXY( int imgX, int imgY, bool over );
bool KiwiRefImage_HandleXYDown( int imgX, int imgY, unsigned int flags );
bool KiwiRefImage_HandleXYMove( int imgX, int imgY );
bool KiwiRefImage_HandleXYUp();
void KiwiRefImage_HandleXYAbort();
bool KiwiRefImage_HandleEscape();
bool KiwiRefImage_HandleDelete();
bool KiwiRefImage_OwnsDelete();

// The image arm of the ordinary G/Move command.  Apply is absolute from the
// begin baseline, matching kiwi_transform's construction branch.
bool KiwiRefImage_CanMove();
bool KiwiRefImage_MoveBegin( float outRef[3] );
void KiwiRefImage_MoveApply( const float total[3] );
void KiwiRefImage_MoveCommit();
void KiwiRefImage_MoveCancel();
// R / S arms: same baseline + undo bracket as Move (MoveBegin first, then
// MoveCommit / MoveCancel); both ABSOLUTE from the baseline.  Rotation about the
// image's own plane normal spins the picture; about any other world axis the
// picture only orbits the pivot (its plane cannot leave the major axes).  Scale
// multiplies the pivot-relative origin per world axis and the width/height by the
// factors carried by the picture's in-plane u/v directions.
void KiwiRefImage_RotateApply( const float pivot[3], int axis, float degrees );
void KiwiRefImage_ScaleApply ( const float pivot[3], const float factor[3] );

// Explorer drop / Edit->Paste. A handled HDROP is finished by the implementation.
bool KiwiRefImage_HandleDropFiles( void *hDrop, int screenX, int screenY );
bool KiwiRefImage_PasteClipboard();

// KIWI2 sidecar hooks. Parse returns true only for a consumed refimage line.
void KiwiRefImage_WriteSidecar( FILE *f );
bool KiwiRefImage_ParseSidecarLine( const char *line );
void KiwiRefImage_ResetForNewMap();

// Whole-store snapshot domain used by kiwi_undo.cpp.
bool KiwiRefImage_UndoPop();
bool KiwiRefImage_RedoPop();
void KiwiRefImage_ClearRedo();
void KiwiRefImage_UndoReset();
