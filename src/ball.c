#include "ball.h"

#include "gpu.h"
#include "heap.h"
#include "physics.h"

#include <assert.h>

#define BALL_SUBDIVISIONS 3
#define BALL_VERTEX_COUNT 258
#define BALL_TRIANGLE_COUNT 512
#define BALL_INDEX_COUNT (BALL_TRIANGLE_COUNT * 3)
#define BALL_MAX_EDGES 768
#define BALL_RADIUS_SLACK 1.1f
/* Render vertices: one per particle plus copies along the texture seam and at
   the poles, which need a second uv (about 25 at 3 subdivisions). */
#define BALL_MAX_RENDER_VERTICES (BALL_VERTEX_COUNT + 64)

#include <math.h>

typedef struct ball_t {
	heap_t* heap;
	gpu_mesh_info_t mesh;
	gpu_vertex_t* vertices;
	uint16_t* vertex_particles;  // particle (from first_particle) each vertex follows
	int vertex_count;
	uint16_t* render_indices;    // into vertices
	uint16_t* indices;           // into the particles: the physics surface
	physics_t* physics;
	/* The ball's particles are consecutive ids starting here, one per vertex. */
	physics_particle_id_t first_particle;
	/* Contact radius of the particles. The rendered surface is pushed out by it,
	   so the visible ball is the surface that actually collides. */
	float particle_radius;
} ball_t;

ball_info_t ballInfoDefault(vec3f_t position, vec3f_t color, float mass) {
	return (ball_info_t){
		.position = position,
		.color = color,
		.radius = 1.0f,
		.mass = mass,
		.structural_compliance = 3.0e-3f,
		.bending_compliance = 3.0e-3f,
		.volume_compliance = 1.0e-8f,
		.velocity_retention = 0.30f
	};
}

bool ballCanCreate(physics_t* physics) {
	return physicsHasCapacity(physics, BALL_VERTEX_COUNT, BALL_MAX_EDGES * 2, 1);
}

ball_t* ballCreate(heap_t* heap, physics_t* physics, const ball_info_t* info) {
	if (!ballCanCreate(physics)) return NULL;
	ball_t* ball = heapAlloc(heap, sizeof(*ball), 8);
	ball->heap = heap;
	ball->vertices = heapAlloc(heap, sizeof(gpu_vertex_t) * BALL_MAX_RENDER_VERTICES,
		_Alignof(gpu_vertex_t));
	ball->vertex_particles = heapAlloc(heap, sizeof(uint16_t) * BALL_MAX_RENDER_VERTICES,
		_Alignof(uint16_t));
	ball->render_indices = heapAlloc(heap, sizeof(uint16_t) * BALL_INDEX_COUNT, _Alignof(uint16_t));
	ball->indices = heapAlloc(heap, sizeof(uint16_t) * BALL_INDEX_COUNT, _Alignof(uint16_t));
	ball->physics = physics;
	ballBuildMesh(ball, info->color);

	const float particle_mass = info->mass / (float)BALL_VERTEX_COUNT;
	/* A particle resting in a surface triangle sits at its circumcenter, so the
	   surface is closed to equal particles when 2r covers the largest
	   circumradius. The slack keeps it closed while the surface stretches. */
	const float particle_radius = info->radius * ballGetMaxCircumradius(ball) * 0.5f *
		BALL_RADIUS_SLACK;
	ball->particle_radius = particle_radius;
	for (int i = 0; i < BALL_VERTEX_COUNT; ++i) {
		/* The first BALL_VERTEX_COUNT vertices are the particles, in order. */
		const float* rest = ball->vertices[i].position;
		vec3f_t local = vec3fScale((vec3f_t){ rest[0], rest[1], rest[2] }, info->radius);
		vec3f_t position = vec3fAdd(info->position, local);
		const physics_particle_id_t particle = physicsAddParticle(physics, position,
			particle_radius, particle_mass);
		if (i == 0) ball->first_particle = particle;
	}
	ballUpdateMesh(ball);
	ballBuildConstraints(ball, physics, info);
	return ball;
}

void ballDestroy(ball_t* ball) {
	if (!ball) return;
	heapFree(ball->heap, ball->indices);
	heapFree(ball->heap, ball->render_indices);
	heapFree(ball->heap, ball->vertex_particles);
	heapFree(ball->heap, ball->vertices);
	heapFree(ball->heap, ball);
}

