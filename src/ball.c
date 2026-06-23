#include "ball.h"

#include "gpu.h"
#include "heap.h"
#include "physics.h"

#define BALL_SUBDIVISIONS 3
#define BALL_VERTEX_COUNT 258
#define BALL_TRIANGLE_COUNT 512
#define BALL_INDEX_COUNT (BALL_TRIANGLE_COUNT * 3)
#define BALL_MAX_EDGES 768

typedef struct ball_t {
	heap_t* heap;
	gpu_mesh_info_t mesh;
	vec3f_t* vertices;
	uint16_t* indices;
	physics_body_t** particles;
} ball_t;

ball_info_t ballInfoDefault(vec3f_t position, vec3f_t color, float mass) {
	return (ball_info_t){
		.position = position,
		.color = color,
		.radius = 1.0f,
		.mass = mass,
		.structural_compliance = 3.0e-5f,
		.bending_compliance = 2.0e-4f,
		.volume_compliance = 1.0e-8f,
		.velocity_retention = 0.90f
	};
}

bool ballCanCreate(physics_t* physics) {
	return physicsHasCapacity(physics, BALL_VERTEX_COUNT, BALL_MAX_EDGES * 2, 1);
}

ball_t* ballCreate(heap_t* heap, physics_t* physics, const ball_info_t* info) {
	if (!ballCanCreate(physics)) return NULL;
	ball_t* ball = heapAlloc(heap, sizeof(*ball), 8);
	ball->heap = heap;
	ball->vertices = heapAlloc(heap, sizeof(vec3f_t) * BALL_VERTEX_COUNT * 3, _Alignof(vec3f_t));
	ball->indices = heapAlloc(heap, sizeof(uint16_t) * BALL_INDEX_COUNT, _Alignof(uint16_t));
	ball->particles = heapAlloc(heap, sizeof(physics_body_t*) * BALL_VERTEX_COUNT,
		_Alignof(physics_body_t*));
	ballBuildMesh(ball, info->color);

	const float particle_mass = info->mass / (float)BALL_VERTEX_COUNT;
	const float particle_radius = info->radius * 0.015f;
	const float collision_radius = info->radius * 0.08f;
	for (int i = 0; i < BALL_VERTEX_COUNT; ++i) {
		vec3f_t local = vec3fScale(ball->vertices[i * 3], info->radius);
		vec3f_t position = vec3fAdd(info->position, local);
		ball->particles[i] = physicsAddBox(physics, position,
			(vec3f_t){ particle_radius, particle_radius, particle_radius }, particle_mass, 0.0f);
		physicsSetBodyCollisionRadius(ball->particles[i], collision_radius);
		ball->vertices[i * 3] = position;
	}
	ballUpdateMesh(ball);
	ballBuildConstraints(ball, physics, info);
	return ball;
}

void ballDestroy(ball_t* ball) {
	if (!ball) return;
	heapFree(ball->heap, ball->particles);
	heapFree(ball->heap, ball->indices);
	heapFree(ball->heap, ball->vertices);
	heapFree(ball->heap, ball);
}

void ballUpdateMesh(ball_t* ball) {
	vec3f_t center = vec3fZero();
	for (int i = 0; i < BALL_VERTEX_COUNT; ++i) {
		center = vec3fAdd(center, ball->particles[i]->position);
	}
	center = vec3fScale(center, 1.0f / (float)BALL_VERTEX_COUNT);

	for (int i = 0; i < BALL_VERTEX_COUNT; ++i) {
		vec3f_t position = ball->particles[i]->position;
		ball->vertices[i * 3] = position;
		ball->vertices[i * 3 + 1] = vec3fNorm(vec3fSub(position, center));
	}
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

	for (int i = 0; i < BALL_VERTEX_COUNT; ++i) {
		ball->vertices[i * 3] = positions[i];
		ball->vertices[i * 3 + 1] = positions[i];
		ball->vertices[i * 3 + 2] = color;
	}
	for (int i = 0; i < BALL_INDEX_COUNT; ++i) ball->indices[i] = triangles_a[i];
	ball->mesh.layout = GPU_MESH_LAYOUT_TRI_P444_N444_C444_I2;
	ball->mesh.dynamic = true;
	ball->mesh.vtx_data = ball->vertices;
	ball->mesh.vtx_data_size = sizeof(vec3f_t) * BALL_VERTEX_COUNT * 3;
	ball->mesh.idx_data = ball->indices;
	ball->mesh.idx_data_size = sizeof(uint16_t) * BALL_INDEX_COUNT;

	heapFree(heap, edge_midpoint);
	heapFree(heap, edge_b);
	heapFree(heap, edge_a);
	heapFree(heap, triangles_b);
	heapFree(heap, triangles_a);
	heapFree(heap, positions);
}

void ballBuildConstraints(ball_t* ball, physics_t* physics, const ball_info_t* info) {
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
				float rest = vec3fDistance(ball->particles[a]->position, ball->particles[b]->position);
				physicsAddDistanceConstraint(physics, ball->particles[a], ball->particles[b],
					rest, info->structural_compliance);
				first_opposite[table_index] = opposite;
			} else if (first_opposite[table_index] != UINT16_MAX) {
				uint16_t other = first_opposite[table_index];
				float rest = vec3fDistance(ball->particles[opposite]->position,
					ball->particles[other]->position);
				physicsAddDistanceConstraint(physics, ball->particles[opposite],
					ball->particles[other], rest, info->bending_compliance);
			}
		}
	}
	physics_volume_constraint_t* volume = physicsAddVolumeConstraint(physics, ball->particles,
		BALL_VERTEX_COUNT, ball->indices, BALL_INDEX_COUNT, info->volume_compliance);
	physicsSetVolumeDamping(volume, info->velocity_retention);
	heapFree(ball->heap, first_opposite);
	heapFree(ball->heap, edge_added);
}
