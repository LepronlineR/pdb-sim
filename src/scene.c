#include "scene.h"

#include "ball.h"
#include "transform.h"
#include "component.h"
#include "ecs.h"
#include "fs.h"
#include "gpu.h"
#include "heap.h"
#include "physics.h"
#include "renderer.h"
#include "shader_loader.h"
#include "timer_object.h"
#include "ui.h"
#include "vec3f.h"
#include "wm.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
	ecs_entity_t camera_entity;

	shader_loader_t* shaders;
	gpu_mesh_info_t cube_mesh;
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

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render) {
	scene_t* scene = heapAlloc(heap, sizeof(*scene), 8);
	memset(scene, 0, sizeof(*scene));
	scene->heap = heap;
	scene->fs = fs;
	scene->window = window;
	scene->render = render;
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

	int physics_type = ecsComponentRegister(scene->ecs, "physics_world",
		sizeof(physics_world_component_t), _Alignof(physics_world_component_t));
	ecs_entity_t physics_entity = ecsEntityAdd(scene->ecs,
		(1ULL << physics_type) | (1ULL << scene->name_type));
	physics_world_component_t* world = ecsEntityGet(scene->ecs, physics_entity, physics_type, true);
	world->physics = physicsCreate(heap);
	world->accumulator = 0.0f;
	physicsSetGravity(world->physics, -9.81f);
	physicsSetSolverIterations(world->physics, 6);
	physicsSetGroundCompliance(world->physics, 0.0f);
	name_component_t* physics_name = ecsEntityGet(scene->ecs, physics_entity, scene->name_type, true);
	strcpy_s(physics_name->name, sizeof(physics_name->name), "physics world");

	sceneLoadResources(scene);
	sceneSpawnCamera(scene);
	sceneSpawnDemo(scene, world->physics);
	return scene;
}

void sceneDestroy(scene_t* scene) {
	sceneDestroyOwnedComponents(scene);
	int physics_type = ecsComponentFind(scene->ecs, "physics_world");
	if (physics_type >= 0) {
		uint64_t mask = (1ULL << physics_type);
		for (ecs_query_t query = ecsQueryCreate(scene->ecs, mask);
			ecsQueryValid(scene->ecs, &query); ecsQueryNext(scene->ecs, &query)) {
			physics_world_component_t* world = ecsQueryGetComponent(scene->ecs, &query, physics_type);
			physicsDestroy(world->physics);
		}
	}
	ecsDestroy(scene->ecs);
	timerObjectDestroy(scene->timer);
	sceneUnloadResources(scene);
	heapFree(scene->heap, scene);
}

void sceneUpdate(scene_t* scene) {
	timerObjectUpdate(scene->timer);
	ecsUpdate(scene->ecs);
	sceneUpdateCamera(scene);
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
	vec3f_t eye = { 8.5f, 6.5f, 11.5f };
	vec3f_t center = { 0.0f, 2.5f, 0.0f };
	vec3f_t up = vec3fUp();
	mat4fMakePerspective(&camera->projection, (float)M_PI / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f);
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
	int mouse_x = 0;
	int mouse_y = 0;
	wmGetMouseLoc(scene->window, &mouse_x, &mouse_y);
	if (!(wmGetMouseMask(scene->window) & k_mouse_button_left) ||
		uiWantsMouse(uiGet(scene->window))) return;

	camera_component_t* camera = ecsEntityGet(scene->ecs, scene->camera_entity,
		scene->camera_type, false);
	if (!camera) return;
	camera->orbit_yaw -= (float)mouse_x * 0.008f;
	camera->orbit_pitch -= (float)mouse_y * 0.008f;
	camera->orbit_pitch = __max(-1.45f, __min(1.45f, camera->orbit_pitch));
	vec3f_t center = {
		camera->orbit_target[0], camera->orbit_target[1], camera->orbit_target[2]
	};
	vec3f_t eye = sceneCameraGetEye(camera);
	vec3f_t up = vec3fUp();
	mat4fMakeLookAt(&camera->view, &eye, &center, &up);
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

void sceneSpawnBall(scene_t* scene, physics_t* physics, const char* name, ball_info_t info) {
	ball_t* ball = ballCreate(scene->heap, physics, &info);
	if (!ball) return;
	ecs_entity_t entity = sceneSpawnModel(scene, name, vec3fZero(), vec3fOne(), ballGetMesh(ball));
	model_component_t* model = ecsEntityGet(scene->ecs, entity, scene->model_type, true);
	model->owned_resource = ball;
	model->update_resource = sceneBallResourceUpdate;
	model->destroy_resource = sceneBallResourceDestroy;
	model->shader_info = shaderLoaderGetPbr(scene->shaders);
}

void sceneSpawnDemo(scene_t* scene, physics_t* physics) {
	sceneSpawnModel(scene, "ground", (vec3f_t){0,-.1f,0}, (vec3f_t){6,.1f,6}, &scene->cube_mesh);
	sceneSpawnBall(scene, physics, "ball 1", ballInfoDefault(
		(vec3f_t){0,4,0}, (vec3f_t){.95f,.30f,.22f}, 1.0f));
	sceneSpawnBall(scene, physics, "ball 2", ballInfoDefault(
		(vec3f_t){0,8,0}, (vec3f_t){.25f,.82f,.42f}, 5.0f));
	sceneSpawnBall(scene, physics, "ball 3", ballInfoDefault(
		(vec3f_t){0,12,0}, (vec3f_t){.1f,.13f,.72f}, 10.0f));
	sceneSpawnBall(scene, physics, "ball 4", ballInfoDefault(
		(vec3f_t){5,12,0}, (vec3f_t){.9f,.82f,.12f}, 0.1f));
}

void sceneUpdateComponents(scene_t* scene) {
	static int spawned_ball_count;
	int physics_type = ecsComponentFind(scene->ecs, "physics_world");
	if (physics_type >= 0) {
		uint64_t mask = (1ULL << physics_type);
		for (ecs_query_t query = ecsQueryCreate(scene->ecs, mask);
			ecsQueryValid(scene->ecs, &query); ecsQueryNext(scene->ecs, &query)) {
			physics_world_component_t* world = ecsQueryGetComponent(scene->ecs, &query, physics_type);
			ui_ball_info_t ui_spawn_info;
			ui_t* ui = uiGet(scene->window);
			while (uiConsumeBallSpawn(ui, &ui_spawn_info)) {
				if (!ballCanCreate(world->physics)) break;
				ball_info_t spawn_info = {
					.position = { ui_spawn_info.position[0], ui_spawn_info.position[1],
						ui_spawn_info.position[2] },
					.color = { ui_spawn_info.color[0], ui_spawn_info.color[1],
						ui_spawn_info.color[2] },
					.radius = ui_spawn_info.radius,
					.mass = ui_spawn_info.mass,
					.structural_compliance = ui_spawn_info.structural_compliance,
					.bending_compliance = ui_spawn_info.bending_compliance,
					.volume_compliance = ui_spawn_info.volume_compliance,
					.velocity_retention = ui_spawn_info.velocity_retention
				};
				char name[32];
				sprintf_s(name, sizeof(name), "spawned ball %d", ++spawned_ball_count);
				sceneSpawnBall(scene, world->physics, name, spawn_info);
			}
			const float fixed_step = 1.0f / 60.0f;
			float dt = (float)timerObjectGetUsDeltaTime(scene->timer) / 1000000.0f;
			world->accumulator += __min(dt, .05f);
			while (world->accumulator >= fixed_step) {
				physicsUpdate(world->physics, fixed_step);
				world->accumulator -= fixed_step;
			}
		}
	}
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
