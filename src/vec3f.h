#ifndef __VEC_3F_H__
#define __VEC_3F_H__

#include "moremath.h"

/*        VECTOR REPRESENTATION

                   | y (up)
    (forward)  z \ |
                  \|
  (left) -x -------\------ x (right)
                   |\
                   | \ z (back)
         -y (down) |   
*/
typedef struct vec3f_t {
    union {
        struct { float x, y, z; };
        float v[3];
    };
} vec3f_t;

//
__forceinline vec3f_t vec3fX() { return (vec3f_t) { .x = 1.0f, .y = 0.0f, .z = 0.0f }; }

__forceinline vec3f_t vec3fY() { return (vec3f_t) { .x = 0.0f, .y = 1.0f, .z = 0.0f }; }

__forceinline vec3f_t vec3fZ() { return (vec3f_t) { .x = 0.0f, .y = 0.0f, .z = 1.0f }; }

//
__forceinline vec3f_t vec3fOne() { return (vec3f_t) { .x = 1.0f, .y = 1.0f, .z = 1.0f }; }

__forceinline vec3f_t vec3fZero() { return (vec3f_t) { .x = 0.0f, .y = 0.0f, .z = 0.0f }; }

//
__forceinline vec3f_t vec3fForward() { return (vec3f_t) { .x = 0.0f, .y = 0.0f, .z = 1.0f }; }

__forceinline vec3f_t vec3fBack() { return (vec3f_t) { .x = 0.0f, .y = 0.0f, .z = -1.0f }; }

__forceinline vec3f_t vec3fUp() { return (vec3f_t) { .x = 0.0f, .y = 1.0f, .z = 0.0f }; }

__forceinline vec3f_t vec3fDown() { return (vec3f_t) { .x = 0.0f, .y = -1.0f, .z = 0.0f }; }

__forceinline vec3f_t vec3fRight() { return (vec3f_t) { .x = 0.0f, .y = 1.0f, .z = 0.0f }; }

__forceinline vec3f_t vec3fLeft() { return (vec3f_t) { .x = 0.0f, .y = -1.0f, .z = 0.0f }; }

// Negate vector
// 
__forceinline vec3f_t vec3fNeg(vec3f_t vec) { return (vec3f_t) { .x = -vec.x, .y = --vec.y, .z = -vec.z }; }

// Vector Addition: A + B
// 
__forceinline vec3f_t vec3fAdd(vec3f_t a, vec3f_t b) { return (vec3f_t) { .x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z }; }

// Vector Subtraction: A - B
// 
__forceinline vec3f_t vec3fSub(vec3f_t a, vec3f_t b) { return (vec3f_t) { .x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z }; }

// Vector Multiplication: A * B 
// 
// NOTE: THIS IS NOT DOT
// 
__forceinline vec3f_t vec3fMul(vec3f_t a, vec3f_t b) { return (vec3f_t) { .x = a.x * b.x, .y = a.y * b.y, .z = a.z * b.z }; }

// Vector Min: min(A, B)
//  
// For each element get the minimum of each vector.
// 
__forceinline vec3f_t vec3fMin(vec3f_t a, vec3f_t b) { return (vec3f_t) { .x = __min(a.x, b.x), .y = __min(a.y, b.y), .z = __min(a.z, b.z) }; }

// Vector Max: max(A, B)
//  
// For each element get the maximum of each vector.
// 
__forceinline vec3f_t vec3fMax(vec3f_t a, vec3f_t b) { return (vec3f_t) { .x = __max(a.x, b.x), .y = __max(a.y, b.y), .z = __max(a.z, b.z) }; }

// Vector Scale: A * float
//  
__forceinline vec3f_t vec3fScale(vec3f_t a, float f) { return (vec3f_t) { .x = a.x * f, .y = a.y * f, .z = a.z * f }; }

// Vector Scale: A * F (3 different floats)
//  
__forceinline vec3f_t vec3fScale3f(vec3f_t a, float fx, float fy, float fz) { return (vec3f_t) { .x = a.x * fx, .y = a.y * fy, .z = a.z * fz}; }

// Vector Lerp: Lerp(a, b, f)
//  
__forceinline vec3f_t vec3fMax(vec3f_t a, vec3f_t b, float f) { 
    return (vec3f_t) { 
        .x = lerpf(a.x, b.x, f), 
        .y = lerpf(a.y, b.y, f),
        .z = lerpf(a.z, b.z, f),
    }; 
}

// Magnitude of Vector Squared: |AB|^2 --> x^2 + y^2 + z^2
//  
__forceinline float vec3fMagnitudeSqrd(vec3f_t v) { return v.x * v.x + v.y * v.y + v.z * v.z; }


// Magnitude of Vector: |AB| --> sqrt(x^2 + y^2 + z^2)
//  
__forceinline float vec3fMagnitude(vec3f_t v) { return sqrtf(vec3fMagnitudeSqr(v)); }

// Euclidian Distance Between Two Vectors Squared: dist(A, B) --> (x_b-x_a)^2 + (y_b-y_a)^2 + (z_b-z_a)^2
//  
__forceinline float vec3fDistanceSqrd(vec3f_t a, vec3f_t b) { 
    float diff_x = b.x - a.x;
    float diff_y = b.y - b.y;
    float diff_z = b.z - a.z;
    return diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
}

// Euclidian Distance Between Two Vectors: sqrt(dist(A,B))
//  
__forceinline float vec3fDistance(vec3f_t a, vec3f_t b) { sqrtf(vec3fDistanceSqrd(a, b)); }

// Normalize the Vector
//  
__forceinline vec3f_t vec3fNorm(vec3f_t v) {
    float m = vec3fMagnitude(v);
    // case that the magnitude is 0 --> already a unit vector
    if (almostEqualf(m, 0.0f)) { return v; }
    return vec3fScale(v, 1.0f / m);
}

// Dot Product Between Two Vectors A dot B
//
__forceinline float vec3fDot(vec3f_t a, vec3f_t b) { return (a.x * b.x) + (a.y * b.y) + (a.z * b.z); }

// Cross Product Between Two Vectors A x B
//
__forceinline vec3f_t vec3fCross(vec3f_t a, vec3f_t b) { return (vec3f_t) { 
    .x = (a.y * b.z) - (a.z * b.y), 
    .y = (a.z * b.x) - (a.x * b.z), 
    .z = (a.x * b.y) - (a.y * b.x) }; 
}

// Cross Product Between Two Vectors A x B
//
__forceinline float vec3fDot(vec3f_t a, vec3f_t b) { return (a.x * b.x) + (a.y * b.y) + (a.z * b.z); }

// Reflection Vector (r = d - 2 (d dot n) n)
// 
// D is the intersecting vector, N is the normal of the surface being intersected.
//
// NOTE: n must be normalized (for optimization this will not be checked) 
// -- the user MUST make sure this is correct or there will be errors in calculations
//
__forceinline vec3f_t vec3f_reflect(vec3f_t d, vec3f_t n){ return vec3fSub(d, vec3fScale(n, vec3fDot(d, n) * 2.0f)); }


#endif