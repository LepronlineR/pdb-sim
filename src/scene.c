#include "scene.h"

#include "ball.h"
#include "cloth.h"
#include "transform.h"
#include "component.h"
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
#include <string.h>

#define SCENE_FIELD_OF_VIEW ((float)M_PI / 3.0f)
#define SCENE_DEFAULT_ASPECT_RATIO (16.0f / 9.0f)
#define SCENE_FIXED_STEP (1.0f / 60.0f)
#define SCENE_MAX_FRAME_STEP 0.05f
/* Particles within this distance of the picked one are grabbed with it, so a
   drag pulls a patch of the surface instead of one stretched vertex. */
#define SCENE_GRAB_RADIUS 0.35f

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

	// interaction
	bool pointer_overridden;
	scene_pointer_t pointer_override;
	bool grabbing;
	float grab_distance;
	int spawned_count;

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

static physics_t* sceneCreatePhysics(heap_t* heap) {
	physics_t* physics = physicsCreate(heap);
	physicsSetGroundCompliance(physics, 0.0f);
	return physics;
}

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render) {
	scene_t* scene = heapAlloc(heap, sizeof(*scene), 8);
	memset(scene, 0, sizeof(*scene));
	scene->heap = heap;
	scene->fs = fs;
	scene->window = window;
	scene->render = render;
	scene->aspect_ratio = SCENE_DEFAULT_ASPECT_RATIO;
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
	sceneSpawnDemo(scene, world->physics);
	return scene;
}

void sceneDestroy(scene_t* scene) {
	sceneDestroyOwnedComponents(scene);
	physicsDestroy(sceneGetWorld(scene)->physics);
	ecsDestroy(scene->ecs);
	timerObjectDestroy(scene->timer);
	sceneUnloadResources(scene);
	heapFree(scene->heap, scene);
}

void sceneUpdate(scene_t* scene) {
	timerObjectUpdate(scene->timer);
	ecsUpdate(scene->ecs);
	sceneUpdateCamera(scene);
	sceneUpdateInteraction(scene);
	sceneUpdateComponents(scene);
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

static bool sceneGetPointer(scene_t* scene, scene_pointer_t* pointer) {
	if (scene->pointer_overridden) {
		*pointer = scene->pointer_override;
		return pointer->width > 0 && pointer->height > 0;
	}
	pointer->grab_held = (wmGetMouseMask(scene->window) & k_mouse_button_right) != 0;
	return wmGetCursorClientPos(scene->window, &pointer->x, &pointer->y,
		&pointer->width, &pointer->height);
}

void sceneSetPointerOverride(scene_t* scene, const scene_pointer_t* pointer) {
	scene->pointer_overridden = pointer != NULL;
	if (pointer) scene->pointer_override = *pointer;
}

int sceneGetGrabbedCount(scene_t* scene) {
	return physicsGetStats(sceneGetWorld(scene)->physics).grabbed_count;
}

/* Builds the world-space ray under the pointer. The view matrix rows are the
   camera's right, up and back axes; the projection flips y for Vulkan, so screen
   y grows downward exactly like client coordinates. */
static bool sceneGetPointerRay(scene_t* scene, const camera_component_t* camera,
	const scene_pointer_t* pointer, vec3f_t* origin, vec3f_t* direction) {
	const int x = pointer->x;
	const int y = pointer->y;
	const int width = pointer->width;
	const int height = pointer->height;
	const float ndc_x = 2.0f * ((float)x + 0.5f) / (float)width - 1.0f;
	const float ndc_y = 2.0f * ((float)y + 0.5f) / (float)height - 1.0f;
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

/* Right mouse button: pick the particle under the cursor and drag it at the depth
   it was picked from. */
void sceneUpdateInteraction(scene_t* scene) {
	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity,
		scene->camera_type, false);
	physics_t* physics = sceneGetWorld(scene)->physics;
	scene_pointer_t pointer;
	vec3f_t origin, direction;
	if (!camera || !sceneGetPointer(scene, &pointer) ||
		!sceneGetPointerRay(scene, camera, &pointer, &origin, &direction)) return;
	const bool held = pointer.grab_held;

	if (!held) {
		if (scene->grabbing) physicsReleaseGrab(physics);
		scene->grabbing = false;
		return;
	}
	if (!scene->grabbing) {
		if (!scene->pointer_overridden && uiWantsMouse(uiGet(scene->window))) return;
		physics_particle_id_t particle;
		float distance;
		if (physicsPickParticle(physics, origin, direction, &particle, &distance)) {
			scene->grab_distance = distance;
			physicsGrab(physics, particle, vec3fAdd(origin, vec3fScale(direction, distance)),
				SCENE_GRAB_RADIUS);
			scene->grabbing = true;
		}
		return;
	}
	physicsMoveGrab(physics, vec3fAdd(origin, vec3fScale(direction, scene->grab_distance)));
}

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
	model->mesh_info = mesh;
	model->shader_info = shaderLoaderGetFlat(scene->shaders);
	model->owned_resource = NULL;
	model->update_resource = NULL;
	model->destroy_resource = NULL;
	strcpy_s(name->name, sizeof(name->name), name_text);
	return entity;
}

