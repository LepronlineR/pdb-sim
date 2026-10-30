#include "scene.h"

#include "ball.h"
#include "cloth.h"
#include "transform.h"
#include "component.h"
#include "debug.h"
#include "ecs.h"
#include "fs.h"
#include "gpu.h"
#include "heap.h"
#include "physics.h"
#include "renderer.h"
#include "shader_loader.h"
#include "texture_library.h"
#include "timer.h"
#include "timer_object.h"
#include "ui.h"
#include "vec3f.h"
#include "wm.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_FIELD_OF_VIEW ((float)M_PI / 3.0f)
#define SCENE_DEFAULT_ASPECT_RATIO (16.0f / 9.0f)
#define SCENE_FIXED_STEP (1.0f / 60.0f)
#define SCENE_MAX_FRAME_STEP 0.05f
/* Particles within this distance of the picked one are grabbed with it, so a
   drag pulls a patch of the surface instead of one stretched vertex. */
#define SCENE_GRAB_RADIUS 0.35f
/* A left press and release that moved less than this many pixels is a click. */
#define SCENE_CLICK_SLOP 4
/* Resolution of the shared unit-sphere mesh used to draw sphere colliders. */
#define SCENE_SPHERE_SEGMENTS 40
#define SCENE_SPHERE_RINGS 20
/* Texture repeats across the ground's top face (24 m wide): one every 2 m. */
#define SCENE_GROUND_UV_SCALE 12.0f

enum {
	SCENE_MAX_OBJECTS = UI_MAX_OBJECTS
};

typedef enum scene_mode_t {
	SCENE_MODE_EDIT = UI_MODE_EDIT,
	SCENE_MODE_PLAYING = UI_MODE_PLAYING,
	SCENE_MODE_PAUSED = UI_MODE_PAUSED
} scene_mode_t;

typedef enum scene_tool_t {
	SCENE_TOOL_MOVE = UI_TOOL_MOVE,  // move gizmo on the selection
	SCENE_TOOL_PIN = UI_TOOL_PIN     // clicking a cloth toggles a pin
} scene_tool_t;

typedef enum scene_object_kind_t {
	SCENE_OBJECT_BALL,
	SCENE_OBJECT_CLOTH,
	SCENE_OBJECT_SPHERE   // a solid, static sphere collider (no soft body)
} scene_object_kind_t;

typedef struct scene_sphere_info_t {
	vec3f_t center;
	vec3f_t color;
	float radius;
} scene_sphere_info_t;

/* How an object's surface is drawn. Visual only: editing it never changes the
   simulation. */
typedef struct scene_material_t {
	texture_id_t texture;
	float uv_scale;  // texture repeats per unit of the mesh's uvs
} scene_material_t;

/* An authored object. The settings are the source of truth; the resource,
   mesh and particle range are runtime data rebuilt with the physics world. */
typedef struct scene_object_t {
	int id;
	scene_object_kind_t kind;
	char name[UI_NAME_LENGTH];
	ball_info_t ball;
	cloth_info_t cloth;
	scene_sphere_info_t sphere;
	scene_material_t material;
	bool transient;  // added during play: removed on Stop
	bool hidden;     // deleted during play: restored on Stop
	ecs_entity_t entity;
	void* resource;  // ball_t* or cloth_t*; NULL for spheres
	int collider;    // spheres: physics collider index, -1 when not created
	gpu_mesh_info_t* mesh;
	physics_particle_id_t first_particle;
	int particle_count;
} scene_object_t;

typedef struct scene_t {
	heap_t* heap;
	fs_t* fs;
	wm_window_t* window;
	renderer_t* render;
	timer_object_t* timer;

	// entity component system
	ecs_t* ecs;
	int transform_type;
	int camera_type;
	int model_type;
	int name_type;
	int physics_type;
	ecs_entity_t camera_entity;
	ecs_entity_t physics_entity;

	shader_loader_t* shaders;
	texture_library_t* textures;
	gpu_mesh_info_t cube_mesh;    // positions only matter: the sky box
	gpu_mesh_info_t ground_mesh;  // a box with per-face normals and uvs
	gpu_vertex_t ground_vertices[24];
	uint16_t ground_indices[36];
	gpu_mesh_info_t sphere_mesh;
	gpu_vertex_t* sphere_vertices;
	uint16_t* sphere_indices;
	ecs_entity_t sky_entity;
	ecs_entity_t ground_entity;
	ui_environment_settings_t environment;
	// width / height of the window's client area, used by projection and picking
	float aspect_ratio;

	scene_mode_t mode;
	scene_tool_t tool;
	scene_object_t objects[SCENE_MAX_OBJECTS];
	int object_count;
	int next_object_id;
	int ball_counter;
	int cloth_counter;
	int sphere_counter;
	int selected_id;
	uint32_t selected_revision;

	// interaction
	bool pointer_overridden;
	scene_pointer_t pointer_override;
	bool select_was_held;
	int select_travel;
	bool grabbing;       // play: a physics grab is active
	bool moving;         // edit: dragging an object around
	int moving_id;
	float grab_distance;
	vec3f_t move_last_point;
	// move gizmo: three rods then three tips, drawn with the sphere mesh
	ecs_entity_t gizmo_entities[6];
	int gizmo_hover_axis;  // -1 when no handle is under the cursor
	int gizmo_drag_axis;   // -1 when no handle is being dragged
	float gizmo_drag_param;

	// stats, averaged over about a second
	float physics_ms_average;
	float dropped_ms_window;
	float dropped_ms_reported;
	float stats_window_seconds;
} scene_t;

typedef struct scene_draw_uniform_t {
	mat4f_t projection;
	mat4f_t model;
	mat4f_t view;
	float camera_position_roughness[4];
	float light_direction_metallic[4];
	float light_color_ambient[4];
	float material[4];
	float texture_params[4];  // model_component_t.texture_params
} scene_draw_uniform_t;

static void sceneBuildSphereMesh(scene_t* scene);
static void sceneUpdateGizmo(scene_t* scene);
static void sceneUpdateEnvironment(scene_t* scene, ui_t* ui);

static physics_world_component_t* sceneGetWorld(scene_t* scene) {
	return ecsEntityGet(scene->ecs, scene->physics_entity, scene->physics_type, true);
}

static physics_t* sceneGetPhysics(scene_t* scene) {
	return sceneGetWorld(scene)->physics;
}

static scene_object_t* sceneFindObject(scene_t* scene, int id) {
	for (int i = 0; i < scene->object_count; ++i) {
		if (scene->objects[i].id == id) return &scene->objects[i];
	}
	return NULL;
}

static vec3f_t sceneObjectPosition(const scene_object_t* object) {
	switch (object->kind) {
		case SCENE_OBJECT_BALL: return object->ball.position;
		case SCENE_OBJECT_CLOTH: return object->cloth.center;
		default: return object->sphere.center;
	}
}

static void sceneObjectSetPosition(scene_object_t* object, vec3f_t position) {
	switch (object->kind) {
		case SCENE_OBJECT_BALL: object->ball.position = position; break;
		case SCENE_OBJECT_CLOTH: object->cloth.center = position; break;
		default: object->sphere.center = position; break;
	}
}

/* Spheres are drawn with the shared unit mesh, placed and sized by the model's
   transform and colored by its tint; soft bodies carry world-space vertices. */
static void sceneObjectUpdateModel(scene_t* scene, scene_object_t* object) {
	model_component_t* model = ecsEntityGet(scene->ecs, object->entity, scene->model_type, true);
	transform_component_t* transform = ecsEntityGet(scene->ecs, object->entity,
		scene->transform_type, true);
	if (!model || !transform) return;
	model->mesh_info = object->mesh;
	model->texture = textureLibraryGet(scene->textures, object->material.texture);
	model->texture_params[0] = object->material.uv_scale;
	transformIdentity(&transform->transform);
	model->tint[0] = model->tint[1] = model->tint[2] = 1.0f;
	if (object->kind == SCENE_OBJECT_SPHERE) {
		const float r = object->sphere.radius;
		transform->transform.translation = object->sphere.center;
		transform->transform.scale = (vec3f_t){ r, r, r };
		model->tint[0] = object->sphere.color.x;
		model->tint[1] = object->sphere.color.y;
		model->tint[2] = object->sphere.color.z;
	}
}

//  --------------------------------------------------------------------------
//								RUNTIME OBJECTS
//

/* Creates the object's ball or cloth in the current physics world and points
   its model at the new mesh. Fails when the world is out of capacity. */
static bool sceneObjectCreateRuntime(scene_t* scene, scene_object_t* object) {
	physics_t* physics = sceneGetPhysics(scene);
	object->resource = NULL;
	object->collider = -1;
	object->mesh = NULL;
	object->particle_count = 0;
	if (object->kind == SCENE_OBJECT_SPHERE) {
		object->collider = physicsAddSphereCollider(physics, object->sphere.center,
			object->sphere.radius);
		if (object->collider >= 0) object->mesh = &scene->sphere_mesh;
	}
	else if (object->kind == SCENE_OBJECT_BALL) {
		ball_t* ball = ballCreate(scene->heap, physics, &object->ball);
		if (ball) {
			object->resource = ball;
			object->mesh = ballGetMesh(ball);
			ballGetParticles(ball, &object->first_particle, &object->particle_count);
		}
	}
	else {
		cloth_t* cloth = clothCreate(scene->heap, physics, &object->cloth);
		if (cloth) {
			object->resource = cloth;
			object->mesh = clothGetMesh(cloth);
			clothGetParticles(cloth, &object->first_particle, &object->particle_count);
		}
	}
	sceneObjectUpdateModel(scene, object);
	return object->mesh != NULL;
}