void ballUpdateMesh(ball_t* ball) {
	const vec3f_t* positions = physicsGetParticlePositions(ball->physics) + ball->first_particle;
	vec3f_t center = vec3fZero();
	for (int i = 0; i < BALL_VERTEX_COUNT; ++i) {
		center = vec3fAdd(center, positions[i]);
	}
	center = vec3fScale(center, 1.0f / (float)BALL_VERTEX_COUNT);

	for (int i = 0; i < ball->vertex_count; ++i) {
		vec3f_t position = positions[ball->vertex_particles[i]];
		vec3f_t normal = vec3fNorm(vec3fSub(position, center));
		vec3f_t surface = vec3fAdd(position, vec3fScale(normal, ball->particle_radius));
		gpu_vertex_t* vertex = &ball->vertices[i];
		vertex->position[0] = surface.x; vertex->position[1] = surface.y; vertex->position[2] = surface.z;
		vertex->normal[0] = normal.x; vertex->normal[1] = normal.y; vertex->normal[2] = normal.z;
	}
}

float ballGetMaxCircumradius(ball_t* ball) {
	float max_radius = 0.0f;
	for (int i = 0; i < BALL_INDEX_COUNT; i += 3) {
		const float* pa = ball->vertices[ball->indices[i]].position;
		const float* pb = ball->vertices[ball->indices[i + 1]].position;
		const float* pc = ball->vertices[ball->indices[i + 2]].position;
		vec3f_t a = { pa[0], pa[1], pa[2] };
		vec3f_t b = { pb[0], pb[1], pb[2] };
		vec3f_t c = { pc[0], pc[1], pc[2] };
		const float ab = vec3fDistance(a, b);
		const float bc = vec3fDistance(b, c);
		const float ca = vec3fDistance(c, a);
		const float twice_area = vec3fMagnitude(vec3fCross(vec3fSub(b, a), vec3fSub(c, a)));
		if (twice_area > 0.0f) {
			max_radius = __max(max_radius, ab * bc * ca / (2.0f * twice_area));
		}
	}
	return max_radius;
}

void ballGetParticles(ball_t* ball, physics_particle_id_t* first_particle, int* particle_count) {
	*first_particle = ball->first_particle;
	*particle_count = BALL_VERTEX_COUNT;
}

gpu_mesh_info_t* ballGetMesh(ball_t* ball) {
	return &ball->mesh;
}

uint16_t ballGetMidpoint(vec3f_t* positions, int* vertex_count,
	uint16_t* edge_a, uint16_t* edge_b, uint16_t* edge_midpoint,
	int* edge_count, uint16_t a, uint16_t b) {
	uint16_t low = __min(a, b);
	uint16_t high = __max(a, b);
	for (int i = 0; i < *edge_count; ++i) {
		if (edge_a[i] == low && edge_b[i] == high) return edge_midpoint[i];
	}
	uint16_t midpoint = (uint16_t)(*vertex_count);
	positions[midpoint] = vec3fNorm(vec3fAdd(positions[a], positions[b]));
	(*vertex_count)++;
	edge_a[*edge_count] = low;
	edge_b[*edge_count] = high;
	edge_midpoint[*edge_count] = midpoint;
	(*edge_count)++;
	return midpoint;
}

static int ballAddRenderVertex(ball_t* ball, uint16_t particle, vec3f_t direction,
	vec3f_t color, float u, float v) {
	assert(ball->vertex_count < BALL_MAX_RENDER_VERTICES);
	const int index = ball->vertex_count++;
	ball->vertex_particles[index] = particle;
	ball->vertices[index] = (gpu_vertex_t){
		.position = { direction.x, direction.y, direction.z },
		.normal = { direction.x, direction.y, direction.z },
		.color = { color.x, color.y, color.z },
		/* Around the ball is twice as long as pole to pole: two texture repeats
		   keep the texels square. */
		.uv = { u * 2.0f, v }
	};
	return index;
}

/* Longitude/latitude uvs from the rest directions. A triangle across the seam
   (u jumps from about 1 back to 0) gets copies of its low-u vertices at u + 1,
   and each pole gets a copy per triangle at that triangle's longitude, so no
   triangle stretches the texture across the whole ball. */
