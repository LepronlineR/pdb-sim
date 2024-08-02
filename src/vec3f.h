#ifndef __VEC_3F_H__
#define __VEC_3F_H__

#include "moremath.h"

typedef struct vec3f_t {
	union {
		struct { float x, y, z; };
		float v[3];
	};
} vec3f_t;

__forceinline vec3f_t vec3f_x()
{
	return (vec3f_t) { .x = 1.0f };
}

__forceinline vec3f_t vec3f_y()
{
	return (vec3f_t) { .y = 1.0f };
}

#endif