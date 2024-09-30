#ifndef __QUATERNION_H__
#define __QUATERNION_H__

#include "vec3f.h"

// Your typical quaternion.
typedef struct quatf_t {
	union {
		struct { float s, x, y, z; };		// s + x<i> + y<j> + z<k>
		struct { float w; vec3f_t v3; };	// w + vec3
	};
} quatf_t;

// Creates a quaternion with no rotation.
quatf_t quatfIdentity();

// Combines the rotation of two quaternions -- a and b -- into a new quaternion.
quatf_t quatfMul(quatf_t a, quatf_t b);

// Computes the inverse of a normalized quaternion.
quatf_t quatfConjugate(quatf_t q);

// Rotates a vector by a quaterion.
// Returns the resulting vector.
vec3f_t quatfRotateVec(quatf_t q, vec3f_t v);

// Converts a quaternion to representation with 3 angles in radians: roll, yaw, pitch.
vec3f_t quatfToEuler(quatf_t q);

// Converts roll, yaw, pitch in radians to a quaternion.
quatf_t quatfFromEuler(vec3f_t euler_angles);

#endif