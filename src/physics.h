#ifndef __PHYSICS_H__
#define __PHYSICS_H__



#include "vec3f.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct physics_t physics_t;

// Particles are referred to by id; ids are dense and assigned in creation order,
// so particles added back to back have consecutive ids.
typedef uint32_t physics_particle_id_t;

typedef struct physics_stats_t {
	int particle_count;
	int distance_constraint_count;
	int soft_body_count;
	int contact_count;
	int grabbed_count;
} physics_stats_t;

typedef struct heap_t heap_t;
typedef struct ecs_t ecs_t;


physics_t* physicsCreate(heap_t* heap);
void physicsDestroy(physics_t* physics);
// Adds a spherical particle. A mass of zero makes it static (pinned).
physics_particle_id_t physicsAddParticle(physics_t* physics, vec3f_t position,
	float radius, float mass);
vec3f_t physicsGetParticlePosition(physics_t* physics, physics_particle_id_t particle);
vec3f_t physicsGetParticleVelocity(physics_t* physics, physics_particle_id_t particle);
void physicsSetParticleVelocity(physics_t* physics, physics_particle_id_t particle,
	vec3f_t velocity);
// Teleports a particle: its previous position moves too, so no velocity results.
void physicsSetParticlePosition(physics_t* physics, physics_particle_id_t particle,
	vec3f_t position);
// Teleports a run of particles by the same offset.
void physicsTranslateParticles(physics_t* physics, physics_particle_id_t first_particle,
	int particle_count, vec3f_t offset);
float physicsGetParticleRadius(physics_t* physics, physics_particle_id_t particle);
// All particle positions, indexed by id. Valid until the next physicsUpdate.
const vec3f_t* physicsGetParticlePositions(physics_t* physics);
void physicsAddDistanceConstraint(physics_t* physics, physics_particle_id_t particle_a,
	physics_particle_id_t particle_b, float rest_length, float compliance);

// Groups particles [first_particle, first_particle + particle_count) into one soft
// body: its constraints are solved together, it is damped as a unit, and its
// particles only collide with each other when self_collision is set (cloth).
//
// RETURN: the soft body index
int physicsAddSoftBody(physics_t* physics, physics_particle_id_t first_particle,
	int particle_count, bool self_collision);
// Fraction of deformation velocity kept per 1/60 s. Rigid motion is not damped.
void physicsSetSoftBodyDamping(physics_t* physics, int soft_body, float retention);
// Fraction of velocity the soft body loses per second to air resistance.
void physicsSetSoftBodyDrag(physics_t* physics, int soft_body, float per_second);
// Keeps the volume enclosed by a closed triangle mesh; indices are relative to
// the soft body's first particle.
void physicsSetSoftBodyVolume(physics_t* physics, int soft_body, const uint16_t* indices,
	int index_count, float compliance);

// Finds the nearest particle along a ray; static (pinned) particles count only
// with include_static.
//
// RETURN: true if one was hit; particle and its distance along the ray are set
bool physicsPickParticle(physics_t* physics, vec3f_t origin, vec3f_t direction,
	bool include_static, physics_particle_id_t* particle, float* distance);
// Attaches the particle, and those of its soft body within radius of it, to
// target; they keep their offsets from it while it moves.
void physicsGrab(physics_t* physics, physics_particle_id_t particle, vec3f_t target,
	float radius);
void physicsMoveGrab(physics_t* physics, vec3f_t target);
void physicsReleaseGrab(physics_t* physics);
bool physicsIsGrabbing(physics_t* physics);

// Adds a static, smooth sphere that particles collide with (no soft body, never
// moves on its own).
//
// RETURN: the collider index, or -1 when full
int physicsAddSphereCollider(physics_t* physics, vec3f_t center, float radius);
void physicsSetSphereCollider(physics_t* physics, int collider, vec3f_t center, float radius);
// Finds the nearest sphere collider along a ray.
//
// RETURN: true if one was hit; collider and the distance to its surface are set
bool physicsPickSphereCollider(physics_t* physics, vec3f_t origin, vec3f_t direction,
	int* collider, float* distance);

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
// Rolling resistance coefficient: a body touching something feels a rolling
// drag of coefficient * weight, so rolling bodies come to rest (rubber ~0.01,
// soft bodies more).
void physicsSetRollingResistance(physics_t* physics, float coefficient);
bool physicsHasCapacity(physics_t* physics, int particle_count,
	int distance_constraint_count, int soft_body_count);
void physicsUpdate(physics_t* physics, float delta_seconds);
physics_stats_t physicsGetStats(physics_t* physics);


#endif
