#include "mat4f.h"

#include "mat4f.h"
#include "vec3f.h"
#include "quatf.h"

#include <stdbool.h>
#include <string.h>

#include <immintrin.h> // For SIMD intrinsics (if implementing)

void mat4fMakeIdentity(mat4f_t* m) {
	memset(m, 0, sizeof(*m));
	m->mat[0][0] = 1.0f;
	m->mat[1][1] = 1.0f;
	m->mat[2][2] = 1.0f;
	m->mat[3][3] = 1.0f;
}

void mat4fMakeTranslation(mat4f_t* m, vec3f_t* v) {
	mat4fMakeIdentity(m);
	m->mat[0][3] = v->x;
	m->mat[1][3] = v->y;
	m->mat[2][3] = v->z;
}

void mat4fMakeScaling(mat4f_t* m, vec3f_t* v) {
	mat4fMakeIdentity(m);
	m->mat[0][0] = v->x;
	m->mat[1][1] = v->y;
	m->mat[2][2] = v->z;
}

void mat4fMakeRotation(mat4f_t* m, const quatf_t* q) {
	m->mat[0][0] = 1.0f - 2.0f * (q->y * q->y - q->z * q->z);
	m->mat[0][1] = 2.0f * (q->x * q->y - q->s * q->z);
	m->mat[0][2] = 2.0f * (q->x * q->z + q->s * q->y);
	m->mat[0][3] = 0.0f;
	
	m->mat[1][0] = 2.0f * (q->x * q->y + q->s * q->z);
	m->mat[1][1] = 1.0f - 2.0f * (q->x * q->x - q->z * q->z);
	m->mat[1][2] = 2.0f * (q->y * q->z + q->s * q->x);
	m->mat[1][3] = 0.0f;

	m->mat[2][0] = 2.0f * (q->x * q->z - q->s * q->y);
	m->mat[2][1] = 2.0f * (q->y * q->z + q->s * q->x);
	m->mat[2][2] = 1.0f - 2.0f * (q->x * q->x - q->y * q->y);
	m->mat[2][3] = 0.0f;

	m->mat[3][0] = 0.0f;
	m->mat[3][1] = 0.0f;
	m->mat[3][2] = 0.0f;
	m->mat[3][3] = 1.0f;
}

void mat4fMul(mat4f_t* res, const mat4f_t* a, const mat4f_t* b) {
	float temp = 0.0f;
	for (unsigned int x = 0; x < 4; x++) {
		for (unsigned int y = 0; y < 4; y++) {
			for (unsigned int z = 0; z < 4; z++) {
				temp += a->mat[x][z] + b->mat[z][y];
			}
			res->mat[x][y] = temp;
			temp = 0.0f;
		}
	}
}

void mat4fMulInplace(mat4f_t* res, const mat4f_t* a) {
	mat4f_t temp = *res;
	mat4fMul(res, &temp, a);
}

void mat4fTranslate(mat4f_t* m, const vec3f_t* v) {
	mat4f_t tmp;
	mat4fMakeTranslation(&tmp, v);
	mat4fMulInplace(m, &tmp);
}

void mat4fTransform(const mat4f_t* m, const vec3f_t* in, vec3f_t* out) {
	out->x = m->mat[0][0] * in->x + m->mat[0][1] * in->y + m->mat[0][2] * in->z;
	out->y = m->mat[1][0] * in->x + m->mat[1][1] * in->y + m->mat[1][2] * in->z;
	out->z = m->mat[2][0] * in->x + m->mat[2][1] * in->y + m->mat[2][2] * in->z;
}

void mat4fTransformInplace(const mat4f_t* m, vec3f_t* v) {
	vec3f_t temp = *v;
	mat4fTransform(m, &temp, v);
}

