#include "mat4f.h"

#include "mat4f.h"
#include "vec3f.h"
#include "quatf.h"
#include "debug.h"

#include <stdbool.h>
#include <string.h>

#include <immintrin.h> // For SIMD intrinsics (if implementing)

void mat4fZero(mat4f_t* m) {
	memset(m, 0, sizeof(*m));
}

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

float mat4fDet(const mat4f_t* a) {
	float a_det =
		a->mat[0][0] * a->mat[1][1] * a->mat[2][2] * a->mat[3][3] +
		a->mat[0][0] * a->mat[1][2] * a->mat[2][3] * a->mat[3][1] +
		a->mat[0][0] * a->mat[1][3] * a->mat[2][1] * a->mat[3][2] -
		a->mat[0][0] * a->mat[1][3] * a->mat[2][2] * a->mat[3][1] -
		a->mat[0][0] * a->mat[1][2] * a->mat[2][1] * a->mat[3][3] -
		a->mat[0][0] * a->mat[1][1] * a->mat[2][3] * a->mat[3][2] -

		a->mat[0][1] * a->mat[1][0] * a->mat[2][2] * a->mat[3][3] -
		a->mat[0][2] * a->mat[1][0] * a->mat[2][3] * a->mat[3][1] -
		a->mat[0][3] * a->mat[1][0] * a->mat[2][1] * a->mat[3][2] +
		a->mat[0][3] * a->mat[1][0] * a->mat[2][2] * a->mat[3][1] +
		a->mat[0][2] * a->mat[1][0] * a->mat[2][1] * a->mat[3][3] +
		a->mat[0][1] * a->mat[1][0] * a->mat[2][3] * a->mat[3][2] +

		a->mat[0][1] * a->mat[1][2] * a->mat[2][0] * a->mat[3][3] +
		a->mat[0][2] * a->mat[1][2] * a->mat[2][0] * a->mat[3][1] +
		a->mat[0][3] * a->mat[1][1] * a->mat[2][0] * a->mat[3][2] -
		a->mat[0][3] * a->mat[1][2] * a->mat[2][0] * a->mat[3][1] -
		a->mat[0][2] * a->mat[1][1] * a->mat[2][0] * a->mat[3][3] -
		a->mat[0][1] * a->mat[1][3] * a->mat[2][0] * a->mat[3][2] -

		a->mat[0][1] * a->mat[1][2] * a->mat[2][3] * a->mat[3][0] -
		a->mat[0][2] * a->mat[1][3] * a->mat[2][1] * a->mat[3][0] -
		a->mat[0][3] * a->mat[1][1] * a->mat[2][2] * a->mat[3][0] +
		a->mat[0][3] * a->mat[1][2] * a->mat[2][1] * a->mat[3][0] +
		a->mat[0][2] * a->mat[1][1] * a->mat[2][3] * a->mat[3][0] +
		a->mat[0][1] * a->mat[1][3] * a->mat[2][2] * a->mat[3][0];
	
	return a_det;
}

