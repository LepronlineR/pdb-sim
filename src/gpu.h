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
typedef struct gpu_mesh_t gpu_mesh_t;


// MESH LAYOUTS
typedef enum gpu_mesh_layout_t {
	GPU_MESH_LAYOUT_TRI_P444_I2,
	GPU_MESH_LAYOUT_TRI_P444_C444_I2,
	GPU_MESH_LAYOUT_COUNT	// use this to track the total amount of mesh layouts (put it at the end)
};


// others
typedef struct heap_t heap_t;
typedef struct wm_window_t wm_window_t;

// error debug
void* gpuError(gpu_t* gpu, const char* fn_name, const char* reason);

// FNs
gpu_t* gpuCreate(heap_t* heap, wm_window_t* window);
void gpuDestroy(gpu_t* gpu);


void gpuCommandDraw(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff);

#endif