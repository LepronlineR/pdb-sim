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
typedef struct gpu_texture_t gpu_texture_t;				// Holds a sampled image

// MESH LAYOUTS
typedef enum gpu_mesh_layout_t {
	GPU_MESH_LAYOUT_TRI_P444_I2,
	GPU_MESH_LAYOUT_TRI_P444_C444_I2,
	GPU_MESH_LAYOUT_TRI_P444_N444_C444_I2,
	GPU_MESH_LAYOUT_TRI_P444_N444_C444_U44_I2,  // gpu_vertex_t: position, normal, color, uv
	GPU_MESH_LAYOUT_COUNT	// use this to track the total amount of mesh layouts (put it at the end)
} gpu_mesh_layout_t;

// Vertex of GPU_MESH_LAYOUT_TRI_P444_N444_C444_U44_I2.
typedef struct gpu_vertex_t {
	float position[3];
	float normal[3];
	float color[3];
	float uv[2];
} gpu_vertex_t;

// SHADER INFO
typedef struct gpu_shader_info_t {
	void* vtx_shader_data;
	size_t vtx_shader_size;
	void* frag_shader_data;
	size_t frag_shader_size;
	// Descriptor bindings: the uniform buffers first (bindings 0..), then the
	// textures as combined image samplers.
	int uniform_buffer_count;
	int texture_count;
	// Drawn behind everything (a skybox): depth test LEQUAL, no depth writes,
	// no culling.
	bool background;
	// Drawn in front of everything (editor gizmos): its depth is squeezed into
	// the nearest GPU_OVERLAY_DEPTH of the depth range, so the scene never hides
	// it while its own parts still sort by depth.
	bool overlay;
} gpu_shader_info_t;

#define GPU_OVERLAY_DEPTH 0.001f

// PIPELINE
typedef struct gpu_pipeline_info_t {
	gpu_shader_t* shader;
	gpu_mesh_layout_t mesh_layout;
	bool background;  // see gpu_shader_info_t.background
	bool overlay;     // see gpu_shader_info_t.overlay
} gpu_pipeline_info_t;

// DESCRIPTOR
typedef struct gpu_descriptor_info_t {
	gpu_shader_t* shader;
	gpu_uniform_buffer_t** uniform_buffers;
	int uniform_buffer_count;
	gpu_texture_t** textures;
	int texture_count;
} gpu_descriptor_info_t;

// TEXTURE
typedef struct gpu_texture_info_t {
	// Identifies the texture to the renderer beyond its address (see
	// gpu_mesh_info_t.id).
	uint64_t id;
	int width;
	int height;
	void* pixels;  // RGBA8, width * height * 4 bytes
	bool srgb;     // color data stored in sRGB (photos, albedo maps)
	bool clamp_v;  // clamp instead of repeat vertically (sky panoramas)
} gpu_texture_info_t;

// UNIFORM BUFFER
typedef struct gpu_uniform_buffer_info_t {
	void* data;
	size_t size;
} gpu_uniform_buffer_info_t;

// MESH
typedef struct gpu_mesh_info_t {
	gpu_mesh_layout_t layout;
	bool dynamic;
	// Identifies the mesh to the renderer beyond its address, which can be reused
	// after a mesh is freed. 0 for static meshes that live for the whole run.
	uint64_t id;
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

// Returns a new non-zero id for gpu_mesh_info_t.id. Call from one thread only.
uint64_t gpuMeshInfoNextId(void);
gpu_mesh_t* gpuCreateMesh(gpu_t* gpu, gpu_mesh_info_t* mesh_info);
void gpuUpdateMeshVertices(gpu_t* gpu, gpu_mesh_t* mesh, const void* data, size_t size);
void gpuDestroyMesh(gpu_t* gpu, gpu_mesh_t* mesh);
void gpuCommandBindMesh(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff, gpu_mesh_t* mesh);

// Returns a new non-zero id for gpu_texture_info_t.id. Call from one thread only.
uint64_t gpuTextureInfoNextId(void);
// Uploads the pixels into a mipmapped, sampled image. Waits for the queue, so
// create textures at load time.
gpu_texture_t* gpuCreateTexture(gpu_t* gpu, const gpu_texture_info_t* texture_info);
void gpuDestroyTexture(gpu_t* gpu, gpu_texture_t* texture);

gpu_shader_t* gpuCreateShader(gpu_t* gpu, gpu_shader_info_t* shader_info);
void gpuDestroyShader(gpu_t* gpu, gpu_shader_t* shader);

void gpuCommandDraw(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff);

void gpuQueueWaitIdle(gpu_t* gpu);

uint32_t gpuGetFrameCount(gpu_t* gpu);
VkInstance gpuGetInstance(gpu_t* gpu);
VkPhysicalDevice gpuGetPhysicalDevice(gpu_t* gpu);
VkDevice gpuGetDevice(gpu_t* gpu);
VkQueue gpuGetQueue(gpu_t* gpu);
uint32_t gpuGetQueueFamilyIndex(gpu_t* gpu);
VkRenderPass gpuGetRenderPass(gpu_t* gpu);
// MSAA samples of the render pass (pipelines drawing into it must match).
VkSampleCountFlagBits gpuGetSampleCount(gpu_t* gpu);
uint32_t gpuGetSwapchainImageCount(gpu_t* gpu);
uint32_t gpuGetMinimumImageCount(gpu_t* gpu);
VkCommandBuffer gpuGetCommandBuffer(gpu_cmd_buff_t* command_buffer);
uint32_t gpuGetMemoryTypeIndex(gpu_t* gpu, uint32_t bits,
	VkMemoryPropertyFlags property_flags);
void gpuCreateMeshLayouts(gpu_t* gpu);
void gpuDestroyMeshLayouts(gpu_t* gpu);
#endif
