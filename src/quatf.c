#include "quatf.h"

quatf_t quatfIdentity() { return (quatf_t) { .s = 1.0f, .x = 0.0f, .y = 0.0f, .z = 0.0f }; }

quatf_t quatfMul(quatf_t a, quatf_t b) {

}

quatf_t quatfConjugate(quatf_t q) {

}

vec3f_t quatfRotateVec(quatf_t q, vec3f_t v) {

}

vec3f_t quatfToEuler(quatf_t q) {

}

quatf_t quatfFromEuler(vec3f_t euler_angles) {

}
