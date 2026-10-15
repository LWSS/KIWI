#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI: Physics Drop's ODE side (kiwi_physdrop_sim.cpp).  That TU includes the engine's physics
// headers and no editor ones; kiwi_physdrop.cpp is the editor side and never sees ODE types.
// Bodies are ODE bodies in the in-tree CoD4 ODE (world PHYS_WORLD_DYNENT, which the editor has
// no other use for); contacts are generated here, not by ODE geoms: the game's box-vs-triangle
// collider against the gathered editor triangles, and ODE's box-box test between the bodies.

#include <vector>

struct XModel;

// One collision box.  For a body description: model space, already scaled by modelscale.
struct kpdPart_t
{
    float center[3];
    float axis[3][3];        // rows: the box axes
    float half[3];           // half extents
};

struct kpdBodyDesc_t
{
    float origin[3];         // the model origin
    float axis[3][3];        // AnglesToAxis rows (forward / left / up) in world space
    float linVel[3];         // world, units / s
    float angVel[3];         // world, radians / s
    float mass;
    float friction;
    float bounce;
    std::vector<kpdPart_t> parts;
};

// The XModel's physics collision (physGeoms: boxes as they are, brushes and cylinders as their
// boxes) scaled about the model origin; false = it carries none (use the bounds box).  Mass,
// friction and bounce are overwritten only when the model names a physPreset.
bool KiwiPhysSim_ModelShape( const XModel *model, float scale, std::vector<kpdPart_t> &parts,
                             float *mass, float *friction, float *bounce );

// The game's dyn_model rule (DynEnt_LoadEntityDef): the xmodel needs a collision LOD.
bool KiwiPhysSim_ModelCanBeDynamic( const XModel *model, bool *hasPreset );

// Contents the game's physics collides with (qcommon.h PHYS_WORLD_CLIPMASK).
int  KiwiPhysSim_ClipMask();

// A fresh world (destroys any bodies); the static triangles are kept.
void KiwiPhysSim_Begin();
// -1 = refused (no parts, or the body pool is full).
int  KiwiPhysSim_AddBody( const kpdBodyDesc_t &desc );
// Destroys every body.
void KiwiPhysSim_End();

// Bodies whose centre falls below z are "lost" (fell out of the map) and stop.
void KiwiPhysSim_SetFloor( float z );

// Static world: front = (b - a) x (c - a), solid behind it; two-sided triangles face each box.
void KiwiPhysSim_ClearStatic();
void KiwiPhysSim_AddStaticTri( const float *a, const float *b, const float *c, bool twoSided );
void KiwiPhysSim_BuildStatic();
int  KiwiPhysSim_StaticTriCount();

// Runs fixed steps for dt seconds; false once every body rests (or the time cap stopped it).
bool  KiwiPhysSim_Advance( float dt );
float KiwiPhysSim_Time();
int   KiwiPhysSim_RestingCount();
bool  KiwiPhysSim_BodyLost( int body );
bool  KiwiPhysSim_BodyPose( int body, float origin[3], float axis[3][3] );   // the model pose
bool  KiwiPhysSim_BodyBounds( int body, float mins[3], float maxs[3] );
int   KiwiPhysSim_PartCount( int body );
// Corner k of a box: bit j set = +half along axis j.  Returns the number of boxes written.
int   KiwiPhysSim_PartBoxes( int body, float ( *corners )[8][3], int maxBoxes );
