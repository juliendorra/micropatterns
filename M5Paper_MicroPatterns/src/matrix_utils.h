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

// The same table entry as a RAW INTEGER, numerator over MP_Q15_ONE.
//
// This is what makes an exactly-integer transform possible. sin(23deg) is
// 0.390731..., which no binary float holds exactly -- but the table does not
// store that, it stores 12803 with the standing rule "divide by 32768". So the
// rotation is a whole number over a bottom number that never changes, and every
// other input the renderer gets (TRANSLATE, SCALE, RADIUS, a pixel index) is
// already whole. Multiply and add whole numbers over a common denominator and
// the denominator never moves, so nothing is ever rounded away: the division
// happens once, at the end, when a pixel index is finally needed.
int32_t mp_sin_q15(int degrees);

static const int32_t MP_Q15_ONE   = 32768;   // the fixed denominator
static const int     MP_Q15_SHIFT = 15;      // ... which is 1 << 15

// floor / round of a Q15 numerator to a whole pixel index. The shift is an
// arithmetic one, so floor is correct on the negative side too -- a plain
// division would truncate toward zero and put a one-pixel seam at the origin.
inline int32_t mp_q15_floor(int64_t n) { return (int32_t)(n >> MP_Q15_SHIFT); }
inline int32_t mp_q15_round(int64_t n) {
    // round() is half-away-from-zero, and this has to match it exactly: the
    // float path it replaces uses round() on line and rect endpoints.
    return (int32_t)(n >= 0 ? ((n + (MP_Q15_ONE / 2)) >> MP_Q15_SHIFT)
                            : -(((-n) + (MP_Q15_ONE / 2)) >> MP_Q15_SHIFT));
}

// Integer square root of a non-negative int64. Exact: returns floor(sqrt(v)).
// Used for a filled circle's per-scanline half-width, which is sqrt of an
// exactly-representable whole number and therefore has no business calling
// sqrtf. Newton from a shift-based seed; converges in a handful of iterations.
inline int64_t mp_isqrt64(int64_t v) {
    // Restoring binary square root: shifts, adds and compares only.
    //
    // The obvious implementation is Newton's method, and it was measurably
    // WORSE here: Newton needs a division per iteration, 64-bit division on
    // Xtensa is a call into libgcc, and the whole point of this function is to
    // stop calling out to a library. Measured on a Watchy, Newton made
    // op_fill_circle 2.4% slower than the sqrtf it replaced.
    //
    // This version emits one digit of the result per iteration and never
    // divides. Bounded at 31 iterations for a 62-bit input; the argument here is
    // (R * 32768)^2, which for a canvas-sized radius is about 2^50.
    if (v <= 0) return 0;
    uint64_t x = (uint64_t)v, res = 0, bit = (uint64_t)1 << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; }
        else                {                 res =  res >> 1; }
        bit >>= 2;
    }
    return (int64_t)res;
}

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