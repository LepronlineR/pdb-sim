#include "physics.h"
#include "heap.h"
#include "debug.h"
#include "semaphore.h"
#include "thread.h"
#include "atomic.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>

/* Solver outline (Macklin et al., "Small Steps in Physics Simulation", 2019 and
   "Detailed Rigid Body Simulation with Extended Position Based Dynamics", 2020):

   physicsUpdate(dt)
     detect contact candidates once, padded by how far particles can move in dt
     for each substep h = dt / substeps
       predict positions
       solve distance + volume constraints per soft body, then contacts
       rebuild velocities from positions
       contact velocity pass (dynamic friction, restitution)
       damp deformation, keeping each soft body's rigid motion

   Particle state is stored as separate arrays indexed by particle id, so the
   solver loops touch only the fields they use. Constraints refer to particles
   by id. */

enum {
	PHYSICS_MAX_PARTICLES = 8192,
	PHYSICS_MAX_DISTANCE_CONSTRAINTS = 32768,
	PHYSICS_MAX_SOFT_BODIES = 32,
	PHYSICS_MAX_VOLUME_INDICES = 1536,
	PHYSICS_MAX_GRABBED_PARTICLES = 96,
	PHYSICS_MAX_CONTACTS = 131072,
	PHYSICS_GRID_TABLE_SIZE = 16384, /* power of two, >= 2 * PHYSICS_MAX_PARTICLES */
	PHYSICS_MAX_WORKERS = 8,
	/* Per-particle loops are a few multiply-adds; below this many per thread,
	   waking workers costs more than it saves. */
	PHYSICS_MIN_PARTICLE_WORK_PER_THREAD = 8192
};

#define PHYSICS_GROUND (-1)
/* Softness of the mouse attachment: low enough to drag firmly, high enough that
   a fast flick does not inject a huge impulse. */
#define PHYSICS_GRAB_COMPLIANCE 1.0e-5f

typedef void (*physics_parallel_func_t)(physics_t* physics, int start, int end,
	float delta_seconds, float inverse_dt_squared);

typedef struct physics_worker_t {
	physics_t* physics;
	int index;
} physics_worker_t;

typedef struct physics_distance_constraint_t {
	physics_particle_id_t particle_a;
	physics_particle_id_t particle_b;
	float rest_length;
	float compliance;
	float lambda;
} physics_distance_constraint_t;

/* A run of consecutive particles simulated as one object: its distance
   constraints form one parallel island, it is damped as a unit, and it may carry
   a volume constraint (closed meshes such as balls). */
typedef struct physics_soft_body_t {
	physics_particle_id_t first_particle;
	int particle_count;
	bool self_collision;
	float total_mass;
	float velocity_retention;
	/* Fraction of velocity lost per second to the air. Rigid-motion damping cannot
	   settle a pinned sheet swinging like a pendulum; drag does. */
	float drag;
	/* Set while any contact pushed on the body this substep. */
	bool touching;
	bool has_volume;
	uint16_t* indices;
	vec3f_t* gradients;
	int index_count;
	float rest_volume;
	float volume_compliance;
	float volume_lambda;
} physics_soft_body_t;

/* A non-penetration constraint between two particles, or a particle and the
   ground plane when particle_b is PHYSICS_GROUND. The normal points from b to a. */
typedef struct physics_contact_t {
	int particle_a;
	int particle_b;
	vec3f_t normal;
	float lambda;
	float normal_speed_before;
} physics_contact_t;

typedef struct physics_t {
	heap_t* heap;
	int particle_count;
	vec3f_t* positions;
	vec3f_t* previous_positions;
	vec3f_t* velocities;
	float* inverse_masses;
	float* radii;
	/* Index of the soft body that owns each particle, or -1. */
	int* particle_owner;
	float max_radius;
	/* Kept sorted by island once built: island i is
	   [distance_island_offsets[i], distance_island_offsets[i + 1]), and the last
	   island holds constraints that span soft bodies or belong to none. */
	physics_distance_constraint_t* distance_constraints;
	int distance_constraint_count;
	int distance_island_offsets[PHYSICS_MAX_SOFT_BODIES + 2];
	bool distance_islands_dirty;
	physics_soft_body_t* soft_bodies;
	int soft_body_count;
	bool soft_bodies_overlap;
	physics_contact_t* contacts;
	int contact_count;
	bool contact_overflow_reported;
	/* Spatial hash over all particles, built with a counting sort: the particles
	   in bucket h are grid_entries[grid_start[h] .. grid_start[h + 1]). */
	int* grid_start;
	int* grid_entries;
	int* grid_cell;
	/* Particles near another soft body; the only ones placed in the grid. */
	int* grid_candidates;
	/* How far each particle can move this frame, padding its contact search. */
	float* grid_travel;
	/* Particles held by the mouse: each is pulled toward grab_target plus its
	   offset at the moment it was grabbed. */
	physics_particle_id_t grabbed_particles[PHYSICS_MAX_GRABBED_PARTICLES];
	vec3f_t grabbed_offsets[PHYSICS_MAX_GRABBED_PARTICLES];
	float grabbed_lambdas[PHYSICS_MAX_GRABBED_PARTICLES];
	int grabbed_count;
	vec3f_t grab_target;
	int substeps;
	int solver_iterations;
	float gravity;
	float ground_height;
	float ground_compliance;
	float static_friction;
	float dynamic_friction;
	float restitution;
	float rolling_resistance;
	int worker_count;
	int parallel_worker_count;
	int parallel_next_chunk;
	int parallel_count;
	float parallel_delta_seconds;
	float parallel_inverse_dt_squared;
	physics_parallel_func_t parallel_func;
	bool worker_shutdown;
	semaphore_t* worker_start;
	semaphore_t* worker_done;
	thread_t* worker_threads[PHYSICS_MAX_WORKERS - 1];
	physics_worker_t workers[PHYSICS_MAX_WORKERS];
} physics_t;

static int physicsWorkerFunc(void* user);
static void physicsDestroyWorkers(physics_t* physics);
static void physicsParallelFor(physics_t* physics, int count, int min_items_per_thread,
	physics_parallel_func_t func, float delta_seconds, float inverse_dt_squared);
static void physicsRunParallelChunk(physics_t* physics);
static void physicsPredictRange(physics_t* physics, int start, int end,
	float delta_seconds, float inverse_dt_squared);
static void physicsSolveIslandsRange(physics_t* physics, int start, int end,
	float delta_seconds, float inverse_dt_squared);
static void physicsReconstructVelocitiesRange(physics_t* physics, int start, int end,
	float delta_seconds, float inverse_dt_squared);
static void physicsResetLambdas(physics_t* physics);
static int physicsDistanceIsland(physics_t* physics,
	const physics_distance_constraint_t* constraint);
static void physicsBuildDistanceIslands(physics_t* physics);
static void physicsSolveDistanceIsland(physics_t* physics, int island,
	float inverse_dt_squared);
static void physicsSolveDistanceConstraint(physics_t* physics,
	physics_distance_constraint_t* constraint, float inverse_dt_squared);
