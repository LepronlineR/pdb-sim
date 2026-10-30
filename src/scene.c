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
#include "timer.h"
#include "timer_object.h"
#include "ui.h"
#include "vec3f.h"
#include "wm.h"

#define _USE_MATH_DEFINES
#include <math.h>
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

enum {
	SCENE_MAX_OBJECTS = UI_MAX_OBJECTS
};

typedef enum scene_mode_t {
	SCENE_MODE_EDIT = UI_MODE_EDIT,
	SCENE_MODE_PLAYING = UI_MODE_PLAYING,
	SCENE_MODE_PAUSED = UI_MODE_PAUSED
} scene_mode_t;

typedef enum scene_object_kind_t {
	SCENE_OBJECT_BALL,
	SCENE_OBJECT_CLOTH
} scene_object_kind_t;

/* An authored object. The settings are the source of truth; the resource,
   mesh and particle range are runtime data rebuilt with the physics world. */
typedef struct scene_object_t {
	int id;
	scene_object_kind_t kind;
	char name[UI_NAME_LENGTH];
	ball_info_t ball;
	cloth_info_t cloth;
	bool transient;  // added during play: removed on Stop
	bool hidden;     // deleted during play: restored on Stop
	ecs_entity_t entity;
	void* resource;  // ball_t* or cloth_t*
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
	gpu_mesh_info_t cube_mesh;
	// width / height of the window's client area, used by projection and picking
	float aspect_ratio;

	scene_mode_t mode;
	scene_object_t objects[SCENE_MAX_OBJECTS];
	int object_count;
	int next_object_id;
	int ball_counter;
	int cloth_counter;
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
} scene_draw_uniform_t;

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
	return object->kind == SCENE_OBJECT_BALL ? object->ball.position : object->cloth.center;
}

static void sceneObjectSetPosition(scene_object_t* object, vec3f_t position) {
	if (object->kind == SCENE_OBJECT_BALL) object->ball.position = position;
	else object->cloth.center = position;
}

//  --------------------------------------------------------------------------
//								RUNTIME OBJECTS
//

/* Creates the object's ball or cloth in the current physics world and points
   its model at the new mesh. Fails when the world is out of capacity. */
