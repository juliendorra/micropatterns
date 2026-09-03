#ifndef MATRIX_UTILS_H
#define MATRIX_UTILS_H

#include <cmath> // For fabs, lroundf
#include <algorithm> // For std::min, std::max

// Represents a 2D affine transformation matrix:
// [m0, m1, m2, m3, m4, m5] corresponds to:
// | m0  m2  m4 |   | xx  xy  tx |
// | m1  m3  m5 | = | yx  yy  ty |
// |  0   0   1 |   |  0   0   1 |
// (x', y') = (m0*x + m2*y + m4, m1*x + m3*y + m5)

// Sets M to an identity matrix
void matrix_identity(float M[6]);

// Applies matrix M to point (x,y) -> (outx, outy)
// Defined inline in the header on purpose: this is called once (often twice) per
// rasterized pixel. With the firmware built without LTO, an out-of-line definition
// in matrix_utils.cpp meant a real windowed call per pixel on the ESP32.
// The expression is byte-for-byte the one that used to live in matrix_utils.cpp,
// so results are unchanged.
inline void matrix_apply_to_point(const float M[6], float x, float y, float& outx, float& outy) {
    outx = M[0] * x + M[2] * y + M[4];
    outy = M[1] * x + M[3] * y + M[5];
}

// Sine of a whole number of degrees, from a 360-entry Q15 table rather than
// sinf. Any integer is accepted; it is reduced modulo 360. See matrix_utils.cpp
// for why 360 entries are the whole input domain rather than a sampling of it.
float mp_sin_deg(int degrees);

// Builds BOTH the rigid transform for (degrees, tx, ty) and its inverse.
//
// There is no general matrix_multiply / matrix_invert pair here any more, and
// that is the point. MicroPatterns' transform is only ever translations and
// rotations, so the caller keeps (degrees, tx, ty) -- the exact state of a rigid
// transform -- and rebuilds from it rather than composing matrices, which is
// what stops the Q15 rotation table drifting under cumulative ROTATE. The
// inverse comes from the transpose and a closed-form determinant, so there is
// no 2x2 minor to compute and no singular case that can fail.
void matrix_set_rigid(float M[6], float Inv[6], int degrees, float tx, float ty);

#endif // MATRIX_UTILS_H