static float physicsCalculateVolume(physics_t* physics, const physics_soft_body_t* body);
static void physicsSolveVolumeConstraint(physics_t* physics, physics_soft_body_t* body,
	float inverse_dt_squared);
static void physicsSolveGrab(physics_t* physics, float inverse_dt_squared);
static uint32_t physicsGridHash(int x, int y, int z);
static void physicsAddContact(physics_t* physics, int particle_a, int particle_b);
static void physicsDetectContacts(physics_t* physics, float delta_seconds);
static void physicsSolveContacts(physics_t* physics, float inverse_dt_squared);
static void physicsSolveContactVelocities(physics_t* physics, float delta_seconds);
static bool physicsInvert3x3(const float m[9], float out[9]);
static void physicsDampenDeformation(physics_t* physics, float delta_seconds);

static int physicsWorkerFunc(void* user) {
	physics_worker_t* worker = (physics_worker_t*)user;
	physics_t* physics = worker->physics;
	for (;;) {
		semaphoreGet(physics->worker_start);
		if (physics->worker_shutdown) {
			break;
		}
		physicsRunParallelChunk(physics);
		semaphoreRelease(physics->worker_done);
	}
	return 0;
}

static void physicsDestroyWorkers(physics_t* physics) {
	if (physics->worker_start) {
		physics->worker_shutdown = true;
		for (int i = 1; i < physics->worker_count; ++i) {
			if (physics->worker_threads[i - 1]) {
				semaphoreRelease(physics->worker_start);
			}
		}
		for (int i = 1; i < physics->worker_count; ++i) {
			if (physics->worker_threads[i - 1]) {
				threadRun(physics->worker_threads[i - 1]);
				physics->worker_threads[i - 1] = NULL;
			}
		}
	}
	if (physics->worker_done) {
		semaphoreDestroy(physics->worker_done);
		physics->worker_done = NULL;
	}
	if (physics->worker_start) {
		semaphoreDestroy(physics->worker_start);
		physics->worker_start = NULL;
	}
	physics->worker_count = 1;
	physics->parallel_worker_count = 1;
	physics->worker_shutdown = false;
}

static void physicsParallelFor(physics_t* physics, int count, int min_items_per_thread,
	physics_parallel_func_t func, float delta_seconds, float inverse_dt_squared) {
	if (count <= 0) {
		return;
	}
	if (min_items_per_thread < 1) {
		min_items_per_thread = 1;
	}
	int worker_count = __min(physics->worker_count,
		(count + min_items_per_thread - 1) / min_items_per_thread);
	worker_count = __max(1, worker_count);
	if (worker_count <= 1 || physics->worker_start == NULL || physics->worker_done == NULL) {
		func(physics, 0, count, delta_seconds, inverse_dt_squared);
		return;
	}

	physics->parallel_func = func;
	physics->parallel_count = count;
	physics->parallel_delta_seconds = delta_seconds;
	physics->parallel_inverse_dt_squared = inverse_dt_squared;
	physics->parallel_worker_count = worker_count;
	physics->parallel_next_chunk = 0;

	for (int i = 1; i < worker_count; ++i) {
		semaphoreRelease(physics->worker_start);
	}
	physicsRunParallelChunk(physics);
	for (int i = 1; i < worker_count; ++i) {
		semaphoreGet(physics->worker_done);
	}

	physics->parallel_func = NULL;
	physics->parallel_count = 0;
	physics->parallel_worker_count = 1;
}

/* Chunks are claimed rather than tied to a worker index: a fast worker can take
   two start tokens in one dispatch, which would otherwise run its chunk twice and
   skip another. */
static void physicsRunParallelChunk(physics_t* physics) {
	const int worker_index = atomicInc(&physics->parallel_next_chunk);
	const int start = (physics->parallel_count * worker_index) /
		physics->parallel_worker_count;
	const int end = (physics->parallel_count * (worker_index + 1)) /
		physics->parallel_worker_count;
	physics->parallel_func(physics, start, end, physics->parallel_delta_seconds,
		physics->parallel_inverse_dt_squared);
}

static void physicsPredictRange(physics_t* physics, int start, int end,
	float delta_seconds, float inverse_dt_squared) {
	(void)inverse_dt_squared;
	vec3f_t* positions = physics->positions;
	vec3f_t* velocities = physics->velocities;
	const float gravity_step = physics->gravity * delta_seconds;
	for (int i = start; i < end; ++i) {
		physics->previous_positions[i] = positions[i];
		if (physics->inverse_masses[i] == 0.0f) {
			continue;
		}
		velocities[i].y += gravity_step;
		positions[i] = vec3fAdd(positions[i], vec3fScale(velocities[i], delta_seconds));
	}
}

/* Each island is one soft body: its own distance constraints followed by its
   volume constraint. Islands touch disjoint particles, so they run in parallel. */
static void physicsSolveIslandsRange(physics_t* physics, int start, int end,
	float delta_seconds, float inverse_dt_squared) {
	(void)delta_seconds;
	for (int i = start; i < end; ++i) {
		physicsSolveDistanceIsland(physics, i, inverse_dt_squared);
		if (physics->soft_bodies[i].has_volume) {
			physicsSolveVolumeConstraint(physics, &physics->soft_bodies[i], inverse_dt_squared);
		}
	}
}

static void physicsSolveDistanceIsland(physics_t* physics, int island,
	float inverse_dt_squared) {
	const int begin = physics->distance_island_offsets[island];
	const int end = physics->distance_island_offsets[island + 1];
	for (int i = begin; i < end; ++i) {
		physicsSolveDistanceConstraint(physics, &physics->distance_constraints[i],
			inverse_dt_squared);
	}
}

static void physicsReconstructVelocitiesRange(physics_t* physics, int start, int end,
	float delta_seconds, float inverse_dt_squared) {
	(void)inverse_dt_squared;
	const float inverse_dt = 1.0f / delta_seconds;
	for (int i = start; i < end; ++i) {
		if (physics->inverse_masses[i] == 0.0f) {
			continue;
		}
		physics->velocities[i] = vec3fScale(
			vec3fSub(physics->positions[i], physics->previous_positions[i]), inverse_dt);
	}
}

static void physicsResetLambdas(physics_t* physics) {
	for (int i = 0; i < physics->distance_constraint_count; ++i) {
		physics->distance_constraints[i].lambda = 0.0f;
	}
	for (int i = 0; i < physics->soft_body_count; ++i) {
		physics->soft_bodies[i].volume_lambda = 0.0f;
	}
	for (int i = 0; i < physics->grabbed_count; ++i) {
		physics->grabbed_lambdas[i] = 0.0f;
	}
	for (int i = 0; i < physics->contact_count; ++i) {
		physics->contacts[i].lambda = 0.0f;
	}
}

static int physicsDistanceIsland(physics_t* physics,
	const physics_distance_constraint_t* constraint) {
	const int owner_a = physics->particle_owner[constraint->particle_a];
	const int owner_b = physics->particle_owner[constraint->particle_b];
	return (owner_a >= 0 && owner_a == owner_b && !physics->soft_bodies_overlap) ?
		owner_a : physics->soft_body_count;
}

