#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Terrain Sculpt — the KIWI replacement for the Advanced Patch Editor (Y).
//
// An armed camera paint tool over the ported patch control grid: raise/dig,
// flatten / set height, smooth, noise, texture-blend weight and vertex colour, with
// circular or square brushes and selectable falloff.  Strokes reuse the ported
// per-patch undo marking (Patch_Paint / sub_45E770 / PMESH_18) so a stroke is one
// legacy undo record, exactly like the original Alt+LMB paint.
//
// The panel owns the settings; the viewport bridge (kiwi_viewport.cpp) owns the
// press/drag/release cycle while the tool is armed.  While armed, bare LMB in the
// camera sculpts (Far Cry Sandbox model); Esc disarms.

void KiwiTerrain_MenuItem();
void KiwiTerrain_Draw();

// Windows-menu / palette / Y-key route (KIWI_CMD_TERRAIN_PANEL, classic 33130).
void KiwiTerrain_TogglePanel();
bool KiwiTerrain_PanelVisible();
// Show the panel with a given tool selected (kiwi_grass.cpp routes here for Grass).
void KiwiTerrain_OpenWithTool( int tool );

// Texture layers live on ONE patch (patchMesh_t.kiwiLayer[4]; weights in the vertex
// colour bytes).  The patch VB upload asks how many extra blended runs to build and
// lets this file rewrite each run's material + vertex alpha.
struct patchMesh_t;
struct curveVert_t;
struct Material;
int  KiwiTerrain_ExtraLayerCount( patchMesh_t *def );
void KiwiTerrain_LayerUpload( patchMesh_t *def, int run, int baseRuns,
                              unsigned int *color, const curveVert_t *verts,
                              int vertCount, Material **material );
// True while an armed sculpt tool owns the patch wireframe: the ported camera draws
// (selected white mesh, unselected pref grid) stand down and DrawWorld draws the grid
// only within the brush's reach around the cursor; Tab hides it entirely.
bool KiwiTerrain_HideWireframe();

// J (KIWI_CMD_JOIN): merge selected edge-adjacent terrain sheets into one grid.
// JoinSelected returns the number of joins made, or -1 when the selection is not two or
// more terrain patches (so the other J arms can run).
bool KiwiTerrain_CanJoinSelected();
int  KiwiTerrain_JoinSelected();

bool KiwiTerrain_IsArmed();
bool KiwiTerrain_HandleDown( int imgX, int imgY, bool shift, bool ctrl );
void KiwiTerrain_HandleDrag( int imgX, int imgY );
void KiwiTerrain_HandleUp();
void KiwiTerrain_HandleAbort();
bool KiwiTerrain_HandleEscape();
// Esc / + / - / Ctrl+wheel radius, Shift+wheel strength.  True = consumed.
bool KiwiTerrain_HandleKey( int vk );
bool KiwiTerrain_HandleWheel( float steps, bool shift, bool ctrl );

void KiwiTerrain_Hover( int imgX, int imgY, bool over );
void KiwiTerrain_DrawWorld();
// Camera ImGui overlay (kiwi_viewport.cpp KiwiVP_DrawCameraOverlay): while the
// height colours are on, a colour scale on the left edge with the heights in the
// display units (yd / ft / in) and a marker at the cursor's height.  Never claims hover.
void KiwiTerrain_DrawOverlay( float imgMinX, float imgMinY, float imgW, float imgH );

// -kiwitest entry points (kiwi_test.cpp `terrain` verb): the tool by name (raise,
// setheight, smooth, noise, texture, colour, grass, trim), a setting by name (outer,
// inner, strength, speed, falloff, shape, chunk, expand, basez, cells, surfaces,
// targetz, unselected, terrainonly), arming, and one whole stroke: a vertical ray
// through (x, y) resolved like the camera cursor, held for `seconds`, then released.
bool KiwiTerrain_TestSetTool( const char *name );
bool KiwiTerrain_TestSet( const char *key, float value );
// Texture paint's brush material ("base" = erase to base).
bool KiwiTerrain_TestSetPaintMaterial( const char *name );
void KiwiTerrain_TestArm( bool armed );
bool KiwiTerrain_TestStroke( float x, float y, float seconds, bool shift, bool ctrl );
