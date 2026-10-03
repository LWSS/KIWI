#pragma once
// KIWI (GLASS): the slice of BO1's "Nitrous" physics math that GlassPhysics uses
// (BO1 src/physics/phys_local.h, phys_util.cpp, phys_render.cpp, nitrous_vehicle.cpp).
// Pure math, no BO1 physics engine: KIWI's colliding shards use ODE (glass_kiwi_phys.cpp).

#include <universal/assertive.h>
#include <cmath>
#include <cstring>

struct dvar_s;
struct dxBody;
struct PhysPreset;

struct alignas(16) phys_vec3 // sizeof=0x10
{
    float x;
    float y;
    float z;
    float w;

    phys_vec3() { }
    phys_vec3(float _x, float _y, float _z)
    {
        x = _x;
        y = _y;
        z = _z;
        w = 0.0f;
    }
    phys_vec3(float _x, float _y, float _z, float _w)
    {
        x = _x;
        y = _y;
        z = _z;
        w = _w;
    }

    inline phys_vec3 &operator+=(const phys_vec3 &v)
    {
        x += v.x;
        y += v.y;
        z += v.z;
        return *this;
    }

    inline phys_vec3 &operator=(const phys_vec3 &v)
    {
        x = v.x;
        y = v.y;
        z = v.z;
        return *this;
    }

    inline float &operator[](unsigned int i)
    {
        iassert(i < 3);
        return (&x)[i];
    }
};
static_assert(sizeof(phys_vec3) == 16);

struct phys_mat44 // sizeof=0x40
{
    phys_mat44(const phys_vec3 *x_, const phys_vec3 *y_, const phys_vec3 *z_, const phys_vec3 *w_)
    {
        this->x = *x_;
        this->y = *y_;
        this->z = *z_;
        this->w = *w_;
    }

    phys_mat44() { }

    void fix_w_column()
    {
        this->x.w = 0.0f;
        this->y.w = 0.0f;
        this->z.w = 0.0f;
        this->w.w = 1.0f;
    }

    phys_mat44 &operator=(const phys_mat44 &that)
    {
        this->x.x = that.x.x;
        this->x.y = that.x.y;
        this->x.z = that.x.z;
        this->y.x = that.y.x;
        this->y.y = that.y.y;
        this->y.z = that.y.z;
        this->z.x = that.z.x;
        this->z.y = that.z.y;
        this->z.z = that.z.z;
        this->w.x = that.w.x;
        this->w.y = that.w.y;
        this->w.z = that.w.z;
        return *this;
    }

    phys_vec3 x;
    phys_vec3 y;
    phys_vec3 z;
    phys_vec3 w; // position
};
static_assert(sizeof(phys_mat44) == 64);

static const phys_vec3 PHYS_X_VEC = { 1.0f, 0.0f, 0.0f, 0.0f };
static const phys_vec3 PHYS_Y_VEC = { 0.0f, 1.0f, 0.0f, 0.0f };
static const phys_vec3 PHYS_Z_VEC = { 0.0f, 0.0f, 1.0f, 0.0f };
static const phys_vec3 PHYS_ZERO_VEC = { 0.0f, 0.0f, 0.0f, 0.0f };
static const phys_mat44 PHYS_IDENTITY_MATRIX(&PHYS_X_VEC, &PHYS_Y_VEC, &PHYS_Z_VEC, &PHYS_ZERO_VEC);

inline const phys_vec3 *__cdecl phys_multiply(phys_vec3 *result, const phys_mat44 *mat, const phys_vec3 *v)
{
    result->x = ((v->x * mat->x.x) + (v->y * mat->y.x)) + (v->z * mat->z.x);
    result->y = ((v->x * mat->x.y) + (v->y * mat->y.y)) + (v->z * mat->z.y);
    result->z = ((v->x * mat->x.z) + (v->y * mat->y.z)) + (v->z * mat->z.z);
    return result;
}

inline const phys_vec3 *phys_full_multiply(phys_vec3 *result, const phys_mat44 *mat, const phys_vec3 *v)
{
    phys_vec3 v8;

    phys_multiply(&v8, mat, v);
    result->x = v8.x + mat->w.x;
    result->y = v8.y + mat->w.y;
    result->z = v8.z + mat->w.z;
    return result;
}