/* Stable counting sort of the distance constraints by owning soft body, so each
   island is contiguous and keeps the original Gauss-Seidel order. Runs only
   after constraints are added. */
static void physicsBuildDistanceIslands(physics_t* physics) {
	const int island_count = physics->soft_body_count;
	const int count = physics->distance_constraint_count;
	int* offsets = physics->distance_island_offsets;
	for (int i = 0; i <= island_count + 1; ++i) offsets[i] = 0;
	for (int i = 0; i < count; ++i) {
		offsets[physicsDistanceIsland(physics, &physics->distance_constraints[i]) + 1]++;
	}
	for (int i = 0; i <= island_count; ++i) offsets[i + 1] += offsets[i];

	physics_distance_constraint_t* sorted = heapAlloc(physics->heap,
		sizeof(physics_distance_constraint_t) * __max(count, 1),
		_Alignof(physics_distance_constraint_t));
	int cursor[PHYSICS_MAX_SOFT_BODIES + 1];
	for (int i = 0; i <= island_count; ++i) cursor[i] = offsets[i];
	for (int i = 0; i < count; ++i) {
		const physics_distance_constraint_t* constraint = &physics->distance_constraints[i];
		sorted[cursor[physicsDistanceIsland(physics, constraint)]++] = *constraint;
	}
	memcpy(physics->distance_constraints, sorted, sizeof(physics_distance_constraint_t) * count);
	heapFree(physics->heap, sorted);
	physics->distance_islands_dirty = false;
}

static void physicsSolveDistanceConstraint(physics_t* physics,
	physics_distance_constraint_t* constraint, float inverse_dt_squared) {
	const physics_particle_id_t a = constraint->particle_a;
	const physics_particle_id_t b = constraint->particle_b;
	const float inverse_mass_a = physics->inverse_masses[a];
	const float inverse_mass_b = physics->inverse_masses[b];
	const float inverse_mass_sum = inverse_mass_a + inverse_mass_b;
	vec3f_t* positions = physics->positions;
	vec3f_t delta = vec3fSub(positions[a], positions[b]);
	const float length = vec3fMagnitude(delta);
	if (length < 1.0e-6f || inverse_mass_sum == 0.0f) {
		return;
	}

	const vec3f_t gradient = vec3fScale(delta, 1.0f / length);
	const float constraint_value = length - constraint->rest_length;
	const float alpha_tilde = constraint->compliance * inverse_dt_squared;
	const float delta_lambda = (-constraint_value - alpha_tilde * constraint->lambda) /
		(inverse_mass_sum + alpha_tilde);
	constraint->lambda += delta_lambda;
	positions[a] = vec3fAdd(positions[a], vec3fScale(gradient, inverse_mass_a * delta_lambda));
	positions[b] = vec3fSub(positions[b], vec3fScale(gradient, inverse_mass_b * delta_lambda));
}

static float physicsCalculateVolume(physics_t* physics, const physics_soft_body_t* body) {
	const vec3f_t* positions = physics->positions + body->first_particle;
	float volume = 0.0f;
	for (int i = 0; i < body->index_count; i += 3) {
		vec3f_t a = positions[body->indices[i]];
		vec3f_t b = positions[body->indices[i + 1]];
		vec3f_t c = positions[body->indices[i + 2]];
		volume += vec3fDot(a, vec3fCross(b, c)) / 6.0f;
	}
	return volume;
}

static void physicsSolveVolumeConstraint(physics_t* physics, physics_soft_body_t* body,
	float inverse_dt_squared) {
	vec3f_t* positions = physics->positions + body->first_particle;
	const float* inverse_masses = physics->inverse_masses + body->first_particle;
	vec3f_t* gradients = body->gradients;
	for (int i = 0; i < body->particle_count; ++i) gradients[i] = vec3fZero();
	float volume = 0.0f;
	for (int i = 0; i < body->index_count; i += 3) {
		uint16_t ia = body->indices[i];
		uint16_t ib = body->indices[i + 1];
		uint16_t ic = body->indices[i + 2];
		vec3f_t a = positions[ia];
		vec3f_t b = positions[ib];
		vec3f_t c = positions[ic];
		volume += vec3fDot(a, vec3fCross(b, c)) / 6.0f;
		gradients[ia] = vec3fAdd(gradients[ia], vec3fScale(vec3fCross(b, c), 1.0f / 6.0f));
		gradients[ib] = vec3fAdd(gradients[ib], vec3fScale(vec3fCross(c, a), 1.0f / 6.0f));
		gradients[ic] = vec3fAdd(gradients[ic], vec3fScale(vec3fCross(a, b), 1.0f / 6.0f));
	}

	float denominator = 0.0f;
	for (int i = 0; i < body->particle_count; ++i) {
		denominator += inverse_masses[i] * vec3fMagnitudeSqrd(gradients[i]);
	}
	const float alpha_tilde = body->volume_compliance * inverse_dt_squared;
	if (denominator + alpha_tilde <= 1.0e-8f) {
		return;
	}
	const float constraint_value = volume - body->rest_volume;
	const float delta_lambda = (-constraint_value - alpha_tilde * body->volume_lambda) /
		(denominator + alpha_tilde);
	body->volume_lambda += delta_lambda;
	for (int i = 0; i < body->particle_count; ++i) {
		positions[i] = vec3fAdd(positions[i],
			vec3fScale(gradients[i], inverse_masses[i] * delta_lambda));
	}
}

static uint32_t physicsGridHash(int x, int y, int z) {
	uint32_t hash = (uint32_t)x * 73856093u;
	hash ^= (uint32_t)y * 19349663u;
	hash ^= (uint32_t)z * 83492791u;
	return hash & (PHYSICS_GRID_TABLE_SIZE - 1);
}

static void physicsAddContact(physics_t* physics, int particle_a, int particle_b) {
	if (physics->contact_count >= PHYSICS_MAX_CONTACTS) {
		if (!physics->contact_overflow_reported) {
			debugPrint(DEBUG_PRINT_WARNING, "Physics: contact capacity reached, dropping contacts.\n");
			physics->contact_overflow_reported = true;
		}
		return;
	}
	physics_contact_t* contact = &physics->contacts[physics->contact_count++];
	contact->particle_a = particle_a;
	contact->particle_b = particle_b;
	contact->normal = vec3fY();
	contact->lambda = 0.0f;
	contact->normal_speed_before = 0.0f;
}

/* Collects every pair that could touch during the next delta_seconds, so the
   substeps only solve a fixed list. Pairs inside one soft body are skipped unless
   it collides with itself (cloth); otherwise its own constraints keep its
   particles apart. */
