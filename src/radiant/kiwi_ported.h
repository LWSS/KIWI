// KIWI: shared declarations for the editor/engine symbols that TUs used to redeclare one
// extern at a time. Each trailing comment names the definition (file:line, IDA address).
// active_brushes / selected_brushes are declared in qe3.h.
#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

struct camera_s;
struct orientation_t;
typedef unsigned int ImGuiID;                                                // imgui.h:162

extern int       g_nUpdateBits;                                              // engine_stubs.cpp:772 0x25D5A74; -1 (W_ALL) refreshes everything
extern float     grid_sizes[11];                                             // engine_stubs.cpp:770 0x6DDE5C; indexed by d_gridsize
extern int       g_qeglobals_redispersePatchVerts;                           // engine_stubs.cpp:797 0x25D5A6B; redisperse-control-points mode
extern int       dword_181F51C;                                              // engine_stubs.cpp:801 0x181F51C; realize-state word, MaterialDef_05..09 AND flag bits into it
extern entity_s *world_entity;                                               // map.cpp:53 0x25D5B30
extern char      currentmap[1024];                                           // map.cpp:56 0x23F18D8; active map/prefab path
extern int       modified;                                                   // map.cpp:57 0x23F179C; map dirty flag
extern entity_s  entities;                                                   // entity.cpp:283 0x23F17A0
extern entity_s  entityInsts;                                                // entity.cpp:286 0x23F1748
extern float     world_orient_matrix[4][3];                                  // entity.cpp:298 0x6DE290
extern undo_s   *g_lastundo;                                                 // undo.cpp:73 0x23F162C
extern bool      g_bRotateMode;                                              // drag.cpp:106 0x23F16D9
extern int       g_windingAlloc;                                             // winding.cpp:27 0x24CE4FC; live windings, also winding.h

int         Sys_Printf( const char *fmt, ... );                              // win_qe3.cpp:122 0x499E90
void        MarkMapModified();                                               // win_qe3.cpp:187 0x499BB0
void        Assert( const char *file, int line, int type, const char *fmt, ... ); // engine_stubs.cpp:737 0x49CEA0; logs to stderr, then iassert(0)
char        Byte4PackPixelColor( float *from, GfxColor *out );               // engine_stubs.cpp:825 0x402AC0
float       Vec3Normalize_R( float *v );                                     // engine_stubs.cpp:873 0x40A5E0; returns the pre-normalize length
float      *AnglesToAxis( float *angles, float (*axis)[3] );                 // engine_stubs.cpp:1097 0x4ABEB0; double math, the editor's own (com_math.h's const-angles overload is the game's float one)
int         LoadFile( const char *filename, void **bufferptr );              // cmdlib.cpp:127 0x40ABD0; byte length or -1, CWD-relative fopen
void        Radiant_FL_Log( const char *fmt, ... );                          // mainfrm.cpp:139; also radiant_frame.h

void        OrientationPosToWorldPos( float *out, const float *pos, const orientation_t *orient ); // draw.cpp:24 0x4BA430; out = origin + axis^T * pos
void        OrientationDirToWorldDir( float *out, const orientation_t *orient, const float *dir ); // draw.cpp:35 0x4BA4B0
int         R_Add3DLine( GfxPointVertex *verts, const orientation_t *orient, const float *p1, const float *p2,
                         const unsigned int *color, char width, int vertCount, int maxVertCount ); // draw.cpp:106 0x40C110; flushes when full, returns the new count
void __cdecl R_SortMaterials();                                              // r_ed_scene.cpp:2447 0x4FD910; also r_init.h

camera_s   *Ed_Camera();                                                     // camwnd.cpp:125; never NULL
void        CamWnd_BuildMatrix();                                            // camwnd.cpp:174 0x403470

LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );                // materialdef.cpp:150 0x431640; lyrMtl or radMtl->name, asserts one is set
qtexture_s *MaterialDef_GetLayeredMaterial( MaterialDef *mtlDef );           // materialdef.cpp:159 0x4314A0
void        MaterialDef_02( MaterialDef *mtlDef, int (*cb)( qtexture_s * ) ); // materialdef.cpp:172 0x431520; cb per layer
void        SetMaterial( const char *tex_name, patchMesh_material *mtlDef ); // materialdef.cpp:92 0x4315C0
int         Init_MaterialLayer( MaterialDef *channel, float sampleSize );    // materialdef.cpp:313 0x472C00
qtexture_s *Texture_GetHandle( const char *name );                           // texwnd.cpp:305 0x45A8E0; registers on first use
void        Ed_EnsureCurrentMaterial_Kiwi();                                 // xywnd.cpp:1517; forwards the static Ed_EnsureCurrentMaterial

eclass_t   *Eclass_ForName( int has_brushes, const char *name );             // eclass.cpp:949 0x482190; makes a placeholder class when missing
char       *ValueForKey2( const entity_s *e, const char *key );              // entity.cpp:66 0x4825C0; "" when absent
int         Entity_GetVec3ForKey( entity_s_def *e, float *out, const char *key ); // entity.cpp:80 0x483860
float       Entity_GetFloatValueForKey( const entity_s *e, const char *key ); // entity.cpp:89 0x4837C0
bool        HasKeyValuePair( entity_s_def *e, const char *key );             // entity.cpp:117 0x4838B0
void        DeleteKey( epair_t **head, const char *key );                    // entity.cpp:142 0x483720
void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:201 0x483690
unsigned    KiwiEntity_KeyGeneration();                                      // entity.cpp; KIWI: moves on every SetKeyValue / DeleteKey
void        Entity_LinkBrush( brush_t *b, entity_s *world_ent );             // entity.cpp:402 0x484FC0; def-list linker
void        Entity_UnlinkBrush( brush_t *b );                                // entity.cpp:469 0x485020
bool        Entity_HasEpairMatch( entity_s *e, const char *key, const char *val ); // entity.cpp:508 0x483930
void        Entity_GetOrientation( entity_s_def *ent, orientation_t *orParent, orientation_t *orOut ); // entity.cpp:605 0x482A70; orParent * ent -> orOut
void        Checkkey_Color( entity_s_def *e, const char *key );              // entity.cpp:1025 0x483210
void        Checkkey_Model( entity_s_def *e, const char *key );              // entity.cpp:1120 0x482F70; IDA Checkkey_Model_0
void        Entity_Free( entity_s *e );                                      // entity.cpp:1550 0x485750
entity_s   *Entity_Create( eclass_t *eclass );                               // entity.cpp:1679 0x484980; new group only if every selected brush is world-owned
void        CreateEntityFromName( const char *str );                         // xywnd.cpp:3197
int         UpdateSelection( int wParam, eclass_t *cls );                    // win_ent.cpp:332 0x497180

void        Brush_CheckBuildFaceVis( selbrush_t *b, const float *orient );   // brush.cpp:175 0x477D70
void        Brush_RemoveFace( brush_t *b, unsigned int faceIndex );          // brush.cpp:321 0x471640; shifts the rest down
brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );          // brush.cpp:425 0x4751E0; arg 1 = MaterialDef source
void        Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:473 0x475300
selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner );                // brush.cpp:620 0x475980; new instance of def (++refCount), linked into owner
void        Brush_Free_R( brush_t *def );                                    // brush.cpp:657 0x475AF0; frees the def only, no list/entity cleanup
void        Brush_AddToList2( selbrush_t *b );                               // brush.cpp:869 0x4765A0; links into selected_brushes
void        Brush_RemoveFromList( selbrush_t *b );                           // brush.cpp:910 0x476680; unlinks, clears next/prev
void        Brush_Free( selbrush_t *b );                                     // brush.cpp:942 0x475BA0; unlink + free, --def->refCount
void        Brush_BuildWindings( brush_t *def, int bFull );                  // brush.cpp:1426 0x477AC0; every face
bool        Model_SetModel( entity_brush_s *b, const float *orientMatrix );  // brush.cpp:5284 0x478780; loads the model + orientation into the instance, true if it has one
void        Brush_UpdateSpecialMaterialFlag( brush_t *def );                 // brush.cpp:5810 0x47B940
void        BrushList_RebuildDisplay( selbrush_t *listHead );                // brush.cpp:6340 0x47D060
void        DrawBrush( selbrush_t *b, const orientation_t *orient, int viewType, int technique, GfxColor *col,
                       char width, int drawFlags, const char *layerPrefix ); // brush.cpp:7230 0x47AFC0
