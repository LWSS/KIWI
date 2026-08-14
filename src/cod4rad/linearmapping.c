/*
 * linearmapping.c — Linear mapping for lightmap UV ↔ world-space conversion.
 */

#include "cod2rad64.h"

/*
================
LU_Decompose3x3

LU decomposition with partial pivoting for a 3x3 matrix
of doubles. Row stride is 24 bytes (3 doubles).
Returns 1 on success, 0 if matrix is singular.
================
*/
int LU_Decompose3x3(double *matrix, int *permutation)
{
    double scaling[3];
    double maxVal, absVal;
    int i, j, k;
    int pivotRow;
    double pivotMax, temp;

    /* Phase 1: compute row scaling factors (1/max per row) */
    for (i = 0; i < 3; i++)
    {
        maxVal = 0.0;
        for (k = 0; k < 3; k++)
        {
            absVal = fabs(matrix[i * 3 + k]);
            if (absVal > maxVal)
                maxVal = absVal;
        }
        if (maxVal == 0.0)
            return 0; /* singular */
        scaling[i] = 1.0 / maxVal;
    }

    /* Phase 2: Crout's algorithm with partial pivoting */
    for (j = 0; j < 3; j++)
    {
        /* upper triangle: rows 0..j-1 */
        for (i = 0; i < j; i++)
        {
            double sum = matrix[i * 3 + j];
            for (k = 0; k < i; k++)
                sum -= matrix[i * 3 + k] * matrix[k * 3 + j];
            matrix[i * 3 + j] = sum;
        }

        /* lower triangle + pivot search: rows j..2 */
        pivotMax = 0.0;
        pivotRow = j;
        for (i = j; i < 3; i++)
        {
            double sum = matrix[i * 3 + j];
            for (k = 0; k < j; k++)
                sum -= matrix[i * 3 + k] * matrix[k * 3 + j];
            matrix[i * 3 + j] = sum;

            absVal = fabs(sum) * scaling[i];
            if (absVal > pivotMax)
            {
                pivotMax = absVal;
                pivotRow = i;
            }
        }

        /* check for singular */
        if (matrix[pivotRow * 3 + j] == 0.0)
            return 0;

        /* swap rows if needed */
        if (j != pivotRow)
        {
            for (k = 0; k < 3; k++)
            {
                temp = matrix[pivotRow * 3 + k];
                matrix[pivotRow * 3 + k] = matrix[j * 3 + k];
                matrix[j * 3 + k] = temp;
            }
            scaling[pivotRow] = scaling[j];
        }

        /* store permutation */
        permutation[j] = pivotRow;

        /* divide column below diagonal by pivot */
        if (j < 2)
        {
            temp = 1.0 / matrix[j * 3 + j];
            for (i = j + 1; i < 3; i++)
                matrix[i * 3 + j] *= temp;
        }
    }

    return 1;
}

static char s_assertDisable_SolveLinearMapping_nan;

/*
================
LU_Solve3x3

Solves Ax = b using the LU decomposition from LU_Decompose3x3.
Forward substitution (Ly = Pb), then backward substitution (Ux = y).
Solution is written back to the b vector.
================
*/
void LU_Solve3x3(double *luMatrix, int *permutation, double *b)
{
    int i, k;
    int firstNonZero;
    double sum;
    double temp;
    int ip;

    /* forward substitution: Ly = Pb */
    firstNonZero = -1;
    for (i = 0; i < 3; i++)
    {
        /* apply permutation: swap b[i] and b[perm[i]] */
        ip = permutation[i];
        temp = b[ip];
        b[ip] = b[i];
        sum = temp;

        if (firstNonZero >= 0)
        {
            for (k = firstNonZero; k < i; k++)
                sum -= luMatrix[i * 3 + k] * b[k];
        }
        else if (sum != 0.0)
        {
            firstNonZero = i;
        }

        b[i] = sum;
    }

    /* backward substitution: Ux = y */
    for (i = 2; i >= 0; i--)
    {
        sum = b[i];
        for (k = i + 1; k < 3; k++)
            sum -= luMatrix[i * 3 + k] * b[k];
        b[i] = sum / luMatrix[i * 3 + i];
    }
}

