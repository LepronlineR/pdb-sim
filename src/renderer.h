#ifndef __RENDERER_H__
#define __RENDERER_H__

typedef struct renderer_t renderer_t;


typedef struct heap_t heap_t;
typedef struct wm_window_t wm_window_t;

typedef struct ecs_entity_t ecs_entity_t;
typedef struct gpu_uniform_buffer_info_t gpu_uniform_buffer_info_t;
typedef struct gpu_mesh_info_t gpu_mesh_info_t;
typedef struct gpu_shader_info_t gpu_shader_info_t;

renderer_t* rendererCreate(heap_t* heap, wm_window_t* window);

void rendererDestroy(renderer_t* render);

// Add a model to the renderer queue
void rendererModelAdd(renderer_t* render, ecs_entity_t* entity, gpu_mesh_info_t* mesh, gpu_shader_info_t* shader, gpu_uniform_buffer_info_t* uniform);

// Finished rendering the frame
void rendererFrameDone(renderer_t* render);

#endif