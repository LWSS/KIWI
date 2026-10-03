// KIWI (GLASS): BO1 "Nitrous" math used by GlassPhysics, and the ODE adapter for colliding shards

#include "glass_kiwi.h"
#include "glass_kiwi_defaults.h"
#include "glass.h"
#include "glass_client.h"

#include <physics/phys_local.h>
#include <qcommon/qcommon.h>
#include <ode/objects.h>
#include <database64/database.h>
#include <universal/com_memory.h>

const dvar_s *phys_gravity_dir;
const dvar_s *glassMaxColliding;

void phys_multiply_mat(phys_mat44 *dest, const phys_mat44 *left, const phys_mat44 *right)
{
    phys_mat44 v4;
    phys_vec3 v12;

    if (dest == left)
    {
        memcpy(&v4, left, sizeof(v4));
        phys_multiply_mat(dest, &v4, right);
    }
    else
    {
        phys_multiply(&v12, left, &right->x);
        dest->x.x = v12.x;
        dest->x.y = v12.y;
        dest->x.z = v12.z;
        phys_multiply(&v12, left, &right->y);
        dest->y.x = v12.x;
        dest->y.y = v12.y;
        dest->y.z = v12.z;
        phys_multiply(&v12, left, &right->z);
        dest->z.x = v12.x;
        dest->z.y = v12.y;
        dest->z.z = v12.z;
    }
}

void phys_full_multiply_mat(phys_mat44 *dest, const phys_mat44 *left, const phys_mat44 *right)
{
    phys_mat44 v4;
    phys_vec3 v15;

    if (dest == left)
    {
        memcpy(&v4, left, sizeof(v4));
        phys_full_multiply_mat(dest, &v4, right);
    }
    else
    {
        phys_multiply(&v15, left, &right->x);
        dest->x.x = v15.x;
        dest->x.y = v15.y;
        dest->x.z = v15.z;
        phys_multiply(&v15, left, &right->y);
        dest->y.x = v15.x;
        dest->y.y = v15.y;
        dest->y.z = v15.z;
        phys_multiply(&v15, left, &right->z);
        dest->z.x = v15.x;
        dest->z.y = v15.y;
        dest->z.z = v15.z;
        phys_full_multiply(&v15, left, &right->w);
        dest->w.x = v15.x;
        dest->w.y = v15.y;
        dest->w.z = v15.z;
    }
}

void phys_full_inverse(phys_mat44 *dest, const phys_mat44 *source)
{
    phys_mat44 mat;
    phys_vec3 v11;

    if (dest == source)
    {
        phys_full_inverse(&mat, source);
        *dest = mat;
    }
    else
    {
        phys_transpose(dest, source);
        phys_multiply(&v11, dest, &source->w);
        dest->w.x = -v11.x;
        dest->w.y = -v11.y;
        dest->w.z = -v11.z;
    }
}

phys_vec3 *phys_cross(phys_vec3 *result, const phys_vec3 *a, const phys_vec3 *b)
{
    float v4;
    float v5;

    v4 = (float)(a->z * b->x) - (float)(a->x * b->z);
    v5 = (float)(a->x * b->y) - (float)(a->y * b->x);
    result->x = (float)(a->y * b->z) - (float)(a->z * b->y);
    result->y = v4;
    result->z = v5;
    return result;
}

void Phys_NitrousMat44ToVec33(const phys_mat44 *inMat, float (*outAxis)[3])
{
    outAxis[0][0] = inMat->x.x;
    outAxis[0][1] = inMat->x.y;
    outAxis[0][2] = inMat->x.z;
    outAxis[1][0] = inMat->y.x;
    outAxis[1][1] = inMat->y.y;
    outAxis[1][2] = inMat->y.z;
    outAxis[2][0] = inMat->z.x;
    outAxis[2][1] = inMat->z.y;
    outAxis[2][2] = inMat->z.z;
}

