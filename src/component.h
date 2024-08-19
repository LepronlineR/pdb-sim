#ifndef __COMPONENT_H__
#define __COMPONENT_H__

// contains all of the default components for basic functionality

// predef
typedef struct transform_t transform_t;
typedef struct mat4f_t mat4f_t;
typedef struct gpu_mesh_info_t gpu_mesh_info_t;
typedef struct gpu_shader_info_t gpu_shader_info_t;

// components
typedef struct transform_component_t {
	transform_t transform;
} transform_component_t;

typedef struct camera_component_t {
	mat4f_t projection;
	mat4f_t view;
} camera_component_t;

typedef struct model_component_t {
	gpu_mesh_info_t* mesh_info;
	gpu_shader_info_t* shader_info;
} model_component_t;

typedef struct model_texture_component_t {
	gpu_mesh_info_t* mesh_info;
	gpu_shader_info_t* shader_info;
} model_texture_component_t;

typedef struct name_component_t {
	char name[32];
} name_component_t;

#endif