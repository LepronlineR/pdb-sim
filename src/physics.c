#include "physics.h"
#include "heap.h"

#include <assert.h>
#include <float.h>
#include <math.h>

enum {
	PHYSICS_MAX_BODIES = 8192,
	PHYSICS_MAX_DISTANCE_CONSTRAINTS = 32768,
	PHYSICS_MAX_VOLUME_CONSTRAINTS = 16,
	PHYSICS_MAX_VOLUME_INDICES = 1536,
	PHYSICS_CONTACT_HASH_SIZE = 1024,
	PHYSICS_CONTACT_ITERATIONS = 2
};

struct physics_distance_constraint_t {
	physics_body_t* body_a;
	physics_body_t* body_b;
	float rest_length;
	float compliance;
	float lambda;
};

struct physics_volume_constraint_t {
	physics_body_t** bodies;
	uint16_t* indices;
	vec3f_t* gradients;
	vec3f_t bounds_min;
	vec3f_t bounds_max;
	vec3f_t center;
	int body_count;
	int index_count;
	float total_mass;
	float rest_volume;
	float compliance;
	float velocity_retention;
	float maximum_particle_radius;
	float lambda;
};

typedef struct physics_t {
	heap_t* heap;
	physics_body_t* bodies;
	int body_count;
	physics_distance_constraint_t* distance_constraints;
	int distance_constraint_count;
	physics_volume_constraint_t* volume_constraints;
	int volume_constraint_count;
	int* contact_hash_heads;
	int* contact_hash_next;
	int* contact_hash_used;
	int contact_hash_used_count;
	int* contact_cell_x;
	int* contact_cell_y;
	int* contact_cell_z;
	int solver_iterations;
	float gravity;
	float ground_height;
	float ground_compliance;
} physics_t;

uint32_t physicsContactHash(int x, int y, int z) {
	uint32_t hash = (uint32_t)x * 73856093u;
	hash ^= (uint32_t)y * 19349663u;
	hash ^= (uint32_t)z * 83492791u;
	return hash & (PHYSICS_CONTACT_HASH_SIZE - 1);
}


physics_t* physicsCreate(heap_t* heap) {
	physics_t* phys = heapAlloc(heap, sizeof(physics_t), 8);
	phys->heap = heap;
	phys->bodies = heapAlloc(heap, sizeof(physics_body_t) * PHYSICS_MAX_BODIES,
		_Alignof(physics_body_t));
	phys->distance_constraints = heapAlloc(heap,
		sizeof(physics_distance_constraint_t) * PHYSICS_MAX_DISTANCE_CONSTRAINTS,
		_Alignof(physics_distance_constraint_t));
	phys->volume_constraints = heapAlloc(heap,
		sizeof(physics_volume_constraint_t) * PHYSICS_MAX_VOLUME_CONSTRAINTS,
		_Alignof(physics_volume_constraint_t));
	phys->contact_hash_heads = heapAlloc(heap, sizeof(int) * PHYSICS_CONTACT_HASH_SIZE,
		_Alignof(int));
	phys->contact_hash_next = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_BODIES,
		_Alignof(int));
	phys->contact_hash_used = heapAlloc(heap, sizeof(int) * PHYSICS_CONTACT_HASH_SIZE,
		_Alignof(int));
	phys->contact_cell_x = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_BODIES,
		_Alignof(int));
	phys->contact_cell_y = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_BODIES,
		_Alignof(int));
	phys->contact_cell_z = heapAlloc(heap, sizeof(int) * PHYSICS_MAX_BODIES,
		_Alignof(int));
	phys->body_count = 0;
	phys->distance_constraint_count = 0;
	phys->volume_constraint_count = 0;
	phys->contact_hash_used_count = 0;
	for (int i = 0; i < PHYSICS_CONTACT_HASH_SIZE; ++i) {
		phys->contact_hash_heads[i] = -1;
	}
	phys->gravity = -9.81f;
	phys->ground_height = 0.0f;
	phys->ground_compliance = 0.0f;
	phys->solver_iterations = 8;

	return phys;
}