void make_rotate(phys_mat44 &m, const phys_vec3 *u, float ca, float sa)
{
    float yy;
    float xsa;
    float xy;
    float zsa;
    float zz;
    float ysa;
    float xz;
    float yz;

    xy = (u->x * u->y) * (1.0f - ca);
    xz = (u->x * u->z) * (1.0f - ca);
    yy = (u->y * u->y) * (1.0f - ca);
    yz = (u->y * u->z) * (1.0f - ca);
    zz = (u->z * u->z) * (1.0f - ca);
    xsa = u->x * sa;
    ysa = u->y * sa;
    zsa = u->z * sa;

    m.x.x = (u->x * u->x) * (1.0f - ca) + ca;
    m.y.x = xy - zsa;
    m.z.x = xz + ysa;
    m.w.x = 0.0f;

    m.x[1] = xy + zsa;
    m.y[1] = yy + ca;
    m.z[1] = yz - xsa;
    m.w[1] = 0.0f;

    m.x[2] = xz - ysa;
    m.y[2] = yz + xsa;
    m.z[2] = zz + ca;
    m.w[2] = 0.0f;

    m.fix_w_column();
}

void make_rotate(phys_mat44 *mat, const phys_vec3 *v, float theta_factor, float max_rotation_radians)
{
    phys_vec3 v8;
    float len;

    len = Vec3Length(&v->x);
    if (len >= 0.0000099999997)
    {
        v8.x = (1.0 / len) * v->x;
        v8.y = (1.0 / len) * v->y;
        v8.z = (1.0 / len) * v->z;

        len = len * theta_factor;
        if (len > max_rotation_radians)
            len = max_rotation_radians;

        make_rotate(*mat, &v8, cos(len), sin(len));
    }
    else
    {
        *mat = PHYS_IDENTITY_MATRIX;
    }
}

void nuge::calc_box_inertia(const phys_vec3 *dim, phys_vec3 *unit_inertia, float *volume)
{
    float v4;
    float v5;
    float v6;
    float moments_4;
    float moments_8;
    float moments_12;
    float v13;

    iassert(((uintptr_t)unit_inertia & 0xF) == 0);
    *volume = dim->x * dim->y * dim->z;
    moments_4 = dim->x * dim->x;
    moments_8 = dim->y * dim->y;
    moments_12 = dim->z * dim->z;
    v13 = dim->x * 0.0833333358168602 * dim->y * dim->z;
    v4 = v13 * moments_4;
    v5 = moments_8 * v13;
    v6 = v13 * moments_12;
    unit_inertia->x = v5 + v6;
    unit_inertia->y = v6 + v4;
    unit_inertia->z = v5 + v4;
}

// Glass shard bodies, so the pane-collision hook (dCollideWorldGeom) can let them
// pass through intact panes as BO1 did (it cleared the shard's pane-collision bit).
static dxBody *s_glassShardBodies[512];
static unsigned int s_glassShardBodyCount;

// Map change: any body still listed belonged to the old ODE worlds.
void GlassKiwi_PhysReset()
{
    s_glassShardBodyCount = 0;
}

bool GlassKiwi_IsGlassShardBody(dxBody *body)
{
    unsigned int i;

    for (i = 0; i < s_glassShardBodyCount; ++i)
    {
        if (s_glassShardBodies[i] == body)
            return true;
    }
    return false;
}

/*
==============
GlassKiwi_PhysTestPanes

dCollideWorldGeom: intact panes block client ODE bodies (not shards). Each pane's submodel
leaf is tested in pane space (panes never rotate) and the contacts moved back.
==============
*/
void GlassKiwi_PhysTestPanes(const objInfo *input, Results *results, dxBody *body)
{
    const Glass *glasses[32];
    objInfo local;
    unsigned int num, i;
    int first, c, k;

    num = GlassCl_AreaGlasses(input->bounds[0], input->bounds[1], glasses, ARRAY_COUNT(glasses));
    if (!num || GlassKiwi_IsGlassShardBody(body))
        return;
    for (i = 0; i < num && results->contactCount < results->maxContacts; ++i)
    {
        local = *input;
        for (k = 0; k < 3; ++k)
        {
            local.pos[k] -= glasses[i]->origin[k];
            local.bodyCenter[k] -= glasses[i]->origin[k];
            local.bounds[0][k] -= glasses[i]->origin[k];
            local.bounds[1][k] -= glasses[i]->origin[k];
        }
        first = results->contactCount;
        CM_TestGeomInLeaf(&CM_ClipHandleToModel(glasses[i]->brushModel)->leaf, &local, results);
        for (c = first; c < results->contactCount; ++c)
        {
            for (k = 0; k < 3; ++k)
                results->contacts[c].contact.pos[k] += glasses[i]->origin[k];
        }
    }
}

