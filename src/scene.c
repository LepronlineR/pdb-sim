#include "scene.h"

#include "heap.h"
#include "fs.h"
#include "wm.h"
#include "renderer.h"
#include "gpu.h"
#include "transform.h"
#include "timer_object.h"
#include "debug.h"
#include "vec3f.h"
#include "ecs.h"
#include "component.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <string.h>
#include <stdint.h>


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

	gpu_mesh_info_t cube_mesh;
	gpu_shader_info_t cube_shader;
	fs_work_t* vert_shader_work;
	fs_work_t* frag_shader_work;
} scene_t;

static void loadResources(scene_t* scene);
static void unloadResources(scene_t* scene);
static void spawnCamera(scene_t* scene);
static void drawModels(scene_t* scene);

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render) {
	scene_t* scene = heapAlloc(heap, sizeof(scene_t), 8);
	scene->heap = heap;
	scene->fs = fs;
	scene->window = window;
	scene->render = render;
	scene->timer = timerObjectCreate(heap, NULL);

	scene->ecs = ecsCreate(heap);
	scene->transform_type = ecsComponentRegister(scene->ecs, "transform", sizeof(transform_component_t), _Alignof(transform_component_t));
	scene->camera_type = ecsComponentRegister(scene->ecs, "camera", sizeof(camera_component_t), _Alignof(camera_component_t));
	scene->model_type = ecsComponentRegister(scene->ecs, "model", sizeof(model_component_t), _Alignof(model_component_t));
	scene->name_type = ecsComponentRegister(scene->ecs, "name", sizeof(name_component_t), _Alignof(name_component_t));

	loadResources(scene);
	spawnCamera(scene);

	return scene;
}

void sceneDestroy(scene_t* scene) {
	ecsDestroy(scene->ecs);
	timerObjectDestroy(scene->timer);
	unloadResources(scene);
	heapFree(scene->heap, scene);
}

void sceneUpdate(scene_t* scene) {
	timerObjectUpdate(scene->timer);
	ecsUpdate(scene->ecs);
	drawModels(scene);
	rendererFrameDone(scene->render);
}

static void loadResources(scene_t* scene) {
	scene->vert_shader_work = fsRead(scene->fs, "shaders/triangle.vert.spv", scene->heap, false, false);
	scene->frag_shader_work = fsRead(scene->fs, "shaders/triangle.frag.spv", scene->heap, false, false);
	scene->cube_shader = (gpu_shader_info_t){
		.vtx_shader_data = fsWorkGetBuffer(scene->vert_shader_work),
		.vtx_shader_size = fsWorkGetSize(scene->vert_shader_work),
		.frag_shader_data = fsWorkGetBuffer(scene->frag_shader_work),
		.frag_shader_size = fsWorkGetSize(scene->frag_shader_work),
		.uniform_buffer_count = 1
	};
	
	static vec3f_t cube_vtx[] = {
		{ -1.0f, -1.0f,  1.0f }, { 0.0f, 1.0f,  1.0f },
		{  1.0f, -1.0f,  1.0f }, { 1.0f, 0.0f,  1.0f },
		{  1.0f,  1.0f,  1.0f }, { 1.0f, 1.0f,  0.0f },
		{ -1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f,  0.0f },
		{ -1.0f, -1.0f, -1.0f }, { 0.0f, 1.0f,  0.0f },
		{  1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f,  1.0f },
		{  1.0f,  1.0f, -1.0f }, { 1.0f, 1.0f,  1.0f },
		{ -1.0f,  1.0f, -1.0f }, { 0.0f, 0.0f,  0.0f },
	};
	static uint16_t cube_idx[] = {
		0, 1, 2,
		2, 3, 0,
		1, 5, 6,
		6, 2, 1,
		7, 6, 5,
		5, 4, 7,
		4, 0, 3,
		3, 7, 4,
		4, 5, 1,
		1, 0, 4,
		3, 2, 6,
		6, 7, 3
	};
	scene->cube_mesh = (gpu_mesh_info_t){
		.layout = GPU_MESH_LAYOUT_TRI_P444_C444_I2,
		.vtx_data = cube_vtx,
		.vtx_data_size = sizeof(cube_vtx),
		.idx_data = cube_idx,
		.idx_data_size = sizeof(cube_idx),
	};
}

static void unloadResources(scene_t* scene) {
	heapFree(scene->heap, fsWorkGetBuffer(scene->vert_shader_work));
	heapFree(scene->heap, fsWorkGetBuffer(scene->frag_shader_work));
	fsWorkDestroy(scene->vert_shader_work);
	fsWorkDestroy(scene->frag_shader_work);
}

static void spawnCamera(scene_t* scene) {
	uint64_t ENTITY_CAMERA_MASK =
		(1ULL << scene->camera_type) |
		(1ULL << scene->name_type);
	scene->camera_entity = ecsEntityAdd(scene->ecs, ENTITY_CAMERA_MASK);

	name_component_t* name = ecsEntityGet(scene->ecs, scene->camera_entity, scene->name_type, true);
	strcpy_s(name->name, sizeof(name->name), "camera");

}

static void drawModels(scene_t* scene) {
	uint64_t QUERY_CAMERA_MASK = (1ULL << scene->camera_type);
	for (ecs_query_t camera_query = ecsQueryCreate(scene->ecs, QUERY_CAMERA_MASK);
		ecsQueryValid(scene->ecs, &camera_query);
		ecsQueryNext(scene->ecs, &camera_query)) {
			
		camera_component_t* camera_component = ecsQueryGetComponent(scene->ecs, &camera_query, scene->camera_type);
		uint64_t QUERY_MODEL_MASK = (1ULL << scene->transform_type) | (1ULL << scene->model_type);

		for (ecs_query_t query = ecsQueryCreate(scene->ecs, QUERY_MODEL_MASK);
			ecsQueryValid(scene->ecs, &query);
			ecsQueryNext(scene->ecs, &query)) {
			
			transform_component_t* transform_comp = ecsQueryGetComponent(scene->ecs, &query, scene->transform_type);
			model_component_t* model_comp = ecsQueryGetComponent(scene->ecs, &query, scene->model_type);
			ecs_entity_t ent = ecsQueryGetEntity(scene->ecs, &query);

			struct {
				mat4f_t projection;
				mat4f_t model;
				mat4f_t view;
			} uniform_data;

			uniform_data.projection = camera_component->projection;
			uniform_data.view = camera_component->view;
			transformConvertToMatrix(&transform_comp->transform, &uniform_data.model);
			gpu_uniform_buffer_info_t uniform_info = {
				.data = &uniform_data, sizeof(uniform_data)
			};
			rendererModelAdd(scene->render, &ent, model_comp->mesh_info, model_comp->shader_info, &uniform_info);
		}
	}
}