inline const phys_vec3 *__cdecl phys_inv_multiply(phys_vec3 *result, const phys_mat44 *mat, const phys_vec3 *v)
{
    float v4;
    float v5;

    v5 = (float)((float)(v->x * mat->z.x) + (float)(v->y * mat->z.y)) + (float)(v->z * mat->z.z);
    v4 = (float)((float)(v->x * mat->y.x) + (float)(v->y * mat->y.y)) + (float)(v->z * mat->y.z);
    result->x = (float)((float)(v->x * mat->x.x) + (float)(v->y * mat->x.y)) + (float)(v->z * mat->x.z);
    result->y = v4;
    result->z = v5;
    return result;
}

inline void __cdecl phys_transpose(phys_mat44 *dest, const phys_mat44 *source)
{
    if (dest == source)
    {
        float tmp;

        tmp = dest->x.y;
        dest->x.y = dest->y.x;
        dest->y.x = tmp;

        tmp = dest->x.z;
        dest->x.z = dest->z.x;
        dest->z.x = tmp;

        tmp = dest->y.z;
        dest->y.z = dest->z.y;
        dest->z.y = tmp;
    }
    else
    {
        dest->x.x = source->x.x;
        dest->x.y = source->y.x;
        dest->x.z = source->z.x;

        dest->y.x = source->x.y;
        dest->y.y = source->y.y;
        dest->y.z = source->z.y;

        dest->z.x = source->x.z;
        dest->z.y = source->y.z;
        dest->z.z = source->z.z;
    }
}

inline void Phys_NitrousVecToVec3(const phys_vec3 *inVector, float *outVector)
{
    outVector[0] = inVector->x;
    outVector[1] = inVector->y;
    outVector[2] = inVector->z;
}

inline void Phys_Vec3ToNitrousVec(const float *const inVector, phys_vec3 *outVector)
{
    outVector->x = inVector[0];
    outVector->y = inVector[1];
    outVector->z = inVector[2];
}

inline float Abs(const phys_vec3 &vec)
{
    return sqrtf((vec.x * vec.x) + (vec.y * vec.y) + (vec.z * vec.z));
}

inline float AbsSquared(const phys_vec3 &vec)
{
    return ((vec.x * vec.x) + (vec.y * vec.y) + (vec.z * vec.z));
}

inline float AbsSquared(const phys_vec3 *a) // BO1 nitrous_vehicle.cpp pointer form
{
    return a->x * a->x + a->y * a->y + a->z * a->z;
}

void phys_multiply_mat(phys_mat44 *dest, const phys_mat44 *left, const phys_mat44 *right);
void phys_full_multiply_mat(phys_mat44 *dest, const phys_mat44 *left, const phys_mat44 *right);
void phys_full_inverse(phys_mat44 *dest, const phys_mat44 *source);
phys_vec3 *phys_cross(phys_vec3 *result, const phys_vec3 *a, const phys_vec3 *b);
void Phys_NitrousMat44ToVec33(const phys_mat44 *inMat, float (*outAxis)[3]);
void make_rotate(phys_mat44 &m, const phys_vec3 *u, float ca, float sa);
void make_rotate(phys_mat44 *mat, const phys_vec3 *v, float theta_factor, float max_rotation_radians);

namespace nuge
{
    void calc_box_inertia(const phys_vec3 *dim, phys_vec3 *unit_inertia, float *volume);
}

// BO1 dvar (phys_main.cpp); KIWI has no gravity-direction dvar. BO1 gravity is
// phys_gravity (-800) * phys_gravity_dir (0 0 1), KIWI's ODE gravity is
// -phys_gravity * physGlob.gravityDirection (0 0 -1): the same vector.
extern const dvar_s *phys_gravity_dir;
extern const dvar_s *glassMaxColliding; // BO1 hard cap 100 (GlassRenderer::AddColidingShard)

// ODE adapter for BO1's colliding "kept" shards: BO1 created them in its world 1
// (Nitrous rigid bodies, GJK box geom); KIWI uses PHYS_WORLD_FX with a box geom.
dxBody *GlassKiwi_PhysObjCreate(float *origin, float *quat, const PhysPreset *physPreset, const float *localMins, const float *localMaxs);
void Phys_ObjDestroy(int worldIndex, dxBody *id);
void Phys_ObjAddForce(dxBody *id, float *worldPos, float *force, int unused);
void Phys_ObjGetVelocities(dxBody *id, float *outVelocity, float *outAngularVelocity);
void GlassKiwi_PhysInit();
void GlassKiwi_PhysReset();
bool GlassKiwi_IsGlassShardBody(dxBody *body);