// CoD4 picks collision sounds by preset type: use glass_chunk's (physics_glass_*)
static int GlassKiwi_ShardSoundClass()
{
    const PhysPreset *preset;

    if (IsFastFileLoad())
        preset = DB_FindXAssetHeader(ASSET_TYPE_PHYSPRESET, GLASS_KIWI_SHARD_PHYSPRESET).physPreset;
    else
        preset = PhysPresetPrecache(GLASS_KIWI_SHARD_PHYSPRESET, (void *(__cdecl *)(int))Hunk_AllocPhysPresetPrecache);
    return preset ? preset->type : 0;
}

/*
==============
GlassKiwi_PhysObjCreate

BO1 made a Nitrous AABB body; KIWI an ODE box in the FX world (like DynEntPieces_SpawnPhysObj).
The ODE pool is shared; on failure the caller falls back to GlassPhysics.
==============
*/
dxBody *GlassKiwi_PhysObjCreate(float *origin, float *quat, const PhysPreset *physPreset, const float *localMins, const float *localMaxs)
{
    dxBody *body;
    float velocity[3] = { 0.0f, 0.0f, 0.0f };
    PhysPreset preset;

    if (s_glassShardBodyCount >= ARRAY_COUNT(s_glassShardBodies))
        return 0;

    preset = *physPreset;
    preset.type = GlassKiwi_ShardSoundClass();
    body = Phys_ObjCreate(PHYS_WORLD_FX, origin, quat, velocity, &preset);
    if (!body)
        return 0;
    Phys_ObjAddGeomBox(PHYS_WORLD_FX, body, localMins, localMaxs);
    s_glassShardBodies[s_glassShardBodyCount++] = body;
    return body;
}

void Phys_ObjDestroy(int worldIndex, dxBody *id)
{
    unsigned int i;

    iassert(worldIndex == 1); // BO1 world 1 = KIWI PHYS_WORLD_FX for glass shards
    for (i = 0; i < s_glassShardBodyCount; ++i)
    {
        if (s_glassShardBodies[i] == id)
        {
            s_glassShardBodies[i] = s_glassShardBodies[--s_glassShardBodyCount];
            break;
        }
    }
    Phys_ObjDestroy(PHYS_WORLD_FX, id);
}

// BO1 adds the impulse the way GlassPhysics::AddForce does (scaled by 0.001, then
// divided by the step time); KIWI's Phys_ObjAddForce divides by the step itself.
void Phys_ObjAddForce(dxBody *id, float *worldPos, float *force, int unused)
{
    float impulse[3];

    impulse[0] = force[0] * 0.001f;
    impulse[1] = force[1] * 0.001f;
    impulse[2] = force[2] * 0.001f;
    Phys_ObjAddForce(PHYS_WORLD_FX, id, worldPos, impulse);
}

void Phys_ObjGetVelocities(dxBody *id, float *outVelocity, float *outAngularVelocity)
{
    const float *linear;
    const float *angular;

    iassert(id);
    linear = dBodyGetLinearVel(id);
    angular = dBodyGetAngularVel(id);
    if (outVelocity)
    {
        outVelocity[0] = linear[0];
        outVelocity[1] = linear[1];
        outVelocity[2] = linear[2];
    }
    if (outAngularVelocity)
    {
        outAngularVelocity[0] = angular[0];
        outAngularVelocity[1] = angular[1];
        outAngularVelocity[2] = angular[2];
    }
}

void GlassKiwi_PhysInit()
{
    if (!phys_gravity_dir)
        phys_gravity_dir = Dvar_RegisterVec3("phys_gravity_dir", 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, DVAR_CHEAT, "Sets the gravity direction (glass shards).");
    if (!glassMaxColliding)
        glassMaxColliding = Dvar_RegisterInt("glassMaxColliding", 64, 0, 100, DVAR_CHEAT, "Max glass shards with ODE rigid bodies (BO1: 100)");
}
