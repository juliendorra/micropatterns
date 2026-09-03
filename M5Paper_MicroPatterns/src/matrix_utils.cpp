#include "matrix_utils.h"
#include <cstdint>

void matrix_identity(float M[6]) {
    M[0] = 1.0f; M[1] = 0.0f;
    M[2] = 0.0f; M[3] = 1.0f;
    M[4] = 0.0f; M[5] = 0.0f;
}

// A 360-entry Q15 sine table, indexed by whole degrees. This is the only
// trigonometry in the renderer, and it is a lookup, not a call into libm.
//
// It is not an approximation of a continuous sine. ROTATE's operand comes from
// resolve(), which returns an int, so the language can only ever ask for a
// whole number of degrees: 360 entries cover the ENTIRE input domain, and
// there is nothing to interpolate between.
//
// Stored as int32_t, not int16_t, for one reason: Q15 puts 1.0 at 32768, which
// does not fit an int16 and would have to be clamped to 32767. That clamp is
// not a rounding detail -- it makes cos(0) = 0.99997, so the IDENTITY rotation
// is not the identity, and every TRANSLATE issued at angle 0 gets quietly
// scaled. With int32 the four cardinal angles are exact (0, +1, 0, -1) and the
// interior keeps full Q15 precision. The extra 720 bytes are noise against the
// 5,357 saved by dropping libm's sine.
//
// Worst-case deviation from sinf away from the cardinals is 3.05e-5 -- one Q15
// ulp, or 0.03 px across the 960-pixel width of an M5Paper canvas.
// See docs/measurements/2026-09-03-sine-table.md.
//
// This does NOT buy speed, and was not adopted for speed. matrix_make_rotation
// runs once per ROTATE instruction -- at most 109 times in the whole corpus,
// zero times in half of it -- while the float multiplies that actually cost
// time are the inverse transform, evaluated per pixel, which this leaves
// untouched. What it buys is 5,364 bytes of binary with libm's sine gone, and
// a spec (README.md, ROTATE) that stops describing a table the code did not
// have.
static const int32_t SIN_Q15[360] = {
         0,    572,   1144,   1715,   2286,   2856,   3425,   3993,   4560,   5126,
      5690,   6252,   6813,   7371,   7927,   8481,   9032,   9580,  10126,  10668,
     11207,  11743,  12275,  12803,  13328,  13848,  14365,  14876,  15384,  15886,
     16384,  16877,  17364,  17847,  18324,  18795,  19261,  19720,  20174,  20622,
     21063,  21498,  21926,  22348,  22763,  23170,  23571,  23965,  24351,  24730,
     25102,  25466,  25822,  26170,  26510,  26842,  27166,  27482,  27789,  28088,
     28378,  28660,  28932,  29197,  29452,  29698,  29935,  30163,  30382,  30592,
     30792,  30983,  31164,  31336,  31499,  31651,  31795,  31928,  32052,  32166,
     32270,  32365,  32449,  32524,  32588,  32643,  32688,  32723,  32748,  32763,
     32768,  32763,  32748,  32723,  32688,  32643,  32588,  32524,  32449,  32365,
     32270,  32166,  32052,  31928,  31795,  31651,  31499,  31336,  31164,  30983,
     30792,  30592,  30382,  30163,  29935,  29698,  29452,  29197,  28932,  28660,
     28378,  28088,  27789,  27482,  27166,  26842,  26510,  26170,  25822,  25466,
     25102,  24730,  24351,  23965,  23571,  23170,  22763,  22348,  21926,  21498,
     21063,  20622,  20174,  19720,  19261,  18795,  18324,  17847,  17364,  16877,
     16384,  15886,  15384,  14876,  14365,  13848,  13328,  12803,  12275,  11743,
     11207,  10668,  10126,   9580,   9032,   8481,   7927,   7371,   6813,   6252,
      5690,   5126,   4560,   3993,   3425,   2856,   2286,   1715,   1144,    572,
         0,   -572,  -1144,  -1715,  -2286,  -2856,  -3425,  -3993,  -4560,  -5126,
     -5690,  -6252,  -6813,  -7371,  -7927,  -8481,  -9032,  -9580, -10126, -10668,
    -11207, -11743, -12275, -12803, -13328, -13848, -14365, -14876, -15384, -15886,
    -16384, -16877, -17364, -17847, -18324, -18795, -19261, -19720, -20174, -20622,
    -21063, -21498, -21926, -22348, -22763, -23170, -23571, -23965, -24351, -24730,
    -25102, -25466, -25822, -26170, -26510, -26842, -27166, -27482, -27789, -28088,
    -28378, -28660, -28932, -29197, -29452, -29698, -29935, -30163, -30382, -30592,
    -30792, -30983, -31164, -31336, -31499, -31651, -31795, -31928, -32052, -32166,
    -32270, -32365, -32449, -32524, -32588, -32643, -32688, -32723, -32748, -32763,
    -32768, -32763, -32748, -32723, -32688, -32643, -32588, -32524, -32449, -32365,
    -32270, -32166, -32052, -31928, -31795, -31651, -31499, -31336, -31164, -30983,
    -30792, -30592, -30382, -30163, -29935, -29698, -29452, -29197, -28932, -28660,
    -28378, -28088, -27789, -27482, -27166, -26842, -26510, -26170, -25822, -25466,
    -25102, -24730, -24351, -23965, -23571, -23170, -22763, -22348, -21926, -21498,
    -21063, -20622, -20174, -19720, -19261, -18795, -18324, -17847, -17364, -16877,
    -16384, -15886, -15384, -14876, -14365, -13848, -13328, -12803, -12275, -11743,
    -11207, -10668, -10126,  -9580,  -9032,  -8481,  -7927,  -7371,  -6813,  -6252,
     -5690,  -5126,  -4560,  -3993,  -3425,  -2856,  -2286,  -1715,  -1144,   -572,
};


float mp_sin_deg(int deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    return (float)SIN_Q15[deg] * (1.0f / 32768.0f);
}

void matrix_set_rigid(float M[6], float Inv[6], int degrees, float tx, float ty) {
    const float s = mp_sin_deg(degrees);
    const float c = mp_sin_deg(degrees + 90);

    M[0] =  c;  M[1] = s;
    M[2] = -s;  M[3] = c;
    M[4] = tx;  M[5] = ty;

    // The inverse is the transpose OVER THE DETERMINANT, and the division is not
    // optional. A table (c,s) is only orthonormal to one Q15 ulp -- c*c + s*s is
    // 1 +/- 6e-5 -- so a bare transpose would leave the forward and inverse
    // transforms disagreeing by that much, and the rasteriser uses both: the
    // forward one to place a shape, the inverse one to look up which pattern
    // pixel each screen pixel lands on. Dividing by the determinant makes the
    // inverse exact for whatever (c,s) the table actually returned.
    //
    // What a rigid transform does buy is that the determinant is c*c + s*s in
    // closed form: no general 2x2 minor, and it cannot be zero, so there is no
    // singular case to detect and no failure the caller has to handle. This runs
    // once per TRANSLATE / ROTATE, never per pixel.
    const float det = c * c + s * s;
    const float invDet = 1.0f / det;
    Inv[0] =  c * invDet;  Inv[1] = -s * invDet;
    Inv[2] =  s * invDet;  Inv[3] =  c * invDet;
    Inv[4] = (-s * ty - c * tx) * invDet;
    Inv[5] = ( s * tx - c * ty) * invDet;
}