/*
================
SolveLinearMapping

Solves a 3x3 linear system with iterative refinement.
Takes 3 float inputs, converts to double, solves, refines,
converts back to float, stores to output at given indices.
================
*/
void SolveLinearMapping(double *origMatrix, double *luMatrix, int *permutation,
                        float input0, float input1, float input2,
                        float *output, int outIdx0, int outIdx1, int outIdx2)
{
    double rhs[3], rhs2[3];
    double solution[3], residual[3];
    int i;

    /* convert float inputs to double */
    rhs[0] = (double)input0;
    rhs[1] = (double)input1;
    rhs[2] = (double)input2;

    /* save original RHS for residual computation */
    rhs2[0] = rhs[0];
    rhs2[1] = rhs[1];
    rhs2[2] = rhs[2];

    /* first solve: Ax = b */
    LU_Solve3x3(luMatrix, permutation, rhs);

    /* save solution */
    solution[0] = rhs[0];
    solution[1] = rhs[1];
    solution[2] = rhs[2];

    /* compute residual: r = A*x - b (orig subtracts rhs FIRST, then adds remaining products) */
    for (i = 0; i < 3; i++)
    {
        residual[i] = solution[0] * origMatrix[i * 3 + 0]
                    - rhs2[i]
                    + solution[1] * origMatrix[i * 3 + 1]
                    + solution[2] * origMatrix[i * 3 + 2];
    }

    /* solve for correction: A*dx = r */
    LU_Solve3x3(luMatrix, permutation, residual);

    /* apply correction: x_refined = x - dx */
    solution[0] -= residual[0];
    solution[1] -= residual[1];
    solution[2] -= residual[2];

    /* convert to float and store at specified indices */
    output[outIdx0] = (float)solution[0];
    output[outIdx1] = (float)solution[1];
    output[outIdx2] = 0.0f;
    output[3] = (float)solution[2];

    /* NaN check */
    Assert(!IS_NAN_FLOAT(output[0]) && !IS_NAN_FLOAT(output[1])
        && !IS_NAN_FLOAT(output[2]) && !IS_NAN_FLOAT(output[3]),
           s_assertDisable_SolveLinearMapping_nan);
}

/*
================
SetupLinearMapping

Sets up the linear mapping data structure from 3 UV coordinates
and a basis vector. Builds a 3x3 matrix, copies it, performs
LU decomposition on the copy. Returns 1 on success, 0 if singular.

Output layout (168 bytes):
  +0x00: original 3x3 matrix (9 doubles = 72 bytes)
  +0x48: LU decomposed copy (9 doubles = 72 bytes)
  +0x90: permutation (3 ints = 12 bytes)
  +0x9C: axis0 (int)
  +0xA0: axis1 (int)
  +0xA4: dominantAxis (int)
================
*/
/* Vec3MajorAxis — in cod2rad64.h */

int SetupLinearMapping(float *basis, float *uv0, float *uv1, float *uv2,
                       LinearMappingData_t *output)
{
    int dominantAxis, axis0, axis1;
    int i;

    /* find the dominant axis of the basis (normal) vector */
    dominantAxis = Vec3MajorAxis(basis);

    /* store axis indices */
    output->dominantAxis = dominantAxis;
    axis1 = (~dominantAxis) & 2;
    output->axis1 = axis1;
    axis0 = (~dominantAxis) & 1;
    output->axis0 = axis0;

    /* build 3x3 matrix from UV coordinates:
     * [ uv0[axis0], uv0[axis1], 1.0 ]
     * [ uv1[axis0], uv1[axis1], 1.0 ]
     * [ uv2[axis0], uv2[axis1], 1.0 ]
     */
    output->origMatrix[0] = (double)uv0[axis0];
    output->origMatrix[1] = (double)uv0[axis1];
    output->origMatrix[2] = 1.0;
    output->origMatrix[3] = (double)uv1[axis0];
    output->origMatrix[4] = (double)uv1[axis1];
    output->origMatrix[5] = 1.0;
    output->origMatrix[6] = (double)uv2[axis0];
    output->origMatrix[7] = (double)uv2[axis1];
    output->origMatrix[8] = 1.0;

    /* copy matrix for LU decomposition (preserving original for refinement) */
    for (i = 0; i < 9; i++)
        output->luMatrix[i] = output->origMatrix[i];

    /* LU decompose the copy */
    return LU_Decompose3x3(output->luMatrix, output->permutation) ? 1 : 0;
}

/*
================
OrientationDirToWorldDir

Converts orientation-space direction to world-space using
the linear mapping. Wrapper that calls SolveLinearMapping
with the stored LU data and axis indices.
================
*/
static char s_assertDisable_OrientationDir_solver;

void OrientationDirToWorldDir(LinearMappingData_t *mapping, float dirX, float dirY,
                              float dirZ, float *output)
{
    Assert(mapping, s_assertDisable_OrientationDir_solver);

    SolveLinearMapping(mapping->origMatrix,
                       mapping->luMatrix,
                       mapping->permutation,
                       dirX, dirY, dirZ,
                       output, mapping->axis0, mapping->axis1, mapping->dominantAxis);
}
