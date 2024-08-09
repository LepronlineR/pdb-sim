#include "transform.h"

void transformIdentity(transform_t* transform) {
	transform->rotation = quatfIdentity();
	transform->translation = vec3fZero();
	transform->scale = vec3fOne();
}

void transformConvertToMatrix(const transform_t* transform, mat4f_t* m) {
	// TODO LATER
}

void transformMul(const transform_t* a, transform_t* b) {
	const vec3f_t trans_scale = vec3fMul(a->translation, a->scale);
	const vec3f_t rot_scale = quatfRotateVec(a->rotation, a->scale);
	
	b->translation = vec3fAdd(rot_scale, a->translation);
	b->rotation = quatfMul(a->rotation, b->rotation);
	b->scale = vec3fMul(b->scale, a->scale);
}

void transformInvert(transform_t* transform) {
	vec3fInvertValues(&transform->scale);
	transform->rotation = quatfConjugate(transform->rotation);
	transform->translation = vec3fMul(transform->scale, quatfRotateVec(transform->rotation, vec3fNeg(transform->translation)));
}

vec3f_t transformTransformVec3f(const transform_t* transform, vec3f_t v) {
	const vec3f_t vec_scale = vec3fMul(v, transform->scale);
	const vec3f_t rot_trans = quatfRotateVec(transform->rotation, vec_scale);
	return vec3fAdd(rot_trans, transform->translation);
}