static void sceneObjectDestroyRuntime(scene_t* scene, scene_object_t* object) {
	if (object->resource) {
		if (object->kind == SCENE_OBJECT_BALL) ballDestroy((ball_t*)object->resource);
		else clothDestroy((cloth_t*)object->resource);
	}
	object->resource = NULL;
	object->collider = -1;
	object->mesh = NULL;
	object->particle_count = 0;
	model_component_t* model = ecsEntityGet(scene->ecs, object->entity, scene->model_type, true);
	if (model) model->mesh_info = NULL;
}

static void sceneObjectUpdateMesh(scene_object_t* object) {
	if (!object->resource) return;
	if (object->kind == SCENE_OBJECT_BALL) ballUpdateMesh((ball_t*)object->resource);
	else clothUpdateMesh((cloth_t*)object->resource);
}

static physics_t* sceneCreatePhysics(heap_t* heap) {
	physics_t* physics = physicsCreate(heap);
	physicsSetGroundCompliance(physics, 0.0f);
	return physics;
}

/* Rebuilds the physics world and every visible object's ball or cloth from the
   authored settings. With keep_state, objects that survive the rebuild carry
   their current particle positions and velocities across, so deleting an
   object during play does not disturb the others. */
static void sceneRebuild(scene_t* scene, bool keep_state) {
	rendererWaitIdle(scene->render);
	physics_t* old_physics = sceneGetPhysics(scene);

	vec3f_t* saved = NULL;
	int saved_offset[SCENE_MAX_OBJECTS];
	int saved_count[SCENE_MAX_OBJECTS];
	if (keep_state) {
		int total = 0;
		for (int i = 0; i < scene->object_count; ++i) total += scene->objects[i].particle_count;
		saved = heapAlloc(scene->heap, sizeof(vec3f_t) * 2 * (size_t)__max(total, 1),
			_Alignof(vec3f_t));
		int offset = 0;
		for (int i = 0; i < scene->object_count; ++i) {
			const scene_object_t* object = &scene->objects[i];
			saved_offset[i] = offset;
			for (int k = 0; k < object->particle_count; ++k) {
				const physics_particle_id_t particle = object->first_particle + (physics_particle_id_t)k;
				saved[offset * 2] = physicsGetParticlePosition(old_physics, particle);
				saved[offset * 2 + 1] = physicsGetParticleVelocity(old_physics, particle);
				++offset;
			}
		}
	}

	for (int i = 0; i < scene->object_count; ++i) {
		saved_count[i] = scene->objects[i].particle_count;
		sceneObjectDestroyRuntime(scene, &scene->objects[i]);
	}
	physicsDestroy(old_physics);
	physics_world_component_t* world = sceneGetWorld(scene);
	world->physics = sceneCreatePhysics(scene->heap);
	world->accumulator = 0.0f;
	scene->grabbing = false;

	physics_t* physics = world->physics;
	for (int i = 0; i < scene->object_count; ++i) {
		scene_object_t* object = &scene->objects[i];
		if (object->hidden) continue;
		sceneObjectCreateRuntime(scene, object);
		if (saved && object->particle_count == saved_count[i]) {
			for (int k = 0; k < object->particle_count; ++k) {
				const physics_particle_id_t particle = object->first_particle + (physics_particle_id_t)k;
				const int at = (saved_offset[i] + k) * 2;
				physicsSetParticlePosition(physics, particle, saved[at]);
				physicsSetParticleVelocity(physics, particle, saved[at + 1]);
			}
			sceneObjectUpdateMesh(object);
		}
	}
	if (saved) heapFree(scene->heap, saved);
}

//  --------------------------------------------------------------------------
//								SCENE OBJECTS
//

static void sceneSelect(scene_t* scene, int id) {
	const scene_object_t* object = sceneFindObject(scene, id);
	if (!object || object->hidden) id = -1;
	if (scene->selected_id != id) {
		scene->selected_id = id;
		scene->selected_revision++;
	}
}

/* Adds an object. In Edit it becomes part of the authored scene; while playing
   it is transient and disappears on Stop. */
static scene_object_t* sceneAddObject(scene_t* scene, scene_object_kind_t kind,
	const ball_info_t* ball, const cloth_info_t* cloth, const scene_sphere_info_t* sphere,
	const char* name) {
	if (scene->object_count >= SCENE_MAX_OBJECTS) return NULL;
	scene_object_t* object = &scene->objects[scene->object_count];
	memset(object, 0, sizeof(*object));
	object->id = scene->next_object_id++;
	object->kind = kind;
	if (ball) object->ball = *ball;
	if (cloth) object->cloth = *cloth;
	if (sphere) object->sphere = *sphere;
	/* Cloth shows the checker by default so its folds read clearly. */
	object->material = (scene_material_t){
		kind == SCENE_OBJECT_CLOTH ? TEXTURE_CHECKER : TEXTURE_NONE, 1.0f
	};
	object->transient = scene->mode != SCENE_MODE_EDIT;
	strcpy_s(object->name, sizeof(object->name), name);

	uint64_t mask = (1ULL << scene->transform_type) | (1ULL << scene->model_type) |
		(1ULL << scene->name_type);
	object->entity = ecsEntityAdd(scene->ecs, mask);
	transform_component_t* transform = ecsEntityGet(scene->ecs, object->entity, scene->transform_type, true);
	model_component_t* model = ecsEntityGet(scene->ecs, object->entity, scene->model_type, true);
	name_component_t* name_component = ecsEntityGet(scene->ecs, object->entity, scene->name_type, true);
	transformIdentity(&transform->transform);
	memset(model, 0, sizeof(*model));
	model->shader_info = shaderLoaderGetPbr(scene->shaders);
	strcpy_s(name_component->name, sizeof(name_component->name), name);

	/* The world is append-only, so adding needs no rebuild. */
	if (!sceneObjectCreateRuntime(scene, object)) {
		debugPrint(DEBUG_PRINT_WARNING, "Scene: no room for %s; the physics world is full.\n", name);
		ecsEntityRemove(scene->ecs, object->entity, true);
		return NULL;
	}
	scene->object_count++;
	return object;
}

static void sceneRemoveObjectAt(scene_t* scene, int index) {
	scene_object_t* object = &scene->objects[index];
	sceneObjectDestroyRuntime(scene, object);
	ecsEntityRemove(scene->ecs, object->entity, true);
	if (scene->selected_id == object->id) sceneSelect(scene, -1);
	for (int i = index; i + 1 < scene->object_count; ++i) {
		scene->objects[i] = scene->objects[i + 1];
	}
	scene->object_count--;
}

/* In Edit the object leaves the authored scene. While playing, authored objects
   are only hidden (Stop brings them back) and transient ones are removed. */
static void sceneDeleteObject(scene_t* scene, int id) {
	scene_object_t* object = sceneFindObject(scene, id);
	if (!object || object->hidden) return;
	rendererWaitIdle(scene->render);
	if (scene->mode == SCENE_MODE_EDIT || object->transient) {
		sceneRemoveObjectAt(scene, (int)(object - scene->objects));
	}
	else {
		object->hidden = true;
		if (scene->selected_id == id) sceneSelect(scene, -1);
	}
	if (scene->moving && scene->moving_id == id) scene->moving = false;
	sceneRebuild(scene, scene->mode != SCENE_MODE_EDIT);
}

static void sceneClearObjects(scene_t* scene) {
	rendererWaitIdle(scene->render);
	while (scene->object_count > 0) sceneRemoveObjectAt(scene, scene->object_count - 1);
	scene->ball_counter = 0;
	scene->cloth_counter = 0;
	scene->sphere_counter = 0;
	scene->moving = false;
	sceneRebuild(scene, false);
}

static vec3f_t sceneSpawnPoint(scene_t* scene) {
	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity,
		scene->camera_type, true);
	return (vec3f_t){ camera->orbit_target[0], camera->orbit_target[1], camera->orbit_target[2] };
}

static void sceneAddBall(scene_t* scene) {
	static const vec3f_t colors[] = {
		{ .95f, .30f, .22f }, { .25f, .82f, .42f }, { .20f, .35f, .90f }, { .90f, .82f, .12f },
		{ .75f, .35f, .85f }, { .95f, .55f, .15f }
	};
	const ball_info_t info = ballInfoDefault(sceneSpawnPoint(scene),
		colors[scene->ball_counter % _countof(colors)], 1.0f);
	char name[UI_NAME_LENGTH];
	sprintf_s(name, sizeof(name), "Ball %d", ++scene->ball_counter);
	scene_object_t* object = sceneAddObject(scene, SCENE_OBJECT_BALL, &info, NULL, NULL, name);
	if (object) sceneSelect(scene, object->id);
}

