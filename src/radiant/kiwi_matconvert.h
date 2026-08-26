#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include <stddef.h>

struct brush_t;
struct EdLayerGeom;
struct face_t;
struct Material;
struct patchMesh_t;
struct qtexture_s;

// Rewrite the active material into the matching lit world family after backing up raw/main
// files under materials/_kiwi_backup; false keeps the original live and fills errOut.
bool KiwiMatConvert_ToLit( const char *materialName, char *errOut, size_t errLen );

// Loaded-material truth used by the map scan and by the lightmap diagnostic renderer.
bool KiwiMatConvert_MaterialIsNonLit( const Material *material );

void KiwiMatConvert_ResetMapHealth();
void KiwiMatConvert_OnMapLoaded();
void KiwiMatConvert_OnMaterialModeChanged( int mode );

// Ported lightmap-mode hooks.  The legacy gameFlags checker remains the fallback for
// healthy materials; these functions only add visibility and the solid-red diagnostic.
bool KiwiMatConvert_ShouldShowBrushInLightmap( brush_t *brush );
void KiwiMatConvert_ApplyFaceLightmapDiagnostic( face_t *face, EdLayerGeom *geom );
void KiwiMatConvert_ApplyPatchLightmapDiagnostic( patchMesh_t *patch,
                                                  unsigned int *colors, int colorCount,
                                                  Material **material );

void KiwiMatConvert_AppendTextureContextMenu( void *menu, qtexture_s *material );
bool KiwiMatConvert_HandleTextureContextCommand( unsigned int command, qtexture_s *material );

void KiwiMatConvert_DrawMapHealth();
