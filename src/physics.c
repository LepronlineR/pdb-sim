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
	PHYSICS_MAX_VOLUME_CONSTRAINTS = 16,
	PHYSICS_MAX_VOLUME_INDICES = 1536,
	PHYSICS_MAX_CONTACTS = 65536,
	PHYSICS_GRID_TABLE_SIZE = 16384, /* power of two, >= 2 * PHYSICS_MAX_PARTICLES */
	PHYSICS_MAX_WORKERS = 8,
	PHYSICS_MIN_PARTICLE_WORK_PER_THREAD = 1024
};

#define PHYSICS_GROUND (-1)

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

struct physics_volume_constraint_t {
	physics_particle_id_t first_particle;
	int particle_count;
	uint16_t* indices;
	vec3f_t* gradients;
	int index_count;
	float total_mass;
	float rest_volume;
	float compliance;
	float velocity_retention;
	float lambda;
};

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
	/* Index of the volume constraint that first claimed each particle, or -1. */
	int* particle_owner;
	float max_radius;
	/* Kept sorted by island once built: island i is
	   [distance_island_offsets[i], distance_island_offsets[i + 1]), and the last
	   island holds constraints that span soft bodies or belong to none. */
	physics_distance_constraint_t* distance_constraints;
	int distance_constraint_count;
	int distance_island_offsets[PHYSICS_MAX_VOLUME_CONSTRAINTS + 2];
	bool distance_islands_dirty;
	physics_volume_constraint_t* volume_constraints;
	int volume_constraint_count;
	bool volume_particles_shared;
	physics_contact_t* contacts;
	int contact_count;
	bool contact_overflow_reported;
	/* Spatial hash over all particles, built with a counting sort: the particles
	   in bucket h are grid_entries[grid_start[h] .. grid_start[h + 1]). */
	int* grid_start;
	int* grid_entries;
	int* grid_cell;
	int substeps;
	int solver_iterations;
	float gravity;
	float ground_height;
	float ground_compliance;
	float static_friction;
	float dynamic_friction;
	float restitution;
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
static float physicsCalculateVolume(physics_t* physics,
	const physics_volume_constraint_t* constraint);
static void physicsSolveVolumeConstraint(physics_t* physics,
	physics_volume_constraint_t* constraint, float inverse_dt_squared);
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
		physicsSolveVolumeConstraint(physics, &physics->volume_constraints[i],
			inverse_dt_squared);
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
	for (int i = 0; i < physics->volume_constraint_count; ++i) {
		physics->volume_constraints[i].lambda = 0.0f;
	}
	for (int i = 0; i < physics->contact_count; ++i) {
		physics->contacts[i].lambda = 0.0f;
	}
}

static int physicsDistanceIsland(physics_t* physics,
	const physics_distance_constraint_t* constraint) {
	const int owner_a = physics->particle_owner[constraint->particle_a];
	const int owner_b = physics->particle_owner[constraint->particle_b];
	return (owner_a >= 0 && owner_a == owner_b && !physics->volume_particles_shared) ?
		owner_a : physics->volume_constraint_count;
}

/* Stable counting sort of the distance constraints by owning soft body, so each
   island is contiguous and keeps the original Gauss-Seidel order. Runs only
   after constraints are added. */
