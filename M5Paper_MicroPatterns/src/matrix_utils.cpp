#include "matrix_utils.h"
#include <cstring> // For memcpy

void matrix_identity(float M[6]) {
    M[0] = 1.0f; M[1] = 0.0f;
    M[2] = 0.0f; M[3] = 1.0f;
    M[4] = 0.0f; M[5] = 0.0f;
}

void matrix_multiply(float R[6], const float A[6], const float B[6]) {
    float tempR[6];
    tempR[0] = A[0] * B[0] + A[2] * B[1];
    tempR[1] = A[1] * B[0] + A[3] * B[1];
    tempR[2] = A[0] * B[2] + A[2] * B[3];
    tempR[3] = A[1] * B[2] + A[3] * B[3];
    tempR[4] = A[0] * B[4] + A[2] * B[5] + A[4];
    tempR[5] = A[1] * B[4] + A[3] * B[5] + A[5];
    memcpy(R, tempR, sizeof(float) * 6);
}

bool matrix_invert(float Inv[6], const float M[6]) {
    float det = M[0] * M[3] - M[1] * M[2];
    if (fabs(det) < 1e-9) { // Check if determinant is too close to zero
        // Matrix is singular or near-singular, cannot invert reliably
        // Optionally, set Inv to identity or an error state
        // For now, return false and leave Inv untouched or set to identity
        // matrix_identity(Inv); // Or leave as is
        return false;
    }

    float invDet = 1.0f / det;
    float tempInv[6];

    tempInv[0] = M[3] * invDet;
    tempInv[1] = -M[1] * invDet;
    tempInv[2] = -M[2] * invDet;
    tempInv[3] = M[0] * invDet;
    tempInv[4] = (M[2] * M[5] - M[3] * M[4]) * invDet;
    tempInv[5] = (M[1] * M[4] - M[0] * M[5]) * invDet;
    
    memcpy(Inv, tempInv, sizeof(float) * 6);
    return true;
}

void matrix_apply_to_point(const float M[6], float x, float y, float& outx, float& outy) {
    outx = M[0] * x + M[2] * y + M[4];
    outy = M[1] * x + M[3] * y + M[5];
}

void matrix_make_translation(float M[6], float dx, float dy) {
    matrix_identity(M);
    M[4] = dx;
    M[5] = dy;
}

void matrix_make_rotation(float M[6], float degrees) {
    float angle_rad = degrees * DEG_TO_RAD_FLOAT;
    float s = sinf(angle_rad);
    float c = cosf(angle_rad);
    matrix_identity(M);
    M[0] = c;  M[1] = s;
    M[2] = -s; M[3] = c;
}