static void sceneSpawnSoftModel(scene_t* scene, const char* name, gpu_mesh_info_t* mesh,
	void* resource, void (*update)(void*), void (*destroy)(void*)) {
	ecs_entity_t entity = sceneSpawnModel(scene, name, vec3fZero(), vec3fOne(), mesh);
	model_component_t* model = ecsEntityGet(scene->ecs, entity, scene->model_type, true);
	model->owned_resource = resource;
	model->update_resource = update;
	model->destroy_resource = destroy;
	model->shader_info = shaderLoaderGetPbr(scene->shaders);
}

void sceneSpawnBall(scene_t* scene, physics_t* physics, const char* name, ball_info_t info) {
	ball_t* ball = ballCreate(scene->heap, physics, &info);
	if (!ball) return;
	sceneSpawnSoftModel(scene, name, ballGetMesh(ball), ball,
		sceneBallResourceUpdate, sceneBallResourceDestroy);
}

void sceneSpawnCloth(scene_t* scene, physics_t* physics, const char* name, cloth_info_t info) {
	cloth_t* cloth = clothCreate(scene->heap, physics, &info);
	if (!cloth) return;
	sceneSpawnSoftModel(scene, name, clothGetMesh(cloth), cloth,
		sceneClothResourceUpdate, sceneClothResourceDestroy);
}

/* A trampoline (cloth pinned along its border) with two balls dropped into it,
   a banner pinned along one edge that falls and hangs, and two balls on the
   ground to roll around. */
void sceneSpawnDemo(scene_t* scene, physics_t* physics) {
	cloth_info_t trampoline = clothInfoDefault((vec3f_t){0,3,0}, (vec3f_t){.92f,.86f,.70f});
	trampoline.pin = CLOTH_PIN_BORDER;
	trampoline.stretch_compliance = 3.0e-3f;
	trampoline.shear_compliance = 3.0e-3f;
	sceneSpawnCloth(scene, physics, "trampoline", trampoline);
	cloth_info_t banner = clothInfoDefault((vec3f_t){-4.5f,5.5f,1.5f}, (vec3f_t){.78f,.20f,.24f});
	banner.resolution = 24;
	banner.size = 2.5f;
	banner.pin = CLOTH_PIN_EDGE;
	sceneSpawnCloth(scene, physics, "banner", banner);

	sceneSpawnBall(scene, physics, "ball 1", ballInfoDefault(
		(vec3f_t){-.85f,5,.45f}, (vec3f_t){.95f,.30f,.22f}, 1.0f));
	sceneSpawnBall(scene, physics, "ball 2", ballInfoDefault(
		(vec3f_t){.85f,6.5f,-.45f}, (vec3f_t){.25f,.82f,.42f}, 1.5f));
	ball_info_t little = ballInfoDefault((vec3f_t){-3,1.2f,-3}, (vec3f_t){.1f,.13f,.72f}, 0.4f);
	little.radius = 0.6f;
	sceneSpawnBall(scene, physics, "ball 3", little);
	sceneSpawnBall(scene, physics, "ball 4", ballInfoDefault(
		(vec3f_t){4.5f,1.2f,1}, (vec3f_t){.9f,.82f,.12f}, 0.5f));
}

/* Tears down every soft body and rebuilds the demo with a fresh physics world. */
static void sceneReset(scene_t* scene) {
	rendererWaitIdle(scene->render);
	uint64_t mask = (1ULL << scene->model_type);
	for (ecs_query_t query = ecsQueryCreate(scene->ecs, mask);
		ecsQueryValid(scene->ecs, &query); ecsQueryNext(scene->ecs, &query)) {
		model_component_t* model = ecsQueryGetComponent(scene->ecs, &query, scene->model_type);
		if (model->destroy_resource) {
			model->destroy_resource(model->owned_resource);
			model->owned_resource = NULL;
			ecsEntityRemove(scene->ecs, ecsQueryGetEntity(scene->ecs, &query), false);
		}
	}
	physics_world_component_t* world = sceneGetWorld(scene);
	physicsDestroy(world->physics);
	world->physics = sceneCreatePhysics(scene->heap);
	world->accumulator = 0.0f;
	scene->grabbing = false;
	scene->spawned_count = 0;
	sceneSpawnDemo(scene, world->physics);
}

static void sceneApplySettings(physics_t* physics, const ui_sim_settings_t* settings) {
	physicsSetSubsteps(physics, settings->substeps);
	physicsSetSolverIterations(physics, settings->iterations);
	physicsSetGravity(physics, settings->gravity);
	physicsSetFriction(physics, settings->static_friction, settings->dynamic_friction);
	physicsSetRestitution(physics, settings->restitution);
	physicsSetRollingResistance(physics, settings->rolling_resistance);
}