static void sceneAddCloth(scene_t* scene) {
	vec3f_t center = sceneSpawnPoint(scene);
	center.y += 2.5f;
	const cloth_info_t info = clothInfoDefault(center, (vec3f_t){ .92f, .86f, .70f });
	char name[UI_NAME_LENGTH];
	sprintf_s(name, sizeof(name), "Cloth %d", ++scene->cloth_counter);
	scene_object_t* object = sceneAddObject(scene, SCENE_OBJECT_CLOTH, NULL, &info, NULL, name);
	if (object) sceneSelect(scene, object->id);
}

static void sceneAddSphere(scene_t* scene) {
	const scene_sphere_info_t info = {
		.center = sceneSpawnPoint(scene), .color = { .55f, .58f, .64f }, .radius = 1.0f
	};
	char name[UI_NAME_LENGTH];
	sprintf_s(name, sizeof(name), "Sphere %d", ++scene->sphere_counter);
	scene_object_t* object = sceneAddObject(scene, SCENE_OBJECT_SPHERE, NULL, NULL, &info, name);
	if (object) sceneSelect(scene, object->id);
}

/* A free cloth above a solid sphere: press Play and it falls and drapes. */
static void sceneLoadDrapeDemo(scene_t* scene) {
	sceneClearObjects(scene);
	const scene_sphere_info_t sphere = {
		.center = { 0, 1.2f, 0 }, .color = { .55f, .58f, .64f }, .radius = 1.2f
	};
	sceneAddObject(scene, SCENE_OBJECT_SPHERE, NULL, NULL, &sphere, "Sphere");
	cloth_info_t cloth = clothInfoDefault((vec3f_t){ 0, 3.6f, 0 }, (vec3f_t){ .80f, .22f, .26f });
	cloth.resolution = 48;
	cloth.size = 4.4f;
	cloth.pin = CLOTH_PIN_NONE;
	sceneAddObject(scene, SCENE_OBJECT_CLOTH, NULL, &cloth, NULL, "Tablecloth");
	scene->sphere_counter = 1;
	scene->cloth_counter = 1;
	sceneSelect(scene, -1);
}

/* A trampoline (cloth pinned along its border) with two balls above it, a
   banner pinned along one edge, and two balls on the ground. */
static void sceneLoadDemo(scene_t* scene) {
	sceneClearObjects(scene);
	cloth_info_t trampoline = clothInfoDefault((vec3f_t){0,3,0}, (vec3f_t){.92f,.86f,.70f});
	trampoline.pin = CLOTH_PIN_BORDER;
	trampoline.stretch_compliance = 3.0e-3f;
	trampoline.shear_compliance = 3.0e-3f;
	sceneAddObject(scene, SCENE_OBJECT_CLOTH, NULL, &trampoline, NULL, "Trampoline");
	cloth_info_t banner = clothInfoDefault((vec3f_t){-4.5f,5.5f,1.5f}, (vec3f_t){.78f,.20f,.24f});
	banner.resolution = 24;
	banner.size = 2.5f;
	banner.pin = CLOTH_PIN_EDGE;
	sceneAddObject(scene, SCENE_OBJECT_CLOTH, NULL, &banner, NULL, "Banner");

	ball_info_t ball = ballInfoDefault((vec3f_t){-.85f,5,.45f}, (vec3f_t){.95f,.30f,.22f}, 1.0f);
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, NULL, "Red ball");
	ball = ballInfoDefault((vec3f_t){.85f,6.5f,-.45f}, (vec3f_t){.25f,.82f,.42f}, 1.5f);
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, NULL, "Green ball");
	ball = ballInfoDefault((vec3f_t){-3,1.2f,-3}, (vec3f_t){.1f,.13f,.72f}, 0.4f);
	ball.radius = 0.6f;
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, NULL, "Small ball");
	ball = ballInfoDefault((vec3f_t){4.5f,1.2f,1}, (vec3f_t){.9f,.82f,.12f}, 0.5f);
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, NULL, "Yellow ball");
	scene->ball_counter = 4;
	scene->cloth_counter = 2;
	sceneSelect(scene, -1);
}

static ui_object_properties_t sceneObjectProperties(const scene_object_t* object) {
	ui_object_properties_t p;
	memset(&p, 0, sizeof(p));
	p.id = object->id;
	p.kind = object->kind == SCENE_OBJECT_BALL ? UI_OBJECT_BALL :
		object->kind == SCENE_OBJECT_CLOTH ? UI_OBJECT_CLOTH : UI_OBJECT_SPHERE;
	strcpy_s(p.name, sizeof(p.name), object->name);
	const vec3f_t position = sceneObjectPosition(object);
	p.position[0] = position.x;
	p.position[1] = position.y;
	p.position[2] = position.z;
	if (object->kind == SCENE_OBJECT_BALL) {
		const ball_info_t* b = &object->ball;
		p.color[0] = b->color.x; p.color[1] = b->color.y; p.color[2] = b->color.z;
		p.mass = b->mass;
		p.velocity_retention = b->velocity_retention;
		p.stretch_compliance = b->structural_compliance;
		p.bending_compliance = b->bending_compliance;
		p.radius = b->radius;
		p.volume_compliance = b->volume_compliance;
	}
	else if (object->kind == SCENE_OBJECT_SPHERE) {
		const scene_sphere_info_t* sphere = &object->sphere;
		p.color[0] = sphere->color.x; p.color[1] = sphere->color.y; p.color[2] = sphere->color.z;
		p.radius = sphere->radius;
	}
	else {
		const cloth_info_t* c = &object->cloth;
		p.color[0] = c->color.x; p.color[1] = c->color.y; p.color[2] = c->color.z;
		p.mass = c->mass;
		p.velocity_retention = c->velocity_retention;
		p.stretch_compliance = c->stretch_compliance;
		p.bending_compliance = c->bend_compliance;
		p.resolution = c->resolution;
		p.size = c->size;
		p.shear_compliance = c->shear_compliance;
		p.drag = c->drag;
		p.pin = (int)c->pin;
		p.pin_count = clothInfoCountPins(c);
	}
	p.texture = (int)object->material.texture;
	p.uv_scale = object->material.uv_scale;
	return p;
}

/* Applies inspector edits to an authored object; values are clamped to what the
   object builders accept. Only valid in Edit. */
static void sceneEditObject(scene_t* scene, const ui_object_properties_t* p) {
	scene_object_t* object = sceneFindObject(scene, p->id);
	if (!object || scene->mode != SCENE_MODE_EDIT) return;
	const vec3f_t position = { p->position[0], p->position[1], p->position[2] };
	const vec3f_t color = { p->color[0], p->color[1], p->color[2] };
	sceneObjectSetPosition(object, position);
	object->material.texture = (texture_id_t)__max(TEXTURE_NONE, __min(p->texture, TEXTURE_SKY - 1));
	object->material.uv_scale = __max(0.1f, __min(20.0f, p->uv_scale));
	if (object->kind == SCENE_OBJECT_BALL) {
		ball_info_t* b = &object->ball;
		b->color = color;
		b->mass = __max(0.01f, p->mass);
		b->velocity_retention = __max(0.0f, __min(1.0f, p->velocity_retention));
		b->structural_compliance = __max(0.0f, p->stretch_compliance);
		b->bending_compliance = __max(0.0f, p->bending_compliance);
		b->radius = __max(0.1f, __min(5.0f, p->radius));
		b->volume_compliance = __max(0.0f, p->volume_compliance);
	}
	else if (object->kind == SCENE_OBJECT_SPHERE) {
		object->sphere.color = color;
		object->sphere.radius = __max(0.1f, __min(10.0f, p->radius));
	}
	else {
		cloth_info_t* c = &object->cloth;
		c->color = color;
		c->mass = __max(0.01f, p->mass);
		c->velocity_retention = __max(0.0f, __min(1.0f, p->velocity_retention));
		c->stretch_compliance = __max(0.0f, p->stretch_compliance);
		c->bend_compliance = __max(0.0f, p->bending_compliance);
		const int resolution = __max(4, __min(64, p->resolution));
		/* Hand-placed pins are grid points, which mean nothing at another
		   resolution. */
		if (resolution != c->resolution) memset(c->pin_mask, 0, sizeof(c->pin_mask));
		c->resolution = resolution;
		c->size = __max(0.5f, __min(10.0f, p->size));
		c->shear_compliance = __max(0.0f, p->shear_compliance);
		c->drag = __max(0.0f, p->drag);
		c->pin = (cloth_pin_t)__max(0, __min(p->pin, CLOTH_PIN_COUNT - 1));
	}
	sceneRebuild(scene, false);
}

//  --------------------------------------------------------------------------
//								PLAY MODE
//

static void scenePlay(scene_t* scene) {
	if (scene->mode != SCENE_MODE_EDIT) return;
	scene->moving = false;
	scene->gizmo_drag_axis = -1;
	scene->mode = SCENE_MODE_PLAYING;
	sceneGetWorld(scene)->accumulator = 0.0f;
}

/* Drops everything that happened during play: transient objects go, hidden ones
   come back, and the world is rebuilt from the authored settings. */
