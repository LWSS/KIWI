#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Terrain Sculpt — the KIWI replacement for the Advanced Patch Editor (Y).
//
// An armed camera paint tool over the ported patch control grid: raise/dig, set
// height, smooth, noise, texture layers and blend, with circular or square brushes and
// selectable falloff.  Height strokes move the terrain as ONE vertex graph (seams are
// shared vertices, containment looks at every patch a point belongs to).  Strokes reuse
// the ported per-patch undo marking (Patch_Paint / Undo_AddPaintedBrush / Patch_PaintFinish) so a stroke is
// one legacy undo record, exactly like the original Alt+LMB paint.
//
// The panel owns the settings; the viewport bridge (kiwi_viewport.cpp) owns the
// press/drag/release cycle while the tool is armed.  While armed, bare LMB in the
// camera sculpts (Far Cry Sandbox model); Y arms, Y or Esc disarms.

void KiwiTerrain_MenuItem();
void KiwiTerrain_Draw();

// Windows-menu / palette route (KIWI_CMD_TERRAIN_PANEL).
void KiwiTerrain_TogglePanel();
bool KiwiTerrain_PanelVisible();
// Y (classic 33130): arm the current tool, opening the panel; Y again disarms.
void KiwiTerrain_ToggleArmed();
// Show the panel with a given tool selected (kiwi_grass.cpp routes here for Grass).
void KiwiTerrain_OpenWithTool( int tool );

// Texture layers live on ONE patch (patchMesh_t.kiwiLayer[4]; weights in the vertex
// colour bytes).  The patch VB upload asks how many extra blended runs to build and
// lets this file rewrite each run's material + vertex alpha.
struct patchMesh_t;
struct curveVert_t;
struct Material;
int  KiwiTerrain_ExtraLayerCount( patchMesh_t *def );
// A layer run's texcoord scale (patchMesh_t.kiwiLayerScale of its slot); false = the base's ST.
bool KiwiTerrain_LayerStScale( patchMesh_t *def, int run, int baseRuns, float out[2] );
void KiwiTerrain_LayerUpload( patchMesh_t *def, int run, int baseRuns,
                              unsigned int *color, const curveVert_t *verts,
                              int vertCount, Material **material );
// True while an armed sculpt tool owns the patch wireframe: the ported camera draws
// (selected white mesh, unselected pref grid) stand down and DrawWorld draws the grid
// only within the brush's reach around the cursor; Tab hides it entirely.
bool KiwiTerrain_HideWireframe();

// J (KIWI_CMD_JOIN): merge selected edge-adjacent terrain sheets into one grid.
// JoinSelected returns the number of joins made, or -1 when the selection is not two or
// more terrain patches (so the other J arms can run).  CanJoinSelected = JoinSelected
// would join at least one pair.
// The texture browser just applied a new BASE material to this selected terrain patch
// (TexWnd_ApplyMaterialAtIndex -> Patch_KiwiReNaturalizeSelected).  Its paint layers stay on
// top; a layer of that same material folds into the base.
void KiwiTerrain_BaseRetextured( patchMesh_t *def );

bool KiwiTerrain_CanJoinSelected();
int  KiwiTerrain_JoinSelected();

bool KiwiTerrain_IsArmed();
// Viewport hint strip (kiwi_hints.cpp): the armed tool's key grammar as chips - the
// panel carries no key text.  Prompts = the tool's own keys (left strip), Keys = the
// ones every tool shares: radius, strength, Tab, Ctrl+Tab, disarm (right strip).
// Static storage; 0 when not armed.
struct kiwiPrompt_t;
int  KiwiTerrain_HudPrompts( const kiwiPrompt_t **out );
int  KiwiTerrain_HudKeys( const kiwiPrompt_t **out );
bool KiwiTerrain_HandleDown( int imgX, int imgY, bool shift, bool ctrl );
void KiwiTerrain_HandleDrag( int imgX, int imgY );
void KiwiTerrain_HandleUp();
void KiwiTerrain_HandleAbort();
// Esc / + / - / Ctrl+wheel radius, Shift+wheel strength, Ctrl+(Shift+)Tab tool.  True = consumed.
bool KiwiTerrain_HandleKey( int vk );
// Armed wheel: Ctrl = radius, Shift = strength, Alt = the tool's own value (Set height:
// the target Z, 8 units a notch, 1 with Shift; every other sculpt tool: strength).
bool KiwiTerrain_HandleWheel( float steps, bool shift, bool ctrl, bool alt = false );