static void sceneHandleSpawns(scene_t* scene, ui_t* ui, physics_t* physics) {
	ui_ball_info_t ball;
	while (uiConsumeBallSpawn(ui, &ball)) {
		ball_info_t info = {
			.position = { ball.position[0], ball.position[1], ball.position[2] },
			.color = { ball.color[0], ball.color[1], ball.color[2] },
			.radius = ball.radius,
			.mass = ball.mass,
			.structural_compliance = ball.structural_compliance,
			.bending_compliance = ball.bending_compliance,
			.volume_compliance = ball.volume_compliance,
			.velocity_retention = ball.velocity_retention
		};
		char name[32];
		sprintf_s(name, sizeof(name), "spawned ball %d", ++scene->spawned_count);
		sceneSpawnBall(scene, physics, name, info);
	}
	ui_cloth_info_t cloth;
	while (uiConsumeClothSpawn(ui, &cloth)) {
		cloth_info_t info = clothInfoDefault(
			(vec3f_t){ cloth.position[0], cloth.position[1], cloth.position[2] },
			(vec3f_t){ cloth.color[0], cloth.color[1], cloth.color[2] });
		info.resolution = cloth.resolution;
		info.size = cloth.size;
		info.mass = cloth.mass;
		info.stretch_compliance = cloth.stretch_compliance;
		info.bend_compliance = cloth.bend_compliance;
		info.velocity_retention = cloth.velocity_retention;
		info.pin = (cloth_pin_t)__max(0, __min(cloth.pin, CLOTH_PIN_COUNT - 1));
		char name[32];
		sprintf_s(name, sizeof(name), "spawned cloth %d", ++scene->spawned_count);
		sceneSpawnCloth(scene, physics, name, info);
	}
}

void sceneUpdateComponents(scene_t* scene) {
	ui_t* ui = uiGet(scene->window);
	if (uiConsumeReset(ui)) {
		sceneReset(scene);
	}
	physics_world_component_t* world = sceneGetWorld(scene);
	sceneHandleSpawns(scene, ui, world->physics);

	ui_sim_settings_t settings = uiGetSimSettings(ui);
	sceneApplySettings(world->physics, &settings);
	const float dt = (float)timerObjectGetUsDeltaTime(scene->timer) / 1000000.0f;
	const uint64_t start_ticks = timerGetTicks();
	int steps = 0;
	if (settings.paused) {
		world->accumulator = 0.0f;
		if (uiConsumeStep(ui)) {
			physicsUpdate(world->physics, SCENE_FIXED_STEP);
			steps = 1;
		}
	}
	else {
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
	physics_stats_t physics_stats = physicsGetStats(world->physics);
	ui_stats_t stats = {
		.physics_ms = scene->physics_ms_average,
		.dropped_ms = scene->dropped_ms_reported,
		.particle_count = physics_stats.particle_count,
		.constraint_count = physics_stats.distance_constraint_count,
		.soft_body_count = physics_stats.soft_body_count,
		.contact_count = physics_stats.contact_count,
		.grabbing = physics_stats.grabbed_count > 0
	};
	uiSetStats(ui, &stats);

	uint64_t model_mask = (1ULL << scene->model_type);
	for (ecs_query_t query = ecsQueryCreate(scene->ecs, model_mask);
		ecsQueryValid(scene->ecs, &query); ecsQueryNext(scene->ecs, &query)) {
		model_component_t* model = ecsQueryGetComponent(scene->ecs, &query, scene->model_type);
		if (model->update_resource) model->update_resource(model->owned_resource);
	}
}

void sceneDestroyOwnedComponents(scene_t* scene) {
	uint64_t mask = (1ULL << scene->model_type);
	for (ecs_query_t query = ecsQueryCreate(scene->ecs, mask);
		ecsQueryValid(scene->ecs, &query); ecsQueryNext(scene->ecs, &query)) {
		model_component_t* model = ecsQueryGetComponent(scene->ecs, &query, scene->model_type);
		if (model->destroy_resource) {
			model->destroy_resource(model->owned_resource);
			model->owned_resource = NULL;
		}
	}
}

void sceneBallResourceUpdate(void* resource) { ballUpdateMesh((ball_t*)resource); }
void sceneBallResourceDestroy(void* resource) { ballDestroy((ball_t*)resource); }
void sceneClothResourceUpdate(void* resource) { clothUpdateMesh((cloth_t*)resource); }
void sceneClothResourceDestroy(void* resource) { clothDestroy((cloth_t*)resource); }

void sceneDrawModels(scene_t* scene) {
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
			ecs_entity_t entity = ecsQueryGetEntity(scene->ecs, &query);
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
			data.material[0] = 1.0f;
			data.material[1] = 1.0f;
			data.material[2] = 1.0f;
			data.material[3] = 1.0f;
			gpu_uniform_buffer_info_t uniform = {.data=&data, .size=sizeof(data)};
			rendererModelAdd(scene->render, &entity, model->mesh_info, model->shader_info, &uniform);
		}
	}
}