void physicsDestroy(physics_t* physics) {
	if (physics) {
		for (int i = 0; i < physics->volume_constraint_count; ++i) {
			heapFree(physics->heap, physics->volume_constraints[i].indices);
			heapFree(physics->heap, physics->volume_constraints[i].bodies);
			heapFree(physics->heap, physics->volume_constraints[i].gradients);
		}
		heapFree(physics->heap, physics->contact_cell_z);
		heapFree(physics->heap, physics->contact_cell_y);
		heapFree(physics->heap, physics->contact_cell_x);
		heapFree(physics->heap, physics->contact_hash_used);
		heapFree(physics->heap, physics->contact_hash_next);
		heapFree(physics->heap, physics->contact_hash_heads);
		heapFree(physics->heap, physics->volume_constraints);
		heapFree(physics->heap, physics->distance_constraints);
		heapFree(physics->heap, physics->bodies);
		heapFree(physics->heap, physics);
	}
}

physics_body_t* physicsAddBox(physics_t* physics, vec3f_t position,
	vec3f_t half_extents, float mass, float restitution) {
	assert(physics->body_count < PHYSICS_MAX_BODIES);
	physics_body_t* body = &physics->bodies[physics->body_count++];
	body->position = position;
	body->previous_position = position;
	body->velocity = vec3fZero();
	body->half_extents = half_extents;
	body->collision_radius = 0.0f;
	body->inverse_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
	body->contact_lambda = 0.0f;
	body->grounded = false;
	(void)restitution;
	return body;
}

void physicsSetBodyCollisionRadius(physics_body_t* body, float radius) {
	body->collision_radius = __max(0.0f, radius);
}

physics_distance_constraint_t* physicsAddDistanceConstraint(physics_t* physics,
	physics_body_t* body_a, physics_body_t* body_b, float rest_length, float compliance) {
	assert(physics->distance_constraint_count < PHYSICS_MAX_DISTANCE_CONSTRAINTS);
	physics_distance_constraint_t* constraint =
		&physics->distance_constraints[physics->distance_constraint_count++];
	constraint->body_a = body_a;
	constraint->body_b = body_b;
	constraint->rest_length = rest_length;
	constraint->compliance = __max(0.0f, compliance);
	constraint->lambda = 0.0f;
	return constraint;
}

