/*
 * cm_tracebox.c — Ray-AABB slab intersection tests for collision tracing.
 */

#include "cod2rad64.h"

static char s_assertDisable_CM_TraceBoundsTest_nan;

/*
================
CM_CalcTraceExtents

Compute inverse delta for each axis of a trace ray.
trace layout: float[9] = { start[3], end[3], invDelta[3] }
Writes invDelta[i] = 1.0f / (start[i] - end[i]), or 0.0f if delta is zero.
================
*/
void CM_CalcTraceExtents(float *trace)
{
    float delta;

    /* axis 0 */
    delta = trace[0] - trace[3];
    if (delta != 0.0f)
        trace[6] = 1.0f / delta;
    else
        trace[6] = 0.0f;

    /* axis 1 */
    delta = trace[1] - trace[4];
    if (delta != 0.0f)
        trace[7] = 1.0f / delta;
    else
        trace[7] = 0.0f;

    /* axis 2 */
    delta = trace[2] - trace[5];
    if (delta != 0.0f)
        trace[8] = 1.0f / delta;
    else
        trace[8] = 0.0f;
}

/*
================
CM_TraceBoundsTest

Slab-based ray vs AABB rejection test. Tests the trace ray
(start/end/invDelta in traceData[9]) against an AABB defined
by absMins[3] and absMaxs[3].

Returns 1 if the ray does NOT intersect the AABB (rejected).
Returns 0 if the ray intersects (trace should continue into this node).

Two-pass algorithm: first pass tests mins with signFlip=-1, second pass
tests maxs with signFlip=+1. Each pass tests all 3 axes using the slab
method, tracking tMin and tMax of the intersection interval.
================
*/
int CM_TraceBoundsTest(float *traceData, float *absMins, float *absMaxs, float fraction)
{
    float *bounds;
    float *invDelta;
    float tMin, tMax;
    float signFlip;
    float d1, d2, t, diff;
    int i;
    int pass;

    invDelta = traceData + 6;
    tMin = 0.0f;
    tMax = fraction;
    signFlip = -1.0f;
    bounds = absMins;

    for (pass = 0; pass < 2; pass++)
    {
        /* NaN/Inf check on current bounds */
        Assert(!IS_NAN(bounds[0]) && !IS_NAN(bounds[1]) && !IS_NAN(bounds[2]),
               s_assertDisable_CM_TraceBoundsTest_nan);

        for (i = 0; i < 3; i++)
        {
            d1 = (traceData[i] - bounds[i]) * signFlip;
            d2 = (traceData[3 + i] - bounds[i]) * signFlip;

            if (d1 > 0.0f)
            {
                if (d2 > 0.0f)
                    return 1;

                /* ray enters slab: compute t */
                t = d1 * invDelta[i] * signFlip;

                if (t >= tMax)
                    return 1;

                diff = tMin - t;
                if (diff < 0.0f)
                    tMin = t;
            }
            else
            {
                if (d2 > 0.0f)
                {
                    /* ray exits slab: compute t */
                    t = d1 * invDelta[i] * signFlip;

                    if (tMin >= t)
                        return 1;

                    diff = t - tMax;
                    if (diff < 0.0f)
                        tMax = t;
                }
            }
        }

        /* switch to second pass */
        signFlip = 1.0f;
        bounds = absMaxs;
    }

    return 0;
}