static void physicsDetectContacts(physics_t* physics, float delta_seconds) {
	physics->contact_count = 0;
	const int count = physics->particle_count;
	if (count == 0) {
		return;
	}
	const vec3f_t* positions = physics->positions;
	const vec3f_t* velocities = physics->velocities;
	const float* radii = physics->radii;
	const float* inverse_masses = physics->inverse_masses;

	/* Each particle is padded by how far it can move this frame, so a fast object
	   only widens the search around itself, not around everything. */
	const float gravity_travel = fabsf(physics->gravity) * delta_seconds * delta_seconds;
	float* travel = physics->grid_travel;
	float max_travel = 0.0f;
	for (int i = 0; i < count; ++i) {
		travel[i] = inverse_masses[i] == 0.0f ? 0.0f :
			vec3fMagnitude(velocities[i]) * delta_seconds + gravity_travel;
		max_travel = __max(max_travel, travel[i]);
	}

	/* The ground needs no grid. */
	for (int i = 0; i < count; ++i) {
		if (inverse_masses[i] == 0.0f) continue;
		if (positions[i].y - radii[i] - physics->ground_height < travel[i]) {
			physicsAddContact(physics, i, PHYSICS_GROUND);
		}
	}

	if (physics->max_radius <= 0.0f) {
		return;
	}
	/* Cells must fit the widest padding of any pair. The cap keeps cells small
	   when something is very fast; such pairs may then be missed. */
	const float margin = __min(2.0f * max_travel, 4.0f * physics->max_radius);
	const float cell_size = 2.0f * physics->max_radius + margin;
	const float inverse_cell_size = 1.0f / cell_size;

	/* Broad phase per soft body: a particle can only touch a particle of another
	   body if it lies within that body's bounds grown by the largest reach, so only
	   those particles enter the grid. Particles without an owner share one extra
	   group, and they and self-colliding bodies always enter. */
	const int group_count = physics->soft_body_count + 1;
	const int loose_group = physics->soft_body_count;
	vec3f_t group_min[PHYSICS_MAX_SOFT_BODIES + 1];
	vec3f_t group_max[PHYSICS_MAX_SOFT_BODIES + 1];
	for (int g = 0; g < group_count; ++g) {
		group_min[g] = (vec3f_t){ FLT_MAX, FLT_MAX, FLT_MAX };
		group_max[g] = (vec3f_t){ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	}
	for (int i = 0; i < count; ++i) {
		const int owner = physics->particle_owner[i];
		const int g = owner >= 0 ? owner : loose_group;
		group_min[g] = vec3fMin(group_min[g], positions[i]);
		group_max[g] = vec3fMax(group_max[g], positions[i]);
	}
	const float reach_max = 2.0f * physics->max_radius + margin;
	for (int g = 0; g < group_count; ++g) {
		const vec3f_t grow = { reach_max, reach_max, reach_max };
		group_min[g] = vec3fSub(group_min[g], grow);
		group_max[g] = vec3fAdd(group_max[g], grow);
	}

	int* candidates = physics->grid_candidates;
	int candidate_count = 0;
	for (int i = 0; i < count; ++i) {
		const int owner = physics->particle_owner[i];
		bool candidate = owner < 0 || physics->soft_bodies[owner].self_collision;
		for (int g = 0; g < group_count && !candidate; ++g) {
			if (g == owner) continue;
			const vec3f_t p = positions[i];
			candidate = p.x >= group_min[g].x && p.x <= group_max[g].x &&
				p.y >= group_min[g].y && p.y <= group_max[g].y &&
				p.z >= group_min[g].z && p.z <= group_max[g].z;
		}
		if (candidate) candidates[candidate_count++] = i;
	}

	int* start = physics->grid_start;
	int* cell = physics->grid_cell;
	memset(start, 0, sizeof(int) * (PHYSICS_GRID_TABLE_SIZE + 1));
	for (int c = 0; c < candidate_count; ++c) {
		const int i = candidates[c];
		cell[i * 3] = (int)floorf(positions[i].x * inverse_cell_size);
		cell[i * 3 + 1] = (int)floorf(positions[i].y * inverse_cell_size);
		cell[i * 3 + 2] = (int)floorf(positions[i].z * inverse_cell_size);
		start[physicsGridHash(cell[i * 3], cell[i * 3 + 1], cell[i * 3 + 2])]++;
	}
	int running = 0;
	for (int h = 0; h < PHYSICS_GRID_TABLE_SIZE; ++h) {
		running += start[h];
		start[h] = running;
	}
	start[PHYSICS_GRID_TABLE_SIZE] = running;
	for (int c = 0; c < candidate_count; ++c) {
		const int i = candidates[c];
		const uint32_t hash = physicsGridHash(cell[i * 3], cell[i * 3 + 1], cell[i * 3 + 2]);
		physics->grid_entries[--start[hash]] = i;
	}

	/* Half-shell search: a particle's own cell plus the 13 neighbours "ahead" of
	   it. Two particles in different cells see each other from exactly one side,
	   so every pair is found once. */
	static const int forward[14][3] = {
		{ 0, 0, 0 },
		{ 1, 0, 0 },
		{ -1, 1, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
		{ -1, -1, 1 }, { 0, -1, 1 }, { 1, -1, 1 },
		{ -1, 0, 1 }, { 0, 0, 1 }, { 1, 0, 1 },
		{ -1, 1, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
	};
	for (int c = 0; c < candidate_count; ++c) {
		const int i = candidates[c];
		const int owner = physics->particle_owner[i];
		const bool self_collision = owner >= 0 && physics->soft_bodies[owner].self_collision;
		for (int n = 0; n < 14; ++n) {
			const int x = cell[i * 3] + forward[n][0];
			const int y = cell[i * 3 + 1] + forward[n][1];
			const int z = cell[i * 3 + 2] + forward[n][2];
			const uint32_t hash = physicsGridHash(x, y, z);
			for (int k = start[hash]; k < start[hash + 1]; ++k) {
				const int j = physics->grid_entries[k];
				/* In the own cell, j > i keeps each pair once. The cell check drops
				   entries that only share the bucket. */
				if ((n == 0 && j <= i) || cell[j * 3] != x || cell[j * 3 + 1] != y ||
					cell[j * 3 + 2] != z) continue;
				if (owner >= 0 && owner == physics->particle_owner[j] && !self_collision) continue;
				if (inverse_masses[i] + inverse_masses[j] == 0.0f) continue;
				const float reach = radii[i] + radii[j] + __min(travel[i] + travel[j], margin);
				if (vec3fMagnitudeSqrd(vec3fSub(positions[i], positions[j])) < reach * reach) {
					physicsAddContact(physics, i, j);
				}
			}
		}
	}
}

/* XPBD inequality constraint C = distance - (r_a + r_b) >= 0, plus static friction
   at the position level: if the tangential slip this substep could be held by
   the normal force, it is undone. */
static void physicsSolveContacts(physics_t* physics, float inverse_dt_squared) {
	vec3f_t* positions = physics->positions;
	const vec3f_t* previous_positions = physics->previous_positions;
	const vec3f_t* velocities = physics->velocities;
	for (int c = 0; c < physics->contact_count; ++c) {
		physics_contact_t* contact = &physics->contacts[c];
		const int a = contact->particle_a;
		const int b = contact->particle_b;
		const bool ground = b == PHYSICS_GROUND;
		const float inverse_mass_a = physics->inverse_masses[a];
		const float inverse_mass_b = ground ? 0.0f : physics->inverse_masses[b];
		const float inverse_mass_sum = inverse_mass_a + inverse_mass_b;
		if (inverse_mass_sum == 0.0f) continue;

		vec3f_t normal;
		float constraint_value;
		float compliance;
		if (!ground) {
			vec3f_t delta = vec3fSub(positions[a], positions[b]);
			const float distance_squared = vec3fMagnitudeSqrd(delta);
			const float minimum_distance = physics->radii[a] + physics->radii[b];
			if (distance_squared >= minimum_distance * minimum_distance) continue;
			const float distance = sqrtf(distance_squared);
			normal = distance > 1.0e-6f ? vec3fScale(delta, 1.0f / distance) : vec3fY();
			constraint_value = distance - minimum_distance;
			compliance = 0.0f;
		}
		else {
			normal = vec3fY();
			constraint_value = positions[a].y - physics->radii[a] - physics->ground_height;
			if (constraint_value >= 0.0f) continue;
			compliance = physics->ground_compliance;
		}

		/* Velocities still hold the predicted values during the position solve,
		   so this is the approach speed restitution is measured against. */
		if (contact->lambda == 0.0f) {
			vec3f_t relative_velocity = ground ? velocities[a] :
				vec3fSub(velocities[a], velocities[b]);
			contact->normal_speed_before = vec3fDot(relative_velocity, normal);
		}

		const float alpha_tilde = compliance * inverse_dt_squared;
		const float delta_lambda = (-constraint_value - alpha_tilde * contact->lambda) /
			(inverse_mass_sum + alpha_tilde);
		contact->lambda += delta_lambda;
		contact->normal = normal;
		positions[a] = vec3fAdd(positions[a], vec3fScale(normal, delta_lambda * inverse_mass_a));
		if (!ground) {
			positions[b] = vec3fSub(positions[b], vec3fScale(normal, delta_lambda * inverse_mass_b));
		}

		vec3f_t slip = vec3fSub(positions[a], previous_positions[a]);
		if (!ground) {
			slip = vec3fSub(slip, vec3fSub(positions[b], previous_positions[b]));
		}
		const vec3f_t tangential_slip = vec3fSub(slip,
			vec3fScale(normal, vec3fDot(slip, normal)));
		const float tangential_lambda = vec3fMagnitude(tangential_slip) / inverse_mass_sum;
		if (tangential_lambda < physics->static_friction * contact->lambda) {
			const vec3f_t correction = vec3fScale(tangential_slip, 1.0f / inverse_mass_sum);
			positions[a] = vec3fSub(positions[a], vec3fScale(correction, inverse_mass_a));
			if (!ground) {
				positions[b] = vec3fAdd(positions[b], vec3fScale(correction, inverse_mass_b));
			}
		}
	}
}

/* Dynamic friction and restitution, applied to contacts that pushed this substep. */
static void physicsSolveContactVelocities(physics_t* physics, float delta_seconds) {
	vec3f_t* velocities = physics->velocities;
	const float rest_speed = 2.0f * fabsf(physics->gravity) * delta_seconds;
	for (int i = 0; i < physics->soft_body_count; ++i) {
		physics->soft_bodies[i].touching = false;
	}
	for (int c = 0; c < physics->contact_count; ++c) {
		physics_contact_t* contact = &physics->contacts[c];
		if (contact->lambda <= 0.0f) continue;
		const int a = contact->particle_a;
		const int b = contact->particle_b;
		const bool ground = b == PHYSICS_GROUND;
		if (physics->particle_owner[a] >= 0) physics->soft_bodies[physics->particle_owner[a]].touching = true;
		if (!ground && physics->particle_owner[b] >= 0) {
			physics->soft_bodies[physics->particle_owner[b]].touching = true;
		}
		const float inverse_mass_a = physics->inverse_masses[a];
		const float inverse_mass_b = ground ? 0.0f : physics->inverse_masses[b];
		const float inverse_mass_sum = inverse_mass_a + inverse_mass_b;
		if (inverse_mass_sum == 0.0f) continue;

		const vec3f_t normal = contact->normal;
		const vec3f_t relative_velocity = ground ? velocities[a] :
			vec3fSub(velocities[a], velocities[b]);
		const float normal_speed = vec3fDot(relative_velocity, normal);
		const vec3f_t tangential_velocity = vec3fSub(relative_velocity,
			vec3fScale(normal, normal_speed));
		const float tangential_speed = vec3fMagnitude(tangential_velocity);

		vec3f_t velocity_change = vec3fZero();
		if (tangential_speed > 1.0e-6f) {
			/* The normal impulse is lambda / h; friction may remove up to mu times
			   that, expressed as a relative velocity change. */
			const float friction_speed = __min(physics->dynamic_friction * contact->lambda *
				inverse_mass_sum / delta_seconds, tangential_speed);
			velocity_change = vec3fScale(tangential_velocity, -friction_speed / tangential_speed);
		}
		/* Slow contacts get no bounce, so resting bodies do not jitter. */
		const float restitution = fabsf(normal_speed) <= rest_speed ? 0.0f : physics->restitution;
		const float target_normal_speed = __max(-restitution * contact->normal_speed_before, 0.0f);
		velocity_change = vec3fAdd(velocity_change,
			vec3fScale(normal, target_normal_speed - normal_speed));

		const vec3f_t impulse = vec3fScale(velocity_change, 1.0f / inverse_mass_sum);
		velocities[a] = vec3fAdd(velocities[a], vec3fScale(impulse, inverse_mass_a));
		if (!ground) {
			velocities[b] = vec3fSub(velocities[b], vec3fScale(impulse, inverse_mass_b));
		}
	}
}

static bool physicsInvert3x3(const float m[9], float out[9]) {
	const float c0 = m[4] * m[8] - m[5] * m[7];
	const float c1 = m[5] * m[6] - m[3] * m[8];
	const float c2 = m[3] * m[7] - m[4] * m[6];
	const float determinant = m[0] * c0 + m[1] * c1 + m[2] * c2;
	if (fabsf(determinant) < 1.0e-12f) {
		return false;
	}
	const float inverse = 1.0f / determinant;
	out[0] = c0 * inverse;
	out[1] = (m[2] * m[7] - m[1] * m[8]) * inverse;
	out[2] = (m[1] * m[5] - m[2] * m[4]) * inverse;
	out[3] = c1 * inverse;
	out[4] = (m[0] * m[8] - m[2] * m[6]) * inverse;
	out[5] = (m[2] * m[3] - m[0] * m[5]) * inverse;
	out[6] = c2 * inverse;
	out[7] = (m[1] * m[6] - m[0] * m[7]) * inverse;
	out[8] = (m[0] * m[4] - m[1] * m[3]) * inverse;
	return true;
}

/* Damps only deformation: each particle's velocity is pulled toward the soft
   body's rigid motion (linear plus angular velocity, Mueller et al. 2007, 3.5),
   so falling, rolling and spinning are left untouched. velocity_retention is
   defined per 1/60 s and rescaled to the substep.

   Bodies in contact also feel rolling resistance: a drag of rolling_resistance
   times the weight, applied as a constant angular deceleration c * g / R (R being
   the radius of gyration). Rolling without slip leaves friction nothing to act
   on, so without this a ball rolls forever; real ones stop through hysteresis in
   the contact patch. */
static void physicsDampenDeformation(physics_t* physics, float delta_seconds) {
	for (int body_index = 0; body_index < physics->soft_body_count; ++body_index) {
		physics_soft_body_t* body = &physics->soft_bodies[body_index];
		const bool rolling = body->touching && physics->rolling_resistance > 0.0f;
		if (body->drag > 0.0f) {
			const float keep = __max(0.0f, 1.0f - body->drag * delta_seconds);
			vec3f_t* body_velocities = physics->velocities + body->first_particle;
			for (int i = 0; i < body->particle_count; ++i) {
				body_velocities[i] = vec3fScale(body_velocities[i], keep);
			}
		}
		if ((body->velocity_retention >= 1.0f && !rolling) || body->total_mass <= 0.0f) continue;
		const float retention = powf(body->velocity_retention, delta_seconds * 60.0f);
		const vec3f_t* positions = physics->positions + body->first_particle;
		vec3f_t* velocities = physics->velocities + body->first_particle;
		const float* inverse_masses = physics->inverse_masses + body->first_particle;

		vec3f_t center = vec3fZero();
		vec3f_t center_velocity = vec3fZero();
		for (int i = 0; i < body->particle_count; ++i) {
			if (inverse_masses[i] <= 0.0f) continue;
			const float mass = 1.0f / inverse_masses[i];
			center = vec3fAdd(center, vec3fScale(positions[i], mass));
			center_velocity = vec3fAdd(center_velocity, vec3fScale(velocities[i], mass));
		}
		center = vec3fScale(center, 1.0f / body->total_mass);
		center_velocity = vec3fScale(center_velocity, 1.0f / body->total_mass);

		vec3f_t angular_momentum = vec3fZero();
		float inertia[9] = { 0 };
		for (int i = 0; i < body->particle_count; ++i) {
			if (inverse_masses[i] <= 0.0f) continue;
			const float mass = 1.0f / inverse_masses[i];
			const vec3f_t r = vec3fSub(positions[i], center);
			angular_momentum = vec3fAdd(angular_momentum,
				vec3fScale(vec3fCross(r, velocities[i]), mass));
			const float r_squared = vec3fMagnitudeSqrd(r);
			inertia[0] += mass * (r_squared - r.x * r.x);
			inertia[4] += mass * (r_squared - r.y * r.y);
			inertia[8] += mass * (r_squared - r.z * r.z);
			inertia[1] -= mass * r.x * r.y;
			inertia[2] -= mass * r.x * r.z;
			inertia[5] -= mass * r.y * r.z;
		}
		inertia[3] = inertia[1];
		inertia[6] = inertia[2];
		inertia[7] = inertia[5];
		float spin_retention = 1.0f;
		float inverse_inertia[9];
		vec3f_t angular_velocity = vec3fZero();
		if (physicsInvert3x3(inertia, inverse_inertia)) {
			angular_velocity = (vec3f_t){
				inverse_inertia[0] * angular_momentum.x + inverse_inertia[1] * angular_momentum.y +
					inverse_inertia[2] * angular_momentum.z,
				inverse_inertia[3] * angular_momentum.x + inverse_inertia[4] * angular_momentum.y +
					inverse_inertia[5] * angular_momentum.z,
				inverse_inertia[6] * angular_momentum.x + inverse_inertia[7] * angular_momentum.y +
					inverse_inertia[8] * angular_momentum.z
			};
			const float spin_speed = vec3fMagnitude(angular_velocity);
			if (rolling && spin_speed > 0.0f) {
				const float gyration_radius = sqrtf((inertia[0] + inertia[4] + inertia[8]) /
					(2.0f * body->total_mass));
				const float slowdown = physics->rolling_resistance * fabsf(physics->gravity) *
					delta_seconds / __max(gyration_radius, 1.0e-3f);
				spin_retention = __max(0.0f, 1.0f - slowdown / spin_speed);
			}
		}

		for (int i = 0; i < body->particle_count; ++i) {
			if (inverse_masses[i] <= 0.0f) continue;
			const vec3f_t r = vec3fSub(positions[i], center);
			const vec3f_t spin = vec3fCross(angular_velocity, r);
			const vec3f_t rigid_velocity = vec3fAdd(center_velocity, spin);
			const vec3f_t deformation = vec3fSub(velocities[i], rigid_velocity);
			velocities[i] = vec3fAdd(vec3fAdd(center_velocity, vec3fScale(spin, spin_retention)),
				vec3fScale(deformation, retention));
		}
	}
}


/* Pulls every grabbed particle toward the mouse target plus the offset it had when
   grabbed, as a soft XPBD attachment so a fast mouse drags rather than teleports. */
static void physicsSolveGrab(physics_t* physics, float inverse_dt_squared) {
	const float alpha_tilde = PHYSICS_GRAB_COMPLIANCE * inverse_dt_squared;
	for (int g = 0; g < physics->grabbed_count; ++g) {
		const physics_particle_id_t particle = physics->grabbed_particles[g];
		const float inverse_mass = physics->inverse_masses[particle];
		if (inverse_mass == 0.0f) continue;
		const vec3f_t target = vec3fAdd(physics->grab_target, physics->grabbed_offsets[g]);
		const vec3f_t delta = vec3fSub(physics->positions[particle], target);
		const float distance = vec3fMagnitude(delta);
		if (distance < 1.0e-6f) continue;
		const vec3f_t gradient = vec3fScale(delta, 1.0f / distance);
		const float delta_lambda = (-distance - alpha_tilde * physics->grabbed_lambdas[g]) /
			(inverse_mass + alpha_tilde);
		physics->grabbed_lambdas[g] += delta_lambda;
		physics->positions[particle] = vec3fAdd(physics->positions[particle],
			vec3fScale(gradient, inverse_mass * delta_lambda));
	}
}


physics_t* physicsCreate(heap_t* heap) {
	physics_t* phys = heapAlloc(heap, sizeof(physics_t), 8);
	memset(phys, 0, sizeof(*phys));
	phys->heap = heap;
	phys->positions = heapAlloc(heap, sizeof(vec3f_t) * PHYSICS_MAX_PARTICLES, _Alignof(vec3f_t));
	phys->previous_positions = heapAlloc(heap, sizeof(vec3f_t) * PHYSICS_MAX_PARTICLES,
		_Alignof(vec3f_t));
	phys->velocities = heapAlloc(heap, sizeof(vec3f_t) * PHYSICS_MAX_PARTICLES, _Alignof(vec3f_t));
	phys->inverse_masses = heapAlloc(heap, sizeof(float) * PHYSICS_MAX_PARTICLES, _Alignof(float));
	phys->radii = heapAlloc(heap, sizeof(float) * PHYSICS_MAX_PARTICLES, _Alignof(float));
	phys->particle_owner = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_PARTICLES, _Alignof(int));
	phys->distance_constraints = heapAlloc(heap,
		sizeof(physics_distance_constraint_t) * PHYSICS_MAX_DISTANCE_CONSTRAINTS,
		_Alignof(physics_distance_constraint_t));
	phys->soft_bodies = heapAlloc(heap, sizeof(physics_soft_body_t) * PHYSICS_MAX_SOFT_BODIES,
		_Alignof(physics_soft_body_t));
	phys->contacts = heapAlloc(heap, sizeof(physics_contact_t) * PHYSICS_MAX_CONTACTS,
		_Alignof(physics_contact_t));
	phys->grid_start = heapAlloc(heap, sizeof(int) * (PHYSICS_GRID_TABLE_SIZE + 1),
		_Alignof(int));
	phys->grid_entries = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_PARTICLES, _Alignof(int));
	phys->grid_cell = heapAlloc(heap, sizeof(int) * 3 * PHYSICS_MAX_PARTICLES, _Alignof(int));
	phys->grid_candidates = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_PARTICLES, _Alignof(int));
	phys->grid_travel = heapAlloc(heap, sizeof(float) * PHYSICS_MAX_PARTICLES, _Alignof(float));
	phys->gravity = -9.81f;
	phys->substeps = 8;
	phys->solver_iterations = 1;
	phys->static_friction = 0.6f;
	phys->dynamic_friction = 0.4f;
	phys->restitution = 0.2f;
	phys->rolling_resistance = 0.15f;
	phys->parallel_worker_count = 1;
	for (int i = 0; i < PHYSICS_MAX_WORKERS; ++i) {
		phys->workers[i].physics = phys;
		phys->workers[i].index = i;
	}
	/* Leave cores for the render and file threads; worker 0 is the caller. */
	phys->worker_count = __max(1, __min(threadGetProcessorCount() - 2, PHYSICS_MAX_WORKERS));
	if (phys->worker_count > 1) {
		phys->worker_start = semaphoreCreate(0, PHYSICS_MAX_WORKERS);
		phys->worker_done = semaphoreCreate(0, PHYSICS_MAX_WORKERS);
		bool worker_failed = phys->worker_start == NULL || phys->worker_done == NULL;
		for (int i = 1; i < phys->worker_count && !worker_failed; ++i) {
			phys->worker_threads[i - 1] = threadCreate(physicsWorkerFunc, &phys->workers[i]);
			worker_failed = phys->worker_threads[i - 1] == NULL;
		}
		if (worker_failed) {
			physicsDestroyWorkers(phys);
			phys->worker_count = 1;
		}
	}

	return phys;
}

