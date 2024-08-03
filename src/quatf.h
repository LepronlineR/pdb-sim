#ifndef __QUATERNION_H__
#define __QUATERNION_H__

#include "vec3f.h"

// Your typical quaternion.
typedef struct quatf_t {
	union {
		struct { float s, x, y, z; };		// s + x<i> + y<j> + z<k>
		struct { float s, i, j, k; };		// s + ix + jy + kz
		struct { vec3f_t v3; float s; };	// s + vec3
	};
} quatf_t;

#endif