static bool sceneObjectCreateRuntime(scene_t* scene, scene_object_t* object) {
	physics_t* physics = sceneGetPhysics(scene);
	object->resource = NULL;
	object->mesh = NULL;
	object->particle_count = 0;
	if (object->kind == SCENE_OBJECT_BALL) {
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
	model_component_t* model = ecsEntityGet(scene->ecs, object->entity, scene->model_type, true);
	if (model) model->mesh_info = object->mesh;
	return object->resource != NULL;
}

static void sceneObjectDestroyRuntime(scene_t* scene, scene_object_t* object) {
	if (object->resource) {
		if (object->kind == SCENE_OBJECT_BALL) ballDestroy((ball_t*)object->resource);
		else clothDestroy((cloth_t*)object->resource);
	}
	object->resource = NULL;
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
	const ball_info_t* ball, const cloth_info_t* cloth, const char* name) {
	if (scene->object_count >= SCENE_MAX_OBJECTS) return NULL;
	scene_object_t* object = &scene->objects[scene->object_count];
	memset(object, 0, sizeof(*object));
	object->id = scene->next_object_id++;
	object->kind = kind;
	if (ball) object->ball = *ball;
	if (cloth) object->cloth = *cloth;
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
	scene_object_t* object = sceneAddObject(scene, SCENE_OBJECT_BALL, &info, NULL, name);
	if (object) sceneSelect(scene, object->id);
}

static void sceneAddCloth(scene_t* scene) {
	vec3f_t center = sceneSpawnPoint(scene);
	center.y += 2.5f;
	const cloth_info_t info = clothInfoDefault(center, (vec3f_t){ .92f, .86f, .70f });
	char name[UI_NAME_LENGTH];
	sprintf_s(name, sizeof(name), "Cloth %d", ++scene->cloth_counter);
	scene_object_t* object = sceneAddObject(scene, SCENE_OBJECT_CLOTH, NULL, &info, name);
	if (object) sceneSelect(scene, object->id);
}

/* A trampoline (cloth pinned along its border) with two balls above it, a
   banner pinned along one edge, and two balls on the ground. */
static void sceneLoadDemo(scene_t* scene) {
	sceneClearObjects(scene);
	cloth_info_t trampoline = clothInfoDefault((vec3f_t){0,3,0}, (vec3f_t){.92f,.86f,.70f});
	trampoline.pin = CLOTH_PIN_BORDER;
	trampoline.stretch_compliance = 3.0e-3f;
	trampoline.shear_compliance = 3.0e-3f;
	sceneAddObject(scene, SCENE_OBJECT_CLOTH, NULL, &trampoline, "Trampoline");
	cloth_info_t banner = clothInfoDefault((vec3f_t){-4.5f,5.5f,1.5f}, (vec3f_t){.78f,.20f,.24f});
	banner.resolution = 24;
	banner.size = 2.5f;
	banner.pin = CLOTH_PIN_EDGE;
	sceneAddObject(scene, SCENE_OBJECT_CLOTH, NULL, &banner, "Banner");

	ball_info_t ball = ballInfoDefault((vec3f_t){-.85f,5,.45f}, (vec3f_t){.95f,.30f,.22f}, 1.0f);
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, "Red ball");
	ball = ballInfoDefault((vec3f_t){.85f,6.5f,-.45f}, (vec3f_t){.25f,.82f,.42f}, 1.5f);
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, "Green ball");
	ball = ballInfoDefault((vec3f_t){-3,1.2f,-3}, (vec3f_t){.1f,.13f,.72f}, 0.4f);
	ball.radius = 0.6f;
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, "Small ball");
	ball = ballInfoDefault((vec3f_t){4.5f,1.2f,1}, (vec3f_t){.9f,.82f,.12f}, 0.5f);
	sceneAddObject(scene, SCENE_OBJECT_BALL, &ball, NULL, "Yellow ball");
	scene->ball_counter = 4;
	scene->cloth_counter = 2;
	sceneSelect(scene, -1);
}

static ui_object_properties_t sceneObjectProperties(const scene_object_t* object) {
	ui_object_properties_t p;
	memset(&p, 0, sizeof(p));
	p.id = object->id;
	p.kind = object->kind == SCENE_OBJECT_BALL ? UI_OBJECT_BALL : UI_OBJECT_CLOTH;
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
	}
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
	else {
		cloth_info_t* c = &object->cloth;
		c->color = color;
		c->mass = __max(0.01f, p->mass);
		c->velocity_retention = __max(0.0f, __min(1.0f, p->velocity_retention));
		c->stretch_compliance = __max(0.0f, p->stretch_compliance);
		c->bend_compliance = __max(0.0f, p->bending_compliance);
		c->resolution = __max(4, __min(64, p->resolution));
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
			case UI_COMMAND_LOAD_DEMO:
				if (scene->mode == SCENE_MODE_EDIT) sceneLoadDemo(scene);
				break;
			case UI_COMMAND_CLEAR:
				if (scene->mode == SCENE_MODE_EDIT) sceneClearObjects(scene);
				break;
			case UI_COMMAND_SELECT: sceneSelect(scene, command.id); break;
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
	sceneSpawnModel(scene, "ground", (vec3f_t){0,-.1f,0}, (vec3f_t){12,.1f,12}, &scene->cube_mesh);
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
	sceneUpdateSimulation(scene);
	sceneDrawModels(scene);
	rendererFrameDone(scene->render);
}

void sceneLoadResources(scene_t* scene) {
	scene->shaders = shaderLoaderCreate(scene->heap, scene->fs);
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
}