void physicsDestroy(physics_t* physics) {
	if (physics) {
		physicsDestroyWorkers(physics);
		for (int i = 0; i < physics->soft_body_count; ++i) {
			if (physics->soft_bodies[i].has_volume) {
				heapFree(physics->heap, physics->soft_bodies[i].indices);
				heapFree(physics->heap, physics->soft_bodies[i].gradients);
			}
		}
		heapFree(physics->heap, physics->grid_travel);
		heapFree(physics->heap, physics->grid_candidates);
		heapFree(physics->heap, physics->grid_cell);
		heapFree(physics->heap, physics->grid_entries);
		heapFree(physics->heap, physics->grid_start);
		heapFree(physics->heap, physics->contacts);
		heapFree(physics->heap, physics->soft_bodies);
		heapFree(physics->heap, physics->distance_constraints);
		heapFree(physics->heap, physics->particle_owner);
		heapFree(physics->heap, physics->radii);
		heapFree(physics->heap, physics->inverse_masses);
		heapFree(physics->heap, physics->velocities);
		heapFree(physics->heap, physics->previous_positions);
		heapFree(physics->heap, physics->positions);
		heapFree(physics->heap, physics);
	}
}

physics_particle_id_t physicsAddParticle(physics_t* physics, vec3f_t position,
	float radius, float mass) {
	assert(physics->particle_count < PHYSICS_MAX_PARTICLES);
	const physics_particle_id_t id = (physics_particle_id_t)physics->particle_count++;
	physics->positions[id] = position;
	physics->previous_positions[id] = position;
	physics->velocities[id] = vec3fZero();
	physics->radii[id] = __max(0.0f, radius);
	physics->inverse_masses[id] = mass > 0.0f ? 1.0f / mass : 0.0f;
	physics->particle_owner[id] = -1;
	physics->max_radius = __max(physics->max_radius, physics->radii[id]);
	return id;
}