static void ballBuildRenderVertices(ball_t* ball, const vec3f_t* directions, vec3f_t color) {
	const float two_pi = 6.28318531f;
	float us[BALL_VERTEX_COUNT];
	float vs[BALL_VERTEX_COUNT];
	int wrapped[BALL_VERTEX_COUNT];
	ball->vertex_count = 0;
	for (int i = 0; i < BALL_VERTEX_COUNT; ++i) {
		const vec3f_t d = directions[i];
		float u = atan2f(d.z, d.x) / two_pi;
		if (u < 0.0f) u += 1.0f;
		us[i] = u;
		vs[i] = acosf(__max(-1.0f, __min(1.0f, d.y))) / 3.14159265f;
		wrapped[i] = -1;
		ballAddRenderVertex(ball, (uint16_t)i, d, color, us[i], vs[i]);
	}
	for (int t = 0; t < BALL_INDEX_COUNT; t += 3) {
		bool pole[3];
		float u_min = 1.0f, u_max = 0.0f;
		for (int k = 0; k < 3; ++k) {
			const uint16_t i = ball->indices[t + k];
			pole[k] = fabsf(directions[i].y) > 0.9999f;
			if (!pole[k]) {
				u_min = __min(u_min, us[i]);
				u_max = __max(u_max, us[i]);
			}
		}
		const bool wrap = u_max - u_min > 0.5f;
		float corner_u[3];
		float u_sum = 0.0f;
		int u_count = 0;
		for (int k = 0; k < 3; ++k) {
			const uint16_t i = ball->indices[t + k];
			corner_u[k] = us[i] + (wrap && us[i] < 0.5f ? 1.0f : 0.0f);
			if (!pole[k]) {
				u_sum += corner_u[k];
				++u_count;
			}
		}
		for (int k = 0; k < 3; ++k) {
			const uint16_t i = ball->indices[t + k];
			int vertex = i;
			if (pole[k]) {
				vertex = ballAddRenderVertex(ball, i, directions[i], color,
					u_count ? u_sum / (float)u_count : 0.0f, vs[i]);
			}
			else if (corner_u[k] != us[i]) {
				if (wrapped[i] < 0) {
					wrapped[i] = ballAddRenderVertex(ball, i, directions[i], color, corner_u[k], vs[i]);
				}
				vertex = wrapped[i];
			}
			ball->render_indices[t + k] = (uint16_t)vertex;
		}
	}
}

void ballBuildMesh(ball_t* ball, vec3f_t color) {
	heap_t* heap = ball->heap;
	vec3f_t* positions = heapAlloc(heap, sizeof(vec3f_t) * BALL_VERTEX_COUNT, _Alignof(vec3f_t));
	uint16_t* triangles_a = heapAlloc(heap, sizeof(uint16_t) * BALL_INDEX_COUNT, _Alignof(uint16_t));
	uint16_t* triangles_b = heapAlloc(heap, sizeof(uint16_t) * BALL_INDEX_COUNT, _Alignof(uint16_t));
	uint16_t* edge_a = heapAlloc(heap, sizeof(uint16_t) * BALL_MAX_EDGES, _Alignof(uint16_t));
	uint16_t* edge_b = heapAlloc(heap, sizeof(uint16_t) * BALL_MAX_EDGES, _Alignof(uint16_t));
	uint16_t* edge_midpoint = heapAlloc(heap, sizeof(uint16_t) * BALL_MAX_EDGES, _Alignof(uint16_t));
	positions[0] = (vec3f_t){ 0.0f, 1.0f, 0.0f };
	positions[1] = (vec3f_t){ 0.0f, -1.0f, 0.0f };
	positions[2] = (vec3f_t){ 1.0f, 0.0f, 0.0f };
	positions[3] = (vec3f_t){ -1.0f, 0.0f, 0.0f };
	positions[4] = (vec3f_t){ 0.0f, 0.0f, 1.0f };
	positions[5] = (vec3f_t){ 0.0f, 0.0f, -1.0f };
	triangles_a[0] = 0; triangles_a[1] = 2; triangles_a[2] = 4;
	triangles_a[3] = 0; triangles_a[4] = 4; triangles_a[5] = 3;
	triangles_a[6] = 0; triangles_a[7] = 3; triangles_a[8] = 5;
	triangles_a[9] = 0; triangles_a[10] = 5; triangles_a[11] = 2;
	triangles_a[12] = 1; triangles_a[13] = 4; triangles_a[14] = 2;
	triangles_a[15] = 1; triangles_a[16] = 3; triangles_a[17] = 4;
	triangles_a[18] = 1; triangles_a[19] = 5; triangles_a[20] = 3;
	triangles_a[21] = 1; triangles_a[22] = 2; triangles_a[23] = 5;
	int vertex_count = 6;
	int triangle_count = 8;

	for (int subdivision = 0; subdivision < BALL_SUBDIVISIONS; ++subdivision) {
		int edge_count = 0;
		int output = 0;
		for (int triangle = 0; triangle < triangle_count; ++triangle) {
			uint16_t a = triangles_a[triangle * 3];
			uint16_t b = triangles_a[triangle * 3 + 1];
			uint16_t c = triangles_a[triangle * 3 + 2];
			uint16_t ab = ballGetMidpoint(positions, &vertex_count,
				edge_a, edge_b, edge_midpoint, &edge_count, a, b);
			uint16_t bc = ballGetMidpoint(positions, &vertex_count,
				edge_a, edge_b, edge_midpoint, &edge_count, b, c);
			uint16_t ca = ballGetMidpoint(positions, &vertex_count,
				edge_a, edge_b, edge_midpoint, &edge_count, c, a);
			triangles_b[output++] = a;  triangles_b[output++] = ab; triangles_b[output++] = ca;
			triangles_b[output++] = ab; triangles_b[output++] = b;  triangles_b[output++] = bc;
			triangles_b[output++] = ca; triangles_b[output++] = bc; triangles_b[output++] = c;
			triangles_b[output++] = ab; triangles_b[output++] = bc; triangles_b[output++] = ca;
		}
		triangle_count *= 4;
		for (int i = 0; i < triangle_count * 3; ++i) triangles_a[i] = triangles_b[i];
	}

	for (int i = 0; i < BALL_INDEX_COUNT; ++i) ball->indices[i] = triangles_a[i];
	ballBuildRenderVertices(ball, positions, color);
	ball->mesh.layout = GPU_MESH_LAYOUT_TRI_P444_N444_C444_U44_I2;
	ball->mesh.dynamic = true;
	ball->mesh.id = gpuMeshInfoNextId();
	ball->mesh.vtx_data = ball->vertices;
	ball->mesh.vtx_data_size = sizeof(gpu_vertex_t) * ball->vertex_count;
	ball->mesh.idx_data = ball->render_indices;
	ball->mesh.idx_data_size = sizeof(uint16_t) * BALL_INDEX_COUNT;

	heapFree(heap, edge_midpoint);
	heapFree(heap, edge_b);
	heapFree(heap, edge_a);
	heapFree(heap, triangles_b);
	heapFree(heap, triangles_a);
	heapFree(heap, positions);
}