static void sceneStop(scene_t* scene) {
	if (scene->mode == SCENE_MODE_EDIT) return;
	rendererWaitIdle(scene->render);
	for (int i = scene->object_count - 1; i >= 0; --i) {
		if (scene->objects[i].transient) sceneRemoveObjectAt(scene, i);
		else scene->objects[i].hidden = false;
	}
	scene->mode = SCENE_MODE_EDIT;
	sceneRebuild(scene, false);
	scene->selected_revision++;
}

static void sceneHandleCommands(scene_t* scene, ui_t* ui) {
	ui_command_t command;
	while (uiPopCommand(ui, &command)) {
		switch (command.type) {
			case UI_COMMAND_PLAY: scenePlay(scene); break;
			case UI_COMMAND_STOP: sceneStop(scene); break;
			case UI_COMMAND_PAUSE:
				if (scene->mode == SCENE_MODE_PLAYING) scene->mode = SCENE_MODE_PAUSED;
				break;
			case UI_COMMAND_RESUME:
				if (scene->mode == SCENE_MODE_PAUSED) scene->mode = SCENE_MODE_PLAYING;
				break;
			case UI_COMMAND_STEP:
				if (scene->mode == SCENE_MODE_PAUSED) physicsUpdate(sceneGetPhysics(scene), SCENE_FIXED_STEP);
				break;
			case UI_COMMAND_ADD_BALL: sceneAddBall(scene); break;
			case UI_COMMAND_ADD_CLOTH: sceneAddCloth(scene); break;
			case UI_COMMAND_ADD_SPHERE: sceneAddSphere(scene); break;
			case UI_COMMAND_LOAD_DRAPE_DEMO:
				if (scene->mode == SCENE_MODE_EDIT) sceneLoadDrapeDemo(scene);
				break;
			case UI_COMMAND_LOAD_DEMO:
				if (scene->mode == SCENE_MODE_EDIT) sceneLoadDemo(scene);
				break;
			case UI_COMMAND_CLEAR:
				if (scene->mode == SCENE_MODE_EDIT) sceneClearObjects(scene);
				break;
			case UI_COMMAND_SELECT: sceneSelect(scene, command.id); break;
			case UI_COMMAND_SET_TOOL:
				scene->tool = command.id == UI_TOOL_PIN ? SCENE_TOOL_PIN : SCENE_TOOL_MOVE;
				break;
			case UI_COMMAND_CLEAR_PINS: {
				scene_object_t* object = sceneFindObject(scene, command.id);
				if (object && object->kind == SCENE_OBJECT_CLOTH && scene->mode == SCENE_MODE_EDIT) {
					memset(object->cloth.pin_mask, 0, sizeof(object->cloth.pin_mask));
					scene->selected_revision++;
					sceneRebuild(scene, false);
				}
				break;
			}
			case UI_COMMAND_DELETE: sceneDeleteObject(scene, command.id); break;
			case UI_COMMAND_EDIT: sceneEditObject(scene, &command.properties); break;
		}
	}
}

//  --------------------------------------------------------------------------
//								SCENE LIFETIME
//

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render) {
	scene_t* scene = heapAlloc(heap, sizeof(*scene), 8);
	memset(scene, 0, sizeof(*scene));
	scene->heap = heap;
	scene->fs = fs;
	scene->window = window;
	scene->render = render;
	scene->aspect_ratio = SCENE_DEFAULT_ASPECT_RATIO;
	scene->mode = SCENE_MODE_EDIT;
	scene->selected_id = -1;
	scene->next_object_id = 1;
	scene->timer = timerObjectCreate(heap, NULL);
	scene->ecs = ecsCreate(heap);
	scene->transform_type = ecsComponentRegister(scene->ecs, "transform",
		sizeof(transform_component_t), _Alignof(transform_component_t));
	scene->camera_type = ecsComponentRegister(scene->ecs, "camera",
		sizeof(camera_component_t), _Alignof(camera_component_t));
	scene->model_type = ecsComponentRegister(scene->ecs, "model",
		sizeof(model_component_t), _Alignof(model_component_t));
	scene->name_type = ecsComponentRegister(scene->ecs, "name",
		sizeof(name_component_t), _Alignof(name_component_t));
	scene->physics_type = ecsComponentRegister(scene->ecs, "physics_world",
		sizeof(physics_world_component_t), _Alignof(physics_world_component_t));

	scene->physics_entity = ecsEntityAdd(scene->ecs,
		(1ULL << scene->physics_type) | (1ULL << scene->name_type));
	physics_world_component_t* world = sceneGetWorld(scene);
	world->physics = sceneCreatePhysics(heap);
	world->accumulator = 0.0f;
	name_component_t* physics_name = ecsEntityGet(scene->ecs, scene->physics_entity,
		scene->name_type, true);
	strcpy_s(physics_name->name, sizeof(physics_name->name), "physics world");

	sceneLoadResources(scene);
	sceneSpawnCamera(scene);
	scene->sky_entity = sceneSpawnModel(scene, "sky", vec3fZero(), vec3fOne(), NULL);
	model_component_t* sky = ecsEntityGet(scene->ecs, scene->sky_entity, scene->model_type, true);
	sky->shader_info = shaderLoaderGetSkybox(scene->shaders);
	scene->ground_entity = sceneSpawnModel(scene, "ground", (vec3f_t){0,-.1f,0},
		(vec3f_t){12,.1f,12}, &scene->ground_mesh);
	model_component_t* ground = ecsEntityGet(scene->ecs, scene->ground_entity, scene->model_type, true);
	ground->shader_info = shaderLoaderGetPbr(scene->shaders);
	ground->texture_params[0] = SCENE_GROUND_UV_SCALE;
	sceneUpdateEnvironment(scene, uiGet(scene->window));
	scene->gizmo_hover_axis = -1;
	scene->gizmo_drag_axis = -1;
	for (int part = 0; part < 6; ++part) {
		scene->gizmo_entities[part] = sceneSpawnModel(scene, "move gizmo", vec3fZero(), vec3fOne(), NULL);
		model_component_t* model = ecsEntityGet(scene->ecs, scene->gizmo_entities[part],
			scene->model_type, true);
		model->shader_info = shaderLoaderGetPbr(scene->shaders);
	}
	return scene;
}

void sceneDestroy(scene_t* scene) {
	for (int i = 0; i < scene->object_count; ++i) {
		sceneObjectDestroyRuntime(scene, &scene->objects[i]);
	}
	physicsDestroy(sceneGetPhysics(scene));
	ecsDestroy(scene->ecs);
	timerObjectDestroy(scene->timer);
	sceneUnloadResources(scene);
	heapFree(scene->heap, scene);
}

void sceneUpdate(scene_t* scene) {
	timerObjectUpdate(scene->timer);
	ecsUpdate(scene->ecs);
	sceneHandleCommands(scene, uiGet(scene->window));
	sceneUpdateCamera(scene);
	sceneUpdateInteraction(scene);
	sceneUpdateGizmo(scene);
	sceneUpdateSimulation(scene);
	sceneUpdateEnvironment(scene, uiGet(scene->window));
	sceneDrawModels(scene);
	rendererFrameDone(scene->render);
}

/* Applies the Environment panel: the sky's visibility, exposure and rotation,
   and the ground's texture (the ground keeps its plain color without one). */
static void sceneUpdateEnvironment(scene_t* scene, ui_t* ui) {
	scene->environment = uiGetEnvironmentSettings(ui);
	const ui_environment_settings_t* environment = &scene->environment;
	model_component_t* sky = ecsEntityGet(scene->ecs, scene->sky_entity, scene->model_type, true);
	sky->texture = textureLibraryGet(scene->textures, TEXTURE_SKY);
	sky->mesh_info = environment->sky && sky->texture ? &scene->cube_mesh : NULL;
	sky->texture_params[0] = environment->sky_exposure;
	sky->texture_params[1] = environment->sky_rotation * (float)M_PI / 180.0f;

	model_component_t* ground = ecsEntityGet(scene->ecs, scene->ground_entity, scene->model_type, true);
	const texture_id_t ground_texture = (texture_id_t)__max(TEXTURE_NONE,
		__min(environment->ground_texture, TEXTURE_SKY - 1));
	ground->texture = textureLibraryGet(scene->textures, ground_texture);
	static const float plain_ground[3] = { .18f, .42f, .62f };
	for (int k = 0; k < 3; ++k) ground->tint[k] = ground->texture ? 1.0f : plain_ground[k];
}

/* A box from -1 to 1 with four vertices per face, so each face has its own
   normal and a full 0..1 uv square. Faces wind counter-clockwise seen from
   outside, like the other meshes. */
