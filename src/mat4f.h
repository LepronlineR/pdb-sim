#ifndef __MAT4F_H__
#define __MAT4F_H__

#include "stdbool.h"

typedef struct quatf_t quatf_t;		// quaternion
typedef struct vec3f_t vec3f_t;		// vector3

// Your typical 4x4 matrix.
typedef struct mat4f_t {
	float mat[4][4];
} mat4f_t;

// Make a zero matrix
//
void mat4fZero(mat4f_t* m);

// Make an identity matrix.
//
void mat4fMakeIdentity(mat4f_t* m);

// Make a translation matrix.
//
void mat4fMakeTranslation(mat4f_t* m, vec3f_t* v);

// Make a scaling matrix.
//
void mat4fMakeScaling(mat4f_t* m, vec3f_t* v);

// Make a rotation matrix with a quaternion.
//
void mat4fMakeRotation(mat4f_t* m, const quatf_t* q);

// Multiply matrix a * b = res
//
void mat4fMul(mat4f_t* res, const mat4f_t* a, const mat4f_t* b);

// Multiply matrix res * a = res
//
void mat4fMulInplace(mat4f_t* res, const mat4f_t* a);

// Translate a vector.
//
void mat4fTranslate(mat4f_t* m, const vec3f_t* v);

// Transform a vector.
//
void mat4fTransform(const mat4f_t* m, const vec3f_t* in, vec3f_t* out);

// Transform a vector in place.
//
void mat4fTransformInplace(const mat4f_t* m, vec3f_t* v);

// Get determinant of matrix.
float mat4fDet(const mat4f_t* a);

// Find the inverse of a matrix A -> B = A^-1
//
// RETURN: FALSE if the determinant is 0 (not invertible)
bool mat4fInverse(const mat4f_t* a, mat4f_t* b);

// Find the inverse of a matrix (inplace)
//
// RETURN: FALSE if the determinant is 0 (not invertible)
bool mat4fInverseInplace(mat4f_t* m);

// Given a field of view angle in radians, width/height aspect ratio, and depth near+far distances, compute a perspective projection matrix.
//
void mat4fMakePerspective(mat4f_t* m, float angle, float aspect, float z_near, float z_far);

// Creates a view matrix given an eye location, the center, and up vector.
// (works the same as OpenGL's lookAt())
//
void mat4fMakeLookAt(mat4f_t* m, const vec3f_t* eye, const vec3f_t* center, const vec3f_t* up);

#endif