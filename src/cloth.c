#include "cloth.h"

#include "gpu.h"
#include "heap.h"
#include "physics.h"

#include <assert.h>
#include <string.h>

#define CLOTH_MIN_RESOLUTION 4
#define CLOTH_MAX_RESOLUTION 64
/* Particle radius relative to the spacing. Below 0.5 so neighbours do not touch
   at rest; above half the diagonal gap (0.354) so the cloth cannot pass
   through itself. */
#define CLOTH_RADIUS_RATIO 0.45f

typedef struct cloth_t {
	heap_t* heap;
	physics_t* physics;
	gpu_mesh_info_t mesh;
	/* Two copies of the sheet, front then back, each vertex position/normal/color. */
	vec3f_t* vertices;
	vec3f_t* normals;
	uint16_t* indices;
	int resolution;
	float particle_radius;
	physics_particle_id_t first_particle;
} cloth_t;

static int clothResolution(const cloth_info_t* info) {
	return __max(CLOTH_MIN_RESOLUTION, __min(info->resolution, CLOTH_MAX_RESOLUTION));
}

static int clothConstraintCount(int n) {
	const int stretch = 2 * n * (n - 1);
	const int shear = 2 * (n - 1) * (n - 1);
	const int bend = 2 * n * (n - 2);
	return stretch + shear + bend;
}

cloth_info_t clothInfoDefault(vec3f_t center, vec3f_t color) {
	return (cloth_info_t){
		.center = center,
		.color = color,
		.resolution = 32,
		.size = 4.0f,
		.mass = 1.0f,
		.stretch_compliance = 1.0e-7f,
		.shear_compliance = 1.0e-6f,
		.bend_compliance = 5.0e-2f,
		.velocity_retention = 0.6f,
		.drag = 0.4f,
		.pin = CLOTH_PIN_CORNERS
	};
}

bool clothCanCreate(physics_t* physics, const cloth_info_t* info) {
	const int n = clothResolution(info);
	return physicsHasCapacity(physics, n * n, clothConstraintCount(n), 1);
}

static bool clothIsPinned(cloth_pin_t pin, int n, int x, int z) {
	switch (pin) {
		case CLOTH_PIN_CORNERS:
			return (x == 0 || x == n - 1) && (z == 0 || z == n - 1);
		case CLOTH_PIN_EDGE:
			return z == 0;
		case CLOTH_PIN_BORDER:
			return x == 0 || z == 0 || x == n - 1 || z == n - 1;
		default:
			return false;
	}
}

