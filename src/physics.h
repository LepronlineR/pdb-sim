#ifndef __PHYSICS_H__
#define __PHYSICS_H__



#include "vec3f.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct physics_t physics_t;

typedef struct physics_body_t {
	vec3f_t position;
	vec3f_t previous_position;
	vec3f_t velocity;
	float radius;
	float inverse_mass;
} physics_body_t;

typedef struct physics_distance_constraint_t physics_distance_constraint_t;
typedef struct physics_volume_constraint_t physics_volume_constraint_t;

typedef struct heap_t heap_t;
typedef struct ecs_t ecs_t;


physics_t* physicsCreate(heap_t* heap);
void physicsDestroy(physics_t* physics);
// Adds a spherical particle. A mass of zero makes it static.
physics_body_t* physicsAddParticle(physics_t* physics, vec3f_t position,
	float radius, float mass);
physics_distance_constraint_t* physicsAddDistanceConstraint(physics_t* physics,
	physics_body_t* body_a, physics_body_t* body_b, float rest_length, float compliance);
physics_volume_constraint_t* physicsAddVolumeConstraint(physics_t* physics,
	physics_body_t** bodies, int body_count, const uint16_t* indices,
	int index_count, float compliance);
// Fraction of deformation velocity kept per 1/60 s. Rigid motion is not damped.
void physicsSetVolumeDamping(physics_volume_constraint_t* constraint, float retention);
void physicsSetGroundHeight(physics_t* physics, float height);
void physicsSetGroundCompliance(physics_t* physics, float compliance);
void physicsSetGravity(physics_t* physics, float gravity_meters_per_second_squared);
// Substeps per physicsUpdate call; each substep runs one prediction, the
// constraint iterations and a velocity pass.
void physicsSetSubsteps(physics_t* physics, int substeps);
// Constraint iterations per substep.
void physicsSetSolverIterations(physics_t* physics, int iterations);
void physicsSetFriction(physics_t* physics, float static_coefficient,
	float dynamic_coefficient);
void physicsSetRestitution(physics_t* physics, float restitution);
bool physicsHasCapacity(physics_t* physics, int body_count,
	int distance_constraint_count, int volume_constraint_count);
void physicsUpdate(physics_t* physics, float delta_seconds);
void physicsSolveDistanceConstraint(physics_distance_constraint_t* constraint,
	float inverse_dt_squared);
void physicsSolveVolumeConstraint(physics_volume_constraint_t* constraint,
	float inverse_dt_squared);
float physicsCalculateVolume(physics_volume_constraint_t* constraint);
int physicsGetContactCount(physics_t* physics);


#endif