void ballBuildConstraints(ball_t* ball, physics_t* physics, const ball_info_t* info) {
	const vec3f_t* positions = physicsGetParticlePositions(physics) + ball->first_particle;
	const int table_count = BALL_VERTEX_COUNT * BALL_VERTEX_COUNT;
	uint8_t* edge_added = heapAlloc(ball->heap, sizeof(uint8_t) * table_count, _Alignof(uint8_t));
	uint16_t* first_opposite = heapAlloc(ball->heap,
		sizeof(uint16_t) * table_count, _Alignof(uint16_t));
	for (int i = 0; i < table_count; ++i) {
		edge_added[i] = 0;
		first_opposite[i] = UINT16_MAX;
	}

	for (int i = 0; i < BALL_INDEX_COUNT; i += 3) {
		uint16_t vertex_a = ball->indices[i];
		uint16_t vertex_b = ball->indices[i + 1];
		uint16_t vertex_c = ball->indices[i + 2];
		for (int edge = 0; edge < 3; ++edge) {
			uint16_t a = edge == 0 ? vertex_a : edge == 1 ? vertex_b : vertex_c;
			uint16_t b = edge == 0 ? vertex_b : edge == 1 ? vertex_c : vertex_a;
			uint16_t opposite = edge == 0 ? vertex_c : edge == 1 ? vertex_a : vertex_b;
			uint16_t low = __min(a, b);
			uint16_t high = __max(a, b);
			int table_index = low * BALL_VERTEX_COUNT + high;
			if (!edge_added[table_index]) {
				edge_added[table_index] = 1;
				float rest = vec3fDistance(positions[a], positions[b]);
				physicsAddDistanceConstraint(physics, ball->first_particle + a,
					ball->first_particle + b, rest, info->structural_compliance);
				first_opposite[table_index] = opposite;
			} else if (first_opposite[table_index] != UINT16_MAX) {
				uint16_t other = first_opposite[table_index];
				float rest = vec3fDistance(positions[opposite], positions[other]);
				physicsAddDistanceConstraint(physics, ball->first_particle + opposite,
					ball->first_particle + other, rest, info->bending_compliance);
			}
		}
	}
	const int soft_body = physicsAddSoftBody(physics, ball->first_particle, BALL_VERTEX_COUNT,
		false);
	physicsSetSoftBodyVolume(physics, soft_body, ball->indices, BALL_INDEX_COUNT,
		info->volume_compliance);
	physicsSetSoftBodyDamping(physics, soft_body, info->velocity_retention);
	heapFree(ball->heap, first_opposite);
	heapFree(ball->heap, edge_added);
}