physics_volume_constraint_t* physicsAddVolumeConstraint(physics_t* physics,
	physics_body_t** bodies, int body_count, const uint16_t* indices,
	int index_count, float compliance) {
	assert(physics->volume_constraint_count < PHYSICS_MAX_VOLUME_CONSTRAINTS);
	assert(body_count <= PHYSICS_MAX_BODIES && index_count <= PHYSICS_MAX_VOLUME_INDICES);
	physics_volume_constraint_t* constraint =
		&physics->volume_constraints[physics->volume_constraint_count++];
	constraint->bodies = heapAlloc(physics->heap, sizeof(physics_body_t*) * body_count,
		_Alignof(physics_body_t*));
	constraint->indices = heapAlloc(physics->heap, sizeof(uint16_t) * index_count,
		_Alignof(uint16_t));
	for (int i = 0; i < body_count; ++i) constraint->bodies[i] = bodies[i];
	for (int i = 0; i < index_count; ++i) constraint->indices[i] = indices[i];
	constraint->gradients = heapAlloc(physics->heap, sizeof(vec3f_t) * body_count,
		_Alignof(vec3f_t));
	constraint->body_count = body_count;
	constraint->index_count = index_count;
	constraint->compliance = __max(0.0f, compliance);
	constraint->velocity_retention = 1.0f;
	constraint->lambda = 0.0f;
	constraint->rest_volume = physicsCalculateVolume(constraint);
	vec3f_t center = vec3fZero();
	constraint->total_mass = 0.0f;
	constraint->maximum_particle_radius = 0.0f;
	for (int i = 0; i < body_count; ++i)
	{
		center = vec3fAdd(center, bodies[i]->position);
		if (bodies[i]->inverse_mass > 0.0f)
			constraint->total_mass += 1.0f / bodies[i]->inverse_mass;
		constraint->maximum_particle_radius = __max(constraint->maximum_particle_radius,
			bodies[i]->collision_radius);
	}
	center = vec3fScale(center, 1.0f / (float)body_count);
	constraint->center = center;
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

void physicsSetSolverIterations(physics_t* physics, int iterations) {
	physics->solver_iterations = __max(1, iterations);
}

bool physicsHasCapacity(physics_t* physics, int body_count,
	int distance_constraint_count, int volume_constraint_count) {
	return physics->body_count + body_count <= PHYSICS_MAX_BODIES &&
		physics->distance_constraint_count + distance_constraint_count <=
			PHYSICS_MAX_DISTANCE_CONSTRAINTS &&
		physics->volume_constraint_count + volume_constraint_count <=
			PHYSICS_MAX_VOLUME_CONSTRAINTS;
}

void physicsUpdate(physics_t* physics, float delta_seconds) {
	if (delta_seconds <= 0.0f) {
		return;
	}

	/* XPBD prediction step (Algorithm 1 in the paper). */
	for (int i = 0; i < physics->body_count; ++i) {
		physics_body_t* body = &physics->bodies[i];
		if (body->inverse_mass == 0.0f) {
			body->previous_position = body->position;
			continue;
		}
		body->previous_position = body->position;
		body->velocity.y += physics->gravity * delta_seconds;
		body->position = vec3fAdd(body->position, vec3fScale(body->velocity, delta_seconds));
		body->contact_lambda = 0.0f;
		body->grounded = false;
	}

	for (int i = 0; i < physics->distance_constraint_count; ++i) {
		physics->distance_constraints[i].lambda = 0.0f;
	}
	for (int i = 0; i < physics->volume_constraint_count; ++i) {
		physics->volume_constraints[i].lambda = 0.0f;
	}

	const float inverse_dt_squared = 1.0f / (delta_seconds * delta_seconds);
	const int contact_start_iteration = __max(0,
		physics->solver_iterations - PHYSICS_CONTACT_ITERATIONS);
	for (int iteration = 0; iteration < physics->solver_iterations; ++iteration) {
		for (int i = 0; i < physics->distance_constraint_count; ++i) {
			physicsSolveDistanceConstraint(&physics->distance_constraints[i], inverse_dt_squared);
		}
		for (int i = 0; i < physics->volume_constraint_count; ++i) {
			physicsSolveVolumeConstraint(&physics->volume_constraints[i], inverse_dt_squared);
		}
		if (iteration >= contact_start_iteration) {
			physicsUpdateSoftBodyBounds(physics);
			physicsSolveSoftBodyContacts(physics);
		}
		for (int i = 0; i < physics->body_count; ++i) {
			physicsSolveGroundConstraint(physics, &physics->bodies[i], inverse_dt_squared);
		}
	}

	/* Reconstruct velocities from the constrained positions. */
	for (int i = 0; i < physics->body_count; ++i) {
		physics_body_t* body = &physics->bodies[i];
		if (body->inverse_mass == 0.0f) {
			continue;
		}
		body->velocity = vec3fScale(vec3fSub(body->position, body->previous_position),
			1.0f / delta_seconds);
		if (body->grounded) {
			body->velocity.x *= 0.98f;
			body->velocity.z *= 0.98f;
		}
	}
	physicsUpdateSoftBodyBounds(physics);
	physicsDampSoftBodyContactVelocities(physics);

	/* Dampen deformation modes while preserving each soft body's center-of-mass
	   velocity. Gravity therefore remains an unmodified 9.81 m/s^2. */
	for (int constraint_idx = 0; constraint_idx < physics->volume_constraint_count; ++constraint_idx) {
		physics_volume_constraint_t* constraint = &physics->volume_constraints[constraint_idx];
		float total_mass = 0.0f;
		vec3f_t center_velocity = vec3fZero();
		for (int i = 0; i < constraint->body_count; ++i) {
			physics_body_t* body = constraint->bodies[i];
			if (body->inverse_mass <= 0.0f) continue;
			float mass = 1.0f / body->inverse_mass;
			total_mass += mass;
			center_velocity = vec3fAdd(center_velocity, vec3fScale(body->velocity, mass));
		}
		if (total_mass <= 0.0f) continue;
		center_velocity = vec3fScale(center_velocity, 1.0f / total_mass);
		for (int i = 0; i < constraint->body_count; ++i) {
			physics_body_t* body = constraint->bodies[i];
			vec3f_t relative = vec3fSub(body->velocity, center_velocity);
			body->velocity = vec3fAdd(center_velocity,
				vec3fScale(relative, constraint->velocity_retention));
		}
	}
}

void physicsDampSoftBodyContactVelocities(physics_t* physics) {
	for (int group_a = 0; group_a < physics->volume_constraint_count; ++group_a) {
		physics_volume_constraint_t* soft_a = &physics->volume_constraints[group_a];
		for (int group_b = group_a + 1; group_b < physics->volume_constraint_count; ++group_b) {
			physics_volume_constraint_t* soft_b = &physics->volume_constraints[group_b];
			if (!physicsSoftBodiesOverlap(soft_a, soft_b)) continue;
			vec3f_t velocity_a = vec3fZero();
			vec3f_t velocity_b = vec3fZero();
			for (int i = 0; i < soft_a->body_count; ++i) {
				physics_body_t* body = soft_a->bodies[i];
				float mass = body->inverse_mass > 0.0f ? 1.0f / body->inverse_mass : 0.0f;
				velocity_a = vec3fAdd(velocity_a, vec3fScale(body->velocity, mass));
			}
			for (int i = 0; i < soft_b->body_count; ++i) {
				physics_body_t* body = soft_b->bodies[i];
				float mass = body->inverse_mass > 0.0f ? 1.0f / body->inverse_mass : 0.0f;
				velocity_b = vec3fAdd(velocity_b, vec3fScale(body->velocity, mass));
			}
			if (soft_a->total_mass <= 0.0f || soft_b->total_mass <= 0.0f) continue;
			velocity_a = vec3fScale(velocity_a, 1.0f / soft_a->total_mass);
			velocity_b = vec3fScale(velocity_b, 1.0f / soft_b->total_mass);
			vec3f_t normal = vec3fNorm(vec3fSub(soft_a->center, soft_b->center));
			if (vec3fMagnitudeSqrd(normal) <= 1.0e-8f) normal = vec3fY();
			float approaching_speed = vec3fDot(vec3fSub(velocity_a, velocity_b), normal);
			if (approaching_speed >= 0.0f) continue;
			float inverse_mass_a = 1.0f / soft_a->total_mass;
			float inverse_mass_b = 1.0f / soft_b->total_mass;
			float impulse = -approaching_speed / (inverse_mass_a + inverse_mass_b);
			vec3f_t change_a = vec3fScale(normal, impulse * inverse_mass_a);
			vec3f_t change_b = vec3fScale(normal, impulse * inverse_mass_b);
			for (int i = 0; i < soft_a->body_count; ++i)
				soft_a->bodies[i]->velocity = vec3fAdd(soft_a->bodies[i]->velocity, change_a);
			for (int i = 0; i < soft_b->body_count; ++i)
				soft_b->bodies[i]->velocity = vec3fSub(soft_b->bodies[i]->velocity, change_b);
		}
	}
}

void physicsUpdateSoftBodyBounds(physics_t* physics) {
	for (int group = 0; group < physics->volume_constraint_count; ++group) {
		physics_volume_constraint_t* soft_body = &physics->volume_constraints[group];
		soft_body->bounds_min = (vec3f_t){ FLT_MAX, FLT_MAX, FLT_MAX };
		soft_body->bounds_max = (vec3f_t){ -FLT_MAX, -FLT_MAX, -FLT_MAX };
		soft_body->center = vec3fZero();
		for (int i = 0; i < soft_body->body_count; ++i) {
			physics_body_t* body = soft_body->bodies[i];
			vec3f_t radius = {
				body->collision_radius, body->collision_radius, body->collision_radius
			};
			soft_body->bounds_min = vec3fMin(soft_body->bounds_min,
				vec3fSub(body->position, radius));
			soft_body->bounds_max = vec3fMax(soft_body->bounds_max,
				vec3fAdd(body->position, radius));
			soft_body->center = vec3fAdd(soft_body->center, body->position);
		}
		soft_body->center = vec3fScale(soft_body->center,
			1.0f / (float)soft_body->body_count);
	}
}

bool physicsSoftBodiesOverlap(physics_volume_constraint_t* a,
	physics_volume_constraint_t* b) {
	return a->bounds_min.x <= b->bounds_max.x && a->bounds_max.x >= b->bounds_min.x &&
		a->bounds_min.y <= b->bounds_max.y && a->bounds_max.y >= b->bounds_min.y &&
		a->bounds_min.z <= b->bounds_max.z && a->bounds_max.z >= b->bounds_min.z;
}

void physicsSolveSoftBodyContacts(physics_t* physics) {
	for (int group_a = 0; group_a < physics->volume_constraint_count; ++group_a) {
		physics_volume_constraint_t* soft_a = &physics->volume_constraints[group_a];
		for (int group_b = group_a + 1; group_b < physics->volume_constraint_count; ++group_b) {
			physics_volume_constraint_t* soft_b = &physics->volume_constraints[group_b];
			if (!physicsSoftBodiesOverlap(soft_a, soft_b)) continue;
			float cell_size = __max(soft_a->maximum_particle_radius,
				soft_b->maximum_particle_radius) * 2.0f;
			if (cell_size <= 0.0f) continue;

			physics->contact_hash_used_count = 0;
			for (int b_index = 0; b_index < soft_b->body_count; ++b_index) {
				vec3f_t position = soft_b->bodies[b_index]->position;
				int cell_x = (int)floorf(position.x / cell_size);
				int cell_y = (int)floorf(position.y / cell_size);
				int cell_z = (int)floorf(position.z / cell_size);
				uint32_t hash = physicsContactHash(cell_x, cell_y, cell_z);
				if (physics->contact_hash_heads[hash] == -1) {
					physics->contact_hash_used[physics->contact_hash_used_count++] = hash;
				}
				physics->contact_cell_x[b_index] = cell_x;
				physics->contact_cell_y[b_index] = cell_y;
				physics->contact_cell_z[b_index] = cell_z;
				physics->contact_hash_next[b_index] = physics->contact_hash_heads[hash];
				physics->contact_hash_heads[hash] = b_index;
			}

			for (int a_index = 0; a_index < soft_a->body_count; ++a_index) {
				physics_body_t* a = soft_a->bodies[a_index];
				int center_x = (int)floorf(a->position.x / cell_size);
				int center_y = (int)floorf(a->position.y / cell_size);
				int center_z = (int)floorf(a->position.z / cell_size);
				for (int z = center_z - 1; z <= center_z + 1; ++z) {
				for (int y = center_y - 1; y <= center_y + 1; ++y) {
				for (int x = center_x - 1; x <= center_x + 1; ++x) {
					uint32_t hash = physicsContactHash(x, y, z);
					for (int b_index = physics->contact_hash_heads[hash]; b_index >= 0;
						b_index = physics->contact_hash_next[b_index]) {
						if (physics->contact_cell_x[b_index] != x ||
							physics->contact_cell_y[b_index] != y ||
							physics->contact_cell_z[b_index] != z) continue;
					physics_body_t* b = soft_b->bodies[b_index];
					const float minimum_distance = a->collision_radius + b->collision_radius;
					vec3f_t delta = vec3fSub(a->position, b->position);
					const float distance_squared = vec3fMagnitudeSqrd(delta);
					if (minimum_distance <= 0.0f ||
						distance_squared >= minimum_distance * minimum_distance) continue;

					const float inverse_mass_sum = a->inverse_mass + b->inverse_mass;
					if (inverse_mass_sum <= 0.0f) continue;
					const float distance = sqrtf(__max(distance_squared, 1.0e-12f));
					vec3f_t normal = distance_squared > 1.0e-12f ?
						vec3fScale(delta, 1.0f / distance) : vec3fX();
					const float correction = (minimum_distance - distance) / inverse_mass_sum;
					a->position = vec3fAdd(a->position,
						vec3fScale(normal, correction * a->inverse_mass));
					b->position = vec3fSub(b->position,
						vec3fScale(normal, correction * b->inverse_mass));
				}
				}
				}
				}
			}

			for (int i = 0; i < physics->contact_hash_used_count; ++i) {
				physics->contact_hash_heads[physics->contact_hash_used[i]] = -1;
			}
		}
	}
}

void physicsSolveDistanceConstraint(physics_distance_constraint_t* constraint, float inverse_dt_squared) {
	physics_body_t* a = constraint->body_a;
	physics_body_t* b = constraint->body_b;
	const float inverse_mass_sum = a->inverse_mass + b->inverse_mass;
	vec3f_t delta = vec3fSub(a->position, b->position);
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
	a->position = vec3fAdd(a->position, vec3fScale(gradient, a->inverse_mass * delta_lambda));
	b->position = vec3fSub(b->position, vec3fScale(gradient, b->inverse_mass * delta_lambda));
}

float physicsCalculateVolume(physics_volume_constraint_t* constraint) {
	float volume = 0.0f;
	for (int i = 0; i < constraint->index_count; i += 3) {
		vec3f_t a = constraint->bodies[constraint->indices[i]]->position;
		vec3f_t b = constraint->bodies[constraint->indices[i + 1]]->position;
		vec3f_t c = constraint->bodies[constraint->indices[i + 2]]->position;
		volume += vec3fDot(a, vec3fCross(b, c)) / 6.0f;
	}
	return volume;
}

void physicsSolveVolumeConstraint(physics_volume_constraint_t* constraint, float inverse_dt_squared) {
	vec3f_t* gradients = constraint->gradients;
	for (int i = 0; i < constraint->body_count; ++i) gradients[i] = vec3fZero();
	float volume = 0.0f;
	for (int i = 0; i < constraint->index_count; i += 3) {
		uint16_t ia = constraint->indices[i];
		uint16_t ib = constraint->indices[i + 1];
		uint16_t ic = constraint->indices[i + 2];
		vec3f_t a = constraint->bodies[ia]->position;
		vec3f_t b = constraint->bodies[ib]->position;
		vec3f_t c = constraint->bodies[ic]->position;
		volume += vec3fDot(a, vec3fCross(b, c)) / 6.0f;
		gradients[ia] = vec3fAdd(gradients[ia], vec3fScale(vec3fCross(b, c), 1.0f / 6.0f));
		gradients[ib] = vec3fAdd(gradients[ib], vec3fScale(vec3fCross(c, a), 1.0f / 6.0f));
		gradients[ic] = vec3fAdd(gradients[ic], vec3fScale(vec3fCross(a, b), 1.0f / 6.0f));
	}

	float denominator = 0.0f;
	for (int i = 0; i < constraint->body_count; ++i) {
		denominator += constraint->bodies[i]->inverse_mass * vec3fMagnitudeSqrd(gradients[i]);
	}
	const float alpha_tilde = constraint->compliance * inverse_dt_squared;
	if (denominator + alpha_tilde <= 1.0e-8f) {
		return;
	}
	const float constraint_value = volume - constraint->rest_volume;
	const float delta_lambda = (-constraint_value - alpha_tilde * constraint->lambda) /
		(denominator + alpha_tilde);
	constraint->lambda += delta_lambda;
	for (int i = 0; i < constraint->body_count; ++i) {
		physics_body_t* body = constraint->bodies[i];
		body->position = vec3fAdd(body->position,
			vec3fScale(gradients[i], body->inverse_mass * delta_lambda));
	}
}

void physicsSolveGroundConstraint(physics_t* physics, physics_body_t* body, float inverse_dt_squared) {
	if (body->inverse_mass == 0.0f) {
		return;
	}
	const float constraint_value = body->position.y - body->half_extents.y - physics->ground_height;
	if (constraint_value >= 0.0f && body->contact_lambda == 0.0f) {
		return;
	}

	const float alpha_tilde = physics->ground_compliance * inverse_dt_squared;
	float delta_lambda = (-constraint_value - alpha_tilde * body->contact_lambda) /
		(body->inverse_mass + alpha_tilde);
	const float new_lambda = __max(0.0f, body->contact_lambda + delta_lambda);
	delta_lambda = new_lambda - body->contact_lambda;
	body->contact_lambda = new_lambda;
	body->position.y += body->inverse_mass * delta_lambda;
	body->grounded = body->contact_lambda > 0.0f;
}
