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
	vec3f_t half_extents;
	float collision_radius;
	float inverse_mass;
	float contact_lambda;
	bool grounded;
} physics_body_t;

typedef struct physics_distance_constraint_t physics_distance_constraint_t;
typedef struct physics_volume_constraint_t physics_volume_constraint_t;

typedef struct heap_t heap_t;
typedef struct ecs_t ecs_t;


physics_t* physicsCreate(heap_t* heap);
void physicsDestroy(physics_t* physics);
physics_body_t* physicsAddBox(physics_t* physics, vec3f_t position,
	vec3f_t half_extents, float mass, float restitution);
void physicsSetBodyCollisionRadius(physics_body_t* body, float radius);
physics_distance_constraint_t* physicsAddDistanceConstraint(physics_t* physics,
	physics_body_t* body_a, physics_body_t* body_b, float rest_length, float compliance);
physics_volume_constraint_t* physicsAddVolumeConstraint(physics_t* physics,
	physics_body_t** bodies, int body_count, const uint16_t* indices,
	int index_count, float compliance);
void physicsSetVolumeDamping(physics_volume_constraint_t* constraint, float retention);
void physicsSetGroundHeight(physics_t* physics, float height);
void physicsSetGroundCompliance(physics_t* physics, float compliance);
void physicsSetGravity(physics_t* physics, float gravity_meters_per_second_squared);
void physicsSetSolverIterations(physics_t* physics, int iterations);
bool physicsHasCapacity(physics_t* physics, int body_count,
	int distance_constraint_count, int volume_constraint_count);
void physicsUpdate(physics_t* physics, float delta_seconds);
void physicsSolveDistanceConstraint(physics_distance_constraint_t* constraint,
	float inverse_dt_squared);
void physicsSolveVolumeConstraint(physics_volume_constraint_t* constraint,
	float inverse_dt_squared);
void physicsSolveGroundConstraint(physics_t* physics, physics_body_t* body,
	float inverse_dt_squared);
void physicsSolveSoftBodyContacts(physics_t* physics);
void physicsDampSoftBodyContactVelocities(physics_t* physics);
void physicsUpdateSoftBodyBounds(physics_t* physics);
bool physicsSoftBodiesOverlap(physics_volume_constraint_t* a,
	physics_volume_constraint_t* b);
float physicsCalculateVolume(physics_volume_constraint_t* constraint);
uint32_t physicsContactHash(int x, int y, int z);


#endif
