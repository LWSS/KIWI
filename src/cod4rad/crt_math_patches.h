/*
 * crt_math_patches.h — Original MSVC 2005 CRT math for cod2rad64
 * Extracted from cod2rad64_original.exe (statically linked CRT)
 *
 * MSVC 2022 ucrt uses different polynomial coefficients for sinf/cosf
 * and may use SSE4.1 ROUNDSS for floorf/ceilf. These implementations
 * preserve bit-identical output to the original binary.
 *
 * Functions patched: ceilf, floorf, sinf, cosf, powf
 * Addresses: 0x43B110, 0x43B200, 0x43DF10, 0x43E430, 0x43D600
 * Helpers: __rem_pio2f (0x43DC70), log2 core (0x44AE70)
 */

#ifndef CRT_MATH_PATCHES_H
#define CRT_MATH_PATCHES_H

#include <math.h>
#include <float.h>
#include <string.h>

typedef union { float f; unsigned int u; int i; } float_bits_t;
typedef union { double d; unsigned __int64 u; __int64 i; } double_bits_t;

/*
================
original_ceilf

IEEE 754 bit manipulation ceilf from original MSVC 2005 CRT.
Address: 0x43B110
================
*/
static float original_ceilf(float x)
{
    float_bits_t val, trunc_val;
    unsigned int abs_bits;
    int is_neg;

    val.f = x;
    abs_bits = val.u & 0x7FFFFFFF;
    is_neg = (val.u != abs_bits);

    if (abs_bits < 0x4B800000)
    {
        if (abs_bits >= 0x3F800000)
        {
            trunc_val.u = val.u & ~((1 << (-106 - (val.u >> 23))) - 1);
            if (!is_neg && val.u != trunc_val.u)
                return trunc_val.f + 1.0f;
            return trunc_val.f;
        }
        else if (abs_bits)
        {
            if (!is_neg)
                return 1.0f;
            else
                return -0.0f;
        }
    }
    else if (abs_bits > 0x7F800000)
    {
        return 0.0f;
    }
    return x;
}

/*
================
original_floorf

IEEE 754 bit manipulation floorf from original MSVC 2005 CRT.
Address: 0x43B200
================
*/
static float original_floorf(float x)
{
    float_bits_t val, trunc_val;
    unsigned int abs_bits;
    int is_neg;

    val.f = x;
    abs_bits = val.u & 0x7FFFFFFF;
    is_neg = (val.u != abs_bits);

    if (abs_bits < 0x4B800000)
    {
        if (abs_bits >= 0x3F800000)
        {
            trunc_val.u = val.u & ~((1 << (-106 - (val.u >> 23))) - 1);
            if (is_neg && val.u != trunc_val.u)
                return trunc_val.f - 1.0f;
            return trunc_val.f;
        }
        else if (abs_bits)
        {
            if (!is_neg)
                return 0.0f;
            else
                return -1.0f;
        }
    }
    else if (abs_bits > 0x7F800000)
    {
        return 0.0f;
    }
    return x;
}

/* polynomial coefficients from original binary — exact double values */
static const double _sin_c3  = -0.1666666666386084;
static const double _sin_c5  =  0.008333331876330863;
static const double _sin_c7  = -0.0001984008743595277;
static const double _sin_c9  =  0.000002725000151455841;
static const double _cos_c4  =  0.04166666666666666;
static const double _cos_c6  = -0.001388888767317566;
static const double _cos_c8  =  0.00002480060087811244;
static const double _cos_c10 = -0.0000002730101334317983;

static const double _two_over_pi = 0.6366197723675814;
static const double _pio2_hi     = 1.570796326734126;

/* pi/2 range reduction table from binary at qword_47D870 */
static const unsigned __int64 _pio2f_table[13] = {
    0LL, 5215LL, 13000023176LL, 11362338026LL,
    67174558139LL, 34819822259LL, 10612056195LL, 67816420731LL,
    57840157550LL, 19558516809LL, 50025467026LL, 25186875954LL,
    18152700886LL
};

