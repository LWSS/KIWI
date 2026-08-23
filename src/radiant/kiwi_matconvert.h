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

// Convert the active material definition in place to the matching lit world family.
// The original raw/main files are moved under materials/_kiwi_backup before the new
// raw definition is written.  False leaves the original active and explains why in errOut.
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

// Texture browser native popup integration.
void KiwiMatConvert_AppendTextureContextMenu( void *menu, qtexture_s *material );
bool KiwiMatConvert_HandleTextureContextCommand( unsigned int command, qtexture_s *material );

// Build window map-health status, bulk action, and confirmation dialog.
void KiwiMatConvert_DrawMapHealth();