static void sceneBuildGroundMesh(scene_t* scene) {
	/* normal, then the face's u and v axes (u x v = normal) */
	static const float faces[6][3][3] = {
		{ { 1,0,0 }, { 0,0,-1 }, { 0,1,0 } }, { { -1,0,0 }, { 0,0,1 }, { 0,1,0 } },
		{ { 0,1,0 }, { 1,0,0 }, { 0,0,-1 } }, { { 0,-1,0 }, { 1,0,0 }, { 0,0,1 } },
		{ { 0,0,1 }, { 1,0,0 }, { 0,1,0 } }, { { 0,0,-1 }, { -1,0,0 }, { 0,1,0 } }
	};
	static const float corners[4][2] = { { -1,-1 }, { 1,-1 }, { 1,1 }, { -1,1 } };
	for (int face = 0; face < 6; ++face) {
		const float* n = faces[face][0];
		const float* a = faces[face][1];
		const float* b = faces[face][2];
		for (int corner = 0; corner < 4; ++corner) {
			const float s = corners[corner][0];
			const float t = corners[corner][1];
			gpu_vertex_t* vertex = &scene->ground_vertices[face * 4 + corner];
			for (int k = 0; k < 3; ++k) {
				vertex->position[k] = n[k] + s * a[k] + t * b[k];
				vertex->normal[k] = n[k];
				vertex->color[k] = 1.0f;
			}
			vertex->uv[0] = (s + 1.0f) * 0.5f;
			vertex->uv[1] = (t + 1.0f) * 0.5f;
		}
		static const uint16_t quad[6] = { 0, 1, 2, 2, 3, 0 };
		for (int k = 0; k < 6; ++k) {
			scene->ground_indices[face * 6 + k] = (uint16_t)(face * 4 + quad[k]);
		}
	}
	scene->ground_mesh = (gpu_mesh_info_t){
		.layout = GPU_MESH_LAYOUT_TRI_P444_N444_C444_U44_I2,
		.vtx_data = scene->ground_vertices,
		.vtx_data_size = sizeof(scene->ground_vertices),
		.idx_data = scene->ground_indices,
		.idx_data_size = sizeof(scene->ground_indices)
	};
}

void sceneLoadResources(scene_t* scene) {
	scene->shaders = shaderLoaderCreate(scene->heap, scene->fs);
	scene->textures = textureLibraryCreate(scene->heap, scene->fs);
	static vec3f_t vertices[] = {
		{-1,-1, 1},{.18f,.42f,.62f}, {1,-1, 1},{.18f,.42f,.62f},
		{ 1, 1, 1},{.18f,.42f,.62f},{-1, 1, 1},{.18f,.42f,.62f},
		{-1,-1,-1},{.18f,.42f,.62f}, {1,-1,-1},{.18f,.42f,.62f},
		{ 1, 1,-1},{.18f,.42f,.62f},{-1, 1,-1},{.18f,.42f,.62f}
	};
	static uint16_t indices[] = {
		0,1,2, 2,3,0, 1,5,6, 6,2,1, 7,6,5, 5,4,7,
		4,0,3, 3,7,4, 4,5,1, 1,0,4, 3,2,6, 6,7,3
	};
	scene->cube_mesh = (gpu_mesh_info_t){
		.layout = GPU_MESH_LAYOUT_TRI_P444_C444_I2,
		.vtx_data = vertices,
		.vtx_data_size = sizeof(vertices),
		.idx_data = indices,
		.idx_data_size = sizeof(indices)
	};
	sceneBuildGroundMesh(scene);
	sceneBuildSphereMesh(scene);
}

/* A unit UV sphere: position, normal, white color and uv per vertex; the uvs
   wrap twice around so texels stay square. Triangles are
   wound like the ball mesh so their outsides are front faces. */
static void sceneBuildSphereMesh(scene_t* scene) {
	const int columns = SCENE_SPHERE_SEGMENTS + 1;
	const int vertex_count = (SCENE_SPHERE_RINGS + 1) * columns;
	const int index_count = SCENE_SPHERE_RINGS * SCENE_SPHERE_SEGMENTS * 6;
	scene->sphere_vertices = heapAlloc(scene->heap, sizeof(gpu_vertex_t) * vertex_count,
		_Alignof(gpu_vertex_t));
	scene->sphere_indices = heapAlloc(scene->heap, sizeof(uint16_t) * index_count,
		_Alignof(uint16_t));
	for (int ring = 0; ring <= SCENE_SPHERE_RINGS; ++ring) {
		const float theta = (float)M_PI * (float)ring / (float)SCENE_SPHERE_RINGS;
		for (int segment = 0; segment <= SCENE_SPHERE_SEGMENTS; ++segment) {
			const float phi = 2.0f * (float)M_PI * (float)segment / (float)SCENE_SPHERE_SEGMENTS;
			const vec3f_t p = { sinf(theta) * cosf(phi), cosf(theta), sinf(theta) * sinf(phi) };
			const int v = ring * columns + segment;
			scene->sphere_vertices[v] = (gpu_vertex_t){
				.position = { p.x, p.y, p.z },
				.normal = { p.x, p.y, p.z },
				.color = { 1.0f, 1.0f, 1.0f },
				.uv = { 2.0f * (float)segment / (float)SCENE_SPHERE_SEGMENTS,
					(float)ring / (float)SCENE_SPHERE_RINGS }
			};
		}
	}
	int index = 0;
	for (int ring = 0; ring < SCENE_SPHERE_RINGS; ++ring) {
		for (int segment = 0; segment < SCENE_SPHERE_SEGMENTS; ++segment) {
			const uint16_t a = (uint16_t)(ring * columns + segment);
			const uint16_t b = (uint16_t)(a + 1);
			const uint16_t c = (uint16_t)(a + columns);
			const uint16_t d = (uint16_t)(c + 1);
			scene->sphere_indices[index++] = a; scene->sphere_indices[index++] = c;
			scene->sphere_indices[index++] = b;
			scene->sphere_indices[index++] = b; scene->sphere_indices[index++] = c;
			scene->sphere_indices[index++] = d;
		}
	}
	scene->sphere_mesh = (gpu_mesh_info_t){
		.layout = GPU_MESH_LAYOUT_TRI_P444_N444_C444_U44_I2,
		.vtx_data = scene->sphere_vertices,
		.vtx_data_size = sizeof(gpu_vertex_t) * vertex_count,
		.idx_data = scene->sphere_indices,
		.idx_data_size = sizeof(uint16_t) * index_count
	};
}

void sceneUnloadResources(scene_t* scene) {
	shaderLoaderDestroy(scene->shaders);
	textureLibraryDestroy(scene->textures);
	heapFree(scene->heap, scene->sphere_indices);
	heapFree(scene->heap, scene->sphere_vertices);
}

void sceneSpawnCamera(scene_t* scene) {
	uint64_t mask = (1ULL << scene->camera_type) | (1ULL << scene->name_type);
	scene->camera_entity = ecsEntityAdd(scene->ecs, mask);
	name_component_t* name = ecsEntityGet(scene->ecs, scene->camera_entity, scene->name_type, true);
	strcpy_s(name->name, sizeof(name->name), "camera");
	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity, scene->camera_type, true);
	vec3f_t eye = { 5.5f, 6.5f, 8.5f };
	vec3f_t center = { -0.5f, 2.2f, 0.0f };
	vec3f_t up = vec3fUp();
	mat4fMakePerspective(&camera->projection, SCENE_FIELD_OF_VIEW, scene->aspect_ratio, 0.1f, 100.0f);
	mat4fMakeLookAt(&camera->view, &eye, &center, &up);
	vec3f_t offset = vec3fSub(eye, center);
	camera->orbit_target[0] = center.x;
	camera->orbit_target[1] = center.y;
	camera->orbit_target[2] = center.z;
	camera->orbit_distance = vec3fMagnitude(offset);
	camera->orbit_yaw = atan2f(offset.x, offset.z);
	camera->orbit_pitch = asinf(offset.y / camera->orbit_distance);
}

static vec3f_t sceneCameraGetEye(const camera_component_t* camera) {
	float horizontal_distance = cosf(camera->orbit_pitch) * camera->orbit_distance;
	vec3f_t center = {
		camera->orbit_target[0], camera->orbit_target[1], camera->orbit_target[2]
	};
	return (vec3f_t){
		center.x + sinf(camera->orbit_yaw) * horizontal_distance,
		center.y + sinf(camera->orbit_pitch) * camera->orbit_distance,
		center.z + cosf(camera->orbit_yaw) * horizontal_distance
	};
}

void sceneUpdateCamera(scene_t* scene) {
	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity,
		scene->camera_type, false);
	if (!camera) return;
	const bool ui_mouse = uiWantsMouse(uiGet(scene->window));
	int cursor_x, cursor_y, width, height;
	if (wmGetCursorClientPos(scene->window, &cursor_x, &cursor_y, &width, &height)) {
		scene->aspect_ratio = (float)width / (float)height;
	}
	mat4fMakePerspective(&camera->projection, SCENE_FIELD_OF_VIEW, scene->aspect_ratio, 0.1f, 100.0f);

	int mouse_x = 0;
	int mouse_y = 0;
	wmGetMouseLoc(scene->window, &mouse_x, &mouse_y);
	if ((wmGetMouseMask(scene->window) & k_mouse_button_left) && !ui_mouse &&
		scene->gizmo_drag_axis < 0) {
		camera->orbit_yaw -= (float)mouse_x * 0.008f;
		camera->orbit_pitch -= (float)mouse_y * 0.008f;
		camera->orbit_pitch = __max(-1.45f, __min(1.45f, camera->orbit_pitch));
		scene->select_travel += abs(mouse_x) + abs(mouse_y);
	}
	const int wheel = wmGetMouseWheel(scene->window);
	if (wheel != 0 && !ui_mouse) {
		camera->orbit_distance *= powf(0.9f, (float)wheel / 120.0f);
		camera->orbit_distance = __max(2.0f, __min(60.0f, camera->orbit_distance));
	}

	vec3f_t center = {
		camera->orbit_target[0], camera->orbit_target[1], camera->orbit_target[2]
	};
	vec3f_t eye = sceneCameraGetEye(camera);
	vec3f_t up = vec3fUp();
	mat4fMakeLookAt(&camera->view, &eye, &center, &up);
}

