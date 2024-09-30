#ifndef __GPU_H__
#define __GPU_H__

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan\vulkan.h>

#include <stdint.h>
#include <stdbool.h>

/* GPU


*/

typedef struct gpu_t gpu_t;								// Holds GPU data
typedef struct gpu_frame_t gpu_frame_t;					// Holds frame data
typedef struct gpu_cmd_buff_t gpu_cmd_buff_t;			// Holds command buffer data
typedef struct gpu_mesh_t gpu_mesh_t;					// Holds mesh information
typedef struct gpu_pipeline_t gpu_pipeline_t;			// Holds pipeline information
typedef struct gpu_descriptor_t gpu_descriptor_t;		// Holds descriptor sets
typedef struct gpu_shader_t gpu_shader_t;
typedef struct gpu_uniform_buffer_t gpu_uniform_buffer_t;

// MESH LAYOUTS
typedef enum gpu_mesh_layout_t {
	GPU_MESH_LAYOUT_TRI_P444_I2,
	GPU_MESH_LAYOUT_TRI_P444_C444_I2,
	GPU_MESH_LAYOUT_COUNT	// use this to track the total amount of mesh layouts (put it at the end)
} gpu_mesh_layout_t;

// SHADER INFO
typedef struct gpu_shader_info_t {
	void* vtx_shader_data;
	size_t vtx_shader_size;
	void* frag_shader_data;
	size_t frag_shader_size;
	int uniform_buffer_count;
} gpu_shader_info_t;

// PIPELINE
typedef struct gpu_pipeline_info_t {
	gpu_shader_t* shader;
	gpu_mesh_layout_t mesh_layout;
} gpu_pipeline_info_t;

// DESCRIPTOR
typedef struct gpu_descriptor_info_t {
	gpu_shader_t* shader;
	gpu_uniform_buffer_t** uniform_buffers;
	int uniform_buffer_count;
} gpu_descriptor_info_t;

// UNIFORM BUFFER
typedef struct gpu_uniform_buffer_info_t {
	void* data;
	size_t size;
} gpu_uniform_buffer_info_t;

// MESH
typedef struct gpu_mesh_info_t {
	gpu_mesh_layout_t layout;
	void* vtx_data;
	void* idx_data;
	size_t vtx_data_size;
	size_t idx_data_size;
} gpu_mesh_info_t;

// others
typedef struct heap_t heap_t;
typedef struct wm_window_t wm_window_t;


// Calls whenever there is an error with a function call
// 
// RETURN: NULL
void* gpuError(gpu_t* gpu, const char* fn_name, const char* reason);


gpu_t* gpuCreate(heap_t* heap, wm_window_t* window);
void gpuDestroy(gpu_t* gpu);

gpu_cmd_buff_t* gpuBeginFrameUpdate(gpu_t* gpu);
void gpuEndFrameUpdate(gpu_t* gpu);

gpu_pipeline_t* gpuCreatePipeline(gpu_t* gpu, const gpu_pipeline_info_t* pipeline_info);
void gpuCommandBindPipeline(gpu_cmd_buff_t* cmd_buff, gpu_pipeline_t* pipeline);
void gpuDestroyPipeline(gpu_t* gpu, gpu_pipeline_t* pipeline);

gpu_descriptor_t* gpuCreateDescriptorSets(gpu_t* gpu, const gpu_descriptor_info_t* descriptor);
void gpuCommandBindDescriptorSets(gpu_cmd_buff_t* cmd_buff, gpu_descriptor_t* descriptor);
void gpuDestroyDescriptorSets(gpu_t* gpu, gpu_descriptor_t* descriptor);

gpu_uniform_buffer_t* gpuCreateUniformBuffer(gpu_t* gpu, const gpu_uniform_buffer_info_t* ub_info);
void gpuUpdateUniformBuffer(gpu_t* gpu, gpu_uniform_buffer_t* ub, const void* data, size_t size);
void gpuDestroyUniformBuffer(gpu_t* gpu, gpu_uniform_buffer_t* ub);

gpu_mesh_t* gpuCreateMesh(gpu_t* gpu, gpu_mesh_info_t* mesh_info);
void gpuDestroyMesh(gpu_t* gpu, gpu_mesh_t* mesh);
void gpuCommandBindMesh(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff, gpu_mesh_t* mesh);

gpu_shader_t* gpuCreateShader(gpu_t* gpu, gpu_shader_info_t* shader_info);
void gpuDestroyShader(gpu_t* gpu, gpu_shader_t* shader);

void gpuCommandDraw(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff);

void gpuQueueWaitIdle(gpu_t* gpu);

uint32_t gpuGetFrameCount(gpu_t* gpu);
#endif