vec3f_t physicsGetParticlePosition(physics_t* physics, physics_particle_id_t particle) {
	return physics->positions[particle];
}

vec3f_t physicsGetParticleVelocity(physics_t* physics, physics_particle_id_t particle) {
	return physics->velocities[particle];
}

void physicsSetParticleVelocity(physics_t* physics, physics_particle_id_t particle,
	vec3f_t velocity) {
	physics->velocities[particle] = velocity;
}

float physicsGetParticleRadius(physics_t* physics, physics_particle_id_t particle) {
	return physics->radii[particle];
}

const vec3f_t* physicsGetParticlePositions(physics_t* physics) {
	return physics->positions;
}

void physicsAddDistanceConstraint(physics_t* physics, physics_particle_id_t particle_a,
	physics_particle_id_t particle_b, float rest_length, float compliance) {
	assert(physics->distance_constraint_count < PHYSICS_MAX_DISTANCE_CONSTRAINTS);
	assert((int)particle_a < physics->particle_count && (int)particle_b < physics->particle_count);
	physics_distance_constraint_t* constraint =
		&physics->distance_constraints[physics->distance_constraint_count++];
	constraint->particle_a = particle_a;
	constraint->particle_b = particle_b;
	constraint->rest_length = rest_length;
	constraint->compliance = __max(0.0f, compliance);
	constraint->lambda = 0.0f;
	physics->distance_islands_dirty = true;
}

