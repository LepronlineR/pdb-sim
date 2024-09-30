#include "quatf.h"
#include "vec3f.h"

#define _USE_MATH_DEFINES
#include <math.h>

quatf_t quatfIdentity() { return (quatf_t) { .s = 1.0f, .x = 0.0f, .y = 0.0f, .z = 0.0f }; }

quatf_t quatfMul(quatf_t a, quatf_t b) {
    quatf_t result;

    result.v3 = vec3fCross(a.v3, b.v3);
    result.v3 = vec3fAdd(result.v3, vec3fScale(b.v3, a.w));
    result.v3 = vec3fAdd(result.v3, vec3fScale(a.v3, b.w));

    result.w = (a.w * b.w) - vec3fDot(a.v3, b.v3);

    return result;
}

quatf_t quatfConjugate(quatf_t q) {
    quatf_t result;
    result.v3 = vec3fNeg(q.v3);
    result.w = q.w;
    return result;
}

vec3f_t quatfRotateVec(quatf_t q, vec3f_t v) {
    vec3f_t t = vec3fScale(vec3fCross(q.v3, v), 2.0f);
    return vec3fAdd(v, vec3fAdd(vec3fScale(t, q.w), vec3fCross(q.v3, t)));
}

vec3f_t quatfToEuler(quatf_t q) {
    vec3f_t angles;

    // roll (x-axis rotation)
    double sinr_cosp = 2 * (q.s * q.x + q.y * q.z);
    double cosr_cosp = 1 - 2 * (q.x * q.x + q.y * q.y);
    angles.x = atan2f(sinr_cosp, cosr_cosp);

    // pitch (y-axis rotation)
    double sinp = sqrtf(1 + 2 * (q.s * q.y - q.x * q.z));
    double cosp = sqrtf(1 - 2 * (q.s * q.y - q.x * q.z));
    angles.y = 2 * atan2f(sinp, cosp) - M_PI / 2;

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (q.s * q.z + q.x * q.y);
    double cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    angles.z = atan2f(siny_cosp, cosy_cosp);

    return angles;
}

quatf_t quatfFromEuler(vec3f_t euler_angles) {

    float roll = euler_angles.x;
    float pitch = euler_angles.y;
    float yaw = euler_angles.z;

    float cr = cosf(roll * 0.5);
    float sr = sinf(roll * 0.5);
    float cp = cosf(pitch * 0.5);
    float sp = sinf(pitch * 0.5);
    float cy = cosf(yaw * 0.5);
    float sy = sinf(yaw * 0.5);

    quatf_t q;
    q.s = cr * cp * cy + sr * sp * sy;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;

    return q;
}
