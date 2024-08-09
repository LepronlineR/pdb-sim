#ifndef __TRANSFORM_H__
#define __TRANSFORM_H__

#include "vec3f.h"
#include "quatf.h"
#include "mat4f.h"

typedef struct transform_t {
	vec3f_t translation;
	vec3f_t scale;
	quatf_t rotation;
} transform_t;

void transformIdentity(transform_t* transform);

void transformConvertToMatrix(const transform_t* transform, mat4f_t* m);

void transformMul(const transform_t* a, transform_t* b);

void transformInvert(transform_t* transform);

vec3f_t transformTransformVec3f(const transform_t* transform, vec3f_t v);

#endif