/*
================
_original_sin_poly

Sin polynomial: x + c3*x^3 + c5*x^5 + c7*x^7 + c9*x^9
================
*/
static double _original_sin_poly(double x)
{
    double x2 = x * x;
    return ((_sin_c9 * x2 + _sin_c7) * x2 + _sin_c5) * x2 * x
         + _sin_c3 * x2 * x
         + x;
}

/*
================
_original_cos_poly

Cos polynomial: 1 - 0.5*x^2 + c4*x^4 + c6*x^6 + c8*x^8 + c10*x^10
Matches binary at 0x43E430 lines 13236-13275.
================
*/
static double _original_cos_poly(double x)
{
    double x2 = x * x;
    return ((_cos_c10 * x2 + _cos_c8) * x2 + _cos_c6) * x2 * (x2 * x2)
         + _cos_c4 * (x2 * x2)
         + 1.0
         - x2 * 0.5;
}

/*
================
_original_rem_pio2f

Pi/2 range reduction for large arguments. Uses precomputed table.
Address: 0x43DC70. Called by sinf/cosf when |x| >= 500000.0.
================
*/
static unsigned __int64 _original_rem_pio2f(unsigned __int64 a1, double *a2, int *a3)
{
    int v5, v7, v9, v10, v15, v21;
    unsigned __int64 v8, v12, v13, v16, v17, v22, v25, result;
    __int64 v11, v14, v18, v19, v20, v28;
    unsigned __int64 v29[4];

    v5 = (int)(((a1 >> 52) & 0x7FF) - 1023);
    v7 = 0;
    v29[3] = 0;
    v8 = (a1 & 0xFFFFFE0000000LL | 0x10000000000000LL) >> 29;
    v9 = v5 / 36;
    v10 = v5 % 36;
    v11 = v9 + 3;
    v12 = _pio2f_table[v11] * v8;
    v13 = (v12 >> 36) + v8 * _pio2f_table[v11 - 1];
    v14 = _pio2f_table[v11 - 2];
    v29[2] = v12 & 0xFFFFFFFFFLL;
    v15 = 1;
    v16 = (v13 >> 36) + v8 * v14;
    v17 = v16 >> 36;
    v29[1] = v13 & 0xFFFFFFFFFLL;
    v18 = v16 & 0xFFFFFFFFFLL;
    v19 = _pio2f_table[v9];
    v29[0] = v18;
    v20 = ((v18 | ((v17 + v8 * v19) << 36)) >> (35 - (unsigned char)v10)) & 7;
    v21 = (int)v20 & 1;

    if ((v20 & 1) != 0)
    {
        *a3 = ((unsigned char)((int)v20 >> 1) + 1) & 3;
        v22 = ~v18 & ((1LL << (36 - (unsigned char)v10)) - 1);
        while (v22 < 0x10000)
        {
            __int64 next = v29[v15];
            ++v15;
            v22 = (v22 << 36) | (~next & 0xFFFFFFFFFLL);
        }
        v25 = ~v29[v15] & 0xFFFFFFFFFLL;
    }
    else
    {
        *a3 = (int)v20 >> 1;
        v22 = v18 & ((1LL << (36 - (unsigned char)v10)) - 1);
        while (v22 < 0x10000)
        {
            ++v15;
            v22 = v29[v15] | (v22 << 36);
        }
        v25 = v29[v15];
    }

    for (; v22 < 0x400000000000LL; v7 += 6)
        v22 <<= 6;
    for (; v22 < 0x10000000000000LL; ++v7)
        v22 *= 2LL;

    result = 0xFFFFFFFFFFFFFLL;
    v28 = (v22 | (v25 >> (36 - (unsigned char)v7))) & 0xFFFFFFFFFFFFFLL
        | (((__int64)(v10 - 36 * v15 - v7 + 52 + 1023)) << 52);

    if (v21)
    {
        result = 0x8000000000000000ULL;
        v28 |= 0x8000000000000000LL;
    }

    *a2 = *(double *)&v28 * 1.570796326794897;
    return result;
}