cloth_t* clothCreate(heap_t* heap, physics_t* physics, const cloth_info_t* info) {
	if (!clothCanCreate(physics, info)) return NULL;
	const int n = clothResolution(info);
	const int vertex_count = n * n;
	const int index_count = (n - 1) * (n - 1) * 6 * 2;

	cloth_t* cloth = heapAlloc(heap, sizeof(*cloth), 8);
	memset(cloth, 0, sizeof(*cloth));
	cloth->heap = heap;
	cloth->physics = physics;
	cloth->resolution = n;
	cloth->vertices = heapAlloc(heap, sizeof(vec3f_t) * vertex_count * 2 * 3, _Alignof(vec3f_t));
	cloth->normals = heapAlloc(heap, sizeof(vec3f_t) * vertex_count, _Alignof(vec3f_t));
	cloth->indices = heapAlloc(heap, sizeof(uint16_t) * index_count, _Alignof(uint16_t));

	const float spacing = info->size / (float)(n - 1);
	cloth->particle_radius = spacing * CLOTH_RADIUS_RATIO;
	const float particle_mass = info->mass / (float)vertex_count;
	const vec3f_t origin = {
		info->center.x - info->size * 0.5f, info->center.y, info->center.z - info->size * 0.5f
	};
	for (int z = 0; z < n; ++z) {
		for (int x = 0; x < n; ++x) {
			const vec3f_t position = {
				origin.x + (float)x * spacing, origin.y, origin.z + (float)z * spacing
			};
			const float mass = clothIsPinned(info->pin, n, x, z) ? 0.0f : particle_mass;
			const physics_particle_id_t particle = physicsAddParticle(physics, position,
				cloth->particle_radius, mass);
			if (x == 0 && z == 0) cloth->first_particle = particle;
		}
	}

	/* Stretch along both grid axes, shear across each cell's diagonals, and
	   bending as stretch constraints that skip one particle. */
	const physics_particle_id_t first = cloth->first_particle;
	const float diagonal = spacing * 1.41421356f;
	for (int z = 0; z < n; ++z) {
		for (int x = 0; x < n; ++x) {
			const physics_particle_id_t p = first + (physics_particle_id_t)(z * n + x);
			if (x + 1 < n) physicsAddDistanceConstraint(physics, p, p + 1, spacing,
				info->stretch_compliance);
			if (z + 1 < n) physicsAddDistanceConstraint(physics, p, p + n, spacing,
				info->stretch_compliance);
			if (x + 1 < n && z + 1 < n) {
				physicsAddDistanceConstraint(physics, p, p + n + 1, diagonal,
					info->shear_compliance);
				physicsAddDistanceConstraint(physics, p + 1, p + n, diagonal,
					info->shear_compliance);
			}
			if (x + 2 < n) physicsAddDistanceConstraint(physics, p, p + 2, 2.0f * spacing,
				info->bend_compliance);
			if (z + 2 < n) physicsAddDistanceConstraint(physics, p, p + 2 * n, 2.0f * spacing,
				info->bend_compliance);
		}
	}
	const int soft_body = physicsAddSoftBody(physics, first, vertex_count, true);
	physicsSetSoftBodyDamping(physics, soft_body, info->velocity_retention);
	physicsSetSoftBodyDrag(physics, soft_body, info->drag);

	/* Front faces are wound so they face +y while flat. All front faces come
	   first: clothUpdateMesh builds normals from that first half only. The back
	   copy reverses the winding and uses the second half of the vertices. */
	const int front_index_count = index_count / 2;
	int index = 0;
	for (int z = 0; z + 1 < n; ++z) {
		for (int x = 0; x + 1 < n; ++x) {
			const uint16_t a = (uint16_t)(z * n + x);
			const uint16_t b = (uint16_t)(a + 1);
			const uint16_t c = (uint16_t)(a + n);
			const uint16_t d = (uint16_t)(c + 1);
			cloth->indices[index++] = a; cloth->indices[index++] = b; cloth->indices[index++] = c;
			cloth->indices[index++] = b; cloth->indices[index++] = d; cloth->indices[index++] = c;
		}
	}
	for (int i = 0; i < front_index_count; i += 3) {
		cloth->indices[index++] = (uint16_t)(vertex_count + cloth->indices[i]);
		cloth->indices[index++] = (uint16_t)(vertex_count + cloth->indices[i + 2]);
		cloth->indices[index++] = (uint16_t)(vertex_count + cloth->indices[i + 1]);
	}
	/* A two-tone check every few particles makes folds and sag readable. */
	const int check = __max(1, n / 8);
	for (int z = 0; z < n; ++z) {
		for (int x = 0; x < n; ++x) {
			const int i = z * n + x;
			const float shade = ((x / check + z / check) & 1) ? 0.78f : 1.0f;
			const vec3f_t color = vec3fScale(info->color, shade);
			cloth->vertices[i * 3 + 2] = color;
			cloth->vertices[(vertex_count + i) * 3 + 2] = vec3fScale(color, 0.7f);
		}
	}

	cloth->mesh.layout = GPU_MESH_LAYOUT_TRI_P444_N444_C444_I2;
	cloth->mesh.dynamic = true;
	cloth->mesh.id = gpuMeshInfoNextId();
	cloth->mesh.vtx_data = cloth->vertices;
	cloth->mesh.vtx_data_size = sizeof(vec3f_t) * vertex_count * 2 * 3;
	cloth->mesh.idx_data = cloth->indices;
	cloth->mesh.idx_data_size = sizeof(uint16_t) * index_count;
	clothUpdateMesh(cloth);
	return cloth;
}

void clothDestroy(cloth_t* cloth) {
	if (!cloth) return;
	heapFree(cloth->heap, cloth->indices);
	heapFree(cloth->heap, cloth->normals);
	heapFree(cloth->heap, cloth->vertices);
	heapFree(cloth->heap, cloth);
}

void clothUpdateMesh(cloth_t* cloth) {
	const int n = cloth->resolution;
	const int vertex_count = n * n;
	const vec3f_t* positions = physicsGetParticlePositions(cloth->physics) + cloth->first_particle;
	vec3f_t* normals = cloth->normals;
	for (int i = 0; i < vertex_count; ++i) normals[i] = vec3fZero();
	/* Area-weighted vertex normals from the front faces (the first half of the
	   indices), oriented toward +y while the cloth lies flat. */
	const int front_index_count = (n - 1) * (n - 1) * 6;
	for (int i = 0; i < front_index_count; i += 3) {
		const uint16_t a = cloth->indices[i];
		const uint16_t b = cloth->indices[i + 1];
		const uint16_t c = cloth->indices[i + 2];
		assert(a < vertex_count && b < vertex_count && c < vertex_count);
		const vec3f_t face = vec3fCross(vec3fSub(positions[c], positions[a]),
			vec3fSub(positions[b], positions[a]));
		normals[a] = vec3fAdd(normals[a], face);
		normals[b] = vec3fAdd(normals[b], face);
		normals[c] = vec3fAdd(normals[c], face);
	}
	/* The collision surface is the particles' spheres, so each side is drawn
	   one radius out from the particle centers. */
	const float offset = cloth->particle_radius;
	for (int i = 0; i < vertex_count; ++i) {
		const vec3f_t normal = vec3fMagnitudeSqrd(normals[i]) > 0.0f ?
			vec3fNorm(normals[i]) : vec3fY();
		cloth->vertices[i * 3] = vec3fAdd(positions[i], vec3fScale(normal, offset));
		cloth->vertices[i * 3 + 1] = normal;
		cloth->vertices[(vertex_count + i) * 3] = vec3fSub(positions[i], vec3fScale(normal, offset));
		cloth->vertices[(vertex_count + i) * 3 + 1] = vec3fScale(normal, -1.0f);
	}
}

gpu_mesh_info_t* clothGetMesh(cloth_t* cloth) {
	return &cloth->mesh;
}