entity_s   *Brush_Move( const float *move, brush_t *def, char snap );        // brush.cpp:7698 0x47BA40; moves faces, patch ctrl points, entity origin
void        Face_MoveTexture( const float *surfDef, const float *normal, float *outVecs, const float *uvBase,
                              float rotate, float crossterm );               // texturevecs.cpp:102 0x45A1C0; texdef -> 2x4 world texture matrix
char        FilterBrush( selbrush_t *b, int full );                          // filters.cpp:675 0x46A1F0; non-zero = hidden/filtered

void        CMainFrame_UpdatePatchToolbarButtons();                          // select.cpp:193 0x42AA70; empty (no toolbar)
void        Select_Brush( selbrush_t *brush, char some_overwrite, char bStatus, char center_grid_on_selection ); // select.cpp:855 0x48DCC0
void        Select_Deselect( int bDeselectFaces );                           // select.cpp:1345 0x48E800
void        Select_Delete();                                                 // select.cpp:1420 0x48E760
void        Select_Move( const float *delta, char bSnap );                   // select.cpp:2021 0x48E9C0
void        Select_ApplyMatrix_SelectedBrushes( int bSnap, float *mat, float deg, char bSwap ); // select.cpp:2095 0x48FD10
void        Select_RotateAxis( int axis, float deg, float (*rot_around)[4][3] ); // select.cpp:2214 0x48FF40
void        SetupVertexSelection();                                          // select.cpp:4278 0x494BC0
int         QE_SingleBrush();                                                // qe3.cpp:378 0x48C8B0

patchMesh_t *MakeNewPatch();                                                 // pmesh.cpp:83 0x437AC0
brush_t    *AddBrushForPatch( patchMesh_t *p, entity_s *world_ent );         // pmesh.cpp:774 0x4386A0
void        Patch_Rebuild( patchMesh_t *p, char doBounds );                  // pmesh.cpp:2072 0x438D80

void        Undo_ClearRedo();                                                // undo.cpp:193 0x45DF20
void        Undo_GeneralStart( const char *operation );                      // undo.cpp:386 0x45E3F0; keeps the pointer
void        Undo_AddBrush( entity_brush_s *pBrushInst );                     // undo.cpp:508 0x45E680; takes the brush DEF; edge picks are not in selected_brushes
void        Undo_AddBrushList( selbrush_t *sb );                             // undo.cpp:558 0x45E7C0; kiwi_dupe's creation bracket omits it
void        Undo_EndBrushList( selbrush_t *brushlist );                      // undo.cpp:587 0x45E870
void        Undo_AddEntity( entity_s *entity );                              // undo.cpp:610 0x45E8B0; entities before their brushes; covers tools outside selected_brushes
void        Undo_AddEntity_W( entity_s *e );                                 // undo.cpp:640 0x45E990; entity + all its brush defs
void        Undo_End();                                                      // undo.cpp:686 0x45EA20

bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1457
void        Radiant_ExecCommand( unsigned int cmdId );                       // mainfrm.cpp:4074; 33220 reaches the static Cmd_OnSelectionAutoCaulk
ImGuiID     ImGuiShell_DockRoot();                                           // imgui_shell.cpp:997
bool        ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );  // imgui_shell.cpp:368; false when off the camera image
selbrush_t *KiwiExtrude_LandDef( brush_t *def );                             // kiwi_extrude.cpp:2333; link, instance, select; also kiwi_extrude.h

void __cdecl R_AddRenderCmdDrawTris( Material *material, MaterialTechniqueType techType, short indexCount,
                                     const uint16_t *indices, short vertexCount, const float (*xyzw)[4],
                                     const float (*normal)[3], float *color, const float (*st)[2] ); // gfx_d3d/r_rendercmds.cpp:2565 0x4FD1C0; KISAK_RADIANT editor delta (P5.4)