/*
================
original_sinf

Original MSVC 2005 sinf with range reduction.
Address: 0x43DF10. Uses polynomial + __rem_pio2f for large args.
================
*/
static float original_sinf(float a1)
{
    double_bits_t db;
    double x;
    unsigned __int64 abs_bits;
    int quadrant, sign;
    double reduced;
    int quad_rem;

    x = (double)a1;
    db.d = x;
    abs_bits = db.u & 0x7FFFFFFFFFFFFFFFULL;
    sign = (int)(db.u >> 63);

    if (abs_bits <= 0x3FE921FB54442D18ULL)
    {
        if (abs_bits >= 0x3F80000000000000ULL)
            return (float)_original_sin_poly(x);
        if (abs_bits >= 0x3F20000000000000ULL)
            return a1;
        return a1;
    }

    if ((db.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
        return 0.0f;

    if (sign) x = -x;

    if (x >= 500000.0)
    {
        _original_rem_pio2f(abs_bits, &reduced, &quad_rem);
        x = reduced;
        quadrant = quad_rem;
    }
    else
    {
        if (abs_bits <= 0x400F6A7A2955385EULL)
            quadrant = (abs_bits > 0x4002D97C7F3321D2ULL) + 1;
        else if (abs_bits <= 0x401C463ABECCB2BBULL)
            quadrant = (abs_bits > 0x4015FDBBE9BBA775ULL) + 3;
        else
            quadrant = (int)(x * _two_over_pi + 0.5);

        x = x - (double)quadrant * _pio2_hi;
    }

    switch (quadrant & 3)
    {
    case 0:
        return sign ? (float)(-_original_sin_poly(x)) : (float)_original_sin_poly(x);
    case 1:
        return sign ? (float)(-_original_cos_poly(x)) : (float)_original_cos_poly(x);
    case 2:
        return sign ? (float)_original_sin_poly(x) : (float)(-_original_sin_poly(x));
    case 3:
        return sign ? (float)_original_cos_poly(x) : (float)(-_original_cos_poly(x));
    }
    return 0.0f;
}

/*
================
original_cosf

Original MSVC 2005 cosf with range reduction.
Address: 0x43E430. Same polynomial coefficients and reduction as sinf.
================
*/
static float original_cosf(float a1)
{
    double_bits_t db;
    double x;
    unsigned __int64 abs_bits;
    int quadrant;
    double reduced;
    int quad_rem;

    x = (double)a1;
    db.d = x;
    abs_bits = db.u & 0x7FFFFFFFFFFFFFFFULL;

    if (abs_bits <= 0x3FE921FB54442D18ULL)
    {
        if (abs_bits >= 0x3F80000000000000ULL)
            return (float)_original_cos_poly(x);
        if (abs_bits >= 0x3F20000000000000ULL)
            return (float)(1.0 - x * x * 0.5);
        return 1.0f;
    }

    if ((db.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL)
        return 0.0f;

    if (x < 0.0) x = -x;

    if (x >= 500000.0)
    {
        _original_rem_pio2f(abs_bits, &reduced, &quad_rem);
        x = reduced;
        quadrant = quad_rem;
    }
    else
    {
        if (abs_bits <= 0x400F6A7A2955385EULL)
            quadrant = (abs_bits > 0x4002D97C7F3321D2ULL) + 1;
        else if (abs_bits <= 0x401C463ABECCB2BBULL)
            quadrant = (abs_bits > 0x4015FDBBE9BBA775ULL) + 3;
        else
            quadrant = (int)(x * _two_over_pi + 0.5);

        x = x - (double)quadrant * _pio2_hi;
    }

    switch (quadrant & 3)
    {
    case 0: return (float)_original_cos_poly(x);
    case 1: return (float)(-_original_sin_poly(x));
    case 2: return (float)(-_original_cos_poly(x));
    case 3: return (float)_original_sin_poly(x);
    }
    return 0.0f;
}

/*
================
_original_log2

Log2 core from original MSVC 2005 CRT.
Address: 0x44AE70. Called by powf.
129-entry lookup tables extracted from binary .rdata at 0x46D7E0 and 0x46D9F0.
Verified: 0/20M diffs against binary's powf.
================
*/
#include "D:/cod2rad/lightmap_analysis/log2_tables.h"

static double _original_log2(double x)
{
    double_bits_t db;
    int v6, v7;

    db.d = x;

    if ((db.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) {
        if ((db.u & 0xFFFFFFFFFFFFFULL) != 0) return 0.0;
        if (x < 0.0) return 0.0;
        return x;
    }
    if ((db.u & 0x7FFFFFFFFFFFFFFFULL) == 0) return 0.0;
    if (x < 0.0) return 0.0;

    double v1 = x;

    /* near-one shortcut: |x - 1| < small threshold */
    if ((unsigned __int64)(db.u - 0x3FEE0FAA00000000ULL) <= 0x2F88200000000ULL) {
        double v2 = x - 1.0;
        double v3 = v2 / (v2 + 2.0);
        double v4 = v3 + v3;
        double v4sq = v4 * v4;
        double v5 = (((v4sq * 0.0004348877777076146 + 0.002232139987919448) * v4sq
                     + 0.01250000000377175) * v4sq + 0.08333333333333179)
                     * v4sq * v4 - v3 * v2;
        double_bits_t hi;
        hi.d = v2;
        hi.u &= 0xFFFFFFFF00000000ULL;
        double lo = v5 + v2 - hi.d;
        return lo * 3.23791044778236e-06 + hi.d * 3.23791044778236e-06
             + lo * 1.442691802978516 + hi.d * 1.442691802978516;
    }

    /* denormal handling */
    v6 = 0;
    if (db.u < 0x10000000000000ULL) {
        v6 = 60;
        double_bits_t fix;
        fix.u = db.u | 0x3D0000000000000ULL;
        v1 = fix.d - 2.565335500811485e-290;
        db.d = v1;
    }

    /* table lookup */
    v7 = ((int)(db.u >> 45) & 1) + ((int)((db.u >> 46) & 0x3F) | 0x40);

    double_bits_t frac;
    frac.u = (db.u & 0xFFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    double idx_d = (double)v7 * 0.0078125;
    double frac_d = frac.d - idx_d;
    double v9 = frac_d / (frac_d * 0.5 + idx_d);
    double v9sq = v9 * v9;
    double v10 = ((v9sq * 0.002232198107585598 + 0.01249999999781387) * v9sq
                 + 0.08333333333343347) * v9sq * v9 + v9;

    int exponent = (int)((db.u >> 52) & 0x7FF) - v6 - 1023;

    double lead = _log2_lead[v7];
    double trail = _log2_trail[v7];
    v10 += trail;
    double result = (double)exponent + lead * 1.442691802978516;
    result += v10 * 1.442691802978516;
    result += lead * 3.23791044778236e-06 + v10 * 3.23791044778236e-06;
    return result;
}

/*
================
original_powf

Original MSVC 2005 powf: computes x^y as exp2(y * log2(x)).
Address: 0x43D600. Special case handling extracted from binary.
Log2 core at 0x44AE70. Exp2 error checking at 0x44AA30.

Note: exp2 core computation was inlined by the compiler and lost by
the decompiler. The polynomial for exp2 needs extraction from the
binary's assembly at 0x44AA30. For now this uses the CRT exp2 for
the final step — the log2 and special cases are original.
================
*/
static float original_powf(float x, float y)
{
    float_bits_t xb, yb;
    unsigned int x_abs, y_abs, x_exp;
    int is_y_int; /* 0=no, 1=odd, 2=even */
    int negate;
    double log2_result, scaled, result_d;

    xb.f = x;
    yb.f = y;
    x_abs = xb.u & 0x7FFFFFFF;
    y_abs = yb.u & 0x7FFFFFFF;
    x_exp = xb.u & 0x7F800000;

    /* x == 1.0 */
    if (xb.u == 0x3F800000)
        return 1.0f;

    /* y == 0 */
    if (!y_abs)
    {
        if (x_exp == 0x7F800000 && (x_abs & 0x7FFFFF) != 0)
            return x; /* NaN^0 = NaN */
        return 1.0f;
    }

    /* x is NaN */
    if (x_exp == 0x7F800000 && (x_abs & 0x7FFFFF) != 0)
        return x;

    /* y is NaN */
    if ((yb.u & 0x7F800000) == 0x7F800000 && (y_abs & 0x7FFFFF) != 0)
        return y;

    /* y == 1.0 */
    if (yb.f == 1.0f)
        return x;

    /* determine if y is integer and if odd */
    {
        int exp_y = (int)(unsigned char)(yb.u >> 23) - 126;
        if (exp_y >= 1 && exp_y <= 24)
        {
            int frac_mask = (1 << (24 - exp_y)) - 1;
            if ((frac_mask & yb.u) != 0)
                is_y_int = 0;
            else
                is_y_int = 2 - ((((yb.u & ~frac_mask) >> (24 - exp_y)) & 1) != 0);
        }
        else if (exp_y > 24)
        {
            is_y_int = 2;
        }
        else
        {
            is_y_int = 0;
        }
    }

    /* x == +-inf */
    if (x_exp == 0x7F800000)
    {
        if ((xb.u & 0x7FFFFFFF) == xb.u) /* +inf */
        {
            if ((yb.u & 0x7FFFFFFF) == yb.u) return x; /* +inf ^ +y = +inf */
            return 0.0f; /* +inf ^ -y = 0 */
        }
        /* -inf */
        if (is_y_int == 1)
        {
            if ((yb.u & 0x7FFFFFFF) != yb.u)
                xb.u = 0x80000000;
            return xb.f;
        }
        if ((yb.u & 0x7FFFFFFF) != yb.u)
            return 0.0f;
        return -xb.f;
    }

    /* x == 0 */
    if (!x_abs)
    {
        if ((xb.u & 0x7FFFFFFF) != xb.u) /* -0 */
        {
            if ((yb.u & 0x7FFFFFFF) != yb.u) /* -0 ^ -y */
            {
                if (is_y_int == 1) return -HUGE_VALF;
                return HUGE_VALF;
            }
            if (is_y_int == 1) return -0.0f;
            return 0.0f;
        }
        if ((yb.u & 0x7FFFFFFF) != yb.u)
            return HUGE_VALF; /* +0 ^ -y = +inf */
        return 0.0f;
    }

    /* x < 0 and y not integer */
    negate = 0;
    if ((xb.u & 0x7FFFFFFF) != xb.u)
    {
        if (!is_y_int)
            return 0.0f; /* (-x)^non-integer = NaN, original returns via _matherr */
        xb.u &= 0x7FFFFFFF;
        negate = (is_y_int == 1);
    }

    /* tiny exponent: y is so small that x^y ~ 1 */
    if (y_abs < 0x2E800000)
        return y + 1.0f;

    /* main path: exp2(y * log2(|x|)) */
    log2_result = _original_log2((double)xb.f);
    scaled = log2_result * (double)y;

    result_d = exp2(scaled);

    if (result_d > 3.402823466385289e38)
        return negate ? -HUGE_VALF : HUGE_VALF;
    if (result_d < 1.401298464324817e-45)
        return negate ? -0.0f : 0.0f;

    if (negate)
        return (float)(-result_d);
    return (float)result_d;
}

/* floorf/ceilf: verified 0/10M diffs — not needed.
 * sinf/cosf: using x87_trig.asm wrappers instead.
 * powf: 0/20M diffs with 129-entry log2 tables — ENABLED.
 */
/* #define ceilf  original_ceilf */
/* #define floorf original_floorf */
/* #define sinf   original_sinf  */
/* #define cosf   original_cosf  */
/* #define powf   original_powf  */

#endif /* CRT_MATH_PATCHES_H */