int physicsAddSoftBody(physics_t* physics, physics_particle_id_t first_particle,
	int particle_count, bool self_collision) {
	assert(physics->soft_body_count < PHYSICS_MAX_SOFT_BODIES);
	assert((int)first_particle + particle_count <= physics->particle_count);
	const int index = physics->soft_body_count++;
	physics_soft_body_t* body = &physics->soft_bodies[index];
	memset(body, 0, sizeof(*body));
	body->first_particle = first_particle;
	body->particle_count = particle_count;
	body->self_collision = self_collision;
	body->velocity_retention = 1.0f;
	for (int i = 0; i < particle_count; ++i) {
		const float inverse_mass = physics->inverse_masses[first_particle + i];
		if (inverse_mass > 0.0f)
			body->total_mass += 1.0f / inverse_mass;
		int* particle_owner = &physics->particle_owner[first_particle + i];
		if (*particle_owner >= 0) {
			physics->soft_bodies_overlap = true;
		}
		else {
			*particle_owner = index;
		}
	}
	physics->distance_islands_dirty = true;
	return index;
}

void physicsSetSoftBodyDamping(physics_t* physics, int soft_body, float retention) {
	physics->soft_bodies[soft_body].velocity_retention = __max(0.0f, __min(retention, 1.0f));
}

void physicsSetSoftBodyDrag(physics_t* physics, int soft_body, float per_second) {
	physics->soft_bodies[soft_body].drag = __max(0.0f, per_second);
}