void sceneUnloadResources(scene_t* scene) {
	shaderLoaderDestroy(scene->shaders);
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
	if ((wmGetMouseMask(scene->window) & k_mouse_button_left) && !ui_mouse) {
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

/* The visible object under the ray, or NULL; particle and distance are set. */
static scene_object_t* scenePickObject(scene_t* scene, vec3f_t origin, vec3f_t direction,
	physics_particle_id_t* particle, float* distance) {
	if (!physicsPickParticle(sceneGetPhysics(scene), origin, direction, particle, distance)) {
		return NULL;
	}
	for (int i = 0; i < scene->object_count; ++i) {
		scene_object_t* object = &scene->objects[i];
		if (!object->hidden && *particle >= object->first_particle &&
			*particle < object->first_particle + (physics_particle_id_t)object->particle_count) {
			return object;
		}
	}
	return NULL;
}

/* Left click selects (or deselects on empty space). Right-drag moves an object
   in Edit and grabs it physically while playing. */
void sceneUpdateInteraction(scene_t* scene) {
	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity,
		scene->camera_type, false);
	scene_pointer_t pointer;
	vec3f_t origin, direction;
	if (!camera || !sceneGetPointer(scene, &pointer) ||
		!sceneGetPointerRay(scene, camera, &pointer, &origin, &direction)) return;
	const bool ui_mouse = !scene->pointer_overridden && uiWantsMouse(uiGet(scene->window));
	physics_t* physics = sceneGetPhysics(scene);

	// selection: a press and release without dragging the camera
	if (pointer.select_held && !scene->select_was_held) {
		scene->select_travel = 0;
	}
	if (!pointer.select_held && scene->select_was_held && !ui_mouse &&
		scene->select_travel < SCENE_CLICK_SLOP) {
		physics_particle_id_t particle;
		float distance;
		scene_object_t* object = scenePickObject(scene, origin, direction, &particle, &distance);
		sceneSelect(scene, object ? object->id : -1);
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
			if (ui_mouse) return;
			physics_particle_id_t particle;
			float distance;
			scene_object_t* object = scenePickObject(scene, origin, direction, &particle, &distance);
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
		physicsTranslateParticles(physics, object->first_particle, object->particle_count, offset);
		sceneObjectSetPosition(object, vec3fAdd(sceneObjectPosition(object), offset));
		if (object->id == scene->selected_id) scene->selected_revision++;
		return;
	}

	if (scene->mode != SCENE_MODE_PLAYING) return;
	if (!scene->grabbing) {
		if (ui_mouse) return;
		physics_particle_id_t particle;
		float distance;
		scene_object_t* object = scenePickObject(scene, origin, direction, &particle, &distance);
		if (object) {
			scene->grab_distance = distance;
			physicsGrab(physics, particle, vec3fAdd(origin, vec3fScale(direction, distance)),
				SCENE_GRAB_RADIUS);
			scene->grabbing = true;
		}
		return;
	}
	physicsMoveGrab(physics, vec3fAdd(origin, vec3fScale(direction, scene->grab_distance)));
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
	for (int i = 0; i < scene->object_count; ++i) {
		const scene_object_t* object = &scene->objects[i];
		if (object->hidden) continue;
		ui_object_t* entry = &view.objects[view.object_count++];
		entry->id = object->id;
		entry->kind = object->kind == SCENE_OBJECT_BALL ? UI_OBJECT_BALL : UI_OBJECT_CLOTH;
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
			data.material[0] = highlighted ? 1.6f : 1.0f;
			data.material[1] = highlighted ? 1.5f : 1.0f;
			data.material[2] = highlighted ? 1.1f : 1.0f;
			data.material[3] = 1.0f;
			gpu_uniform_buffer_info_t uniform = {.data=&data, .size=sizeof(data)};
			rendererModelAdd(scene->render, &entity, model->mesh_info, model->shader_info, &uniform);
		}
	}
}
