#ifndef MATRIX_UTILS_H
#define MATRIX_UTILS_H

#include <cmath> // For sinf, cosf, sqrtf
#include <algorithm> // For std::min, std::max

// Represents a 2D affine transformation matrix:
// [m0, m1, m2, m3, m4, m5] corresponds to:
// | m0  m2  m4 |   | xx  xy  tx |
// | m1  m3  m5 | = | yx  yy  ty |
// |  0   0   1 |   |  0   0   1 |
// (x', y') = (m0*x + m2*y + m4, m1*x + m3*y + m5)

// Sets M to an identity matrix
void matrix_identity(float M[6]);

// Multiplies A * B and stores in R. R = A * B.
// R can be the same as A or B.
void matrix_multiply(float R[6], const float A[6], const float B[6]);

// Inverts M and stores in Inv. Returns false if not invertible.
// Inv can be the same as M.
bool matrix_invert(float Inv[6], const float M[6]);

// Applies matrix M to point (x,y) -> (outx, outy)
void matrix_apply_to_point(const float M[6], float x, float y, float& outx, float& outy);

// Creates a translation matrix in M
void matrix_make_translation(float M[6], float dx, float dy);

// Creates a rotation matrix in M (around 0,0) for angle in degrees
void matrix_make_rotation(float M[6], float degrees);

// Constant for converting degrees to radians
const float DEG_TO_RAD_FLOAT = M_PI / 180.0f;

#endif // MATRIX_UTILS_H