//  --------------------------------------------------------------------------
//								INTERACTION
//

static bool sceneGetPointer(scene_t* scene, scene_pointer_t* pointer) {
	if (scene->pointer_overridden) {
		*pointer = scene->pointer_override;
		return pointer->width > 0 && pointer->height > 0;
	}
	const uint32_t buttons = wmGetMouseMask(scene->window);
	pointer->select_held = (buttons & k_mouse_button_left) != 0;
	pointer->grab_held = (buttons & k_mouse_button_right) != 0;
	return wmGetCursorClientPos(scene->window, &pointer->x, &pointer->y,
		&pointer->width, &pointer->height);
}

void sceneSetPointerOverride(scene_t* scene, const scene_pointer_t* pointer) {
	scene->pointer_overridden = pointer != NULL;
	if (pointer) scene->pointer_override = *pointer;
}

/* Builds the world-space ray under the pointer. The view matrix rows are the
   camera's right, up and back axes; the projection flips y for Vulkan, so screen
   y grows downward exactly like client coordinates. */
static bool sceneGetPointerRay(scene_t* scene, const camera_component_t* camera,
	const scene_pointer_t* pointer, vec3f_t* origin, vec3f_t* direction) {
	const float ndc_x = 2.0f * ((float)pointer->x + 0.5f) / (float)pointer->width - 1.0f;
	const float ndc_y = 2.0f * ((float)pointer->y + 0.5f) / (float)pointer->height - 1.0f;
	const float tan_half = tanf(SCENE_FIELD_OF_VIEW * 0.5f);
	const vec3f_t right = { camera->view.mat[0][0], camera->view.mat[0][1], camera->view.mat[0][2] };
	const vec3f_t up = { camera->view.mat[1][0], camera->view.mat[1][1], camera->view.mat[1][2] };
	const vec3f_t back = { camera->view.mat[2][0], camera->view.mat[2][1], camera->view.mat[2][2] };
	*origin = sceneCameraGetEye(camera);
	*direction = vec3fNorm(vec3fAdd(vec3fSub(
		vec3fScale(right, ndc_x * tan_half * scene->aspect_ratio),
		vec3fScale(up, ndc_y * tan_half)), vec3fScale(back, -1.0f)));
	return true;
}

/* The nearest visible object under the ray, or NULL. distance is set; particle
   is set for soft bodies and left as UINT32_MAX for spheres. */
static scene_object_t* scenePickObject(scene_t* scene, vec3f_t origin, vec3f_t direction,
	bool include_static, physics_particle_id_t* particle, float* distance) {
	physics_t* physics = sceneGetPhysics(scene);
	float particle_distance = FLT_MAX;
	float collider_distance = FLT_MAX;
	int collider = -1;
	if (!physicsPickParticle(physics, origin, direction, include_static, particle,
		&particle_distance)) {
		particle_distance = FLT_MAX;
	}
	if (!physicsPickSphereCollider(physics, origin, direction, &collider, &collider_distance)) {
		collider_distance = FLT_MAX;
	}
	if (particle_distance == FLT_MAX && collider_distance == FLT_MAX) return NULL;
	const bool sphere_first = collider_distance < particle_distance;
	*distance = sphere_first ? collider_distance : particle_distance;
	if (sphere_first) *particle = UINT32_MAX;
	for (int i = 0; i < scene->object_count; ++i) {
		scene_object_t* object = &scene->objects[i];
		if (object->hidden) continue;
		if (sphere_first ? object->collider == collider && object->kind == SCENE_OBJECT_SPHERE :
			*particle >= object->first_particle &&
			*particle < object->first_particle + (physics_particle_id_t)object->particle_count) {
			return object;
		}
	}
	return NULL;
}

/* Moves an object in Edit without rebuilding: its particles (or collider) are
   teleported and its authored position updated. Callers rebuild when the drag
   ends, so the authored world matches what Stop will rebuild. */
static void sceneTranslateObject(scene_t* scene, scene_object_t* object, vec3f_t offset) {
	physics_t* physics = sceneGetPhysics(scene);
	physicsTranslateParticles(physics, object->first_particle, object->particle_count, offset);
	sceneObjectSetPosition(object, vec3fAdd(sceneObjectPosition(object), offset));
	if (object->collider >= 0) {
		physicsSetSphereCollider(physics, object->collider, object->sphere.center,
			object->sphere.radius);
		sceneObjectUpdateModel(scene, object);
	}
	if (object->id == scene->selected_id) scene->selected_revision++;
}

//  --------------------------------------------------------------------------
//								MOVE GIZMO
//

static const vec3f_t scene_gizmo_axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

/* The selected object while the move gizmo is showing, or NULL. */
static scene_object_t* sceneGizmoTarget(scene_t* scene) {
	if (scene->mode != SCENE_MODE_EDIT || scene->tool != SCENE_TOOL_MOVE) return NULL;
	scene_object_t* object = sceneFindObject(scene, scene->selected_id);
	return object && !object->hidden ? object : NULL;
}

/* Handles start at the object's position and reach past its surface. */
static float sceneGizmoLength(const scene_object_t* object) {
	switch (object->kind) {
		case SCENE_OBJECT_BALL: return object->ball.radius * 1.1f + 0.8f;
		case SCENE_OBJECT_CLOTH: return object->cloth.size * 0.5f + 0.8f;
		default: return object->sphere.radius + 0.8f;
	}
}

/* Closest approach between the ray origin + s * direction (s >= 0) and the axis
   line center + t * axis. Returns false when they are parallel. */
static bool sceneRayAxisClosest(vec3f_t origin, vec3f_t direction, vec3f_t center, vec3f_t axis,
	float* s, float* t) {
	const vec3f_t w = vec3fSub(origin, center);
	const float b = vec3fDot(direction, axis);
	const float d = vec3fDot(direction, w);
	const float e = vec3fDot(axis, w);
	const float denominator = 1.0f - b * b;
	if (denominator < 1.0e-5f) return false;
	*s = (b * e - d) / denominator;
	*t = (e - b * d) / denominator;
	return true;
}

/* The handle under the ray, or -1. A handle counts if the ray passes within a
   tolerance that grows with distance, so it stays easy to hit from afar. */
static int sceneGizmoPick(scene_t* scene, const scene_object_t* object, vec3f_t origin,
	vec3f_t direction) {
	const vec3f_t center = sceneObjectPosition(object);
	const float length = sceneGizmoLength(object);
	int best_axis = -1;
	float best = FLT_MAX;
	for (int axis = 0; axis < 3; ++axis) {
		float s, t;
		if (!sceneRayAxisClosest(origin, direction, center, scene_gizmo_axes[axis], &s, &t)) continue;
		t = __max(length * 0.2f, __min(length, t));  // the pivot itself is not a handle
		s = __max(0.0f, vec3fDot(vec3fSub(vec3fAdd(center, vec3fScale(scene_gizmo_axes[axis], t)),
			origin), direction));
		const vec3f_t on_ray = vec3fAdd(origin, vec3fScale(direction, s));
		const vec3f_t on_axis = vec3fAdd(center, vec3fScale(scene_gizmo_axes[axis], t));
		const float miss = vec3fDistance(on_ray, on_axis);
		const float tolerance = 0.08f + 0.012f * s;
		if (miss < tolerance && miss < best) {
			best = miss;
			best_axis = axis;
		}
	}
	(void)scene;
	return best_axis;
}

/* Places the gizmo's rods and tips (stretched copies of the sphere mesh) on the
   selected object, or hides them. */
static void sceneUpdateGizmo(scene_t* scene) {
	scene_object_t* object = sceneGizmoTarget(scene);
	for (int part = 0; part < 6; ++part) {
		const int axis = part % 3;
		const bool tip = part >= 3;
		model_component_t* model = ecsEntityGet(scene->ecs, scene->gizmo_entities[part],
			scene->model_type, true);
		transform_component_t* transform = ecsEntityGet(scene->ecs, scene->gizmo_entities[part],
			scene->transform_type, true);
		if (!object) {
			model->mesh_info = NULL;
			continue;
		}
		const vec3f_t center = sceneObjectPosition(object);
		const float length = sceneGizmoLength(object);
		const vec3f_t direction = scene_gizmo_axes[axis];
		transformIdentity(&transform->transform);
		if (tip) {
			transform->transform.translation = vec3fAdd(center, vec3fScale(direction, length));
			transform->transform.scale = (vec3f_t){ 0.14f, 0.14f, 0.14f };
		}
		else {
			const float thickness = 0.045f;
			transform->transform.translation = vec3fAdd(center, vec3fScale(direction, length * 0.5f));
			transform->transform.scale = vec3fAdd(vec3fScale(direction, length * 0.5f - thickness),
				(vec3f_t){ thickness, thickness, thickness });
		}
		const bool active = axis == scene->gizmo_drag_axis ||
			(scene->gizmo_drag_axis < 0 && axis == scene->gizmo_hover_axis);
		static const float colors[3][3] = { { 1.0f, .22f, .22f }, { .30f, 1.0f, .30f }, { .30f, .45f, 1.0f } };
		model->mesh_info = &scene->sphere_mesh;
		model->tint[0] = active ? 1.4f : colors[axis][0];
		model->tint[1] = active ? 1.2f : colors[axis][1];
		model->tint[2] = active ? 0.2f : colors[axis][2];
	}
}

