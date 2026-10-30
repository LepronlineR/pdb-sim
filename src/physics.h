#ifndef __PHYSICS_H__
#define __PHYSICS_H__



#include "vec3f.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct physics_t physics_t;

// Particles are referred to by id; ids are dense and assigned in creation order,
// so particles added back to back have consecutive ids.
typedef uint32_t physics_particle_id_t;

typedef struct physics_volume_constraint_t physics_volume_constraint_t;

typedef struct heap_t heap_t;
typedef struct ecs_t ecs_t;


physics_t* physicsCreate(heap_t* heap);
void physicsDestroy(physics_t* physics);
// Adds a spherical particle. A mass of zero makes it static.
physics_particle_id_t physicsAddParticle(physics_t* physics, vec3f_t position,
	float radius, float mass);
vec3f_t physicsGetParticlePosition(physics_t* physics, physics_particle_id_t particle);
vec3f_t physicsGetParticleVelocity(physics_t* physics, physics_particle_id_t particle);
void physicsSetParticleVelocity(physics_t* physics, physics_particle_id_t particle,
	vec3f_t velocity);
float physicsGetParticleRadius(physics_t* physics, physics_particle_id_t particle);
// All particle positions, indexed by id. Valid until the next physicsUpdate.
const vec3f_t* physicsGetParticlePositions(physics_t* physics);
void physicsAddDistanceConstraint(physics_t* physics, physics_particle_id_t particle_a,
	physics_particle_id_t particle_b, float rest_length, float compliance);
// The volume is enclosed by particles [first_particle, first_particle + particle_count);
// indices are triangles relative to first_particle.
physics_volume_constraint_t* physicsAddVolumeConstraint(physics_t* physics,
	physics_particle_id_t first_particle, int particle_count, const uint16_t* indices,
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
bool physicsHasCapacity(physics_t* physics, int particle_count,
	int distance_constraint_count, int volume_constraint_count);
void physicsUpdate(physics_t* physics, float delta_seconds);
int physicsGetContactCount(physics_t* physics);


#endif