void KiwiTerrain_Hover( int imgX, int imgY, bool over );
void KiwiTerrain_DrawWorld();
// Camera ImGui overlay (kiwi_viewport.cpp KiwiVP_DrawCameraOverlay): the tool's name
// flashed at the top centre when it is armed or swapped; while the height colours are
// on, a colour scale on the left edge with the heights in the display units (yd / ft /
// in) and a marker at the cursor's height.  Never claims hover.
void KiwiTerrain_DrawOverlay( float imgMinX, float imgMinY, float imgW, float imgH );

// -kiwitest entry points (kiwi_test.cpp `terrain` verb): the tool by name (raise,
// setheight, smooth, noise, texture, blend, grass, trim), a setting by name (outer,
// inner, strength, speed, falloff, shape, chunk, cell, expand, basezon, basez, defaults,
// texfixed, texsize, relaytex (the panel's Texture size re-lay of the selection),
// targetz, unselected, carry, contain, ...), arming, and one whole stroke: a
// vertical ray through (x, y) resolved like the camera cursor, held for `seconds`, then
// released.
bool KiwiTerrain_TestSetTool( const char *name );
bool KiwiTerrain_TestSet( const char *key, float value );
// Texture paint's brush material ("base" = erase to base).
bool KiwiTerrain_TestSetPaintMaterial( const char *name );
bool KiwiTerrain_TestTessellate( float cell );     // 0 = keep the panel's cell size
bool KiwiTerrain_TestSplit( float chunk );         // 0 = keep the panel's chunk size
void KiwiTerrain_TestArm( bool armed );
bool KiwiTerrain_TestStroke( float x, float y, float seconds, bool shift, bool ctrl );
// `expect terrainz x y z`: the highest terrain surface under (x, y) (a vertical ray).
bool KiwiTerrain_TestHeightAt( float x, float y, float *outZ );
// `expect terrainmatch x0 y0 x1 y1`: the patches under the two points share grid axes and
// texture mapping (a gap fill against its neighbour).  `why` gets the measured values.
bool KiwiTerrain_TestGridMatch( float x0, float y0, float x1, float y1, char *why, int whySize );
// `expect terraincell x y size`: the mean cell edge of the patch under (x, y), both grid axes.
bool KiwiTerrain_TestCellAt( float x, float y, float *outCellX, float *outCellY );
// `expect terraintex x y size`: world units one repeat of the base texture covers along S and
// T on the patch under (x, y) (its texture plane, least squares).
bool KiwiTerrain_TestTexRepeat( float x, float y, float *outS, float *outT );
// `expect terrainlayer x y <material|base> <weight>`: under (x, y), the base material's name
// (`base`), or the weight 0..255 of the named layer at the nearest control point (-1 = the
// patch has no such layer).  False when no terrain is under the point.
bool KiwiTerrain_TestBaseMaterial( float x, float y, char *out, int outSize );
// `terrain layerscale <material> <scale>`: the panel's layer tiling, map-wide, one undo record
// (false when no terrain carries the material).  `expect terrainlayerscale x y <material> <s>`:
// that layer's stored slot scale and the scale its camera run draws with (false = no such layer
// under the point, or s != t).
bool KiwiTerrain_TestSetLayerScale( const char *material, float scale );
// `terrain basetiling <x> <y> <tiling>`: the base row's tiling for the base material of the
// patch under (x, y), map-wide, one undo record.  `expect terrainbasetiling x y <t>`: read back.
bool KiwiTerrain_TestSetBaseTiling( float x, float y, float tiling );
// `expect terrainrows <n>`: the panel's "Layers of the selected patch" rows as last drawn - how
// many, how far the widest ends past the column, the tallest row's line step and one line's.
int  KiwiTerrain_TestLayerRows( float *outOverflow, float *outStep, float *outLine );
bool KiwiTerrain_TestBaseTiling( float x, float y, float *outTiling );
bool KiwiTerrain_TestLayerScale( float x, float y, const char *material, float *outStored, float *outDrawn );
bool KiwiTerrain_TestLayerWeight( float x, float y, const char *material, int *outWeight );