/* Toggles the pin of the cloth particle under the ray (Pin tool). Returns false
   if the ray hit no cloth particle. */
static bool sceneTogglePinAt(scene_t* scene, vec3f_t origin, vec3f_t direction) {
	physics_particle_id_t particle;
	float distance;
	scene_object_t* object = scenePickObject(scene, origin, direction, true, &particle, &distance);
	if (!object || object->kind != SCENE_OBJECT_CLOTH || particle == UINT32_MAX) return false;
	const int n = clothInfoResolution(&object->cloth);
	const int index = (int)(particle - object->first_particle);
	clothInfoTogglePin(&object->cloth, index % n, index / n);
	sceneSelect(scene, object->id);
	scene->selected_revision++;
	sceneRebuild(scene, false);
	return true;
}

/* Edit mode: the Move tool's gizmo drags along one axis; a left click selects
   (or, with the Pin tool, toggles a cloth pin); right-drag moves an object
   freely. Play mode: left click selects, right-drag grabs physically. */
void sceneUpdateInteraction(scene_t* scene) {
	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity,
		scene->camera_type, false);
	scene_pointer_t pointer;
	vec3f_t origin, direction;
	if (!camera || !sceneGetPointer(scene, &pointer) ||
		!sceneGetPointerRay(scene, camera, &pointer, &origin, &direction)) return;
	const bool ui_mouse = !scene->pointer_overridden && uiWantsMouse(uiGet(scene->window));
	physics_t* physics = sceneGetPhysics(scene);
	const bool select_pressed = pointer.select_held && !scene->select_was_held;
	const bool select_released = !pointer.select_held && scene->select_was_held;

	// move gizmo: hover, start, drag along the axis, end
	scene_object_t* gizmo_target = sceneGizmoTarget(scene);
	if (scene->gizmo_drag_axis >= 0) {
		const vec3f_t axis = scene_gizmo_axes[scene->gizmo_drag_axis];
		float s, t;
		if (gizmo_target && pointer.select_held &&
			sceneRayAxisClosest(origin, direction, sceneObjectPosition(gizmo_target), axis, &s, &t)) {
			/* t is measured from the object's current position, which moves with the
			   drag, so the step is t minus where the grab point sits now. */
			const float step = t - scene->gizmo_drag_param;
			if (step != 0.0f) sceneTranslateObject(scene, gizmo_target, vec3fScale(axis, step));
		}
		if (!pointer.select_held || !gizmo_target) {
			scene->gizmo_drag_axis = -1;
			scene->select_travel = SCENE_CLICK_SLOP;  // the release is not a click
			sceneRebuild(scene, false);
		}
	}
	else if (gizmo_target && !ui_mouse) {
		scene->gizmo_hover_axis = sceneGizmoPick(scene, gizmo_target, origin, direction);
		if (select_pressed && scene->gizmo_hover_axis >= 0) {
			float s, t;
			if (sceneRayAxisClosest(origin, direction, sceneObjectPosition(gizmo_target),
				scene_gizmo_axes[scene->gizmo_hover_axis], &s, &t)) {
				scene->gizmo_drag_axis = scene->gizmo_hover_axis;
				scene->gizmo_drag_param = t;
			}
		}
	}
	else {
		scene->gizmo_hover_axis = -1;
	}

	// selection: a press and release without dragging the camera or a handle
	if (select_pressed && scene->gizmo_drag_axis < 0) {
		scene->select_travel = 0;
	}
	if (select_released && !ui_mouse && scene->select_travel < SCENE_CLICK_SLOP) {
		const bool pinned = scene->mode == SCENE_MODE_EDIT && scene->tool == SCENE_TOOL_PIN &&
			sceneTogglePinAt(scene, origin, direction);
		if (!pinned) {
			physics_particle_id_t particle;
			float distance;
			scene_object_t* object = scenePickObject(scene, origin, direction, true, &particle, &distance);
			sceneSelect(scene, object ? object->id : -1);
		}
	}
	scene->select_was_held = pointer.select_held;

	if (!pointer.grab_held) {
		if (scene->grabbing) physicsReleaseGrab(physics);
		scene->grabbing = false;
		if (scene->moving) {
			/* Rebuild once at the end of a move so the authored world is built from
			   the new position exactly as Stop will rebuild it. */
			scene->moving = false;
			sceneRebuild(scene, false);
		}
		return;
	}

	if (scene->mode == SCENE_MODE_EDIT) {
		if (!scene->moving) {
			if (ui_mouse || scene->gizmo_drag_axis >= 0) return;
			physics_particle_id_t particle;
			float distance;
			scene_object_t* object = scenePickObject(scene, origin, direction, true, &particle, &distance);
			if (!object) return;
			sceneSelect(scene, object->id);
			scene->moving = true;
			scene->moving_id = object->id;
			scene->grab_distance = distance;
			scene->move_last_point = vec3fAdd(origin, vec3fScale(direction, distance));
			return;
		}
		scene_object_t* object = sceneFindObject(scene, scene->moving_id);
		if (!object) {
			scene->moving = false;
			return;
		}
		const vec3f_t point = vec3fAdd(origin, vec3fScale(direction, scene->grab_distance));
		const vec3f_t offset = vec3fSub(point, scene->move_last_point);
		scene->move_last_point = point;
		sceneTranslateObject(scene, object, offset);
		return;
	}

	if (scene->mode != SCENE_MODE_PLAYING) return;
	if (!scene->grabbing) {
		if (ui_mouse) return;
		physics_particle_id_t particle;
		float distance;
		scene_object_t* object = scenePickObject(scene, origin, direction, false, &particle, &distance);
		if (object && object->kind != SCENE_OBJECT_SPHERE) {
			scene->grab_distance = distance;
			physicsGrab(physics, particle, vec3fAdd(origin, vec3fScale(direction, distance)),
				SCENE_GRAB_RADIUS);
			scene->grabbing = true;
		}
		return;
	}
	physicsMoveGrab(physics, vec3fAdd(origin, vec3fScale(direction, scene->grab_distance)));
}

/* Projects a world point to client pixels with the same camera the ray uses. */
static bool sceneWorldToScreen(scene_t* scene, vec3f_t point, int width, int height, int* x, int* y) {
	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity,
		scene->camera_type, false);
	if (!camera) return false;
	mat4f_t view_projection;
	mat4fMul(&view_projection, &camera->projection, &camera->view);
	float clip[4];
	for (int row = 0; row < 4; ++row) {
		clip[row] = view_projection.mat[row][0] * point.x + view_projection.mat[row][1] * point.y +
			view_projection.mat[row][2] * point.z + view_projection.mat[row][3];
	}
	if (clip[3] <= 0.0f) return false;
	*x = (int)((clip[0] / clip[3] + 1.0f) * 0.5f * (float)width);
	*y = (int)((clip[1] / clip[3] + 1.0f) * 0.5f * (float)height);
	return true;
}

bool sceneGetObjectScreenPosition(scene_t* scene, int id, int width, int height, int* x, int* y) {
	const scene_object_t* object = sceneFindObject(scene, id);
	return object && sceneWorldToScreen(scene, sceneObjectPosition(object), width, height, x, y);
}

bool sceneGetGizmoHandle(scene_t* scene, int axis, int width, int height, int* x, int* y) {
	const scene_object_t* object = sceneGizmoTarget(scene);
	if (!object || axis < 0 || axis > 2) return false;
	const vec3f_t point = vec3fAdd(sceneObjectPosition(object),
		vec3fScale(scene_gizmo_axes[axis], sceneGizmoLength(object) * 0.8f));
	return sceneWorldToScreen(scene, point, width, height, x, y);
}

//  --------------------------------------------------------------------------
//								SIMULATION
//

static void sceneApplySettings(physics_t* physics, const ui_sim_settings_t* settings) {
	physicsSetSubsteps(physics, settings->substeps);
	physicsSetSolverIterations(physics, settings->iterations);
	physicsSetGravity(physics, settings->gravity);
	physicsSetFriction(physics, settings->static_friction, settings->dynamic_friction);
	physicsSetRestitution(physics, settings->restitution);
	physicsSetRollingResistance(physics, settings->rolling_resistance);
}