void physicsSetSoftBodyVolume(physics_t* physics, int soft_body, const uint16_t* indices,
	int index_count, float compliance) {
	assert(index_count <= PHYSICS_MAX_VOLUME_INDICES);
	physics_soft_body_t* body = &physics->soft_bodies[soft_body];
	assert(!body->has_volume);
	body->indices = heapAlloc(physics->heap, sizeof(uint16_t) * index_count, _Alignof(uint16_t));
	for (int i = 0; i < index_count; ++i) body->indices[i] = indices[i];
	body->gradients = heapAlloc(physics->heap, sizeof(vec3f_t) * body->particle_count,
		_Alignof(vec3f_t));
	body->index_count = index_count;
	body->volume_compliance = __max(0.0f, compliance);
	body->volume_lambda = 0.0f;
	body->rest_volume = physicsCalculateVolume(physics, body);
	body->has_volume = true;
}

bool physicsPickParticle(physics_t* physics, vec3f_t origin, vec3f_t direction,
	physics_particle_id_t* particle, float* distance) {
	direction = vec3fNorm(direction);
	float best = FLT_MAX;
	int best_particle = -1;
	for (int i = 0; i < physics->particle_count; ++i) {
		if (physics->inverse_masses[i] == 0.0f) continue;
		const vec3f_t to_particle = vec3fSub(physics->positions[i], origin);
		const float along = vec3fDot(to_particle, direction);
		if (along <= 0.0f || along >= best) continue;
		/* Twice the radius, so the ray cannot slip between neighbouring particles. */
		const float reach = 2.0f * physics->radii[i];
		const float off_ray_squared = vec3fMagnitudeSqrd(to_particle) - along * along;
		if (off_ray_squared < reach * reach) {
			best = along;
			best_particle = i;
		}
	}
	if (best_particle < 0) {
		return false;
	}
	*particle = (physics_particle_id_t)best_particle;
	*distance = best;
	return true;
}

void physicsGrab(physics_t* physics, physics_particle_id_t particle, vec3f_t target,
	float radius) {
	physics->grabbed_count = 0;
	physics->grab_target = target;
	const int owner = physics->particle_owner[particle];
	const vec3f_t center = physics->positions[particle];
	int first = particle;
	int end = particle + 1;
	if (owner >= 0) {
		first = physics->soft_bodies[owner].first_particle;
		end = first + physics->soft_bodies[owner].particle_count;
	}
	for (int i = first; i < end && physics->grabbed_count < PHYSICS_MAX_GRABBED_PARTICLES; ++i) {
		if (i != (int)particle &&
			vec3fMagnitudeSqrd(vec3fSub(physics->positions[i], center)) > radius * radius) continue;
		physics->grabbed_particles[physics->grabbed_count] = (physics_particle_id_t)i;
		physics->grabbed_offsets[physics->grabbed_count] =
			vec3fSub(physics->positions[i], target);
		physics->grabbed_lambdas[physics->grabbed_count] = 0.0f;
		physics->grabbed_count++;
	}
}

void physicsMoveGrab(physics_t* physics, vec3f_t target) {
	physics->grab_target = target;
}

void physicsReleaseGrab(physics_t* physics) {
	physics->grabbed_count = 0;
}

bool physicsIsGrabbing(physics_t* physics) {
	return physics->grabbed_count > 0;
}

void physicsSetGroundHeight(physics_t* physics, float height) {
	physics->ground_height = height;
}

void physicsSetGroundCompliance(physics_t* physics, float compliance) {
	physics->ground_compliance = __max(0.0f, compliance);
}

void physicsSetGravity(physics_t* physics, float gravity_meters_per_second_squared) {
	physics->gravity = gravity_meters_per_second_squared;
}

void physicsSetSubsteps(physics_t* physics, int substeps) {
	physics->substeps = __max(1, substeps);
}

void physicsSetSolverIterations(physics_t* physics, int iterations) {
	physics->solver_iterations = __max(1, iterations);
}

void physicsSetFriction(physics_t* physics, float static_coefficient,
	float dynamic_coefficient) {
	physics->static_friction = __max(0.0f, static_coefficient);
	physics->dynamic_friction = __max(0.0f, dynamic_coefficient);
}

void physicsSetRestitution(physics_t* physics, float restitution) {
	physics->restitution = __max(0.0f, __min(restitution, 1.0f));
}

void physicsSetRollingResistance(physics_t* physics, float coefficient) {
	physics->rolling_resistance = __max(0.0f, coefficient);
}

bool physicsHasCapacity(physics_t* physics, int particle_count,
	int distance_constraint_count, int soft_body_count) {
	return physics->particle_count + particle_count <= PHYSICS_MAX_PARTICLES &&
		physics->distance_constraint_count + distance_constraint_count <=
			PHYSICS_MAX_DISTANCE_CONSTRAINTS &&
		physics->soft_body_count + soft_body_count <= PHYSICS_MAX_SOFT_BODIES;
}

physics_stats_t physicsGetStats(physics_t* physics) {
	return (physics_stats_t){
		.particle_count = physics->particle_count,
		.distance_constraint_count = physics->distance_constraint_count,
		.soft_body_count = physics->soft_body_count,
		.contact_count = physics->contact_count,
		.grabbed_count = physics->grabbed_count
	};
}

void physicsUpdate(physics_t* physics, float delta_seconds) {
	if (delta_seconds <= 0.0f) {
		return;
	}
	if (physics->distance_islands_dirty) {
		physicsBuildDistanceIslands(physics);
	}
	physicsDetectContacts(physics, delta_seconds);

	const float substep_seconds = delta_seconds / (float)physics->substeps;
	const float inverse_dt_squared = 1.0f / (substep_seconds * substep_seconds);
	const int island_count = physics->soft_body_count;
	for (int substep = 0; substep < physics->substeps; ++substep) {
		physicsParallelFor(physics, physics->particle_count, PHYSICS_MIN_PARTICLE_WORK_PER_THREAD,
			physicsPredictRange, substep_seconds, 0.0f);
		physicsResetLambdas(physics);

		for (int iteration = 0; iteration < physics->solver_iterations; ++iteration) {
			/* Constraints that span soft bodies (the last island) go first, serially. */
			physicsSolveDistanceIsland(physics, island_count, inverse_dt_squared);
			if (physics->soft_bodies_overlap) {
				physicsSolveIslandsRange(physics, 0, island_count, 0.0f, inverse_dt_squared);
			}
			else {
				physicsParallelFor(physics, island_count, 1,
					physicsSolveIslandsRange, 0.0f, inverse_dt_squared);
			}
			physicsSolveContacts(physics, inverse_dt_squared);
			physicsSolveGrab(physics, inverse_dt_squared);
		}

		physicsParallelFor(physics, physics->particle_count, PHYSICS_MIN_PARTICLE_WORK_PER_THREAD,
			physicsReconstructVelocitiesRange, substep_seconds, 0.0f);
		/* Per substep, not per frame: an impact lasts only a few substeps, and
		   damping once per frame lets most of its energy bounce back. */
		physicsSolveContactVelocities(physics, substep_seconds);
		physicsDampenDeformation(physics, substep_seconds);
	}
}