static void physicsBuildDistanceIslands(physics_t* physics) {
	const int island_count = physics->volume_constraint_count;
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
	int cursor[PHYSICS_MAX_VOLUME_CONSTRAINTS + 1];
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

static float physicsCalculateVolume(physics_t* physics,
	const physics_volume_constraint_t* constraint) {
	const vec3f_t* positions = physics->positions + constraint->first_particle;
	float volume = 0.0f;
	for (int i = 0; i < constraint->index_count; i += 3) {
		vec3f_t a = positions[constraint->indices[i]];
		vec3f_t b = positions[constraint->indices[i + 1]];
		vec3f_t c = positions[constraint->indices[i + 2]];
		volume += vec3fDot(a, vec3fCross(b, c)) / 6.0f;
	}
	return volume;
}

static void physicsSolveVolumeConstraint(physics_t* physics,
	physics_volume_constraint_t* constraint, float inverse_dt_squared) {
	vec3f_t* positions = physics->positions + constraint->first_particle;
	const float* inverse_masses = physics->inverse_masses + constraint->first_particle;
	vec3f_t* gradients = constraint->gradients;
	for (int i = 0; i < constraint->particle_count; ++i) gradients[i] = vec3fZero();
	float volume = 0.0f;
	for (int i = 0; i < constraint->index_count; i += 3) {
		uint16_t ia = constraint->indices[i];
		uint16_t ib = constraint->indices[i + 1];
		uint16_t ic = constraint->indices[i + 2];
		vec3f_t a = positions[ia];
		vec3f_t b = positions[ib];
		vec3f_t c = positions[ic];
		volume += vec3fDot(a, vec3fCross(b, c)) / 6.0f;
		gradients[ia] = vec3fAdd(gradients[ia], vec3fScale(vec3fCross(b, c), 1.0f / 6.0f));
		gradients[ib] = vec3fAdd(gradients[ib], vec3fScale(vec3fCross(c, a), 1.0f / 6.0f));
		gradients[ic] = vec3fAdd(gradients[ic], vec3fScale(vec3fCross(a, b), 1.0f / 6.0f));
	}

	float denominator = 0.0f;
	for (int i = 0; i < constraint->particle_count; ++i) {
		denominator += inverse_masses[i] * vec3fMagnitudeSqrd(gradients[i]);
	}
	const float alpha_tilde = constraint->compliance * inverse_dt_squared;
	if (denominator + alpha_tilde <= 1.0e-8f) {
		return;
	}
	const float constraint_value = volume - constraint->rest_volume;
	const float delta_lambda = (-constraint_value - alpha_tilde * constraint->lambda) /
		(denominator + alpha_tilde);
	constraint->lambda += delta_lambda;
	for (int i = 0; i < constraint->particle_count; ++i) {
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
   substeps only solve a fixed list. Pairs inside one soft body are skipped:
   its distance and volume constraints already keep its particles apart. */
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

	float max_speed = 0.0f;
	for (int i = 0; i < count; ++i) {
		max_speed = __max(max_speed, vec3fMagnitude(velocities[i]));
	}
	const float gravity_travel = fabsf(physics->gravity) * delta_seconds * delta_seconds;

	/* The ground needs no grid: pad each particle by its own possible travel. */
	for (int i = 0; i < count; ++i) {
		if (inverse_masses[i] == 0.0f) continue;
		const float margin = vec3fMagnitude(velocities[i]) * delta_seconds + gravity_travel;
		if (positions[i].y - radii[i] - physics->ground_height < margin) {
			physicsAddContact(physics, i, PHYSICS_GROUND);
		}
	}

	if (physics->max_radius <= 0.0f) {
		return;
	}
	/* Two particles close at up to twice the fastest speed. The cap keeps cells
	   small when something is very fast; such pairs may then be missed. */
	const float margin = __min(2.0f * max_speed * delta_seconds + gravity_travel,
		4.0f * physics->max_radius);
	const float cell_size = 2.0f * physics->max_radius + margin;
	const float inverse_cell_size = 1.0f / cell_size;

	int* start = physics->grid_start;
	int* cell = physics->grid_cell;
	memset(start, 0, sizeof(int) * (PHYSICS_GRID_TABLE_SIZE + 1));
	for (int i = 0; i < count; ++i) {
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
	for (int i = 0; i < count; ++i) {
		const uint32_t hash = physicsGridHash(cell[i * 3], cell[i * 3 + 1], cell[i * 3 + 2]);
		physics->grid_entries[--start[hash]] = i;
	}

	for (int i = 0; i < count; ++i) {
		const int owner = physics->particle_owner[i];
		for (int z = cell[i * 3 + 2] - 1; z <= cell[i * 3 + 2] + 1; ++z) {
		for (int y = cell[i * 3 + 1] - 1; y <= cell[i * 3 + 1] + 1; ++y) {
		for (int x = cell[i * 3] - 1; x <= cell[i * 3] + 1; ++x) {
			const uint32_t hash = physicsGridHash(x, y, z);
			for (int k = start[hash]; k < start[hash + 1]; ++k) {
				const int j = physics->grid_entries[k];
				/* j > i visits each pair once; the cell check drops entries that
				   only share the bucket, which would otherwise be found twice. */
				if (j <= i || cell[j * 3] != x || cell[j * 3 + 1] != y ||
					cell[j * 3 + 2] != z) continue;
				if (owner >= 0 && owner == physics->particle_owner[j]) continue;
				if (inverse_masses[i] + inverse_masses[j] == 0.0f) continue;
				const float reach = radii[i] + radii[j] + margin;
				if (vec3fMagnitudeSqrd(vec3fSub(positions[i], positions[j])) < reach * reach) {
					physicsAddContact(physics, i, j);
				}
			}
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
	for (int c = 0; c < physics->contact_count; ++c) {
		physics_contact_t* contact = &physics->contacts[c];
		if (contact->lambda <= 0.0f) continue;
		const int a = contact->particle_a;
		const int b = contact->particle_b;
		const bool ground = b == PHYSICS_GROUND;
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
   defined per 1/60 s and rescaled to the substep. */
static void physicsDampenDeformation(physics_t* physics, float delta_seconds) {
	for (int constraint_idx = 0; constraint_idx < physics->volume_constraint_count;
		++constraint_idx) {
		physics_volume_constraint_t* constraint = &physics->volume_constraints[constraint_idx];
		if (constraint->velocity_retention >= 1.0f || constraint->total_mass <= 0.0f) continue;
		const float retention = powf(constraint->velocity_retention, delta_seconds * 60.0f);
		const vec3f_t* positions = physics->positions + constraint->first_particle;
		vec3f_t* velocities = physics->velocities + constraint->first_particle;
		const float* inverse_masses = physics->inverse_masses + constraint->first_particle;

		vec3f_t center = vec3fZero();
		vec3f_t center_velocity = vec3fZero();
		for (int i = 0; i < constraint->particle_count; ++i) {
			if (inverse_masses[i] <= 0.0f) continue;
			const float mass = 1.0f / inverse_masses[i];
			center = vec3fAdd(center, vec3fScale(positions[i], mass));
			center_velocity = vec3fAdd(center_velocity, vec3fScale(velocities[i], mass));
		}
		center = vec3fScale(center, 1.0f / constraint->total_mass);
		center_velocity = vec3fScale(center_velocity, 1.0f / constraint->total_mass);

		vec3f_t angular_momentum = vec3fZero();
		float inertia[9] = { 0 };
		for (int i = 0; i < constraint->particle_count; ++i) {
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
		}

		for (int i = 0; i < constraint->particle_count; ++i) {
			if (inverse_masses[i] <= 0.0f) continue;
			const vec3f_t r = vec3fSub(positions[i], center);
			const vec3f_t rigid_velocity = vec3fAdd(center_velocity,
				vec3fCross(angular_velocity, r));
			const vec3f_t deformation = vec3fSub(velocities[i], rigid_velocity);
			velocities[i] = vec3fAdd(rigid_velocity, vec3fScale(deformation, retention));
		}
	}
}


physics_t* physicsCreate(heap_t* heap) {
	physics_t* phys = heapAlloc(heap, sizeof(physics_t), 8);
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
	phys->volume_constraints = heapAlloc(heap,
		sizeof(physics_volume_constraint_t) * PHYSICS_MAX_VOLUME_CONSTRAINTS,
		_Alignof(physics_volume_constraint_t));
	phys->contacts = heapAlloc(heap, sizeof(physics_contact_t) * PHYSICS_MAX_CONTACTS,
		_Alignof(physics_contact_t));
	phys->grid_start = heapAlloc(heap, sizeof(int) * (PHYSICS_GRID_TABLE_SIZE + 1),
		_Alignof(int));
	phys->grid_entries = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_PARTICLES, _Alignof(int));
	phys->grid_cell = heapAlloc(heap, sizeof(int) * 3 * PHYSICS_MAX_PARTICLES, _Alignof(int));
	for (int i = 0; i < PHYSICS_MAX_VOLUME_CONSTRAINTS + 2; ++i) {
		phys->distance_island_offsets[i] = 0;
	}
	phys->particle_count = 0;
	phys->max_radius = 0.0f;
	phys->distance_constraint_count = 0;
	phys->distance_islands_dirty = false;
	phys->volume_constraint_count = 0;
	phys->volume_particles_shared = false;
	phys->contact_count = 0;
	phys->contact_overflow_reported = false;
	phys->gravity = -9.81f;
	phys->ground_height = 0.0f;
	phys->ground_compliance = 0.0f;
	phys->substeps = 8;
	phys->solver_iterations = 1;
	phys->static_friction = 0.6f;
	phys->dynamic_friction = 0.4f;
	phys->restitution = 0.2f;
	phys->parallel_worker_count = 1;
	phys->parallel_count = 0;
	phys->parallel_next_chunk = 0;
	phys->parallel_delta_seconds = 0.0f;
	phys->parallel_inverse_dt_squared = 0.0f;
	phys->parallel_func = NULL;
	phys->worker_shutdown = false;
	phys->worker_start = NULL;
	phys->worker_done = NULL;
	for (int i = 0; i < PHYSICS_MAX_WORKERS - 1; ++i) {
		phys->worker_threads[i] = NULL;
	}
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
		for (int i = 0; i < physics->volume_constraint_count; ++i) {
			heapFree(physics->heap, physics->volume_constraints[i].indices);
			heapFree(physics->heap, physics->volume_constraints[i].gradients);
		}
		heapFree(physics->heap, physics->grid_cell);
		heapFree(physics->heap, physics->grid_entries);
		heapFree(physics->heap, physics->grid_start);
		heapFree(physics->heap, physics->contacts);
		heapFree(physics->heap, physics->volume_constraints);
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

physics_volume_constraint_t* physicsAddVolumeConstraint(physics_t* physics,
	physics_particle_id_t first_particle, int particle_count, const uint16_t* indices,
	int index_count, float compliance) {
	assert(physics->volume_constraint_count < PHYSICS_MAX_VOLUME_CONSTRAINTS);
	assert(index_count <= PHYSICS_MAX_VOLUME_INDICES);
	assert((int)first_particle + particle_count <= physics->particle_count);
	physics_volume_constraint_t* constraint =
		&physics->volume_constraints[physics->volume_constraint_count++];
	constraint->first_particle = first_particle;
	constraint->particle_count = particle_count;
	constraint->indices = heapAlloc(physics->heap, sizeof(uint16_t) * index_count,
		_Alignof(uint16_t));
	for (int i = 0; i < index_count; ++i) constraint->indices[i] = indices[i];
	constraint->gradients = heapAlloc(physics->heap, sizeof(vec3f_t) * particle_count,
		_Alignof(vec3f_t));
	constraint->index_count = index_count;
	constraint->compliance = __max(0.0f, compliance);
	constraint->velocity_retention = 1.0f;
	constraint->lambda = 0.0f;
	constraint->rest_volume = physicsCalculateVolume(physics, constraint);
	constraint->total_mass = 0.0f;
	for (int i = 0; i < particle_count; ++i) {
		const float inverse_mass = physics->inverse_masses[first_particle + i];
		if (inverse_mass > 0.0f)
			constraint->total_mass += 1.0f / inverse_mass;
	}

	const int owner = physics->volume_constraint_count - 1;
	for (int i = 0; i < particle_count; ++i) {
		int* particle_owner = &physics->particle_owner[first_particle + i];
		if (*particle_owner >= 0 && *particle_owner != owner) {
			physics->volume_particles_shared = true;
		}
		else {
			*particle_owner = owner;
		}
	}
	physics->distance_islands_dirty = true;
	return constraint;
}

void physicsSetVolumeDamping(physics_volume_constraint_t* constraint, float retention) {
	constraint->velocity_retention = __max(0.0f, __min(retention, 1.0f));
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

bool physicsHasCapacity(physics_t* physics, int particle_count,
	int distance_constraint_count, int volume_constraint_count) {
	return physics->particle_count + particle_count <= PHYSICS_MAX_PARTICLES &&
		physics->distance_constraint_count + distance_constraint_count <=
			PHYSICS_MAX_DISTANCE_CONSTRAINTS &&
		physics->volume_constraint_count + volume_constraint_count <=
			PHYSICS_MAX_VOLUME_CONSTRAINTS;
}

int physicsGetContactCount(physics_t* physics) {
	return physics->contact_count;
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
	const int island_count = physics->volume_constraint_count;
	for (int substep = 0; substep < physics->substeps; ++substep) {
		physicsParallelFor(physics, physics->particle_count, PHYSICS_MIN_PARTICLE_WORK_PER_THREAD,
			physicsPredictRange, substep_seconds, 0.0f);
		physicsResetLambdas(physics);

		for (int iteration = 0; iteration < physics->solver_iterations; ++iteration) {
			/* Constraints that span soft bodies (the last island) go first, serially. */
			physicsSolveDistanceIsland(physics, island_count, inverse_dt_squared);
			if (physics->volume_particles_shared) {
				physicsSolveIslandsRange(physics, 0, island_count, 0.0f, inverse_dt_squared);
			}
			else {
				physicsParallelFor(physics, island_count, 1,
					physicsSolveIslandsRange, 0.0f, inverse_dt_squared);
			}
			physicsSolveContacts(physics, inverse_dt_squared);
		}

		physicsParallelFor(physics, physics->particle_count, PHYSICS_MIN_PARTICLE_WORK_PER_THREAD,
			physicsReconstructVelocitiesRange, substep_seconds, 0.0f);
		physicsSolveContactVelocities(physics, substep_seconds);
		physicsDampenDeformation(physics, substep_seconds);
	}
}