static void scenePublishView(scene_t* scene, ui_t* ui) {
	ui_scene_view_t view;
	memset(&view, 0, sizeof(view));
	view.mode = (ui_play_mode_t)scene->mode;
	view.tool = (ui_tool_t)scene->tool;
	for (int i = 0; i < scene->object_count; ++i) {
		const scene_object_t* object = &scene->objects[i];
		if (object->hidden) continue;
		ui_object_t* entry = &view.objects[view.object_count++];
		entry->id = object->id;
		entry->kind = object->kind == SCENE_OBJECT_BALL ? UI_OBJECT_BALL :
			object->kind == SCENE_OBJECT_CLOTH ? UI_OBJECT_CLOTH : UI_OBJECT_SPHERE;
		strcpy_s(entry->name, sizeof(entry->name), object->name);
		entry->transient = object->transient;
	}
	view.selected_id = scene->selected_id;
	const scene_object_t* selected = sceneFindObject(scene, scene->selected_id);
	if (selected) view.selected = sceneObjectProperties(selected);
	view.selected_revision = scene->selected_revision;

	const physics_stats_t physics_stats = physicsGetStats(sceneGetPhysics(scene));
	view.stats = (ui_stats_t){
		.physics_ms = scene->physics_ms_average,
		.dropped_ms = scene->dropped_ms_reported,
		.particle_count = physics_stats.particle_count,
		.constraint_count = physics_stats.distance_constraint_count,
		.soft_body_count = physics_stats.soft_body_count,
		.contact_count = physics_stats.contact_count,
		.grabbing = physics_stats.grabbed_count > 0
	};
	uiSetSceneView(ui, &view);
}

/* Steps physics only while playing, then refreshes meshes and the UI view. */
void sceneUpdateSimulation(scene_t* scene) {
	ui_t* ui = uiGet(scene->window);
	physics_world_component_t* world = sceneGetWorld(scene);
	ui_sim_settings_t settings = uiGetSimSettings(ui);
	sceneApplySettings(world->physics, &settings);

	const float dt = (float)timerObjectGetUsDeltaTime(scene->timer) / 1000000.0f;
	const uint64_t start_ticks = timerGetTicks();
	int steps = 0;
	if (scene->mode == SCENE_MODE_PLAYING) {
		/* Time beyond the clamp is dropped rather than simulated, which shows up as
		   slow motion; it is reported so that is visible. */
		if (dt > SCENE_MAX_FRAME_STEP) scene->dropped_ms_window += (dt - SCENE_MAX_FRAME_STEP) * 1000.0f;
		world->accumulator += __min(dt, SCENE_MAX_FRAME_STEP);
		while (world->accumulator >= SCENE_FIXED_STEP) {
			physicsUpdate(world->physics, SCENE_FIXED_STEP);
			world->accumulator -= SCENE_FIXED_STEP;
			++steps;
		}
	}
	const float physics_ms = (float)timerTicksToUs(timerGetTicks() - start_ticks) / 1000.0f;
	if (steps > 0) {
		scene->physics_ms_average += (physics_ms / (float)steps - scene->physics_ms_average) * 0.05f;
	}
	scene->stats_window_seconds += dt;
	if (scene->stats_window_seconds >= 1.0f) {
		scene->dropped_ms_reported = scene->dropped_ms_window;
		scene->dropped_ms_window = 0.0f;
		scene->stats_window_seconds = 0.0f;
	}

	for (int i = 0; i < scene->object_count; ++i) {
		if (!scene->objects[i].hidden) sceneObjectUpdateMesh(&scene->objects[i]);
	}
	scenePublishView(scene, ui);
}

//  --------------------------------------------------------------------------
//								QUERIES
//

int sceneGetObjectCount(scene_t* scene) {
	int count = 0;
	for (int i = 0; i < scene->object_count; ++i) {
		if (!scene->objects[i].hidden) ++count;
	}
	return count;
}

int sceneGetSelectedId(scene_t* scene) {
	return scene->selected_id;
}

int sceneGetMode(scene_t* scene) {
	return (int)scene->mode;
}

int sceneGetGrabbedCount(scene_t* scene) {
	return physicsGetStats(sceneGetPhysics(scene)).grabbed_count;
}

int sceneGetTextureCount(scene_t* scene) {
	return textureLibraryLoadedCount(scene->textures);
}

bool sceneIsSkyVisible(scene_t* scene) {
	const model_component_t* sky = ecsEntityGet(scene->ecs, scene->sky_entity, scene->model_type, true);
	return sky->mesh_info != NULL;
}

bool sceneGetObjectProperties(scene_t* scene, int id, ui_object_properties_t* properties) {
	const scene_object_t* object = sceneFindObject(scene, id);
	if (!object) return false;
	*properties = sceneObjectProperties(object);
	return true;
}

uint64_t sceneHashParticles(scene_t* scene) {
	physics_t* physics = sceneGetPhysics(scene);
	const int count = physicsGetStats(physics).particle_count;
	const unsigned char* bytes = (const unsigned char*)physicsGetParticlePositions(physics);
	uint64_t hash = 14695981039346656037ull;  // FNV-1a
	for (size_t i = 0; i < sizeof(vec3f_t) * (size_t)count; ++i) {
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	}
	return hash ^ (uint64_t)count;
}

//  --------------------------------------------------------------------------
//								RENDERING
//

ecs_entity_t sceneSpawnModel(scene_t* scene, const char* name_text,
	vec3f_t position, vec3f_t scale, gpu_mesh_info_t* mesh) {
	uint64_t mask = (1ULL << scene->transform_type) | (1ULL << scene->model_type) |
		(1ULL << scene->name_type);
	ecs_entity_t entity = ecsEntityAdd(scene->ecs, mask);
	transform_component_t* transform = ecsEntityGet(scene->ecs, entity, scene->transform_type, true);
	model_component_t* model = ecsEntityGet(scene->ecs, entity, scene->model_type, true);
	name_component_t* name = ecsEntityGet(scene->ecs, entity, scene->name_type, true);
	transformIdentity(&transform->transform);
	transform->transform.translation = position;
	transform->transform.scale = scale;
	memset(model, 0, sizeof(*model));
	model->mesh_info = mesh;
	model->shader_info = shaderLoaderGetFlat(scene->shaders);
	model->tint[0] = model->tint[1] = model->tint[2] = 1.0f;
	strcpy_s(name->name, sizeof(name->name), name_text);
	return entity;
}

void sceneDrawModels(scene_t* scene) {
	const scene_object_t* selected = sceneFindObject(scene, scene->selected_id);
	uint64_t camera_mask = (1ULL << scene->camera_type);
	for (ecs_query_t camera_query = ecsQueryCreate(scene->ecs, camera_mask);
		ecsQueryValid(scene->ecs, &camera_query); ecsQueryNext(scene->ecs, &camera_query)) {
		camera_component_t* camera = ecsQueryGetComponent(scene->ecs, &camera_query, scene->camera_type);
		vec3f_t camera_position = sceneCameraGetEye(camera);
		uint64_t model_mask = (1ULL << scene->transform_type) | (1ULL << scene->model_type);
		for (ecs_query_t query = ecsQueryCreate(scene->ecs, model_mask);
			ecsQueryValid(scene->ecs, &query); ecsQueryNext(scene->ecs, &query)) {
			transform_component_t* transform = ecsQueryGetComponent(scene->ecs, &query, scene->transform_type);
			model_component_t* model = ecsQueryGetComponent(scene->ecs, &query, scene->model_type);
			if (!model->mesh_info) continue;  // hidden, or out of capacity
			ecs_entity_t entity = ecsQueryGetEntity(scene->ecs, &query);
			const bool highlighted = selected &&
				memcmp(&selected->entity, &entity, sizeof(entity)) == 0;
			scene_draw_uniform_t data;
			data.projection = camera->projection;
			data.view = camera->view;
			transformConvertToMatrix(&transform->transform, &data.model);
			data.camera_position_roughness[0] = camera_position.x;
			data.camera_position_roughness[1] = camera_position.y;
			data.camera_position_roughness[2] = camera_position.z;
			data.camera_position_roughness[3] = 0.72f;
			data.light_direction_metallic[0] = 0.35f;
			data.light_direction_metallic[1] = -0.82f;
			data.light_direction_metallic[2] = 0.45f;
			data.light_direction_metallic[3] = 0.0f;
			data.light_color_ambient[0] = 4.6f;
			data.light_color_ambient[1] = 4.2f;
			data.light_color_ambient[2] = 3.6f;
			data.light_color_ambient[3] = 0.045f;
			/* The selected object is drawn brighter and warmer. */
			data.material[0] = model->tint[0] * (highlighted ? 1.6f : 1.0f);
			data.material[1] = model->tint[1] * (highlighted ? 1.5f : 1.0f);
			data.material[2] = model->tint[2] * (highlighted ? 1.1f : 1.0f);
			data.material[3] = 1.0f;
			memcpy(data.texture_params, model->texture_params, sizeof(data.texture_params));
			gpu_uniform_buffer_info_t uniform = {.data=&data, .size=sizeof(data)};
			rendererModelAdd(scene->render, &entity, model->mesh_info, model->shader_info,
				model->texture, &uniform);
		}
	}
}