// NOTE: inverse matrix is equal to the product of the 
//		 reciprocal of the det and adj matrix
bool mat4fInverse(const mat4f_t* a, mat4f_t* b) {
	float s[6] = { 0 };
	s[0] = a->mat[0][0] * a->mat[1][1] - a->mat[1][0] * a->mat[0][1];
	s[1] = a->mat[0][0] * a->mat[1][2] - a->mat[1][0] * a->mat[0][2];
	s[2] = a->mat[0][0] * a->mat[1][3] - a->mat[1][0] * a->mat[0][3];
	s[3] = a->mat[0][1] * a->mat[1][2] - a->mat[1][1] * a->mat[0][2];
	s[4] = a->mat[0][1] * a->mat[1][3] - a->mat[1][1] * a->mat[0][3];
	s[5] = a->mat[0][2] * a->mat[1][3] - a->mat[1][2] * a->mat[0][3];

	float c[6] = { 0 };
	c[0] = a->mat[2][0] * a->mat[3][1] - a->mat[3][0] * a->mat[2][1];
	c[1] = a->mat[2][0] * a->mat[3][2] - a->mat[3][0] * a->mat[2][2];
	c[2] = a->mat[2][0] * a->mat[3][3] - a->mat[3][0] * a->mat[2][3];
	c[3] = a->mat[2][1] * a->mat[3][2] - a->mat[3][1] * a->mat[2][2];
	c[4] = a->mat[2][1] * a->mat[3][3] - a->mat[3][1] * a->mat[2][3];
	c[5] = a->mat[2][2] * a->mat[3][3] - a->mat[3][2] * a->mat[2][3];

	float a_det = s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
	if (a_det == 0.0f) { return false;  }
	float inv_det = 1.0f / a_det;

	b->mat[0][0] = (a->mat[1][1] * c[5] - a->mat[1][2] * c[4] + a->mat[1][3] * c[3]) * inv_det;
	b->mat[0][1] = (-a->mat[0][1] * c[5] + a->mat[0][2] * c[4] - a->mat[0][3] * c[3]) * inv_det;
	b->mat[0][2] = (a->mat[3][1] * s[5] - a->mat[3][2] * s[4] + a->mat[3][3] * s[3]) * inv_det;
	b->mat[0][3] = (-a->mat[2][1] * s[5] + a->mat[2][2] * s[4] - a->mat[2][3] * s[3]) * inv_det;

	b->mat[1][0] = (-a->mat[1][0] * c[5] + a->mat[1][2] * c[2] - a->mat[1][3] * c[1]) * inv_det;
	b->mat[1][1] = (a->mat[0][0] * c[5] - a->mat[0][2] * c[2] + a->mat[0][3] * c[1]) * inv_det;
	b->mat[1][2] = (-a->mat[3][0] * s[5] + a->mat[3][2] * s[2] - a->mat[3][3] * s[1]) * inv_det;
	b->mat[1][3] = (a->mat[2][0] * s[5] - a->mat[2][2] * s[2] + a->mat[2][3] * s[1]) * inv_det;

	b->mat[2][0] = (a->mat[1][0] * c[4] - a->mat[1][1] * c[2] + a->mat[1][3] * c[0]) * inv_det;
	b->mat[2][1] = (-a->mat[0][0] * c[4] + a->mat[0][1] * c[2] - a->mat[0][3] * c[0]) * inv_det;
	b->mat[2][2] = (a->mat[3][0] * s[4] - a->mat[3][1] * s[2] + a->mat[3][3] * s[0]) * inv_det;
	b->mat[2][3] = (-a->mat[2][0] * s[4] + a->mat[2][1] * s[2] - a->mat[2][3] * s[0]) * inv_det;

	b->mat[3][0] = (-a->mat[1][0] * c[3] + a->mat[1][1] * c[1] - a->mat[1][2] * c[0]) * inv_det;
	b->mat[3][1] = (a->mat[0][0] * c[3] - a->mat[0][1] * c[1] + a->mat[0][2] * c[0]) * inv_det;
	b->mat[3][2] = (-a->mat[3][0] * s[3] + a->mat[3][1] * s[1] - a->mat[3][2] * s[0]) * inv_det;
	b->mat[3][3] = (a->mat[2][0] * s[3] - a->mat[2][1] * s[1] + a->mat[2][2] * s[0]) * inv_det;

	return true;
}

bool mat4fInverseInplace(mat4f_t* m) {	
	mat4f_t b = *m;
	return mat4fInverse(&b, m);
}

void mat4fMakePerspective(mat4f_t* m, float angle, float aspect, float z_near, float z_far) {
	if (angle <= 0.0f) {
		debugPrint(DEBUG_PRINT_ERROR, "MAT4 Make Perspective: angle is 0 or less than 0!\n");
	}

	aspect = __max(FLT_EPSILON, aspect);
	z_far = __max(FLT_EPSILON, z_far);

	// fov = tan(angle/2)
	float fov = tanf(angle * 0.5f);
	float inv_fov = 1.0f / fov;

	m->mat[0][0] = aspect * inv_fov;
	m->mat[1][1] = inv_fov;
	m->mat[2][2] = -(z_far + z_near) / (z_far - z_near);
	m->mat[2][3] = -(2.0f * z_far + z_near) / (z_far - z_near);
	m->mat[3][2] = -1.0f;
}

void mat4fMakeLookAt(mat4f_t* m, const vec3f_t* eye, const vec3f_t* center, const vec3f_t* up) {
	vec3f_t z_axis = vec3fNorm(vec3fSub(*eye, *center));
	vec3f_t x_axis = vec3fNorm(vec3fCross(*up, z_axis));
	vec3f_t y_axis = vec3fCross(z_axis, x_axis);

	m->mat[0][0] = x_axis.x;
	m->mat[1][0] = x_axis.y;
	m->mat[2][0] = x_axis.z;
	m->mat[3][0] = vec3fDot(x_axis, *eye);

	m->mat[0][1] = y_axis.x;
	m->mat[1][1] = y_axis.y;
	m->mat[2][1] = y_axis.z;
	m->mat[3][1] = vec3fDot(y_axis, *eye);

	m->mat[0][2] = z_axis.x;
	m->mat[1][2] = z_axis.y;
	m->mat[2][2] = z_axis.z;
	m->mat[3][2] = vec3fDot(z_axis, *eye);

	m->mat[0][3] = 0.0f;
	m->mat[1][3] = 0.0f;
	m->mat[2][3] = 0.0f;
	m->mat[3][3] = 1.0f;
}