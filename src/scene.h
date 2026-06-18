#ifndef __SCENE_H__
#define __SCENE_H__

#include "ball.h"
#include "ecs.h"

typedef struct scene_t scene_t;

typedef struct fs_t fs_t;
typedef struct heap_t heap_t;
typedef struct renderer_t renderer_t;
typedef struct wm_window_t wm_window_t;
typedef struct physics_t physics_t;
typedef struct gpu_mesh_info_t gpu_mesh_info_t;

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render);

void sceneDestroy(scene_t* scene);

void sceneUpdate(scene_t* scene);
void sceneLoadResources(scene_t* scene);
void sceneUnloadResources(scene_t* scene);
void sceneSpawnCamera(scene_t* scene);
ecs_entity_t sceneSpawnModel(scene_t* scene, const char* name,
	vec3f_t position, vec3f_t scale, gpu_mesh_info_t* mesh);
void sceneSpawnBall(scene_t* scene, physics_t* physics, const char* name,
	ball_info_t info);
void sceneSpawnDemo(scene_t* scene, physics_t* physics);
void sceneUpdateComponents(scene_t* scene);
void sceneUpdateCamera(scene_t* scene);
void sceneDestroyOwnedComponents(scene_t* scene);
void sceneDrawModels(scene_t* scene);
void sceneBallResourceUpdate(void* resource);
void sceneBallResourceDestroy(void* resource);

#endif
